// Copyright 2026 MontroneDSP.
// SPDX-License-Identifier: GPL-3.0-or-later
// GrooveClock.h against an independent Part clock-state simulation.
// The simulation does not call unitsBeforeTick to build its expected sum.

#include "Engine/GrooveClock.h"

#include <cstdint>
#include <cstdio>

namespace {

int failures = 0;

void expect(bool condition, const char* message)
{
    if (! condition)
    {
        std::printf("FAIL: %s\n", message);
        ++failures;
    }
}

int scaleAt(int groove, int index, int amount)
{
    const int product = swaraxt::groove::kLut[groove][index] * amount;
    const int swung = product >= 0 ? product / 512 : -((-product + 511) / 512);
    return 128 + swung;
}

std::int64_t simulateUnits(std::uint64_t tick, int clocksPerStep, int length, int groove, int amount)
{
    int step = 0xff;
    int prescaler = 0;
    std::int64_t units = 0;
    for (std::uint64_t clock = 0; clock < tick; ++clock)
    {
        units += scaleAt(groove, step & 0x0f, amount);
        if (prescaler == 0)
        {
            step = (step + 1) & 0xff;
            if (step >= length)
                step = 0;
        }
        ++prescaler;
        if (prescaler >= clocksPerStep)
            prescaler = 0;
    }
    return units;
}

std::int64_t retiredPerStepUnits(std::uint64_t tick, int clocksPerStep, int length, int groove, int amount)
{
    if (tick == 0)
        return 0;
    const auto step = tick / static_cast<std::uint64_t>(clocksPerStep);
    const auto position = tick % static_cast<std::uint64_t>(clocksPerStep);
    const int index = step == 0 ? 15 : static_cast<int>((step - 1) % static_cast<std::uint64_t>(length));
    std::int64_t prefix = 0;
    if (step > 0)
    {
        prefix += scaleAt(groove, 15, amount) * static_cast<std::int64_t>(clocksPerStep);
        for (std::uint64_t s = 1; s < step; ++s)
        {
            const int previous = static_cast<int>((s - 1) % static_cast<std::uint64_t>(length));
            prefix += static_cast<std::int64_t>(clocksPerStep) * scaleAt(groove, previous, amount);
        }
    }
    return prefix + static_cast<std::int64_t>(position) * scaleAt(groove, index, amount);
}

} // namespace

int main()
{
    const int amounts[] = { 0, 1, 16, 63, 64, 96, 127 };
    const int divisions[] = { 1, 2, 3, 4, 6, 8, 12, 16, 24, 32, 48, 96 };
    for (int groove = 0; groove < 6; ++groove)
    {
        for (int length = 1; length <= 16; ++length)
        {
            for (int amount : amounts)
            {
                for (int division : divisions)
                {
                    for (std::uint64_t tick = 0; tick <= 80; ++tick)
                    {
                        const auto expected = simulateUnits(tick, division, length, groove, amount);
                        const auto actual = swaraxt::groove::unitsBeforeTick(
                            tick, length, division, groove, amount);
                        if (expected != actual)
                        {
                            std::printf("FAIL g=%d L=%d amt=%d D=%d tick=%llu exp=%lld got=%lld\n",
                                        groove, length, amount, division,
                                        static_cast<unsigned long long>(tick),
                                        static_cast<long long>(expected),
                                        static_cast<long long>(actual));
                            return 1;
                        }
                    }
                }
            }
        }
    }

    const auto firmware = simulateUnits(6, 6, 16, 0, 127);
    const auto retired = retiredPerStepUnits(6, 6, 16, 0, 127);
    expect(firmware == 891, "swing amount 127 tick 6 is 891 scale units");
    expect(retired == 576, "retired per-step model is 576 at the same tick");
    expect(firmware != retired, "per-step scaling is not the firmware clock");

    const auto late = simulateUnits(1000003, 6, 16, 5, 127);
    expect(late == swaraxt::groove::unitsBeforeTick(1000003, 16, 6, 5, 127),
           "closed form matches the state machine past one million clocks");

    int minScale = 1000;
    int maxScale = 0;
    for (int groove = 0; groove < 6; ++groove)
        for (int index = 0; index < 16; ++index)
        {
            const int scale = scaleAt(groove, index, 127);
            if (scale < minScale)
                minScale = scale;
            if (scale > maxScale)
                maxScale = scale;
        }
    expect(minScale == 96 && maxScale == 159, "full-amount scales stay inside [96, 159]");

    if (failures != 0)
    {
        std::printf("%d groove clock checks failed\n", failures);
        return 1;
    }
    std::printf("groove clock checks passed\n");
    return 0;
}
