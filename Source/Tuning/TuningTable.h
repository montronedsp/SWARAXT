// Copyright 2026 MontroneDSP.
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include <cstddef>
#include <cstdint>
#include <vector>

namespace swaraxt::tuning {

// Versioned little-endian table. The repeating interval is `periodCents`
// and is not assumed to be 1200 (an octave). Degree 0 is the implicit tonic.
// Scala pitch N (1-based) lives at degree N; the last Scala pitch is the period.
inline constexpr char kMagic[4] = { 'S', 'W', 'T', '1' };
inline constexpr uint16_t kFormatVersion = 1;
inline constexpr int kMaxDegrees = 2048;
inline constexpr int kMidiNotes = 128;
inline constexpr int kNameBytes = 64;

struct TuningSnapshot {
    uint16_t degreeCount = 0;       // pitches strictly inside one period
    uint16_t formalPeriod = 0;      // scale degrees per period (usually degreeCount)
    uint16_t referenceNote = 69;
    double referenceHz = 440.0;
    double periodCents = 1200.0;
    char name[kNameBytes] {};
    // Cents of degree 1..degreeCount measured from the tonic. Empty when degreeCount == 0.
    std::vector<double> degreeCents;
    // MIDI note -> signed scale degree; assignment is separate from degree sign.
    int16_t midiDegree[kMidiNotes] {};
    double midiHz[kMidiNotes] {};
    // Legacy tables use negative degrees as unmapped. Direct Scala maps also
    // permit signed degrees: a positive stored frequency identifies assignment.
    // This is derived from the existing SWT1 frequency array when reading.
    bool signedKeyboardMapping = false;

    bool assigned(int note) const noexcept
    {
        return note >= 0 && note < kMidiNotes
            && (midiDegree[note] >= 0 || (signedKeyboardMapping && midiHz[note] > 0.0));
    }

    bool mapped(int midiNote) const noexcept
    {
        return assigned(midiNote)
            && midiHz[midiNote] > 0.0;
    }

    double hzForMidi(int midiNote) const noexcept
    {
        return mapped(midiNote) ? midiHz[midiNote] : 0.0;
    }

    // degree may be negative. Period repeats by `formalPeriod` degrees.
    double hzForDegree(int degree) const noexcept;

    // Equal-tempered conversion of a continuous CV semitone (MIDI-note units).
    // This is the LINEAR pitch space, not a scale lookup.
    static double equalHzFromCvSemitones(double semitonesFromMidi0, double referenceNote, double referenceHz) noexcept;

    // Nearest mapped MIDI note in log-frequency. Returns -1 when nothing is mapped.
    // Ties resolve toward the lower MIDI note.
    int nearestMappedMidi(double hz) const noexcept;
};

struct ParseError {
    const char* message = "ok";
};

// The legacy dialect is retained for existing build-time conversions only.
// Direct user imports explicitly select standard Scala semantics.
enum class ScalaSyntax { legacy, standard };
// Parse Scala text plus an optional KBM. On success, `out` is fully validated.
bool parseScalaAndKeyboard(const char* sclText, const char* kbmText, TuningSnapshot& out, ParseError& error,
                           ScalaSyntax syntax = ScalaSyntax::legacy);

bool writeTuningTable(const TuningSnapshot& snapshot, std::vector<uint8_t>& bytes, ParseError& error);
bool readTuningTable(const uint8_t* bytes, size_t size, TuningSnapshot& out, ParseError& error);

}  // namespace swaraxt::tuning
