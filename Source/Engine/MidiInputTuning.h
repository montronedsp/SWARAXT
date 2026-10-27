// Copyright 2026 MontroneDSP.
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include "Tuning/PublishedTuning.h"
#include "Tuning/ScalePitch.h"

#include <cmath>
#include <cstdint>

namespace swaraxt {

// Keyboard and arpeggiator input policy. Independent of sequencer pitchMode.
enum class MidiInputTuningMode : uint8_t {
    off = 0,
    tuned = 1,
    quantized = 2
};

struct MidiInputPitch {
    uint16_t word = 0;
    bool valid = false;
};

// TUNED: MIDI key -> SWT1 keyboard degree -> frequency.
// Unmapped KBM keys (midiMapped == false) are silent. They stay in the physical
// key list for sequencer transposition (chromatic note-60, not a scale-degree
// pass) and for sustain bookkeeping, but they never enter the mono allocator.
// QUANTIZED: MIDI key -> 12-TET Hz (69 = 440) -> nearest scale candidate.
// OFF is not resolved here; callers keep Part::Tune().
inline MidiInputPitch resolveMidiInputPitch(MidiInputTuningMode mode,
                                            const tuning::PublishedTuning* tuning,
                                            uint8_t note) noexcept
{
    MidiInputPitch result;
    if (mode == MidiInputTuningMode::off || tuning == nullptr || ! tuning->valid || note >= 128)
        return result;

    double hz = 0.0;
    if (mode == MidiInputTuningMode::tuned)
    {
        const int degree = tuning->midiDegree[note];
        if (! tuning->midiMapped[note])
            return result;
        hz = tuning->hzForDegree(degree);
    }
    else
    {
        const double source = 440.0 * std::pow(2.0, (static_cast<double>(note) - 69.0) / 12.0);
        const auto chosen = tuning->quantizeHz(source);
        if (! chosen.valid)
            return result;
        hz = chosen.hz;
    }

    if (! (hz > 0.0))
        return result;
    result.word = ScalePitch::toVoicePitchFromHz(hz);
    result.valid = true;
    return result;
}

}  // namespace swaraxt
