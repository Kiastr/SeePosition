#pragma once

/**
 * @file FingerprintGate.h
 * @brief 多模板指纹门 —— 复用 everplus 的 WFP1 匹配机制，扩成「一个文件装多套模板」。
 *
 * 原机制（反汇编 0x180048640 / 0x180046CF0）：
 *   16 带对数功率 → 1 秒（50 帧）均值 → 减去 16 带均值（去电平）
 *   → 与模板逐行做余弦相似度 → 取最大值，≥ 0.97 判定命中
 *
 * 原版只有两套噪声模板（wind10 / npp）。这里把加载器扩成任意多套，
 * 每套带一个标签，于是可以同时装：
 *   wind    —— 风声，逆向产物
 *   rumble  —— 低频轰鸣，逆向产物（原 npp）
 *   voice   —— 人声，用公开语料按同一条分析链生成
 *
 * 仍然不做任何音频处理：命中只表示「这段声音不该被画成方向点」。
 *
 * 文件格式 EWG1（全部小端）：
 *   char[4]  "EWG1"
 *   u32      version = 1
 *   u32      bands   = 16
 *   u32      count
 *   重复 count 次：
 *     u32    labelLen
 *     char[] label（UTF-8，不含结尾 0）
 *     u32    rows
 *     f32[rows * bands]  每行已去均值并 L2 归一化
 */

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

#include "BandAnalyzer.h"

namespace delta
{

class FingerprintGate
{
public:
    /** 匹配结果 */
    struct Match
    {
        float cosine = -1.0f;   ///< 最大余弦
        int templateIndex = -1; ///< 命中的模板下标
        int row = -1;           ///< 命中的模板行
    };

    /** 一套模板 */
    struct Template
    {
        std::string label;
        int rowCount = 0;
        std::vector<float> rows; ///< rowCount * kBandCount，行已去均值并单位化
    };

    //--------------------------------------------------------------------------
    // 加载
    //--------------------------------------------------------------------------

    /** 每个模板最多保留多少行（超采样会让匹配变慢且更容易误命中） */
    void setMaxRowsPerTemplate(int n) noexcept { maxRowsPerTemplate = std::max(1, n); }

    /**
     * @brief 从 EWG1 文件加载全部模板
     * @return 成功返回 true；文件不存在、魔数错、版本不匹配都返回 false（不抛异常）
     */
    bool loadFile(const char* path)
    {
        templates.clear();
        std::FILE* f = std::fopen(path, "rb");
        if (f == nullptr)
            return false;

        char magic[4] {};
        std::uint32_t version = 0;
        std::uint32_t bands = 0;
        std::uint32_t count = 0;

        bool ok = std::fread(magic, 1, 4, f) == 4
                  && std::memcmp(magic, "EWG1", 4) == 0
                  && std::fread(&version, 4, 1, f) == 1 && version == 1
                  && std::fread(&bands, 4, 1, f) == 1 && bands == (std::uint32_t) kBandCount
                  && std::fread(&count, 4, 1, f) == 1 && count > 0 && count <= 64;

        if (! ok)
        {
            std::fclose(f);
            return false;
        }

        for (std::uint32_t t = 0; t < count && ok; ++t)
        {
            std::uint32_t labelLen = 0;
            if (std::fread(&labelLen, 4, 1, f) != 1 || labelLen == 0 || labelLen > 64)
            {
                ok = false;
                break;
            }

            std::string label(labelLen, '\0');
            if (std::fread(label.data(), 1, labelLen, f) != labelLen)
            {
                ok = false;
                break;
            }

            std::uint32_t rows = 0;
            if (std::fread(&rows, 4, 1, f) != 1 || rows == 0 || rows > 2000000)
            {
                ok = false;
                break;
            }

            std::vector<float> raw((std::size_t) rows * kBandCount);
            if (std::fread(raw.data(), sizeof(float), raw.size(), f) != raw.size())
            {
                ok = false;
                break;
            }

            Template tpl;
            tpl.label = label;
            tpl.rowCount = (int) rows;
            tpl.rows = std::move(raw);
            templates.push_back(std::move(tpl));
        }

        std::fclose(f);
        if (! ok)
        {
            templates.clear();
            return false;
        }

        for (auto& tpl : templates)
            decimateAndNormalise(tpl);

        reset();
        return true;
    }

