// Copyright 2026 MontroneDSP. SPDX-License-Identifier: GPL-3.0-or-later
// Compares the shipping looper with independent historical, physical-ring and
// logical-loop models. Production DSP is not modified by this test.
#include "Engine/DspBoard/BoardEffects.h"
#include "Engine/DspBoard/BoardProcessor.h"
#include "Engine/DspBoard/BoardResources.h"

#include <cmath>
#include <cstdio>
#include <vector>

namespace {
using namespace swaraxt::board;
using namespace swaraxt::board::arithmetic;
using namespace swaraxt::board::resources;

enum class Guard { historical, physical, logical };

struct Model
{
    std::int8_t line[1281] {};
    std::uint16_t loopPtr = 0;
    std::uint16_t loopStart = 0;
    std::uint16_t loopDuration = 0;
    std::uint8_t loopPitch = 0;
    std::uint32_t loopPhase = 0;
    std::uint8_t delayCounter = 0;
    std::int16_t delayInput = 0;
    Guard guard = Guard::historical;

    void reset(bool offsetBinary)
    {
        *this = Model{};
        guard = Guard::historical;
        if (offsetBinary)
            for (auto& sample : line)
                sample = -128;
    }

    void record(Block& block, std::uint8_t time)
    {
        loopPitch = time;
        const auto decimate = lut_res_delay_decimation[time];
        const auto duration = lut_res_delay_duration[time];
        const auto gain = static_cast<std::uint8_t>(lut_res_delay_filter_gain[time]);
        auto write = static_cast<unsigned>(loopPtr);
        for (const auto sample : block)
        {
            if (delayCounter >= decimate)
            {
                delayCounter = 0;
                const auto stored = s8(S16ClipS8(delayInput) + 128);
                line[write] = stored;
                if (guard == Guard::physical && write == 0)
                    line[1024] = stored;
                if (guard == Guard::historical && write == 1023)
                    line[1024] = stored;
                if (++write >= 1024)
                    write -= 1024;
                ++loopPtr;
            }
            delayInput = add(delayInput, S16U8MulShift8(sub(s16(shift(sample, 4)), delayInput), gain));
            ++delayCounter;
        }
        loopPtr &= 1023;
        loopStart = static_cast<std::uint16_t>((loopPtr + 1024u - duration) & 1023u);
        loopPhase = 0;
        loopDuration = duration;
    }

