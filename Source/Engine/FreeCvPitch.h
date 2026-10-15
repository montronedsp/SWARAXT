// Copyright 2026 MontroneDSP.
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include <cstdint>

namespace swaraxt {

// Persistent free-CV pitches are semitones from MIDI note 0, stored at
// 1/1024 semitone. The voice pitch word is 128 units per semitone, so the
// sounding value is the stored pitch rounded to the nearest 1/128 semitone
// (100/128 cent, about 0.781 cent). Storage itself is about 0.098 cent.
struct FreeCvPitch {
    static constexpr int32_t kUnitsPerSemitone = 1024;
    static constexpr int32_t kVoiceUnitsPerSemitone = 128;
    static constexpr int32_t kVoiceQuantum = kUnitsPerSemitone / kVoiceUnitsPerSemitone;
    // Pitch word 0 is the muted-scale sentinel. The lowest sounding target is MIDI 1.
    static constexpr int32_t kMinUnits = kUnitsPerSemitone;
    static constexpr int32_t kMaxUnits = 127 * kUnitsPerSemitone;

    static constexpr int32_t fromMidiNote(int note) noexcept
    {
        if (note < 0)
            note = 0;
        if (note > 127)
            note = 127;
        return static_cast<int32_t>(note) * kUnitsPerSemitone;
    }

    static constexpr int32_t clampUnits(int32_t units) noexcept
    {
        if (units < 0)
            return 0;
        if (units > kMaxUnits)
            return kMaxUnits;
        return units;
    }

    static constexpr uint16_t toVoicePitch(int32_t units) noexcept
    {
        if (units < kMinUnits)
            units = kMinUnits;
        if (units > kMaxUnits)
            units = kMaxUnits;
        return static_cast<uint16_t>((units + (kVoiceQuantum / 2)) / kVoiceQuantum);
    }
};

}  // namespace swaraxt