    //--------------------------------------------------------------------------
    // 运行
    //--------------------------------------------------------------------------

    void reset() noexcept
    {
        history.fill({});
        historyPos = 0;
        historyFilled = 0;
        cachedMatch = Match {};
        matchedLabel.clear();
    }

    /** 喂入一帧 16 带**线性**功率（不是 dB） */
    void pushFrame(const std::array<float, kBandCount>& power) noexcept
    {
        std::array<float, kBandCount> logBands {};
        for (int b = 0; b < kBandCount; ++b)
            logBands[(std::size_t) b] = std::log10(std::max(power[(std::size_t) b], 1.0e-30f));

        history[(std::size_t) historyPos] = logBands;
        historyPos = (historyPos + 1) % kContext;
        if (historyFilled < kContext)
            ++historyFilled;
    }

    /** 1 秒窗口是否已填满 */
    bool ready() const noexcept { return historyFilled >= kContext && ! templates.empty(); }

    int templateCount() const noexcept { return (int) templates.size(); }

    int templateRowCount(int index) const noexcept
    {
        return (index >= 0 && index < (int) templates.size()) ? templates[(std::size_t) index].rowCount : 0;
    }

    const std::string& templateLabel(int index) const noexcept
    {
        static const std::string empty;
        return (index >= 0 && index < (int) templates.size()) ? templates[(std::size_t) index].label : empty;
    }

    /**
     * @brief 用当前 1 秒窗口做一次匹配
     * @return 最佳匹配；窗口未填满时返回余弦 -1
     */
    struct Probe
    {
        std::array<float, kBandCount> v {};
        bool valid = false;
    };

    Probe makeProbe() const noexcept
    {
        Probe p;
        if (! ready())
            return p;

        std::array<double, kBandCount> mean {};
        for (const auto& row : history)
            for (int b = 0; b < kBandCount; ++b)
                mean[(std::size_t) b] += (double) row[(std::size_t) b];
        for (auto& v : mean)
            v /= (double) kContext;

        // 去电平：与 everplus 0x1800486D6 的 v = mu − mean(mu) 一致
        double dc = 0.0;
        for (double v : mean)
            dc += v;
        dc /= (double) kBandCount;

        double norm = 0.0;
        for (int b = 0; b < kBandCount; ++b)
        {
            const double v = mean[(std::size_t) b] - dc;
            p.v[(std::size_t) b] = (float) v;
            norm += v * v;
        }
        if (norm < 1.0e-12)
            return p;

        const float inv = (float) (1.0 / std::sqrt(norm));
        for (auto& v : p.v)
            v *= inv;
        p.valid = true;
        return p;
    }

    Match evaluate() const noexcept
    {
        const Probe probe = makeProbe();
        if (! probe.valid)
            return Match {};

        Match best;
        for (int t = 0; t < (int) templates.size(); ++t)
        {
            const auto& tpl = templates[(std::size_t) t];
            for (int r = 0; r < tpl.rowCount; ++r)
            {
                const float* row = tpl.rows.data() + (std::size_t) r * kBandCount;
                float dot = 0.0f;
                for (int b = 0; b < kBandCount; ++b)
                    dot += probe.v[(std::size_t) b] * row[b];
                if (dot > best.cosine)
                {
                    best.cosine = dot;
                    best.templateIndex = t;
                    best.row = r;
                }
            }
        }
        return best;
    }

