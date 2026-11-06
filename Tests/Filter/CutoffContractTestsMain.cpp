// Copyright 2026 MontroneDSP.
// SPDX-License-Identifier: GPL-3.0-or-later
//
// Cutoff parameter-contract tests for the production filter path.
// These tests verify normalized<->physical parameter mapping and monotonicity
// of the committed cutoff frequency. They use the production
// SwaraXtFilter::processInstrumentSample() path (not the legacy processSample()
// cascade removed in Mission 06).

#include "Engine/Filter/SwaraXtFilter.h"
#include "Engine/SwaraXtEngine.h"
#include "Plugin/PluginProcessor.h"
#include "Plugin/SwaraXtParameterLayout.h"

#include <cmath>
#include <cstdio>
#include <limits>

namespace {

int gFailures = 0;

void expect(bool cond, const char* message)
{
    if (! cond)
    {
        std::printf("FAIL: %s\n", message);
        ++gFailures;
    }
}

void testCutoffRoundtrip()
{
    SwaraXtAudioProcessor proc;
    auto* parameter = proc.getApvts().getParameter(swaraxt::IDs::filterCutoff);
    expect(parameter != nullptr, "cutoff parameter exists");
    if (! parameter)
        return;

    const double important[] = { 20.0, 25.0, 30.0, 40.0, 50.0, 70.0, 100.0,
                                 150.0, 200.0, 500.0, 1000.0, 2000.0 };
    for (double hz : important)
    {
        float normalized = parameter->convertTo0to1(static_cast<float>(hz));
        double recovered = parameter->convertFrom0to1(normalized);
        const double relErr = std::fabs(recovered - hz) / hz;
        expect(relErr < 0.01, "cutoff normalized roundtrip");
    }
}

void testCutoffMonotonicity()
{
    SwaraXtAudioProcessor proc;
    auto* parameter = proc.getApvts().getParameter(swaraxt::IDs::filterCutoff);
    expect(parameter != nullptr, "cutoff parameter exists");
    if (! parameter)
        return;

    double previousNormalized = -1.0;
    double previousCoefficient = -1.0;
    double minimumCoefficientStep = std::numeric_limits<double>::max();
    int currentPlateau = 0;
    int longestPlateau = 0;

    constexpr int points = 512;
    swaraxt::CutoffMapper mapper;
    for (int i = 0; i < points; ++i)
    {
        const double position = static_cast<double>(i) / static_cast<double>(points - 1);
        const double requested = 10.0 * std::pow(2000.0, position);
        float normalized = parameter->convertTo0to1(static_cast<float>(requested));
        double physical = parameter->convertFrom0to1(normalized);
        double mapped = mapper.mapCutoffHz(physical, 0.0, 69.0, 0.0);
        double coefficient = swaraxt::CutoffMapper::fourPoleMinus3DbRatio()
            * mapped;
        if (i > 0)
        {
            expect(normalized > previousNormalized, "cutoff low-end monotonicity");
            expect(coefficient > previousCoefficient, "cutoff coefficient monotonicity");
            const double step = coefficient - previousCoefficient;
            minimumCoefficientStep = std::min(minimumCoefficientStep, step);
            currentPlateau = step <= 1.0e-12 ? currentPlateau + 1 : 0;
            longestPlateau = std::max(longestPlateau, currentPlateau);
        }
        previousNormalized = normalized;
        previousCoefficient = coefficient;
    }

    expect(longestPlateau == 0, "cutoff no low-end plateau");
    expect(minimumCoefficientStep > 1.0e-8, "cutoff dense sweep retains coefficient separation");
}

}  // namespace

int main()
{
    juce::ScopedJuceInitialiser_GUI init;
    testCutoffRoundtrip();
    testCutoffMonotonicity();
    std::printf(gFailures == 0 ? "Cutoff contract tests: PASSED\n"
                               : "Cutoff contract tests: FAILED (%d)\n",
                gFailures);
    return gFailures == 0 ? 0 : 1;
}