    void replay(Block& block, std::uint8_t pitchCode)
    {
        const auto pitchShift = static_cast<std::uint8_t>(128 - (pitchCode >> 1) + (loopPitch >> 1));
        auto increment = lut_res_pitch_ratio[pitchShift];
        const auto scaling = lut_res_delay_phase_scaling[loopPitch];
        if (scaling != 0)
            increment = static_cast<std::uint16_t>(S16U16MulShift16(s16(increment), scaling));
        if (increment == 0)
            increment = 1;
        for (auto& sample : block)
        {
            const auto integral = loopPhase >> 8;
            const auto read = (loopStart + integral) & 1023u;
            unsigned next = read + 1u;
            if (guard == Guard::logical)
                next = integral + 1u >= loopDuration ? loopStart : (read + 1u) & 1023u;
            sample = s16((U8MixU16(static_cast<std::uint8_t>(line[read]),
                static_cast<std::uint8_t>(line[next]), static_cast<std::uint8_t>(loopPhase)) >> 4) - 2048);
            loopPhase = phaseAdd(loopPhase, increment);
            if ((loopPhase >> 8) >= loopDuration)
                loopPhase = (loopPhase - (static_cast<std::uint32_t>(loopDuration) << 8)) & 0xffffffu;
        }
    }
};

struct Stats
{
    int samples = 0;
    int first = -1;
    int maxAbs = 0;
    double sum = 0;
    int peak = 0;
    int seam = 0;
};

Stats compare(const std::vector<std::int16_t>& a, const std::vector<std::int16_t>& b)
{
    Stats stats;
    stats.samples = static_cast<int>(std::min(a.size(), b.size()));
    for (int i = 0; i < stats.samples; ++i)
    {
        const int delta = std::abs(static_cast<int>(a[static_cast<std::size_t>(i)]) - b[static_cast<std::size_t>(i)]);
        if (delta != 0 && stats.first < 0)
            stats.first = i;
        stats.maxAbs = std::max(stats.maxAbs, delta);
        stats.sum += static_cast<double>(delta) * delta;
        stats.peak = std::max(stats.peak, std::abs(static_cast<int>(a[static_cast<std::size_t>(i)])));
        if (i > 0)
            stats.seam = std::max(stats.seam, std::abs(a[static_cast<std::size_t>(i)] - a[static_cast<std::size_t>(i - 1)]));
    }
    return stats;
}

void printStats(const char* label, const Stats& stats)
{
    const double rms = stats.samples == 0 ? 0.0 : std::sqrt(stats.sum / stats.samples);
    std::printf("%s samples=%d first=%d maxAbs=%d rms=%.6f peak=%d seam=%d\n",
        label, stats.samples, stats.first, stats.maxAbs, rms, stats.peak, stats.seam);
}

Block ramp(int origin)
{
    Block block {};
    for (std::size_t i = 0; i < block.size(); ++i)
        block[i] = static_cast<std::int16_t>(std::clamp(origin + static_cast<int>(i) * 17 - 400, -2048, 2047));
    return block;
}

std::vector<std::int16_t> runEffects(std::uint8_t time, int recordBlocks, const std::vector<std::uint8_t>& pitches,
                                     bool offsetBinary)
{
    BoardEffects effects;
    effects.reset(offsetBinary);
    std::vector<std::int16_t> out;
    for (int i = 0; i < recordBlocks; ++i)
    {
        auto block = ramp(i * 40);
        effects.process(block, Effect::looper, time, 0, 120);
    }
    for (auto pitch : pitches)
    {
        for (int i = 0; i < 80; ++i)
        {
            Block block {};
            effects.process(block, Effect::looper, pitch, 200, 120);
            out.insert(out.end(), block.begin(), block.end());
        }
    }
    return out;
}

std::vector<std::int16_t> runModel(Guard guard, std::uint8_t time, int recordBlocks,
                                   const std::vector<std::uint8_t>& pitches, bool offsetBinary)
{
    Model model;
    model.reset(offsetBinary);
    model.guard = guard;
    std::vector<std::int16_t> out;
    for (int i = 0; i < recordBlocks; ++i)
    {
        auto block = ramp(i * 40);
        model.record(block, time);
    }
    for (auto pitch : pitches)
    {
        for (int i = 0; i < 80; ++i)
        {
            Block block {};
            model.replay(block, pitch);
            out.insert(out.end(), block.begin(), block.end());
        }
    }
    return out;
}

std::vector<std::int16_t> runProcessor(std::uint8_t time, int recordBlocks, std::uint8_t replayCv)
{
    BoardProcessor processor;
    processor.reset(Effect::looper);
    BoardControl control;
    control.effect = Effect::looper;
    control.cv1 = time;
    control.cv2 = 0;
    for (int i = 0; i < recordBlocks; ++i)
    {
        FloatBlock block {};
        const auto source = ramp(i * 40);
        for (std::size_t n = 0; n < block.size(); ++n)
            block[n] = static_cast<float>(source[n]) / 2048.0f;
        processor.processClassicFx(block, control);
    }
    control.cv1 = replayCv;
    control.cv2 = 200;
    std::vector<std::int16_t> out;
    for (int i = 0; i < 80; ++i)
    {
        FloatBlock block {};
        processor.processClassicFx(block, control);
        for (float sample : block)
            out.push_back(static_cast<std::int16_t>(std::lround(sample * 2048.0f)));
    }
    return out;
}
}

