#pragma once

/**
 * @file BandAnalyzer.h
 * @brief 16 带实时分析器 —— 逐条移植 everplus 的真实分析链（0x180046CF0 / 0x180045B60）。
 *
 * 完整链条（每一步都在反汇编里核对过）：
 *
 *   20 ms 帧（48 kHz 下 960 样本）
 *     → 6:1 盒式抽取到 8 kHz（160 点）
 *     → Tukey 窗 α=0.25：t=min(i,n−1−i)/(n−1)，t<0.125 时 0.5(1+cos(π(8t−1)))
 *     → 补零到 256 点复数 FFT（bin = 31.25 Hz）
 *     → 16 带盒式重叠积分，带边界 100·32^(b/16)，覆盖 100–3200 Hz
 *
 * 本类只做「特征提取」，不做任何音频处理，也不改变播放的声音。
 * 不依赖 JUCE，可在任意平台单独编译与测试。
 */

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <vector>

namespace delta
{

/** 频带数量，与 everplus WFP1 模板严格一致 */
inline constexpr int kBandCount = 16;

/** FFT 点数，与 everplus 一致（31.25 Hz / bin） */
inline constexpr int kFftSize = 256;

/** 分析帧长（秒） */
inline constexpr double kFrameSeconds = 0.02;

/** 第 b 个频带的下边界（Hz） */
inline float bandLowHz(int b) noexcept
{
    return 100.0f * std::pow(32.0f, (float) b / (float) kBandCount);
}

/** 第 b 个频带的上边界（Hz） */
inline float bandHighHz(int b) noexcept
{
    return 100.0f * std::pow(32.0f, (float) (b + 1) / (float) kBandCount);
}

/** 第 b 个频带的几何中心（Hz），用作 EQ 曲线取样点 */
inline float bandCenterHz(int b) noexcept
{
    return std::sqrt(bandLowHz(b) * bandHighHz(b));
}

// =============================================================================
// 定点基 2 FFT（256 点）。自己写是为了让整个模块脱离 JUCE 单独测试。
// =============================================================================
class Radix2Fft
{
public:
    void prepare(int size)
    {
        n = size;
        const int bits = (int) std::lround(std::log2((double) n));
        rev.assign((std::size_t) n, 0);
        for (int i = 0; i < n; ++i)
        {
            int r = 0;
            for (int b = 0; b < bits; ++b)
                if ((i >> b) & 1)
                    r |= 1 << (bits - 1 - b);
            rev[(std::size_t) i] = r;
        }
        twCos.assign((std::size_t) (n / 2), 1.0f);
        twSin.assign((std::size_t) (n / 2), 0.0f);
        for (int k = 0; k < n / 2; ++k)
        {
            const double phase = -2.0 * 3.14159265358979323846 * (double) k / (double) n;
            twCos[(std::size_t) k] = (float) std::cos(phase);
            twSin[(std::size_t) k] = (float) std::sin(phase);
        }
    }

    /** 原地复数 FFT（输入已按位反转重排，输出为自然序） */
    void forward(float* re, float* im) const noexcept
    {
        for (int i = 0; i < n; ++i)
        {
            const int j = rev[(std::size_t) i];
            if (i < j)
            {
                std::swap(re[i], re[j]);
                std::swap(im[i], im[j]);
            }
        }

        for (int len = 2; len <= n; len <<= 1)
        {
            const int half = len >> 1;
            const int step = n / len;
            for (int i = 0; i < n; i += len)
            {
                for (int j = 0, k = 0; j < half; ++j, k += step)
                {
                    const float wr = twCos[(std::size_t) k];
                    const float wi = twSin[(std::size_t) k];
                    const int a = i + j;
                    const int b = a + half;
                    const float vr = re[b] * wr - im[b] * wi;
                    const float vi = re[b] * wi + im[b] * wr;
                    const float ur = re[a];
                    const float ui = im[a];
                    re[a] = ur + vr;
                    im[a] = ui + vi;
                    re[b] = ur - vr;
                    im[b] = ui - vi;
                }
            }
        }
    }

private:
    int n = 0;
    std::vector<int> rev;
    std::vector<float> twCos, twSin;
};

// =============================================================================
// 16 带分析器
// =============================================================================
class BandAnalyzer
{
public:
    /** 一帧的分析结果：逐带的左右声道能量（已按窗与带权重归一） */
    struct Frame
    {
        std::array<float, kBandCount> energyL {};
        std::array<float, kBandCount> energyR {};
        float totalL = 0.0f;
        float totalR = 0.0f;
        std::uint64_t index = 0;

        float energy(int b) const noexcept { return energyL[(std::size_t) b] + energyR[(std::size_t) b]; }

        /** 该带的声像位置（-1 左 ~ +1 右），能量过小时返回 0 */
        float balance(int b) const noexcept
        {
            const float l = energyL[(std::size_t) b];
            const float r = energyR[(std::size_t) b];
            const float sum = l + r;
            return sum > 1.0e-12f ? (r - l) / sum : 0.0f;
        }
    };

