// Copyright 2026 MontroneDSP.
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include "Tuning/TuningTable.h"

#include <algorithm>
#include <cmath>
#include <cstring>

namespace swaraxt::tuning {

// Fixed table the audio thread can read without allocation. Slots live for the
// engine lifetime. A writer fills an unpublished slot, then publishes its index.
struct PublishedTuning {
    static constexpr int kMaxInterior = 256;

    uint16_t formalPeriod = 12;
    uint16_t degreeCount = 0;
    uint16_t referenceNote = 69;
    double referenceHz = 440.0;
    double periodCents = 1200.0;
    double degreeCents[kMaxInterior] {};
    int16_t midiDegree[kMidiNotes] {};
    char name[kNameBytes] {};
    bool valid = false;

    bool fillFrom(const TuningSnapshot& snapshot) noexcept
    {
        valid = false;
        if (snapshot.formalPeriod == 0 || snapshot.degreeCount == 0
            || snapshot.degreeCount > kMaxInterior
            || snapshot.degreeCents.size() != snapshot.degreeCount
            || ! std::isfinite(snapshot.referenceHz) || snapshot.referenceHz <= 0.0
            || ! std::isfinite(snapshot.periodCents) || snapshot.periodCents <= 0.0
            || snapshot.referenceNote >= kMidiNotes
            || snapshot.midiDegree[snapshot.referenceNote] < 0)
            return false;
        formalPeriod = snapshot.formalPeriod;
        degreeCount = snapshot.degreeCount;
        referenceNote = snapshot.referenceNote;
        referenceHz = snapshot.referenceHz;
        periodCents = snapshot.periodCents;
        std::fill(std::begin(degreeCents), std::end(degreeCents), 0.0);
        for (uint16_t i = 0; i < degreeCount; ++i)
            degreeCents[i] = snapshot.degreeCents[i];
        for (int note = 0; note < kMidiNotes; ++note)
            midiDegree[note] = snapshot.midiDegree[note];
        std::memcpy(name, snapshot.name, kNameBytes);
        name[kNameBytes - 1] = '\0';
        valid = true;
        return true;
    }

    double centsForDegree(int degree) const noexcept
    {
        const int period = formalPeriod > 0 ? static_cast<int>(formalPeriod) : 1;
        int periodIndex = 0;
        int inside = degree;
        if (inside >= 0)
        {
            periodIndex = inside / period;
            inside %= period;
        }
        else
        {
            periodIndex = -(((-inside) + period - 1) / period);
            inside = degree - periodIndex * period;
        }
        double cents = static_cast<double>(periodIndex) * periodCents;
        if (inside > 0)
        {
            const int index = inside - 1;
            if (index >= 0 && index < static_cast<int>(degreeCount))
                cents += degreeCents[index];
        }
        return cents;
    }

    // Degree 0 is the scale tonic. The reference note's mapped degree sounds at referenceHz.
    double hzForDegree(int degree) const noexcept
    {
        if (! valid || referenceHz <= 0.0 || periodCents <= 0.0 || formalPeriod == 0)
            return 0.0;
        if (referenceNote >= kMidiNotes || midiDegree[referenceNote] < 0)
            return 0.0;
        const double cents = centsForDegree(degree) - centsForDegree(midiDegree[referenceNote]);
        const double hz = referenceHz * std::pow(2.0, cents / 1200.0);
        return std::isfinite(hz) && hz > 0.0 ? hz : 0.0;
    }

    // Held-key offset in scale degrees relative to the mapped tonic key.
    // Unmapped or ambiguous keys contribute 0 rather than a chromatic shift.
    int transpositionDegrees(int heldNote) const noexcept
    {
        if (! valid || heldNote < 0 || heldNote >= kMidiNotes)
            return 0;
        constexpr int tonicKey = 60;
        if (midiDegree[heldNote] < 0 || midiDegree[tonicKey] < 0)
            return 0;
        return static_cast<int>(midiDegree[heldNote]) - static_cast<int>(midiDegree[tonicKey]);
    }
};

}  // namespace swaraxt::tuning
