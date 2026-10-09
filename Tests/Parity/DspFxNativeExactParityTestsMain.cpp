// Copyright 2026 MontroneDSP.
// SPDX-License-Identifier: GPL-3.0-or-later
// Exact native-rate DSP-board FX parity against production BoardProcessor.

#include "Engine/DspBoard/BoardProcessor.h"
#include "Parity/ExactParityGoldens.h"

#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <vector>

namespace {

using swaraxt::board::Block;
using swaraxt::board::BoardControl;
using swaraxt::board::BoardProcessor;
using swaraxt::board::Effect;
using swaraxt::board::FloatBlock;
using swaraxt::board::blockSize;
using swaraxt::board::effectFromChoice;
using swaraxt::board::sampleRate;

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

enum class Stimulus : int { silence = 0, impulse, sine, noise };

void fillBlock(FloatBlock& x, Stimulus stimulus, int sampleIndex, std::uint32_t& rng)
{
    constexpr double kPi = 3.14159265358979323846;
    for (std::size_t i = 0; i < blockSize; ++i)
    {
        const int n = sampleIndex + static_cast<int>(i);
        float v = 0.0f;
        switch (stimulus)
        {
            case Stimulus::silence:
                v = 0.0f;
                break;
            case Stimulus::impulse:
                v = n == 0 ? 0.75f : 0.0f;
                break;
            case Stimulus::sine:
                v = static_cast<float>(0.5 * std::sin(2.0 * kPi * 440.0 * static_cast<double>(n) / sampleRate));
                break;
            case Stimulus::noise:
                rng = rng * 1664525u + 1013904223u;
                v = static_cast<float>(static_cast<int>(rng >> 24) - 128) / 128.0f;
                break;
        }
        x[i] = v;
    }
}

std::vector<float> renderClassicFx(Effect effect,
                                   Stimulus stimulus,
                                   std::uint8_t cv1,
                                   std::uint8_t cv2,
                                   std::uint8_t tempo,
                                   int blocks)
{
    BoardProcessor processor;
    BoardControl control;
    control.effect = effect;
    control.cv1 = cv1;
    control.cv2 = cv2;
    control.tempo = tempo;
    processor.reset(effect);
    std::vector<float> out;
    out.reserve(static_cast<std::size_t>(blocks) * blockSize);
    std::uint32_t rng = 0x21u;
    int sampleIndex = 0;
    for (int b = 0; b < blocks; ++b)
    {
        FloatBlock x{};
        fillBlock(x, stimulus, sampleIndex, rng);
        sampleIndex += static_cast<int>(blockSize);
        processor.processClassicFx(x, control);
        for (float s : x)
            out.push_back(s);
    }
    return out;
}

void addCase(const char* name, const std::vector<float>& audio)
{
    const auto h = hashBits(audio);
    std::printf("GOLDEN native %s samples=%zu hash=%016llx\n",
                name, audio.size(), static_cast<unsigned long long>(h));
    expect(allFinite(audio), name);
    const auto* expected = swaraxt_golden::find(swaraxt_golden::kNative, swaraxt_golden::kNativeCount, name);
    expect(expected != nullptr, name);
    if (expected != nullptr)
    {
        expect(h == expected->hash, name);
        expect(audio.size() == expected->samples, name);
    }
}

}  // namespace

