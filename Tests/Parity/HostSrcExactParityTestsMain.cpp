// Copyright 2026 MontroneDSP.
// SPDX-License-Identifier: GPL-3.0-or-later
// Exact native-to-host SRC parity against production PolyphaseFirResampler.

#include "Engine/SampleRate/PolyphaseFirResampler.h"
#include "Parity/ExactParityGoldens.h"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <vector>

#ifndef SWARAXT_SRC_FIR_TAPS
#define SWARAXT_SRC_FIR_TAPS 256
#endif
#ifndef SWARAXT_SRC_FIR_PHASES
#define SWARAXT_SRC_FIR_PHASES 256
#endif
#ifndef SWARAXT_SRC_FIR_STOPBAND_DB
#define SWARAXT_SRC_FIR_STOPBAND_DB 100.0
#endif

namespace {

using Converter = swaraxt::PolyphaseFirResampler<SWARAXT_SRC_FIR_TAPS, SWARAXT_SRC_FIR_PHASES, true>;

constexpr double kNative = 20000000.0 / 510.0;
constexpr double kPi = 3.14159265358979323846;
int gFailures = 0;

void expect(bool ok, const char* name)
{
    if (! ok)
    {
        std::printf("FAIL: %s\n", name);
        ++gFailures;
        return;
    }
    std::printf("PASS: %s\n", name);
}

std::uint64_t hashBits(const std::vector<float>& samples)
{
    std::uint64_t h = 1469598103934665603ull;
    for (float sample : samples)
    {
        std::uint32_t bits = 0;
        std::memcpy(&bits, &sample, sizeof(bits));
        for (int i = 0; i < 4; ++i)
        {
            h ^= static_cast<std::uint8_t>((bits >> (8 * i)) & 0xffu);
            h *= 1099511628211ull;
        }
    }
    return h;
}

bool allFinite(const std::vector<float>& samples)
{
    for (float s : samples)
        if (! std::isfinite(s))
            return false;
    return true;
}

enum class Stimulus : int { silence, impulse, sine, noise, transientThenSilence };

std::vector<float> nativeStimulus(Stimulus stimulus, int nativeCount)
{
    std::vector<float> x(static_cast<std::size_t>(nativeCount), 0.0f);
    std::uint32_t rng = 0x21u;
    for (int n = 0; n < nativeCount; ++n)
    {
        switch (stimulus)
        {
            case Stimulus::silence:
                x[static_cast<std::size_t>(n)] = 0.0f;
                break;
            case Stimulus::impulse:
                x[static_cast<std::size_t>(n)] = n == 0 ? 1.0f : 0.0f;
                break;
            case Stimulus::sine:
                x[static_cast<std::size_t>(n)] = static_cast<float>(
                    std::sin(2.0 * kPi * 1000.0 * static_cast<double>(n) / kNative));
                break;
            case Stimulus::noise:
                rng = rng * 1664525u + 1013904223u;
                x[static_cast<std::size_t>(n)] =
                    static_cast<float>(static_cast<int>(rng >> 24) - 128) / 128.0f;
                break;
            case Stimulus::transientThenSilence:
                if (n < 40)
                    x[static_cast<std::size_t>(n)] = (n & 1) ? 0.8f : -0.8f;
                else
                    x[static_cast<std::size_t>(n)] = 0.0f;
                break;
        }
    }
    return x;
}

// Mirrors the production engine: fill to queueTarget, then read one host sample.
std::vector<float> convert(double hostRate,
                           const std::vector<float>& native,
                           int hostSamples,
                           const int* hostBlocks,
                           int hostBlockCount)
{
    Converter converter { SWARAXT_SRC_FIR_STOPBAND_DB };
    converter.setStep(kNative, hostRate);
    converter.reset();
    std::vector<float> host;
    host.reserve(static_cast<std::size_t>(hostSamples));
    std::size_t nativeIndex = 0;
    auto pushUntilTarget = [&]() {
        int guard = 0;
        while (converter.size() < converter.queueTargetSize() && guard++ < Converter::kTaps + 32)
        {
            const float v = nativeIndex < native.size() ? native[nativeIndex++] : 0.0f;
            converter.push(v);
        }
    };
    int produced = 0;
    int block = 0;
    while (produced < hostSamples)
    {
        int n = hostBlocks != nullptr && hostBlockCount > 0
            ? hostBlocks[block % hostBlockCount]
            : 64;
        if (n < 1)
            n = 1;
        n = std::min(n, hostSamples - produced);
        for (int i = 0; i < n; ++i)
        {
            pushUntilTarget();
            float y = 0.0f;
            if (converter.size() >= converter.minimumReadableSize())
                y = converter.readInterpolated();
            else
                ++gFailures, std::printf("FAIL: SRC underflow hostRate=%.1f\n", hostRate);
            host.push_back(y);
        }
        produced += n;
        ++block;
    }
    return host;
}

void checkCase(const char* name, double hostRate, Stimulus stimulus)
{
    const int hostSamples = static_cast<int>(hostRate * 0.04);
    const int nativeNeed = static_cast<int>(std::ceil(hostSamples * kNative / hostRate))
        + Converter::kTaps + 64;
    const auto native = nativeStimulus(stimulus, nativeNeed);
    const int regular[] = { 64 };
    const int irregular[] = { 7, 13, 64, 96, 1, 128 };
    const auto a = convert(hostRate, native, hostSamples, regular, 1);
    const auto b = convert(hostRate, native, hostSamples, irregular, 6);
    expect(allFinite(a) && allFinite(b), name);
    expect(a.size() == static_cast<std::size_t>(hostSamples), "host sample count");
    expect(hashBits(a) == hashBits(b), "host block partitioning invariance");
    const auto again = convert(hostRate, native, hostSamples, regular, 1);
    expect(hashBits(a) == hashBits(again), "SRC repeatable");
    const auto h = hashBits(a);
    std::printf("GOLDEN src %s samples=%zu hash=%016llx\n",
                name, a.size(), static_cast<unsigned long long>(h));
    const auto* expected = swaraxt_golden::find(swaraxt_golden::kSrc, swaraxt_golden::kSrcCount, name);
    expect(expected != nullptr, name);
    if (expected != nullptr)
    {
        expect(h == expected->hash, name);
        expect(a.size() == expected->samples, name);
    }
}

void latencyAndDrain(double hostRate)
{
    Converter converter { SWARAXT_SRC_FIR_STOPBAND_DB };
    converter.setStep(kNative, hostRate);
    converter.reset();
    int input = 0;
    int peakIndex = 0;
    float peak = 0.0f;
    const int length = static_cast<int>(std::ceil(1024.0 * hostRate / kNative));
    for (int i = 0; i < length; ++i)
    {
        while (converter.size() < converter.queueTargetSize())
            converter.push(input++ == 0 ? 1.0f : 0.0f);
        const float y = converter.readInterpolated();
        expect(std::isfinite(y), "impulse finite");
        if (std::abs(y) > peak)
        {
            peak = std::abs(y);
            peakIndex = i;
        }
    }
    const double expected = Converter::groupDelayNativeSamples() * hostRate / kNative;
    expect(std::abs(static_cast<double>(peakIndex) - expected) <= 1.0,
           "impulse peak matches declared causal SRC latency");

    converter.reset();
    converter.setStep(kNative, hostRate);
    for (int i = 0; i < Converter::kTaps; ++i)
        converter.push(0.0f);
    expect(converter.readInterpolated() == 0.0f, "reset silence is exact zero");
}

}  // namespace

int main()
{
    static_assert(SWARAXT_SRC_FIR_TAPS == 256, "production SRC tap count");
    const double rates[] = { 44100.0, 48000.0, 88200.0, 96000.0, 176400.0, 192000.0 };
    const char* rateName[] = { "44100", "48000", "88200", "96000", "176400", "192000" };
    const struct { Stimulus s; const char* n; } stimuli[] = {
        { Stimulus::silence, "silence" },
        { Stimulus::impulse, "impulse" },
        { Stimulus::sine, "sine1k" },
        { Stimulus::noise, "noise" },
        { Stimulus::transientThenSilence, "transient" },
    };
    for (int r = 0; r < 6; ++r)
    {
        latencyAndDrain(rates[r]);
        for (const auto& st : stimuli)
        {
            char name[80];
            std::snprintf(name, sizeof(name), "%s_%s", rateName[r], st.n);
            checkCase(name, rates[r], st.s);
        }
    }
    std::printf("SRC exact parity failures=%d\n", gFailures);
    return gFailures == 0 ? 0 : 1;
}
