// Copyright 2026 MontroneDSP.
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include <cstdint>

namespace swaraxt {

// Single routing policy for sequencer MIDI note output.
// SCALE and QUANTIZED CV are named now so later phases do not grow ad-hoc checks.
// Phase A can select only legacy and linear. Quantized CV is allowed by policy
// but has no producer until Phase C.
enum class SequencerPitchRoute : uint8_t {
    legacy = 0,
    linear = 1,
    scale = 2,
    quantizedCv = 3
};

constexpr bool sequencerRouteEmitsMidiNotes(SequencerPitchRoute route) noexcept
{
    return route == SequencerPitchRoute::legacy
        || route == SequencerPitchRoute::quantizedCv;
}

constexpr SequencerPitchRoute routeFromStoredPitchMode(uint8_t pitchMode) noexcept
{
    return pitchMode == 1 ? SequencerPitchRoute::linear : SequencerPitchRoute::legacy;
}

}  // namespace swaraxt
