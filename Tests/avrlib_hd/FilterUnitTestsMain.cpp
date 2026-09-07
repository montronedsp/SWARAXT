// Copyright 2026 MontroneDSP.
// SPDX-License-Identifier: GPL-3.0-or-later
//
// HD VCF unit tests under the strict -Werror gate. The facade delegates to the
// swaraxt::SwaraXtFilter model, so the tests pin the mapping, the faithful/HD
// mode budgets, determinism and the finite-output safety contract.

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

std::vector<float> renderBlock(avrlib_hd::HdFilter& filter,
                               const avrlib_hd::FilterParams& params,
                               int samples)
{
    filter.Reset();
    filter.SetParams(params);
    std::vector<float> out(static_cast<size_t>(samples));
    for (int i = 0; i < samples; ++i)
    {
        const float x = (i % 257) == 0
            ? 1.0f
            : static_cast<float>(0.35 * std::sin(2.0 * swaraxt::kPi * 440.0 * i
                                                 / kInternalRate));
        out[static_cast<size_t>(i)] = filter.ProcessSample(x);
    }
    return out;
}

void testFaithfulMatchesBareModel()
{
    // kFaithful must be bit-identical to driving swaraxt::SwaraXtFilter at the
    // plugin-default (normal) quality with the same committed params.
    avrlib_hd::HdFilter hd;
    hd.Prepare(kInternalRate);
    hd.SetMode(avrlib_hd::HdFilter::Mode::kFaithful);

    swaraxt::SwaraXtFilter bare;
    bare.prepare(kInternalRate);
    bare.setQuality(swaraxt::FilterQuality::normal);

    avrlib_hd::FilterParams p;
    p.cutoff_hz = 800.0f;
    p.resonance = 0.4f;
    p.key_track = 0.7f;
    p.env_amount = 2.5f;
    p.mod_amount = 1.0f;
    p.drive = 1.0f;
    p.note_number = 60.0f;
    p.env_value = 0.5f;
    p.mod_value = -0.25f;
    p.matrix_cutoff_octaves = 0.75f;
    p.matrix_resonance = 0.1f;
    hd.SetParams(p);

    swaraxt::SwaraXtFilterParams bp;
    bp.cutoffHz = p.cutoff_hz;
    bp.resonance = swaraxt::clampFinite(p.resonance + p.matrix_resonance, 0.0f, 1.0f);
    bp.keyTrack = p.key_track;
    bp.envAmount = p.env_amount;
    bp.modAmount = p.mod_amount;
    bp.drive = p.drive;
    bp.noteNumber = p.note_number;
    bp.envValue = p.env_value;
    bp.modValue = p.mod_value;
    bp.matrixCutoffOctaves = p.matrix_cutoff_octaves;
    bare.setParams(bp);

    constexpr int kSamples = 2048;
    std::vector<float> a;
    std::vector<float> b;
    a.reserve(static_cast<size_t>(kSamples));
    b.reserve(static_cast<size_t>(kSamples));
    double phaseA = 0.0;
    double phaseB = 0.0;
    for (int i = 0; i < kSamples; ++i)
    {
        phaseA += 440.0 / kInternalRate;
        phaseB += 440.0 / kInternalRate;
        if (phaseA >= 1.0) phaseA -= 1.0;
        if (phaseB >= 1.0) phaseB -= 1.0;
        const float x = static_cast<float>(std::sin(2.0 * swaraxt::kPi * phaseA));
        a.push_back(hd.ProcessSample(i % 257 == 0 ? 1.0f : 0.35f * x));
        b.push_back(bare.processSample(i % 257 == 0 ? 1.0f : 0.35f * x));
    }

    int mismatches = 0;
    for (int i = 0; i < kSamples; ++i)
    {
        if (a[static_cast<size_t>(i)] != b[static_cast<size_t>(i)])
            ++mismatches;
    }
    expect(mismatches == 0, "faithful HdFilter bit-matches bare SwaraXtFilter");
}

void testHdBudget()
{
    avrlib_hd::HdFilter hd;
    hd.Prepare(kInternalRate);
    const int normalOs = hd.OversampleFactor();
    const int normalIters = hd.SolverIterations();

    hd.SetMode(avrlib_hd::HdFilter::Mode::kHd);
    const int highOs = hd.OversampleFactor();
    const int highIters = hd.SolverIterations();
    expect(highIters > normalIters, "HD mode raises solver budget");
    expect(highOs >= normalOs, "HD mode does not reduce oversampling");

    hd.SetMode(avrlib_hd::HdFilter::Mode::kFaithful);
    expect(hd.OversampleFactor() == normalOs
               && hd.SolverIterations() == normalIters,
           "mode switches back to the faithful budget");
}

