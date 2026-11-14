#pragma once

/**
 * @file VoiceGate.h
 * @brief 人声门 —— 判断「这一秒的声音是不是人声」，供方向可视化用来抑制误报。
 *
 * 只有两条判据同时成立才算人声：
 *
 *   1. 指纹相似度 ≥ 0.97
 *      复用 everplus 的 WFP1 匹配机制（16 带对数功率 + 1 秒均值 + 去电平 + 余弦），
 *      模板标签为 "voice"。
 *
 *   2. 包络起伏 ≥ 4.0 dB
 *      语音有音节节奏，1 秒内总能量的 dB 标准差实测 10.4 dB；
 *      而稳态噪声（风 1.0 / 低频轰鸣 2.1 / 纯音 0.0 / 白噪 0.6）都在 2.2 dB 以下。
 *
 * 为什么需要第二条：实测 440 Hz 纯音对人声模板的余弦能到 0.970，正好卡在阈值上，
 * 单靠指纹会在稳态音上误判。加上包络起伏后，纯音的余弦再高也过不了门。
 *
 * 本类不做任何音频处理，只输出一个布尔判断。
 */

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <vector>

#include "BandAnalyzer.h"
#include "FingerprintGate.h"

namespace delta
{

class VoiceGate
{
public:
    /** 指纹命中阈值（开 / 关，用迟滞避免边缘抖动） */
    float cosineOn = 0.97f;
    float cosineOff = 0.94f;

    /** 包络起伏下限（dB）：语音实测 10.4，稳态噪声 ≤ 2.2 */
    float envelopeStdOnDb = 6.0f;
    float envelopeStdOffDb = 4.5f;

    bool loadFile(const char* path)
    {
        const bool ok = gate.loadFile(path);
        reset();
        return ok;
    }

    bool loaded() const noexcept { return gate.templateCount() > 0; }
    int templateCount() const noexcept { return gate.templateCount(); }

    void reset() noexcept
    {
        gate.reset();
        history.fill(0.0f);
        historyPos = 0;
        historyFilled = 0;
        lastCosine = -1.0f;
        lastStdDb = 0.0f;
        isActive = false;
    }

    /** 喂入一帧 16 带线性功率，并立即更新判定 */
    void pushFrame(const std::array<float, kBandCount>& power) noexcept
    {
        gate.pushFrame(power);

        const float total = power[0] + power[1] + power[2] + power[3] + power[4] + power[5]
                            + power[6] + power[7] + power[8] + power[9] + power[10] + power[11]
                            + power[12] + power[13] + power[14] + power[15];
        const float db = 10.0f * std::log10(std::max(total, 1.0e-20f));

        history[(std::size_t) historyPos] = db;
        historyPos = (historyPos + 1) % kContext;
        if (historyFilled < kContext)
            ++historyFilled;

        if (historyFilled >= kContext)
        {
            float mean = 0.0f;
            for (float v : history)
                mean += v;
            mean /= (float) kContext;
            float var = 0.0f;
            for (float v : history)
            {
                const float d = v - mean;
                var += d * d;
            }
            lastStdDb = std::sqrt(var / (float) kContext);
        }

        if (! gate.ready())
        {
            lastCosine = -1.0f;
            return;
        }

        gate.update();
        lastCosine = gate.bestCosineForLabel("voice");

        const bool onOk = lastCosine >= cosineOn && lastStdDb >= envelopeStdOnDb;
        const bool keepOn = lastCosine >= cosineOff && lastStdDb >= envelopeStdOffDb;
        isActive = isActive ? keepOn : onOk;
    }

    bool ready() const noexcept { return historyFilled >= kContext && gate.templateCount() > 0; }
    bool active() const noexcept { return isActive; }
    float lastVoiceCosine() const noexcept { return lastCosine; }
    float lastEnvelopeStdDb() const noexcept { return lastStdDb; }

private:
    static constexpr int kContext = 50;

    FingerprintGate gate;
    std::array<float, kContext> history {};
    int historyPos = 0;
    int historyFilled = 0;
    float lastCosine = -1.0f;
    float lastStdDb = 0.0f;
    bool isActive = false;
};

} // namespace delta
