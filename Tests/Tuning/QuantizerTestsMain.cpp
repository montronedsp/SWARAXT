// Copyright 2026 MontroneDSP.
// SPDX-License-Identifier: GPL-3.0-or-later

#include "Tuning/PublishedTuning.h"
#include "QuantizerTestSupport.h"

#include <cmath>
#include <cstdio>
#include <algorithm>
#include <fstream>
#include <iterator>
#include <limits>
#include <string>
#include <vector>

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

void densePlayableBand()
{
    swaraxt::tuning::TuningSnapshot snapshot;
    swaraxt::tuning::ParseError error;
    expect(quantizer_test::makeEdo(256, snapshot, error), "valid 256-EDO SCL parses");
    expect(snapshot.degreeCount == 255 && snapshot.formalPeriod == 256
               && snapshot.periodCents == 1200.0, "256-EDO has 255 interiors and an octave period");
    swaraxt::tuning::PublishedTuning table;
    expect(table.fillFrom(snapshot), "256-EDO publication succeeds");
    const auto c8 = table.quantizeHz(quantizer_test::midiHz(108));
    std::printf("256-EDO C8 expected=%.12f actual=%.12f degree=%d\n",
                quantizer_test::edo256Hz(108), c8.hz, c8.degree);
    expect(c8.valid && c8.degree == 841
               && std::abs(c8.hz - quantizer_test::midiHz(108)) < 1.0e-8,
           "256-EDO C8 selects degree 841 instead of a truncated endpoint");
    int mismatches = 0;
    for (int note = 0; note < 128; ++note)
    {
        const auto actual = table.quantizeHz(quantizer_test::midiHz(note));
        if (! actual.valid || actual.degree != quantizer_test::edo256Degree(note)
            || std::abs(1200.0 * std::log2(actual.hz / quantizer_test::edo256Hz(note))) > 1.0e-7)
            ++mismatches;
    }
    std::printf("256-EDO MIDI 0..127 quantizer mismatches=%d/128\n", mismatches);
    expect(mismatches == 0, "all MIDI 0..127 targets match the independent 256-EDO formula");
}

// Enumerate the entire existing degree domain independently of periodic search.
class ExhaustiveReference {
public:
    explicit ExhaustiveReference(const swaraxt::tuning::TuningSnapshot& snapshot)
    {
        for (int degree = -32768; degree <= 32767; ++degree)
        {
            const double hz = snapshot.hzForDegree(degree);
            if (hz > 0.0 && std::isfinite(hz))
                pitches.push_back({ hz, degree, true });
        }
        std::sort(pitches.begin(), pitches.end(), [](Pitch a, Pitch b) {
            return a.hz < b.hz || (a.hz == b.hz && a.degree < b.degree);
        });
        pitches.erase(std::unique(pitches.begin(), pitches.end(), [](Pitch a, Pitch b) {
            return a.hz == b.hz;
        }), pitches.end());
    }

    using Pitch = swaraxt::tuning::PublishedTuning::QuantizedPitch;

    Pitch nearest(double source) const
    {
        const auto upper = std::lower_bound(pitches.begin(), pitches.end(), source,
            [](Pitch pitch, double hz) { return pitch.hz < hz; });
        if (upper == pitches.begin()) return *upper;
        if (upper == pitches.end()) return pitches.back();
        const auto lower = upper - 1;
        const long double low = std::abs(std::log2(static_cast<long double>(lower->hz) / source));
        const long double high = std::abs(std::log2(static_cast<long double>(upper->hz) / source));
        return low <= high ? *lower : *upper;
    }

private:
    std::vector<Pitch> pitches;
};

