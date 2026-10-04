// Copyright 2026 MontroneDSP.
// SPDX-License-Identifier: GPL-3.0-or-later
//
// HdFilter parity: faithful mode must be bit-identical to the live engine's
// use of swaraxt::SwaraXtFilter for the same committed parameters across a grid
// of Shruthi-derived control values, and the mapping helpers must reproduce the
// optional facade's explicit extra offsets exactly.

#include "avrlib_hd/filter.h"

#include <cmath>
#include <cstdint>
#include <cstdio>
#include <vector>

namespace {

int gFailures = 0;

void expect(bool cond, const char* name)
{
    if (! cond)
    {
        std::printf("FAIL: %s\n", name);
        ++gFailures;
    }
}

constexpr double kInternalRate = 20000000.0 / 510.0;

constexpr float kShruthiCutoffUnitsPerOctave = 12.0f * 128.0f;
constexpr float kShruthiDestinationFullScale = 255.0f * 64.0f;

struct ShruthiControl {
    int16_t cutoff_matrix_delta;
    int16_t resonance_matrix_delta;
    float panel_resonance;
    float panel_key_track;
};

// Converts explicit diagnostic offsets to the optional facade parameters.
avrlib_hd::FilterParams deriveParams(const ShruthiControl& c, float cutoff_hz, float note)
{
    avrlib_hd::FilterParams p;
    p.cutoff_hz = cutoff_hz;
    p.resonance = c.panel_resonance;
    p.key_track = c.panel_key_track;
    p.drive = 1.0f;
    p.note_number = note;
    p.matrix_cutoff_octaves = static_cast<float>(c.cutoff_matrix_delta)
        / kShruthiCutoffUnitsPerOctave;
    p.matrix_resonance = static_cast<float>(c.resonance_matrix_delta)
        / kShruthiDestinationFullScale;
    return p;
}

void runFaithfulParityCase(const ShruthiControl& c, float cutoff_hz, float note)
{
    const avrlib_hd::FilterParams p = deriveParams(c, cutoff_hz, note);

    avrlib_hd::HdFilter hd;
    hd.Prepare(kInternalRate);
    hd.SetMode(avrlib_hd::HdFilter::Mode::kFaithful);
    hd.Reset();
    hd.SetParams(p);

    swaraxt::SwaraXtFilter bare;
    bare.prepare(kInternalRate);
    bare.reset();
    swaraxt::SwaraXtFilterParams bp;
    bp.cutoffHz = p.cutoff_hz;
    bp.resonance = p.resonance + p.matrix_resonance;
    bp.keyTrack = p.key_track;
    bp.drive = p.drive;
    bp.noteNumber = p.note_number;
    bp.matrixCutoffOctaves = p.matrix_cutoff_octaves;
    bare.setParams(bp);

    constexpr int kSamples = 128;
    for (int i = 0; i < kSamples; ++i)
    {
        const double phase = 2.0 * swaraxt::kPi * 220.0 * static_cast<double>(i)
                             / kInternalRate;
        const float x = (i % 129 == 0)
            ? 1.0f
            : static_cast<float>(0.4 * std::sin(phase));
        const float a = hd.ProcessSample(x);
        const float b = bare.processSample(x);
        expect(a == b, "faithful HdFilter equals bare SwaraXtFilter sample");
        if (a != b)
            return;
    }
}

void testFaithfulParityGrid()
{
    int cases = 0;
    for (int16_t cd : {int16_t(0), int16_t(1024), int16_t(-2048)})
        for (int16_t rd : {int16_t(0), int16_t(128), int16_t(-64)})
            for (float resonance : {0.0f, 0.8f})
                for (float cutoff : {10.0f, 800.0f, 5000.0f, 20000.0f})
                    for (float tracking : {0.0f, 1.0f})
                        for (float note : {36.0f, 69.0f})
                        {
                            runFaithfulParityCase({cd, rd, resonance, tracking}, cutoff, note);
                            ++cases;
                        }
    std::printf("filter faithful parity: %d cases\n", cases);
}

void testMappingScaling()
{
    const auto p = deriveParams({1536, 0, 0.0f, 0.0f}, 1000.0f, 69.0f);
    expect(std::fabs(p.matrix_cutoff_octaves - 1.0f) < 1.0e-6f,
           "explicit cutoff offset scales to one octave");
    const auto low = deriveParams({-3072, 0, 0.0f, 0.0f}, 1000.0f, 69.0f);
    expect(low.matrix_cutoff_octaves == -2.0f, "negative explicit cutoff offset");
    const auto res = deriveParams({0, 8160, 0.0f, 0.0f}, 1000.0f, 69.0f);
    expect(res.matrix_resonance == 0.5f, "explicit resonance offset scaling");
}

}  // namespace

int main()
{
    testFaithfulParityGrid();
    testMappingScaling();
    if (gFailures == 0)
    {
        std::printf("AvrlibHdFilterParityTests: all passed\n");
        return 0;
    }
    std::printf("AvrlibHdFilterParityTests: %d FAILURES\n", gFailures);
    return 1;
}