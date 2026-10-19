// Copyright 2026 MontroneDSP.
// SPDX-License-Identifier: GPL-3.0-or-later

#include "Tuning/TuningTable.h"

#include <cmath>
#include <cstdio>

namespace {

int failures = 0;

void expect(bool condition, const char* message)
{
    if (! condition)
    {
        std::printf("FAIL: %s\n", message);
        ++failures;
    }
}

const char* kTwelve =
    "! 12-tet\n"
    "12-tone equal temperament\n"
    "12\n"
    "100.0\n200.0\n300.0\n400.0\n500.0\n600.0\n"
    "700.0\n800.0\n900.0\n1000.0\n1100.0\n1200.0\n";

const char* kJust =
    "! 5-limit\n"
    "5-limit major\n"
    "12\n"
    "16/15\n9/8\n6/5\n5/4\n4/3\n45/32\n"
    "3/2\n8/5\n5/3\n16/9\n15/8\n2/1\n";

const char* kTritave =
    "! non-octave\n"
    "tritave three\n"
    "3\n"
    "13/9\n"
    "3/2\n"
    "3/1\n";

const char* kUnmapped =
    "! partial map\n"
    "2\n"
    "60\n"
    "127\n"
    "60\n"
    "60\n"
    "261.6255653006\n"
    "12\n"
    "0\n"
    "x\n";

}  // namespace

int main()
{
    swaraxt::tuning::ParseError error;
    swaraxt::tuning::TuningSnapshot twelve;
    expect(swaraxt::tuning::parseScalaAndKeyboard(kTwelve, nullptr, twelve, error), error.message);
    expect(std::abs(twelve.periodCents - 1200.0) < 1.0e-9, "12-tet period is 1200 cents");
    expect(std::abs(twelve.hzForMidi(69) - 440.0) < 1.0e-6, "A4 is 440 Hz");
    expect(std::abs(twelve.hzForMidi(81) / twelve.hzForMidi(69) - 2.0) < 1.0e-6, "one octave doubles frequency");
    expect(twelve.nearestMappedMidi(440.0) == 69, "nearest mapped note of 440 Hz is A4");

    swaraxt::tuning::TuningSnapshot just;
    expect(swaraxt::tuning::parseScalaAndKeyboard(kJust, nullptr, just, error), "just intonation parses");
    expect(std::abs(just.hzForDegree(7) / just.hzForDegree(0) - 1.5) < 1.0e-6
               || std::abs(just.midiHz[67] / just.midiHz[60] - 1.5) < 1.0e-4,
           "just fifth is 3/2");
    expect(std::abs(just.midiHz[67] / just.midiHz[60] - 1.5) < 1.0e-6, "MIDI just fifth is 3/2");

    swaraxt::tuning::TuningSnapshot tritave;
    expect(swaraxt::tuning::parseScalaAndKeyboard(kTritave, nullptr, tritave, error), "non-octave scale parses");
    expect(std::abs(tritave.periodCents - 1200.0 * std::log2(3.0)) < 1.0e-6, "period is a 3/1 tritave");
    expect(std::abs(tritave.periodCents - 1200.0) > 100.0, "tritave is not an octave");

    std::vector<uint8_t> bytes;
    expect(swaraxt::tuning::writeTuningTable(twelve, bytes, error), "table writes");
    swaraxt::tuning::TuningSnapshot loaded;
    expect(swaraxt::tuning::readTuningTable(bytes.data(), bytes.size(), loaded, error), error.message);
    expect(std::abs(loaded.hzForMidi(69) - 440.0) < 1.0e-6, "round trip keeps A4");
    expect(std::abs(loaded.periodCents - twelve.periodCents) < 1.0e-9, "round trip keeps the period");

    swaraxt::tuning::TuningSnapshot bad;
    expect(! swaraxt::tuning::parseScalaAndKeyboard("only a name\n", nullptr, bad, error), "malformed scala is rejected");
    expect(! swaraxt::tuning::parseScalaAndKeyboard("bad\n2\nnot-a-pitch\n1200.0\n", nullptr, bad, error),
           "malformed pitch is rejected");
    expect(! swaraxt::tuning::parseScalaAndKeyboard("bad\n1\n0.0\n", nullptr, bad, error),
           "non-positive period is rejected");

    swaraxt::tuning::TuningSnapshot unmapped;
    expect(swaraxt::tuning::parseScalaAndKeyboard(kTwelve, kUnmapped, unmapped, error), error.message);
    expect(unmapped.mapped(60), "first mapped key is valid");
    expect(! unmapped.mapped(61), "x leaves a key unmapped");
    expect(unmapped.hzForMidi(61) == 0.0, "unmapped key has no frequency");

    const double cv = swaraxt::tuning::TuningSnapshot::equalHzFromCvSemitones(69.5, 69.0, 440.0);
    expect(cv > 440.0 && cv < twelve.hzForMidi(70), "continuous CV sits between equal semitones");
    expect(std::abs(cv / 440.0 - std::pow(2.0, 0.5 / 12.0)) < 1.0e-9, "CV conversion is equal temperament");

    for (size_t length = 0; length < bytes.size(); ++length)
        expect(!swaraxt::tuning::readTuningTable(bytes.data(), length, loaded, error),
               "every truncated tuning table is rejected");
    auto wrongMagic = bytes;
    wrongMagic[0] ^= 0xff;
    expect(!swaraxt::tuning::readTuningTable(wrongMagic.data(), wrongMagic.size(), loaded, error),
           "wrong tuning magic is rejected");
    bytes.push_back(0);
    expect(! swaraxt::tuning::readTuningTable(bytes.data(), bytes.size(), loaded, error), "trailing bytes are rejected");

    if (failures == 0)
    {
        std::printf("tuning table tests: PASSED\n");
        return 0;
    }
    std::printf("tuning table tests: FAILED (%d)\n", failures);
    return 1;
}