void compareBand(const swaraxt::tuning::TuningSnapshot& snapshot, const char* label)
{
    swaraxt::tuning::PublishedTuning table;
    expect(table.fillFrom(snapshot), label);
    const ExhaustiveReference reference(snapshot);
    int mismatches = 0;
    for (int probe = 0; probe <= 2048; ++probe)
    {
        const double source = 440.0 * std::pow(2.0, (127.0 * probe / 2048.0 - 69.0) / 12.0);
        const auto expected = reference.nearest(source);
        const auto actual = table.quantizeHz(source);
        // The source itself is rounded to double at exact mathematical midpoints.
        // Compare distances here; the explicit midpoint tests verify tie policy.
        const double bestDistance = std::abs(1200.0 * std::log2(expected.hz / source));
        const double actualDistance = std::abs(1200.0 * std::log2(actual.hz / source));
        if (! actual.valid || actualDistance > bestDistance + 1.0e-7)
        {
            if (mismatches < 3)
                std::printf("oracle discrepancy %s source=%.17g expected=%.17g degree=%d actual=%.17g degree=%d\n",
                            label, source, expected.hz, expected.degree, actual.hz, actual.degree);
            ++mismatches;
        }
    }
    std::printf("%s: exhaustive-degree oracle mismatches=%d/2049 periodic=%d\n",
                label, mismatches, table.quantizerUsesPeriods ? 1 : 0);
    expect(mismatches == 0, label);
}

void periodicFamilies()
{
    swaraxt::tuning::TuningSnapshot snapshot;
    swaraxt::tuning::ParseError error;
    for (int divisions : { 128, 256, 257 })
    {
        expect(quantizer_test::makeEdo(divisions, snapshot, error), "dense EDO parses");
        compareBand(snapshot, "dense EDO");
    }
    // A micro-period spans far more repetitions than the old 1024-period scan.
    snapshot.periodCents = 1.0;
    for (size_t i = 0; i < snapshot.degreeCents.size(); ++i)
        snapshot.degreeCents[i] = (static_cast<double>(i) + 1.0) / 257.0;
    compareBand(snapshot, "micro-period within signed degree bounds");

    snapshot.formalPeriod = 12;
    snapshot.periodCents = 1200.0;
    snapshot.degreeCount = 5;
    snapshot.degreeCents = { 700.0, 100.0, 100.0, 1300.0, -1100.0 };
    snapshot.midiDegree[69] = 0;
    compareBand(snapshot, "non-monotonic, duplicate and cross-period intervals");
    swaraxt::tuning::PublishedTuning table;
    expect(table.fillFrom(snapshot), "duplicate fixture fills");
    const auto duplicate = table.quantizeHz(440.0 * std::pow(2.0, 100.0 / 1200.0));
    std::printf("duplicate actual Hz=%.17g degree=%d\n", duplicate.hz, duplicate.degree);
    expect(duplicate.valid && duplicate.degree == -8, "duplicate frequency keeps the lowest degree across periods");

    snapshot.formalPeriod = 65535;
    snapshot.degreeCount = 2;
    snapshot.degreeCents = { 700.0, 100.0 };
    compareBand(snapshot, "largest formal period and implicit tonic residues");
    expect(table.fillFrom(snapshot), "implicit tonic fixture fills");
    expect(table.quantizeHz(220.0).degree == -32768, "implicit tonic range keeps the lowest signed degree");

    snapshot.formalPeriod = 1;
    compareBand(snapshot, "period one ignores unreachable interior residues");
}

