// Copyright 2026 MontroneDSP. SPDX-License-Identifier: GPL-3.0-or-later
// Expected voltages are calculated in this file from the firmware byte law.
// They do not call CutoffMapper.
#include "Engine/Filter/SwaraXtFilter.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <limits>

namespace {
int failures = 0;

void check(bool ok, const char* message)
{
    if (!ok)
    {
        ++failures;
        std::printf("FAIL: %s\n", message);
    }
}

double firmwareCv(int byte, double trackingOctaves)
{
    return std::clamp((static_cast<double>(byte) + 24.0 * trackingOctaves) * (5.0 / 255.0), 0.0, 5.0);
}

// Default calibration maps a finite panel frequency through an identity
// stage conversion. The board then inverts that frequency with the firmware law.
double negativeCvFallback(double panelHz)
{
    const double hz = std::clamp(std::isfinite(panelHz) ? panelHz : 0.0, 10.0, 20000.0);
    return std::clamp((254.0 + 24.0 * std::log2(std::max(hz, 1.0) / 20000.0)) * (5.0 / 255.0), 0.0, 5.0);
}

double readCv(swaraxt::SwaraXtFilter& filter, const swaraxt::SwaraXtFilterParams& params)
{
    filter.setParams(params);
    for (int i = 0; i < 4; ++i)
        filter.processInstrumentSample(i == 0 ? 0.25f : 0.0f, 1.0f);
    return filter.boardCoreForTests().cutoffCv();
}
}

int main()
{
    const int bytes[] = {0, 1, 127, 254, 255};
    for (int byte : bytes)
    {
        const double expected = firmwareCv(byte, 0.0);
        swaraxt::SwaraXtFilter filter;
        filter.prepare(48000.0);
        swaraxt::SwaraXtFilterParams params;
        params.cutoffHz = byte == 255 ? 10.0f : 20000.0f;
        params.boardCutoffCvVolts = expected;
        const double actual = readCv(filter, params);
        check(std::abs(actual - expected) < 1.0e-9, "explicit firmware CV is the board target");

        swaraxt::SwaraXtFilter diagnostic;
        diagnostic.prepare(48000.0);
        swaraxt::SwaraXtFilterParams fallback;
        fallback.cutoffHz = params.cutoffHz;
        fallback.keyTrack = 0.0f;
        fallback.matrixCutoffOctaves = 0.0f;
        fallback.boardCutoffCvVolts = -1.0;
        const double fromHz = readCv(diagnostic, fallback);
        if (byte == 0 || byte == 255)
            check(std::abs(actual - fromHz) > 1.0e-3,
                "explicit CV stays independent of the conflicting Hertz fallback");
    }

    for (double trim : {-1.0, 0.0, 1.0})
    {
        swaraxt::SwaraXtFilter filter;
        filter.prepare(44100.0);
        swaraxt::SwaraXtFilterParams params;
        params.cutoffHz = 10.0f;
        params.boardCutoffCvVolts = firmwareCv(128, trim);
        check(std::abs(readCv(filter, params) - params.boardCutoffCvVolts) < 1.0e-9,
            "tracking trim reaches the board in volts");
    }

    for (double hz : {10.0, 440.0, 20000.0})
    {
        swaraxt::SwaraXtFilter filter;
        filter.prepare(96000.0);
        swaraxt::SwaraXtFilterParams params;
        params.cutoffHz = static_cast<float>(hz);
        params.keyTrack = 0.0f;
        params.noteNumber = 69.0f;
        params.matrixCutoffOctaves = 0.0f;
        params.boardCutoffCvVolts = -1.0;
        check(std::abs(readCv(filter, params) - negativeCvFallback(hz)) < 1.0e-6,
            "negative CV follows the independent Hertz fallback");
    }

    {
        const double note = 81.0;
        const double key = 1.0;
        const double panel = 1000.0;
        const double musical = std::clamp(panel * std::pow(2.0, (note - 69.0) / 12.0 * key), 10.0, 20000.0);
        swaraxt::SwaraXtFilter filter;
        filter.prepare(48000.0);
        swaraxt::SwaraXtFilterParams params;
        params.cutoffHz = static_cast<float>(panel);
        params.keyTrack = static_cast<float>(key);
        params.noteNumber = static_cast<float>(note);
        params.matrixCutoffOctaves = 0.0f;
        params.boardCutoffCvVolts = -1.0;
        check(std::abs(readCv(filter, params) - negativeCvFallback(musical)) < 1.0e-6,
            "negative-CV key tracking uses the independent musical frequency");
    }

    swaraxt::SwaraXtFilter invalid;
    invalid.prepare(48000.0);
    swaraxt::SwaraXtFilterParams bad;
    bad.cutoffHz = std::numeric_limits<float>::quiet_NaN();
    bad.boardCutoffCvVolts = firmwareCv(200, 0.0);
    const double explicitDespiteNan = readCv(invalid, bad);
    check(std::abs(explicitDespiteNan - bad.boardCutoffCvVolts) < 1.0e-9,
        "a nonfinite Hertz value does not replace an explicit CV");
    check(std::isfinite(invalid.processInstrumentSample(0.2f, 1.0f)),
        "explicit-CV rendering stays finite when Hertz is nonfinite");

    swaraxt::SwaraXtFilter poisoned;
    poisoned.prepare(48000.0);
    swaraxt::SwaraXtFilterParams nanFallback;
    nanFallback.cutoffHz = std::numeric_limits<float>::infinity();
    nanFallback.keyTrack = 0.0f;
    nanFallback.matrixCutoffOctaves = 0.0f;
    nanFallback.boardCutoffCvVolts = -1.0;
    const double collapsed = readCv(poisoned, nanFallback);
    check(std::isfinite(poisoned.processInstrumentSample(0.2f, 1.0f)),
        "nonfinite fallback Hertz does not poison the sample");
    check(std::abs(collapsed - negativeCvFallback(0.0)) < 1.0e-5,
        "nonfinite fallback Hertz collapses to the minimum panel frequency");

    invalid.reset();
    invalid.prepare(22050.0);
    check(std::isfinite(invalid.processInstrumentSample(0.0f, 0.0f)), "reset and reprepare stay finite");

    for (auto quality : {swaraxt::FilterQuality::eco, swaraxt::FilterQuality::normal, swaraxt::FilterQuality::high})
    {
        swaraxt::SwaraXtFilter filter;
        filter.prepare(48000.0);
        swaraxt::SwaraXtFilterParams params;
        params.boardCutoffCvVolts = firmwareCv(180, 0.0);
        params.cutoffHz = 10.0f;
        filter.setParams(params);
        filter.setQuality(quality);
        filter.prepare(88200.0);
        check(std::abs(readCv(filter, params) - params.boardCutoffCvVolts) < 1.0e-9,
            "quality and sample-rate changes keep the explicit CV");
    }

    std::printf("Cutoff authority: %d failures\n", failures);
    return failures == 0 ? 0 : 1;
}
