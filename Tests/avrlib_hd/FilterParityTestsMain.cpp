// Copyright 2026 MontroneDSP.
// SPDX-License-Identifier: GPL-3.0-or-later
//
// HdFilter parity: faithful mode must be bit-identical to the live engine's
// use of swaraxt::SwaraXtFilter for the same committed parameters across a grid
// of Shruthi-derived control values, and the mapping helpers must reproduce the
// engine's updateFilterFromShruthi scaling exactly.

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
    uint8_t env_source;
    uint8_t lfo2_source;
    int16_t cutoff_matrix_delta;
    int16_t resonance_matrix_delta;
    float panel_resonance;
    float panel_env_amount;
    float panel_mod_amount;
    float panel_key_track;
};

// Reproduces SwaraXtEngine::updateFilterFromShruthi param derivation.
avrlib_hd::FilterParams deriveParams(const ShruthiControl& c, float cutoff_hz, float note)
{
    avrlib_hd::FilterParams p;
    p.cutoff_hz = cutoff_hz;
    p.resonance = c.panel_resonance;
    p.key_track = c.panel_key_track;
    p.env_amount = c.panel_env_amount * 4.0f;
    p.mod_amount = c.panel_mod_amount * 2.0f;
    p.drive = 1.0f;
    p.note_number = note;
    p.env_value = static_cast<float>(c.env_source) / 255.0f;
    p.mod_value = static_cast<float>(static_cast<int>(c.lfo2_source) - 128) / 128.0f;
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
    bp.envAmount = p.env_amount;
    bp.modAmount = p.mod_amount;
    bp.drive = p.drive;
    bp.noteNumber = p.note_number;
    bp.envValue = p.env_value;
    bp.modValue = p.mod_value;
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
    const uint8_t envs[] = {0, 128, 255};
    const uint8_t lfos[] = {0, 128, 255};
    const int16_t cutDeltas[] = {0, 1024, -2048};
    const int16_t resDeltas[] = {0, 128, -64};
    const float panelResonances[] = {0.0f, 0.8f};
    const float panelEnv[] = {0.0f, 1.0f};
    const float panelMod[] = {0.0f, 0.25f};
    const float panels[]{10.0f, 800.0f, 5000.0f, 20000.0f};
    const float notes[]{36.0f, 69.0f};
    const float keyTracks[] = {0.0f, 1.0f};
    int cases = 0;
    for (uint8_t env : envs)
        for (uint8_t lfo : lfos)
            for (int16_t cd : cutDeltas)
                for (int16_t rd : resDeltas)
                    for (float pr : panelResonances)
                        for (float pe : panelEnv)
                            for (float pm : panelMod)
                                for (float cutoff : panels)
                                    for (float kt : keyTracks)
                                        for (float note : notes)
                                        {
                                            ShruthiControl c;
                                            c.env_source = env;
                                            c.lfo2_source = lfo;
                                            c.cutoff_matrix_delta = cd;
                                            c.resonance_matrix_delta = rd;
                                            c.panel_resonance = pr;
                                            c.panel_env_amount = pe;
                                            c.panel_mod_amount = pm;
                                            c.panel_key_track = kt;
                                            runFaithfulParityCase(c, cutoff, note);
                                            ++cases;
                                        }
    std::printf("filter faithful parity: %d cases\n", cases);
}

void testMappingScaling()
{
    // 1536 cutoff units (12 semitones * 128) is one octave of matrix cutoff,
    // mirroring kShruthiCutoffUnitsPerOctave = 12*128.
    const avrlib_hd::FilterParams p = deriveParams(
        {255u, 128u, 1536, 0, 0.0f, 0.0f, 0.0f, 0.0f}, 1000.0f, 69.0f);
    expect(std::fabs(p.matrix_cutoff_octaves - 1.0f) < 1.0e-6f,
           "cutoff delta full-scale is one octave");
    expect(std::fabs(p.env_value - 1.0f) < 1.0e-6f, "env source maps to 1.0");
    expect(p.mod_value == 0.0f, "midpoint LFO maps to zero");
    expect(p.mod_value - 1.0f < 0.0f && p.mod_value > -1.0f, "LFO range bounded");

    const avrlib_hd::FilterParams low = deriveParams(
        {0u, 255u, -16384, 0, 0.0f, 0.0f, 0.0f, 0.0f}, 1000.0f, 69.0f);
    expect(std::fabs(low.mod_value + 1.0f) > 0.999f, "LFO high source is -1 (-2 semitones)");

    const avrlib_hd::FilterParams env2 = deriveParams(
        {255u, 128u, 0, 0, 0.0f, 1.0f, 0.5f, 1.0f}, 1000.0f, 69.0f);
    expect(env2.env_amount == 4.0f, "panel env amount scaled by 4 (octaves)");
    expect(env2.mod_amount == 1.0f, "panel mod amount scaled by 2");
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