void boundariesAndTies()
{
    swaraxt::tuning::TuningSnapshot snapshot;
    swaraxt::tuning::ParseError error;
    expect(quantizer_test::makeEdo(256, snapshot, error), "dense tie fixture parses");
    swaraxt::tuning::PublishedTuning table;
    expect(table.fillFrom(snapshot), "dense tie fixture fills");
    int wrong = 0;
    for (int degree = -1463; degree < 1247; degree += 7)
    {
        const double lower = table.hzForDegree(degree);
        const double upper = table.hzForDegree(degree + 1);
        const double middle = std::sqrt(lower * upper);
        if (table.quantizeHz(middle).degree != degree
            || table.quantizeHz(middle * std::exp2(-1.0e-6 / 1200.0)).degree != degree
            || table.quantizeHz(middle * std::exp2(1.0e-6 / 1200.0)).degree != degree + 1)
            ++wrong;
    }
    expect(wrong == 0, "logarithmic midpoints choose lower; both sides choose nearest");
    expect(table.quantizeHz(table.hzForDegree(-32768)).degree == -32768,
           "lower signed degree endpoint is included");
    expect(table.quantizeHz(table.hzForDegree(32767)).degree == 32767,
           "upper signed degree endpoint is included");
    expect(table.quantizeHz(table.hzForDegree(-32768) * 0.5).degree == -32768,
           "sources below representable degrees clamp to the nearest endpoint");
    expect(table.quantizeHz(table.hzForDegree(32767) * 2.0).degree == 32767,
           "sources above representable degrees clamp to the nearest endpoint");

    // Distinct sub-millicent pitches must not disappear through cache deduplication.
    snapshot.periodCents = 0.1;
    for (size_t i = 0; i < snapshot.degreeCents.size(); ++i)
        snapshot.degreeCents[i] = (static_cast<double>(i) + 1.0) * 0.1 / 256.0;
    expect(table.fillFrom(snapshot), "sub-millicent scale fills");
    const double exact = table.hzForDegree(31);
    expect(table.quantizeHz(exact).degree == 31, "distinct sub-millicent scale pitch survives quantization");
}

void malformedInputs()
{
    using Limits = std::numeric_limits<double>;
    swaraxt::tuning::TuningSnapshot snapshot;
    swaraxt::tuning::ParseError error;
    expect(quantizer_test::makeEdo(256, snapshot, error), "invalid-source fixture parses");
    swaraxt::tuning::PublishedTuning table;
    expect(table.fillFrom(snapshot), "invalid-source fixture fills");
    for (double source : { 0.0, -440.0, Limits::infinity(), -Limits::infinity(), Limits::quiet_NaN() })
        expect(! table.quantizeHz(source).valid, "invalid source returns no pitch");
    for (double source : { Limits::denorm_min(), Limits::min(), Limits::max() })
    {
        const auto result = table.quantizeHz(source);
        expect(! result.valid || (std::isfinite(result.hz) && result.hz > 0.0),
               "finite source extremes never return a nonfinite pitch");
    }
    const auto good = snapshot;
    snapshot.degreeCents[10] = Limits::quiet_NaN();
    expect(! table.fillFrom(snapshot) && ! table.quantizeHz(440.0).valid,
           "nonfinite interior invalidates a previously filled table");
    snapshot = good;
    snapshot.periodCents = 0.0;
    expect(! table.fillFrom(snapshot), "zero period is rejected");
    snapshot = good;
    snapshot.referenceHz = Limits::infinity();
    expect(! table.fillFrom(snapshot), "nonfinite reference is rejected");
    snapshot = good;
    snapshot.degreeCount = 256;
    expect(! table.fillFrom(snapshot), "inconsistent degree count is rejected");
    const uint8_t invalid[] = { 'S', 'W', 'T', '1' };
    expect(! swaraxt::tuning::readTuningTable(invalid, sizeof(invalid), snapshot, error),
           "truncated SWT1 is rejected");
}

void factoryTunings(const char* directory)
{
    for (const char* name : { "12-edo", "19-edo", "24-edo", "31-edo", "just-major", "tritave" })
    {
        std::ifstream file(std::string(directory) + "/" + name + ".scl");
        const std::string text((std::istreambuf_iterator<char>(file)), std::istreambuf_iterator<char>());
        swaraxt::tuning::TuningSnapshot snapshot;
        swaraxt::tuning::ParseError error;
        expect(swaraxt::tuning::parseScalaAndKeyboard(text.c_str(), nullptr, snapshot, error), name);
        compareBand(snapshot, name);
    }
}

} // namespace

int main(int argc, char** argv)
{
    densePlayableBand();
    periodicFamilies();
    boundariesAndTies();
    malformedInputs();
    expect(argc == 2, "factory scale source directory is supplied");
    if (argc == 2)
        factoryTunings(argv[1]);
    std::printf("quantizer tests: %s (%d failures)\n", failures == 0 ? "PASSED" : "FAILED", failures);
    return failures == 0 ? 0 : 1;
}