int main()
{
    int failures = 0;
    int below = -1, equal = -1, above = -1;
    for (int time = 0; time < 256; ++time)
    {
        const int duration = lut_res_delay_duration[static_cast<std::size_t>(time)];
        if (below < 0 && duration < 1024) below = time;
        if (equal < 0 && duration == 1024) equal = time;
        if (above < 0 && duration > 1024) above = time;
    }
    int minimum = 65535, maximum = 0, exact = 0;
    for (int time = 0; time < 256; ++time)
    {
        const int duration = lut_res_delay_duration[static_cast<std::size_t>(time)];
        minimum = std::min(minimum, duration);
        maximum = std::max(maximum, duration);
        exact += duration == 1024 ? 1 : 0;
    }
    std::printf("duration-table min=%d max=%d exact1024=%d\n", minimum, maximum, exact);
    std::printf("duration-below=%d value=%d decimate=%d\n", below,
        below < 0 ? -1 : lut_res_delay_duration[static_cast<std::size_t>(below)],
        below < 0 ? -1 : lut_res_delay_decimation[static_cast<std::size_t>(below)]);
    std::printf("duration-equal=%d value=%d decimate=%d\n", equal,
        equal < 0 ? -1 : lut_res_delay_duration[static_cast<std::size_t>(equal)],
        equal < 0 ? -1 : lut_res_delay_decimation[static_cast<std::size_t>(equal)]);
    std::printf("duration-above=%d value=%d decimate=%d\n", above,
        above < 0 ? -1 : lut_res_delay_duration[static_cast<std::size_t>(above)],
        above < 0 ? -1 : lut_res_delay_decimation[static_cast<std::size_t>(above)]);

    const std::uint8_t pitches[] = {0, 64, 127};
    int physicalDiffs = 0, logicalDiffs = 0, cases = 0;
    for (int time : {below, equal, above})
    {
        if (time < 0)
            continue;
        for (bool offset : {false, true})
        {
            for (int blocks : {1, 30, 120})
            {
                const auto production = runEffects(static_cast<std::uint8_t>(time), blocks,
                    {pitches[0], pitches[1], pitches[2]}, offset);
                const auto historical = runModel(Guard::historical, static_cast<std::uint8_t>(time), blocks,
                    {pitches[0], pitches[1], pitches[2]}, offset);
                const auto physical = runModel(Guard::physical, static_cast<std::uint8_t>(time), blocks,
                    {pitches[0], pitches[1], pitches[2]}, offset);
                const auto logical = runModel(Guard::logical, static_cast<std::uint8_t>(time), blocks,
                    {pitches[0], pitches[1], pitches[2]}, offset);
                const auto versusHistorical = compare(production, historical);
                const auto versusPhysical = compare(production, physical);
                const auto versusLogical = compare(production, logical);
                ++cases;
                if (versusPhysical.first >= 0) ++physicalDiffs;
                if (versusLogical.first >= 0) ++logicalDiffs;
                std::printf("case time=%d blocks=%d offset=%d duration=%d\n", time, blocks, offset ? 1 : 0,
                    lut_res_delay_duration[static_cast<std::size_t>(time)]);
                printStats("  historical", versusHistorical);
                printStats("  physical", versusPhysical);
                printStats("  logical", versusLogical);
                if (versusHistorical.first >= 0)
                {
                    ++failures;
                    std::printf("FAIL production diverged from the historical guard model\n");
                }
                const int half = static_cast<int>(production.size() / 2);
                if (half > 8)
                {
                    int firstPeak = 0, secondPeak = 0;
                    for (int i = 0; i < half; ++i)
                        firstPeak = std::max(firstPeak, std::abs(static_cast<int>(production[static_cast<std::size_t>(i)])));
                    for (int i = half; i < static_cast<int>(production.size()); ++i)
                        secondPeak = std::max(secondPeak, std::abs(static_cast<int>(production[static_cast<std::size_t>(i)])));
                    if (firstPeak > 32 && secondPeak > firstPeak * 4)
                    {
                        ++failures;
                        std::printf("FAIL replay peak grew from %d to %d\n", firstPeak, secondPeak);
                    }
                }
            }
        }
    }

    const auto processor = runProcessor(static_cast<std::uint8_t>(below < 0 ? 0 : below), 40, 200);
    const auto effects = runEffects(static_cast<std::uint8_t>(below < 0 ? 0 : below), 40, {200}, true);
    const auto processorStats = compare(processor, effects);
    printStats("processor-vs-effects", processorStats);
    if (processorStats.first >= 0)
    {
        ++failures;
        std::printf("FAIL BoardProcessor looper diverged from BoardEffects\n");
    }

    BoardProcessor muted;
    muted.reset(Effect::looper);
    BoardControl control;
    control.effect = Effect::looper;
    control.cv2 = 200;
    FloatBlock silence {};
    silence.fill(0.25f);
    muted.processClassicFx(silence, control);
    int mutePeak = 0;
    for (float sample : silence)
        mutePeak = std::max(mutePeak, std::abs(static_cast<int>(std::lround(sample * 2048.0f))));
    std::printf("unrecorded-processor-replay-peak=%d\n", mutePeak);
    if (mutePeak != 0)
    {
        ++failures;
        std::printf("FAIL unrecorded BoardProcessor replay was not muted\n");
    }

    BoardEffects raw;
    raw.reset(true);
    Block rawBlock {};
    rawBlock.fill(0);
    raw.process(rawBlock, Effect::looper, 0, 200, 120);
    int rawPeak = 0;
    for (auto sample : rawBlock)
        rawPeak = std::max(rawPeak, std::abs(static_cast<int>(sample)));
    std::printf("offset-binary-effects-replay-peak=%d\n", rawPeak);

    std::printf("cases=%d physicalDiffs=%d logicalDiffs=%d failures=%d\n",
        cases, physicalDiffs, logicalDiffs, failures);
    std::printf("decision=%s\n", failures == 0 ? "REVISE" : "INVESTIGATE");
    return failures == 0 ? 0 : 1;
}