    /** 执行一次匹配并把结果缓存下来（供 UI / 触发门读取） */
    void update() noexcept
    {
        cachedMatch = evaluate();

        // 每套模板各自的最高分：既用于诊断，也用于「人声」与其他噪声的仲裁
        perTemplate.assign(templates.size(), -1.0f);
        if (cachedMatch.templateIndex >= 0)
        {
            // evaluate() 只返回全局最优，这里再补算每套的最高分
            const auto probe = makeProbe();
            if (probe.valid)
            {
                for (std::size_t t = 0; t < templates.size(); ++t)
                {
                    const auto& tpl = templates[t];
                    float best = -1.0f;
                    for (int r = 0; r < tpl.rowCount; ++r)
                    {
                        const float* row = tpl.rows.data() + (std::size_t) r * kBandCount;
                        float dot = 0.0f;
                        for (int b = 0; b < kBandCount; ++b)
                            dot += probe.v[(std::size_t) b] * row[b];
                        best = std::max(best, dot);
                    }
                    perTemplate[t] = best;
                }
            }
        }

        matchedLabel = (cachedMatch.templateIndex >= 0)
                           ? templates[(std::size_t) cachedMatch.templateIndex].label
                           : std::string {};
    }

    /** 某套模板的最高余弦（未匹配时 -1） */
    float bestCosineFor(int index) const noexcept
    {
        return (index >= 0 && index < (int) perTemplate.size()) ? perTemplate[(std::size_t) index] : -1.0f;
    }

    /** 按标签取最高余弦 */
    float bestCosineForLabel(const char* label) const noexcept
    {
        for (int i = 0; i < (int) templates.size(); ++i)
            if (templates[(std::size_t) i].label == label)
                return bestCosineFor(i);
        return -1.0f;
    }

    const Match& lastMatch() const noexcept { return cachedMatch; }
    const std::string& lastMatchedLabel() const noexcept { return matchedLabel; }

    /** 是否命中指定标签（阈值 + 迟滞，避免边缘抖动） */
    bool matchesLabel(const char* label, float onThreshold = 0.97f, float offThreshold = 0.94f) const noexcept
    {
        if (cachedMatch.templateIndex < 0)
            return false;
        const auto& lbl = templates[(std::size_t) cachedMatch.templateIndex].label;
        if (lbl != label)
            return false;
        return cachedMatch.cosine >= (wasActive ? offThreshold : onThreshold);
    }

    void setActive(bool v) noexcept { wasActive = v; }
    bool active() const noexcept { return wasActive; }

private:
    static constexpr int kContext = 50; ///< 50 帧 × 20 ms = 1 秒

    void decimateAndNormalise(Template& tpl) const
    {
        // 行太多时均匀抽稀：匹配是取最大值，抽稀只降低覆盖率，不改变判定上界太多
        if (tpl.rowCount > maxRowsPerTemplate)
        {
            std::vector<float> kept;
            kept.reserve((std::size_t) maxRowsPerTemplate * kBandCount);
            for (int i = 0; i < maxRowsPerTemplate; ++i)
            {
                const int src = (int) ((long long) i * tpl.rowCount / maxRowsPerTemplate);
                const float* row = tpl.rows.data() + (std::size_t) src * kBandCount;
                kept.insert(kept.end(), row, row + kBandCount);
            }
            tpl.rows = std::move(kept);
            tpl.rowCount = maxRowsPerTemplate;
        }

        // 防御性重归一化：文件可能来自别的工具
        for (int r = 0; r < tpl.rowCount; ++r)
        {
            float* row = tpl.rows.data() + (std::size_t) r * kBandCount;
            float dc = 0.0f;
            for (int b = 0; b < kBandCount; ++b)
                dc += row[b];
            dc /= (float) kBandCount;
            float norm = 0.0f;
            for (int b = 0; b < kBandCount; ++b)
            {
                row[b] -= dc;
                norm += row[b] * row[b];
            }
            const float inv = (norm > 1.0e-12f) ? (1.0f / std::sqrt(norm)) : 0.0f;
            for (int b = 0; b < kBandCount; ++b)
                row[b] *= inv;
        }
    }

    std::vector<Template> templates;
    std::array<std::array<float, kBandCount>, kContext> history {};
    int historyPos = 0;
    int historyFilled = 0;
    int maxRowsPerTemplate = 2000;
    Match cachedMatch {};
    std::vector<float> perTemplate;
    std::string matchedLabel;
    bool wasActive = false;
};

} // namespace delta
