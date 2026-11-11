// Copyright 2026 MontroneDSP.
// SPDX-License-Identifier: GPL-3.0-or-later

#include "Engine/DspBoard/BoardEffects.h"

#include <cstddef>
#include <iostream>
#include <stdexcept>

namespace swaraxt::board {
namespace {
void require(bool ok, const char* label)
{
    if (! ok)
        throw std::runtime_error(label);
}
}

void delayWrapTests()
{
    const Effect effectsToRun[] = {
        Effect::delay, Effect::delayFeedback, Effect::delayDub,
        Effect::crushDelayFeedback, Effect::crushDelayDub,
        Effect::delay16, Effect::delay12, Effect::delay8, Effect::delay3_16
    };
    const std::uint8_t times[] = { 0, 0, 0, 0, 0, 80, 64, 40, 96 };
    const std::uint8_t amounts[] = { 200, 200, 180, 200, 180, 0, 0, 0, 0 };
    const std::uint8_t tempos[] = { 120, 120, 120, 120, 120, 120, 90, 140, 120 };
    for (std::size_t index = 0; index < sizeof(effectsToRun) / sizeof(effectsToRun[0]); ++index)
    {
        BoardEffects effects;
        effects.reset();
        effects.line_[1280] = 42;
        Block block {};
        block.fill(1600);
        std::uint16_t previous = 0;
        bool wrapped = false;
        for (int i = 0; i < 2500 && ! wrapped; ++i)
        {
            effects.process(block, effectsToRun[index], times[index], amounts[index], tempos[index]);
            if (effects.delayPtr_ < previous)
                wrapped = true;
            previous = effects.delayPtr_;
        }
        require(wrapped, "delay processing reaches the ring boundary");
        require(effects.delayPtr_ < 1280, "delay pointer stays inside the active ring");
        require(effects.line_[1280] == 42, "the guard cell is not a delay write");
        effects.process(block, effectsToRun[index], times[index], amounts[index], tempos[index]);
        require(effects.line_[1280] == 42, "the following block still leaves the guard cell unused");
        require(effects.delayPtr_ < 1280, "the following block keeps the pointer inside the ring");
    }
    std::cout << "Delay ring invariant: PASS\n";
}
}

int main()
{
    try
    {
        swaraxt::board::delayWrapTests();
    }
    catch (const std::exception& error)
    {
        std::cerr << "FAIL: " << error.what() << '\n';
        return 1;
    }
    return 0;
}
