// Copyright 2026 MontroneDSP.
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include "Engine/FreeCvPitch.h"

#include <cmath>
#include <cstdint>

namespace swaraxt {

// Numerical conversion into the voice's 128-unit equal-tempered coordinate.
// The scale period itself is not forced to 1200 cents; only the voice word is.
struct ScalePitch {
    static uint16_t toVoicePitchFromHz(double hz) noexcept
    {
        if (! std::isfinite(hz) || hz <= 0.0)
            return static_cast<uint16_t>(FreeCvPitch::kMinUnits / FreeCvPitch::kVoiceQuantum);
        const double semitones = 69.0 + 12.0 * std::log2(hz / 440.0);
        if (! std::isfinite(semitones))
            return static_cast<uint16_t>(FreeCvPitch::kMinUnits / FreeCvPitch::kVoiceQuantum);
        const double units = semitones * static_cast<double>(FreeCvPitch::kUnitsPerSemitone);
        if (! std::isfinite(units))
            return static_cast<uint16_t>(FreeCvPitch::kMinUnits / FreeCvPitch::kVoiceQuantum);
        const auto rounded = static_cast<int32_t>(std::llround(units));
        return FreeCvPitch::toVoicePitch(rounded);
    }

    // Nearest conventional MIDI note for an external Note On.
    // Halfway cases round away from zero (the higher note for positive pitch).
    // This is an integer approximation of a microtonal frequency, not the
    // internal voice word and not a promise of exact Scala playback.
    static uint8_t nearestMidiNote(double hz) noexcept
    {
        if (! std::isfinite(hz) || hz <= 0.0)
            return 0;
        const double note = 69.0 + 12.0 * std::log2(hz / 440.0);
        if (! std::isfinite(note))
            return 0;
        const auto rounded = static_cast<int>(std::floor(note + 0.5));
        if (rounded < 0)
            return 0;
        if (rounded > 127)
            return 127;
        return static_cast<uint8_t>(rounded);
    }
};

}  // namespace swaraxt