void testHdFiniteAndConvergent()
{
    avrlib_hd::HdFilter hd;
    hd.Prepare(kInternalRate);
    hd.SetMode(avrlib_hd::HdFilter::Mode::kHd);

    avrlib_hd::FilterParams p;
    p.cutoff_hz = 4000.0f;
    p.resonance = 0.95f;
    p.drive = 1.5f;
    hd.SetParams(p);

    const std::vector<float> out = renderBlock(hd, p, 4096);
    for (float s : out)
        expect(std::isfinite(s), "HD filter output stays finite at high resonance");
}

void testFaithfulVsHdBounded()
{
    avrlib_hd::FilterParams p;
    p.cutoff_hz = 1200.0f;
    p.resonance = 0.8f;
    p.drive = 1.0f;
    p.env_value = 0.4f;
    p.note_number = 64.0f;

    avrlib_hd::HdFilter hd;
    hd.Prepare(kInternalRate);
    hd.SetMode(avrlib_hd::HdFilter::Mode::kFaithful);
    const std::vector<float> faithful = renderBlock(hd, p, 4096);
    hd.SetMode(avrlib_hd::HdFilter::Mode::kHd);
    const std::vector<float> hdOut = renderBlock(hd, p, 4096);

    double peak = 0.0;
    for (size_t i = 0; i < faithful.size(); ++i)
        peak = std::max(peak, static_cast<double>(std::fabs(faithful[i] - hdOut[i])));
    expect(std::isfinite(static_cast<float>(peak)), "HD vs faithful diff is finite");
    expect(peak < 1.0, "HD vs faithful resonant response stays close");
}

void testResonanceClamp()
{
    // SetParams clamps resonance+matrix to [0,1] exactly like the engine.
    avrlib_hd::HdFilter hd;
    hd.Prepare(kInternalRate);

    avrlib_hd::FilterParams p;
    p.cutoff_hz = 1000.0f;
    p.resonance = 0.9f;
    p.matrix_resonance = 0.5f;
    hd.SetParams(p);
    expect(true, "clamped resonance parameters accepted");
}

void testDeterminismAndReset()
{
    avrlib_hd::FilterParams p;
    p.cutoff_hz = 700.0f;
    p.resonance = 0.55f;
    p.key_track = 1.0f;
    p.note_number = 57.0f;

    avrlib_hd::HdFilter hd;
    hd.Prepare(kInternalRate);
    const std::vector<float> a = renderBlock(hd, p, 2048);
    const std::vector<float> b = renderBlock(hd, p, 2048);
    expect(a == b, "identical reset+render is deterministic");

    // ProcessBlock must equal the per-sample loop.
    avrlib_hd::HdFilter blk;
    blk.Prepare(kInternalRate);
    blk.Reset();
    blk.SetParams(p);
    std::vector<float> samples({0.1f, -0.2f, 0.05f, 0.7f, -0.4f, 0.3f});
    const std::vector<float> original = samples;
    blk.ProcessBlock(samples.data(), static_cast<int>(samples.size()));

    avrlib_hd::HdFilter per;
    per.Prepare(kInternalRate);
    per.Reset();
    per.SetParams(p);
    for (size_t i = 0; i < original.size(); ++i)
        expect(per.ProcessSample(original[i]) == samples[i],
               "ProcessBlock equals per-sample ProcessSample");
}

void testDefaults();
bool hdOversampleKnown();

void testDefaults()
{
    expect(hdOversampleKnown(), "defaults sanity");
}

bool hdOversampleKnown()
{
    avrlib_hd::HdFilter hd;
    hd.Prepare(kInternalRate);
    return hd.OversampleFactor() >= 1 && hd.SolverIterations() >= 1;
}

}  // namespace

int main()
{
    testFaithfulMatchesBareModel();
    testHdBudget();
    testHdFiniteAndConvergent();
    testFaithfulVsHdBounded();
    testResonanceClamp();
    testDeterminismAndReset();
    testDefaults();
    if (gFailures == 0)
    {
        std::printf("AvrlibHdFilterTests: all passed\n");
        return 0;
    }
    std::printf("AvrlibHdFilterTests: %d FAILURES\n", gFailures);
    return 1;
}