    void prepare(double sampleRate)
    {
        rate = (sampleRate > 1.0) ? sampleRate : 48000.0;
        decim = std::max(1, (int) std::lround(rate / 8000.0));
        frameLen = std::max(8, (int) std::lround(rate * kFrameSeconds));
        downLen = std::max(4, frameLen / decim);
        effectiveRate = rate / (double) decim;

        // Tukey α=0.25，与 everplus 初始化函数 0x180045B60 完全一致
        window.assign((std::size_t) downLen, 1.0f);
        const float denom = (float) std::max(1, downLen - 1);
        for (int i = 0; i < downLen; ++i)
        {
            const float t = (float) std::min(i, downLen - 1 - i) / denom;
            window[(std::size_t) i] = (t < 0.125f)
                                          ? 0.5f * (1.0f + std::cos(3.14159265358979323846f * (8.0f * t - 1.0f)))
                                          : 1.0f;
        }

        fft.prepare(kFftSize);
        binHz = effectiveRate / (double) kFftSize;

        bufL.assign((std::size_t) frameLen, 0.0f);
        bufR.assign((std::size_t) frameLen, 0.0f);
        re.assign((std::size_t) kFftSize, 0.0f);
        im.assign((std::size_t) kFftSize, 0.0f);
        reset();
    }

    void reset() noexcept
    {
        std::fill(bufL.begin(), bufL.end(), 0.0f);
        std::fill(bufR.begin(), bufR.end(), 0.0f);
        writePos = 0;
        count = 0;
        latest = Frame {};
    }

    /**
     * @brief 喂入一个立体声样本
     * @return true 表示这一样本凑满了一帧，latest 已更新
     */
    bool push(float l, float r) noexcept
    {
        if (bufL.empty())
            return false;

        bufL[(std::size_t) writePos] = l;
        bufR[(std::size_t) writePos] = r;
        if (++writePos < frameLen)
            return false;

        writePos = 0;
        analyseFrame();
        return true;
    }

    const Frame& frame() const noexcept { return latest; }
    std::uint64_t frameCount() const noexcept { return count; }
    double effectiveRateHz() const noexcept { return effectiveRate; }
    double binWidthHz() const noexcept { return binHz; }
    int frameLengthSamples() const noexcept { return frameLen; }

private:
    void analyseFrame() noexcept
    {
        // 1) 盒式抽取：每 decim 个样本求平均（everplus 用 6:1）
        std::array<float, 512> downL {};
        std::array<float, 512> downR {};
        const int n = std::min(downLen, 512);
        for (int i = 0; i < n; ++i)
        {
            float sl = 0.0f;
            float sr = 0.0f;
            for (int k = 0; k < decim; ++k)
            {
                const int idx = i * decim + k;
                if (idx >= frameLen)
                    break;
                sl += bufL[(std::size_t) idx];
                sr += bufR[(std::size_t) idx];
            }
            const float g = 1.0f / (float) decim;
            downL[(std::size_t) i] = sl * g;
            downR[(std::size_t) i] = sr * g;
        }

        // 2) 加窗 + 补零
        std::fill(re.begin(), re.end(), 0.0f);
        std::fill(im.begin(), im.end(), 0.0f);
        for (int i = 0; i < n; ++i)
            re[(std::size_t) i] = downL[(std::size_t) i] * window[(std::size_t) i];
        fft.forward(re.data(), im.data());
        accumulateBands(re, im, latest.energyL);

        std::fill(re.begin(), re.end(), 0.0f);
        std::fill(im.begin(), im.end(), 0.0f);
        for (int i = 0; i < n; ++i)
            re[(std::size_t) i] = downR[(std::size_t) i] * window[(std::size_t) i];
        fft.forward(re.data(), im.data());
        accumulateBands(re, im, latest.energyR);

        float tl = 0.0f;
        float tr = 0.0f;
        for (int b = 0; b < kBandCount; ++b)
        {
            tl += latest.energyL[(std::size_t) b];
            tr += latest.energyR[(std::size_t) b];
        }
        latest.totalL = tl;
        latest.totalR = tr;
        latest.index = ++count;
    }

    /**
     * 带积分：把每个 FFT bin 看作宽度 binHz 的矩形，与频带 [lo,hi] 求重叠宽度加权。
     * 这正是 everplus 0x180045B60 里算滤波器组权重的做法（重叠宽 ÷ 带宽），
     * 只是这里直接在频域积分，省掉 16×129 权重矩阵。
     */
    void accumulateBands(const std::vector<float>& reBuf,
                         const std::vector<float>& imBuf,
                         std::array<float, kBandCount>& out) const noexcept
    {
        std::array<float, kBandCount> acc {};
        const double nyquist = effectiveRate * 0.5;

        for (int b = 0; b < kBandCount; ++b)
        {
            const double lo = std::min((double) bandLowHz(b), nyquist);
            const double hi = std::min((double) bandHighHz(b), nyquist);
            if (hi <= lo)
                continue;

            const int k0 = std::max(0, (int) std::floor(lo / binHz));
            const int k1 = std::min(kFftSize / 2, (int) std::ceil(hi / binHz));
            double sum = 0.0;
            for (int k = k0; k <= k1; ++k)
            {
                const double binLo = (double) k * binHz;
                const double binHi = binLo + binHz;
                const double overlap = std::min(binHi, hi) - std::max(binLo, lo);
                if (overlap <= 0.0)
                    continue;
                const float rr = reBuf[(std::size_t) k];
                const float ii = imBuf[(std::size_t) k];
                const double power = (double) rr * rr + (double) ii * ii;
                sum += power * (overlap / binHz);
            }
            acc[(std::size_t) b] = (float) sum;
        }
        out = acc;
    }

    double rate = 48000.0;
    double effectiveRate = 8000.0;
    double binHz = 31.25;
    int decim = 6;
    int frameLen = 960;
    int downLen = 160;
    int writePos = 0;
    std::uint64_t count = 0;

    std::vector<float> window;
    std::vector<float> bufL, bufR;
    std::vector<float> re, im;
    Radix2Fft fft;
    Frame latest;
};

} // namespace delta
