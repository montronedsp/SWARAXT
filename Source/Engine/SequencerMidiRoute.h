// Copyright 2026 MontroneDSP.
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include <cstdint>

namespace swaraxt {

// Sequencer MIDI note policy.
// LEGACY and QUANTIZED CV emit. LINEAR, SCALE and any unknown stored mode stay silent.
enum class SequencerPitchRoute : uint8_t {
    legacy = 0,
    linear = 1,
    scale = 2,
    quantizedCv = 3,
    invalid = 255
};

constexpr bool sequencerRouteEmitsMidiNotes(SequencerPitchRoute route) noexcept
{
    return route == SequencerPitchRoute::legacy
        || route == SequencerPitchRoute::quantizedCv;
}

constexpr SequencerPitchRoute routeFromStoredPitchMode(uint8_t pitchMode) noexcept
{
    switch (pitchMode)
    {
        case 0: return SequencerPitchRoute::legacy;
        case 1: return SequencerPitchRoute::linear;
        case 2: return SequencerPitchRoute::scale;
        case 3: return SequencerPitchRoute::quantizedCv;
        default: return SequencerPitchRoute::invalid;
    }
}

}  // namespace swaraxt
