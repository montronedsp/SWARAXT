// Copyright 2026 MontroneDSP.
// SPDX-License-Identifier: GPL-3.0-or-later
// Host-timeline image of the Shruthi internal-clock groove.
// The scale of the gap after clock k is the swing sampled at that clock,
// before Part::Clock() advances the step. Step starts at 0xff and production
// masks the LUT index with 0x0f, so the first gap reads entry 15.
#pragma once

#include <cstdint>

namespace swaraxt::groove {

// Signed Shruthi groove LUTs (resources.cc, shruthi-1 56bfe78).
inline constexpr int kLut[6][16] = {
    { 127, 127, -127, -127, 127, 127, -127, -127, 127, 127, -127, -127, 127, 127, -127, -127 },
    { 127, -127, 127, -127, 127, -127, 127, -127, 127, -127, 127, -127, 127, -127, 127, -127 },
    { -63, -63, 127, 0, -127, 0, 0, 88, 0, 0, 88, -50, -88, 0, 88, 0 },
    { 19, 44, 93, -4, 32, -53, -90, -127, 117, 32, -102, -53, 105, -53, 93, -53 },
    { 88, -101, 107, -95, 88, -88, 50, -38, 65, -88, 101, -95, 101, -127, 63, -31 },
    { 70, -84, 84, -112, 84, -98, 112, -98, 54, -70, 127, -84, 127, -112, 84, -84 }
};

// floor(lut * amount / 512): S8U8MulShift8 then an arithmetic >> 1.
inline constexpr int swing(int lutEntry, int amount) noexcept
{
    const int product = lutEntry * amount;
    if (product >= 0)
        return product / 512;
    return -((-product + 511) / 512);
}

// Clock-period numerator. The firmware threshold byte is this value;
// a straight clock is 128.
inline constexpr int scaleNum(int groove, int index, int amount) noexcept
{
    return 128 + swing(kLut[groove][index], amount);
}

// LUT index sampled at clock `interval` (the gap that follows that clock).
// clocksPerStep >= 1, length in 1..16.
inline int intervalIndex(std::uint64_t interval, int length, int clocksPerStep) noexcept
{
    if (interval == 0)
        return 15;
    const auto step = (interval - 1) / static_cast<std::uint64_t>(clocksPerStep);
    return static_cast<int>(step % static_cast<std::uint64_t>(length));
}

// Sum of scale numerators over the gaps before `tick`. Straight time is
// 128 per clock; PPQ is the sum divided by 3072 (128 * 24).
inline std::int64_t unitsBeforeTick(std::uint64_t tick, int length, int clocksPerStep,
                                    int groove, int amount) noexcept
{
    if (tick == 0)
        return 0;
    std::int64_t sum = scaleNum(groove, 15, amount);
    if (tick == 1)
        return sum;

    const std::uint64_t count = tick - 1;
    const auto group = static_cast<std::uint64_t>(length) * static_cast<std::uint64_t>(clocksPerStep);
    const std::uint64_t cycles = count / group;
    const std::uint64_t remainder = count % group;

    std::int64_t cycle = 0;
    for (int index = 0; index < length; ++index)
        cycle += static_cast<std::int64_t>(clocksPerStep) * scaleNum(groove, index, amount);
    sum += static_cast<std::int64_t>(cycles) * cycle;

    const int fullSteps = static_cast<int>(remainder / static_cast<std::uint64_t>(clocksPerStep));
    const int partial = static_cast<int>(remainder % static_cast<std::uint64_t>(clocksPerStep));
    for (int index = 0; index < fullSteps; ++index)
        sum += static_cast<std::int64_t>(clocksPerStep) * scaleNum(groove, index, amount);
    if (partial != 0)
        sum += static_cast<std::int64_t>(partial) * scaleNum(groove, fullSteps, amount);
    return sum;
}

} // namespace swaraxt::groove