int main()
{
    int cases = 0;

    {
        BoardProcessor processor;
        BoardControl off;
        off.effect = Effect::off;
        processor.reset(Effect::off);
        FloatBlock x{};
        x.fill(0.1234567f);
        const auto identical = x;
        processor.processClassicFx(x, off);
        expect(x == identical, "Classic Off bypass identity");
        std::vector<float> audio(x.begin(), x.end());
        addCase("off_identity", audio);
        ++cases;
    }

    const char* programName[] = {
        "off", "distortion", "crush", "combPositive", "combNegative", "ringMod",
        "delay", "delayFeedback", "delayDub", "crushDelayFeedback", "crushDelayDub",
        "delay16", "delay12", "delay8", "delay3_16", "looper", "pitch"
    };

    for (int choice = 0; choice <= 16; ++choice)
    {
        const auto effect = effectFromChoice(choice);
        char name[96];
        std::snprintf(name, sizeof(name), "%s_sine_cv128_0_t120", programName[choice]);
        addCase(name, renderClassicFx(effect, Stimulus::sine, 128, 0, 120, 48));
        std::snprintf(name, sizeof(name), "%s_silence", programName[choice]);
        addCase(name, renderClassicFx(effect, Stimulus::silence, 128, 0, 120, 16));
        std::snprintf(name, sizeof(name), "%s_impulse_tail", programName[choice]);
        addCase(name, renderClassicFx(effect, Stimulus::impulse, 128, 32, 120, 80));
        std::snprintf(name, sizeof(name), "%s_noise", programName[choice]);
        addCase(name, renderClassicFx(effect, Stimulus::noise, 128, 0, 120, 32));
        cases += 4;
    }

    const int extremePrograms[] = { 1, 2, 3, 5, 7, 9, 16 };
    for (int choice : extremePrograms)
    {
        const auto effect = effectFromChoice(choice);
        char name[96];
        std::snprintf(name, sizeof(name), "%s_sine_cv1min", programName[choice]);
        addCase(name, renderClassicFx(effect, Stimulus::sine, 0, 0, 120, 32));
        std::snprintf(name, sizeof(name), "%s_sine_cv1max", programName[choice]);
        addCase(name, renderClassicFx(effect, Stimulus::sine, 254, 0, 120, 32));
        std::snprintf(name, sizeof(name), "%s_sine_cv2max", programName[choice]);
        addCase(name, renderClassicFx(effect, Stimulus::sine, 128, 252, 120, 32));
        cases += 3;
    }

    addCase("delay16_tempo40",
            renderClassicFx(Effect::delay16, Stimulus::impulse, 200, 0, 40, 64));
    addCase("delay16_tempo240",
            renderClassicFx(Effect::delay16, Stimulus::impulse, 200, 0, 240, 64));
    cases += 2;

    {
        BoardProcessor processor;
        BoardControl control;
        control.effect = Effect::looper;
        control.cv1 = 128;
        control.cv2 = 0;
        processor.reset(Effect::looper);
        std::vector<float> out;
        std::uint32_t rng = 0x21u;
        int sampleIndex = 0;
        for (int b = 0; b < 24; ++b)
        {
            FloatBlock x{};
            fillBlock(x, Stimulus::sine, sampleIndex, rng);
            sampleIndex += static_cast<int>(blockSize);
            processor.processClassicFx(x, control);
            for (float s : x)
                out.push_back(s);
        }
        expect(processor.hasValidLoop(), "looper recorded");
        control.cv2 = 200;
        rng = 0x21u;
        for (int b = 0; b < 24; ++b)
        {
            FloatBlock x{};
            fillBlock(x, Stimulus::silence, sampleIndex, rng);
            sampleIndex += static_cast<int>(blockSize);
            processor.processClassicFx(x, control);
            for (float s : x)
                out.push_back(s);
        }
        addCase("looper_record_replay", out);
        ++cases;
    }

    {
        BoardProcessor processor;
        BoardControl control;
        control.effect = Effect::distortion;
        processor.reset(Effect::distortion);
        FloatBlock x{};
        std::uint32_t rng = 0x21u;
        fillBlock(x, Stimulus::sine, 0, rng);
        processor.processClassicFx(x, control);
        control.effect = Effect::delay;
        processor.reset(Effect::delay);
        x.fill(0.0f);
        processor.processClassicFx(x, control);
        BoardProcessor fresh;
        fresh.reset(Effect::delay);
        FloatBlock expected{};
        fresh.processClassicFx(expected, control);
        expect(x == expected, "program reset drops prior FX state");
        std::vector<float> audio(x.begin(), x.end());
        addCase("program_change_reset", audio);
        ++cases;
    }

    {
        const auto a = renderClassicFx(Effect::delayFeedback, Stimulus::sine, 180, 200, 120, 40);
        const auto b = renderClassicFx(Effect::delayFeedback, Stimulus::sine, 180, 200, 120, 40);
        expect(hashBits(a) == hashBits(b), "native delayFeedback repeatable");
    }

    std::printf("native golden cases=%d failures=%d\n", cases, gFailures);
    return gFailures == 0 ? 0 : 1;
}
