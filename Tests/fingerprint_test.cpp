// 独立验证：16 带分析器 + 多模板指纹门。
// 不依赖 JUCE，Linux / Windows 都能直接编译：
//   g++ -std=c++20 -O2 -ISource Tests/fingerprint_test.cpp -o fp_test
//   ./fp_test Assets/fingerprints.ewg sample.wav
//
// 输出：每秒的最佳匹配标签与余弦，以及四个场景的判定汇总。

#include "Dsp/BandAnalyzer.h"
#include "Dsp/FingerprintGate.h"
#include "Dsp/VoiceGate.h"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

namespace
{

struct Audio
{
    std::vector<float> mono;
    int sampleRate = 48000;
};

/** 极简 WAV 读取：16-bit / 24-bit / float32，混合为单声道 */
bool readWav(const char* path, Audio& out)
{
    std::FILE* f = std::fopen(path, "rb");
    if (f == nullptr)
        return false;

    char riff[12] {};
    if (std::fread(riff, 1, 12, f) != 12 || std::memcmp(riff, "RIFF", 4) != 0)
    {
        std::fclose(f);
        return false;
    }

    int channels = 0;
    int bits = 0;
    int rate = 0;
    int format = 0;
    std::vector<std::uint8_t> data;

    while (true)
    {
        char id[4] {};
        std::uint32_t size = 0;
        if (std::fread(id, 1, 4, f) != 4 || std::fread(&size, 4, 1, f) != 1)
            break;

        if (std::memcmp(id, "fmt ", 4) == 0)
        {
            std::uint16_t fmt = 0, ch = 0, ba = 0, b = 0;
            std::uint32_t sr = 0, br = 0;
            std::fread(&fmt, 2, 1, f);
            std::fread(&ch, 2, 1, f);
            std::fread(&sr, 4, 1, f);
            std::fread(&br, 4, 1, f);
            std::fread(&ba, 2, 1, f);
            std::fread(&b, 2, 1, f);
            format = fmt;
            channels = ch;
            rate = (int) sr;
            bits = b;
            if (size > 16)
                std::fseek(f, (long) (size - 16), SEEK_CUR);
        }
        else if (std::memcmp(id, "data", 4) == 0)
        {
            data.resize(size);
            if (std::fread(data.data(), 1, size, f) != size)
                data.clear();
            break;
        }
        else
        {
            std::fseek(f, (long) size + (long) (size & 1), SEEK_CUR);
        }
    }
    std::fclose(f);

    if (data.empty() || channels <= 0 || rate <= 0)
        return false;

    const std::size_t frames = data.size() / (std::size_t) (channels * (bits / 8));
    out.mono.assign(frames, 0.0f);
    out.sampleRate = rate;

    for (std::size_t i = 0; i < frames; ++i)
    {
        float acc = 0.0f;
        for (int c = 0; c < channels; ++c)
        {
            const std::size_t idx = i * (std::size_t) channels + (std::size_t) c;
            float v = 0.0f;
            if (bits == 16)
                v = (float) ((const std::int16_t*) data.data())[idx] / 32768.0f;
            else if (bits == 24)
            {
                const std::uint8_t* p = data.data() + idx * 3;
                std::int32_t s = (std::int32_t) (p[0] | (p[1] << 8) | (p[2] << 16));
                if (s & 0x800000)
                    s -= 0x1000000;
                v = (float) s / 8388608.0f;
            }
            else if (bits == 32 && format == 3)
                v = ((const float*) data.data())[idx];
            else if (bits == 32)
                v = (float) (((const std::int32_t*) data.data())[idx] / 2147483648.0);
            acc += v;
        }
        out.mono[i] = acc / (float) channels;
    }
    return true;
}

} // namespace

int main(int argc, char** argv)
{
    if (argc < 3)
    {
        std::printf("用法: %s <fingerprints.ewg> <audio.wav> [更多.wav ...]\n", argv[0]);
        return 1;
    }

    delta::VoiceGate gate;
    if (! gate.loadFile(argv[1]))
    {
        std::printf("加载模板失败: %s\n", argv[1]);
        return 2;
    }

    std::printf("模板 %d 套\n\n", gate.templateCount());

    for (int a = 2; a < argc; ++a)
    {
        Audio in;
        if (! readWav(argv[a], in))
        {
            std::printf("%s: 读取失败\n", argv[a]);
            continue;
        }

        delta::BandAnalyzer analyzer;
        analyzer.prepare((double) in.sampleRate);
        gate.reset();

        float bestByTemplate[8] {};
        float peakCos = -1.0f;
        float peakStd = 0.0f;
        int activeFrames = 0;

        for (float sample : in.mono)
        {
            if (! analyzer.push(sample, sample))
                continue;
            const auto& f = analyzer.frame();

            std::array<float, delta::kBandCount> power {};
            for (int b = 0; b < delta::kBandCount; ++b)
                power[(std::size_t) b] = f.energy(b);

            gate.pushFrame(power);
            if (! gate.ready())
                continue;

            peakCos = std::max(peakCos, gate.lastVoiceCosine());
            peakStd = std::max(peakStd, gate.lastEnvelopeStdDb());
            if (gate.active())
                ++activeFrames;
        }

        const bool hit = activeFrames > 0;
        std::printf("%s\n", argv[a]);
        std::printf("  人声余弦峰值 %.3f   包络起伏峰值 %.2f dB   命中帧 %d\n", peakCos, peakStd, activeFrames);
        std::printf("  判定: %s\n\n", hit ? "判为人声（应抑制）" : "非人声");
    }

    return 0;
}
