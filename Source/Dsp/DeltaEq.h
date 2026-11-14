#pragma once

/**
 * @file DeltaEq.h
 * @brief everplus 逆向得到的 10 段 EQ 曲线，作为「三角洲特化」频带权重。
 *
 * 数据来源：EverplusApo.dll 的 84 字节原生配置结构（见 DISASM.md）
 *   偏移 0x18 起 10 个 float，对应 31.25 / 62.5 / 125 / 250 / 500 /
 *   1000 / 2000 / 4000 / 8000 / 16000 Hz。
 *   默认值 [-6, -2, +4, +3, -2, -1, +1, +1, 0, -2] dB。
 *
 * 这条曲线是厂商针对这个游戏调出来的：抬高 125–250 Hz（脚步的「咚」），
 * 压低 31.25–62.5 Hz（地鸣/引擎底噪），轻微抬 2–4 kHz（换弹、布料摩擦）。
 * 它比任何凭感觉写的 bandVote 权重都更贴近实际混音。
 *
 * 注意适用边界：everplus 的 16 带检测器只覆盖 100–3200 Hz，所以曲线里
 * 31.25 / 62.5 Hz 那两个点**不参与**本项目的频带加权（那一段根本不进分析）。
 * 真正的次低频轰鸣由指纹门的 rumble 模板负责，而不是靠加权压掉。
 *
 * 注意：这是**分析权重**，不是音频处理。可视化器不改变任何声音，
 * 只是让方向投票更偏向脚步声所在的频段。
 */

#include <algorithm>
#include <array>
#include <cmath>

namespace delta
{

/** EQ 曲线的 10 个频点（Hz），与 everplus 配置一致 */
inline constexpr std::array<float, 10> kEqFreqs {
    31.25f, 62.5f, 125.0f, 250.0f, 500.0f, 1000.0f, 2000.0f, 4000.0f, 8000.0f, 16000.0f
};

/** 对应的增益（dB），everplus 出厂默认值 */
inline constexpr std::array<float, 10> kEqDb {
    -6.0f, -2.0f, 4.0f, 3.0f, -2.0f, -1.0f, 1.0f, 1.0f, 0.0f, -2.0f
};

/**
 * @brief 在给定频率上对 EQ 曲线做对数插值
 * @param hz 频率（Hz），超出 31.25–16000 时取端点值
 * @return 该频点的增益（dB）
 */
inline float eqGainDbAt(float hz) noexcept
{
    const float f = std::clamp(hz, kEqFreqs.front(), kEqFreqs.back());
    const float lf = std::log2(f);

    for (std::size_t i = 0; i + 1 < kEqFreqs.size(); ++i)
    {
        const float a = kEqFreqs[i];
        const float b = kEqFreqs[i + 1];
        if (f <= b)
        {
            const float t = (lf - std::log2(a)) / (std::log2(b) - std::log2(a));
            return kEqDb[i] + t * (kEqDb[i + 1] - kEqDb[i]);
        }
    }
    return kEqDb.back();
}

/**
 * @brief 把 EQ 增益换成线性权重
 * @param hz       频带中心频率
 * @param strength 0 = 完全平坦（权重恒为 1），1 = 完全按 EQ 曲线
 *
 * 限制在 [0.1, 4.0]：避免 −6 dB 以下把某个频带彻底静音，
 * 也避免 +12 dB 时单个频带独裁整个投票。
 */
inline float eqWeightAt(float hz, float strength = 1.0f) noexcept
{
    const float db = eqGainDbAt(hz);
    const float linear = std::pow(10.0f, db / 20.0f);
    const float s = std::clamp(strength, 0.0f, 1.0f);
    return std::clamp(1.0f + s * (linear - 1.0f), 0.1f, 4.0f);
}

} // namespace delta
