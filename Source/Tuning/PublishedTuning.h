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
        buildQuantizer();
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

    // Precomputed playable scale pitches, sorted by log frequency.
    // Coverage is the oscillator's MIDI 0..127 band, plus one period beyond
    // each end when the table can express it. At most kMaxCandidates entries;
    // a pathological micro-period stops at that cap and documents the limit
    // by leaving quantizerSaturated set. Duplicate frequencies keep the
    // lower degree. Non-monotonic degree lists are sorted before search.
    static constexpr int kMaxCandidates = 2048;
    static constexpr double kPlayableMinHz = 8.175798915643707;   // MIDI 0
    static constexpr double kPlayableMaxHz = 12543.853951415975;  // MIDI 127

    struct QuantizedPitch {
        double hz = 0.0;
        int degree = 0;
        bool valid = false;
    };

    double quantizerHz[kMaxCandidates] {};
    double quantizerLogHz[kMaxCandidates] {};
    int16_t quantizerDegree[kMaxCandidates] {};
    uint16_t quantizerCount = 0;
    bool quantizerSaturated = false;

    void buildQuantizer() noexcept
    {
        quantizerCount = 0;
        quantizerSaturated = false;
        if (! valid)
            return;
        const int period = formalPeriod > 0 ? static_cast<int>(formalPeriod) : 1;
        int degree = 0;
        for (int step = 0; step < 1024; ++step)
        {
            const double hz = hzForDegree(degree - period);
            if (! (hz > 0.0) || ! std::isfinite(hz) || hz < kPlayableMinHz * 0.5)
                break;
            degree -= period;
        }
        const double below = hzForDegree(degree - period);
        if (below > 0.0 && std::isfinite(below))
            degree -= period;

        struct Raw {
            double hz;
            double logHz;
            int degree;
        };
        Raw raw[kMaxCandidates];
        int rawCount = 0;
        for (int guard = 0; guard < 16384 && rawCount < kMaxCandidates; ++guard, ++degree)
        {
            const double hz = hzForDegree(degree);
            if (! (hz > 0.0) || ! std::isfinite(hz))
                continue;
            if (hz > kPlayableMaxHz * 2.0 && rawCount > 0)
                break;
            if (hz < kPlayableMinHz * 0.25)
                continue;
            if (degree < -32768 || degree > 32767)
                continue;
            raw[rawCount++] = { hz, std::log(hz), degree };
        }
        if (rawCount == kMaxCandidates)
            quantizerSaturated = true;
        std::sort(raw, raw + rawCount, [](const Raw& a, const Raw& b) {
            if (a.logHz < b.logHz)
                return true;
            if (b.logHz < a.logHz)
                return false;
            return a.degree < b.degree;
        });
        for (int i = 0; i < rawCount; ++i)
        {
            if (quantizerCount > 0)
            {
                const double previous = quantizerHz[quantizerCount - 1];
                const double cents = std::abs(1200.0 * std::log2(raw[i].hz / previous));
                if (cents < 0.001)
                    continue;
            }
            quantizerHz[quantizerCount] = raw[i].hz;
            quantizerLogHz[quantizerCount] = raw[i].logHz;
            quantizerDegree[quantizerCount] = static_cast<int16_t>(raw[i].degree);
            ++quantizerCount;
        }
    }

    // Nearest scale frequency in cents. An exact halfway source keeps the
    // lower-frequency candidate. Sources outside the table use the nearest
    // playable extreme. An empty table does not invent a pitch.
    QuantizedPitch quantizeHz(double sourceHz) const noexcept
    {
        QuantizedPitch result;
        if (quantizerCount == 0 || ! (sourceHz > 0.0) || ! std::isfinite(sourceHz))
            return result;
        const double sourceLog = std::log(sourceHz);
        int lo = 0;
        int hi = static_cast<int>(quantizerCount);
        while (lo < hi)
        {
            const int mid = lo + ((hi - lo) / 2);
            if (quantizerLogHz[mid] < sourceLog)
                lo = mid + 1;
            else
                hi = mid;
        }
        const int upper = lo;
        if (upper <= 0)
        {
            result.hz = quantizerHz[0];
            result.degree = quantizerDegree[0];
            result.valid = result.hz > 0.0;
            return result;
        }
        if (upper >= static_cast<int>(quantizerCount))
        {
            const int last = static_cast<int>(quantizerCount) - 1;
            result.hz = quantizerHz[last];
            result.degree = quantizerDegree[last];
            result.valid = result.hz > 0.0;
            return result;
        }
        const double lowerHz = quantizerHz[upper - 1];
        const double upperHz = quantizerHz[upper];
        const double lowerCents = std::abs(1200.0 * std::log2(lowerHz / sourceHz));
        const double upperCents = std::abs(1200.0 * std::log2(upperHz / sourceHz));
        const bool chooseLower = lowerCents <= upperCents;
        result.hz = chooseLower ? lowerHz : upperHz;
        result.degree = chooseLower ? quantizerDegree[upper - 1] : quantizerDegree[upper];
        result.valid = result.hz > 0.0 && std::isfinite(result.hz);
        return result;
    }
};

}  // namespace swaraxt::tuning
