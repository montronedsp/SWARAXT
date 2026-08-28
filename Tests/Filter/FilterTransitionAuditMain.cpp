// Copyright 2026 MontroneDSP.
// SPDX-License-Identifier: GPL-3.0-or-later
// Kernel-level cutoff/resonance transition audit. No JUCE.

#include "Engine/Filter/SwaraXtFilter.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <string>
#include <vector>

namespace {

constexpr double kFs = 39215.6862745098; // SWARA internal rate 20e6/510

struct JumpReport
{
    const char* name = "";
    float baselineMaxDy = 0.0f;
    float boundaryDy = 0.0f;
    float followMaxDy = 0.0f;
    float nextDy = 0.0f;
    float peak = 0.0f;
    int invalid = 0;
    bool isolatedClick = false;
};

float saw(int n, double hz)
{
    const double phase = std::fmod(static_cast<double>(n) * hz / kFs, 1.0);
    return static_cast<float>(2.0 * phase - 1.0);
}

float sine(int n, double hz)
{
    return static_cast<float>(std::sin(2.0 * 3.14159265358979323846 * hz * static_cast<double>(n) / kFs));
}

JumpReport measureJump(const char* name,
                       bool useSaw,
                       float startCutoff,
                       float endCutoff,
                       float startRes,
                       float endRes,
                       int settle,
                       int jumpAt)
{
    swaraxt::SwaraXtFilter filter;
    filter.prepare(kFs);
    swaraxt::SwaraXtFilterParams params;
    params.cutoffHz = startCutoff;
    params.resonance = startRes;
    filter.setParams(params);
    filter.setQuality(swaraxt::FilterQuality::high);

    std::vector<float> y(static_cast<size_t>(jumpAt + 512));
    JumpReport report;
    report.name = name;
    for (int n = 0; n < static_cast<int>(y.size()); ++n)
    {
        if (n == jumpAt)
        {
            params.cutoffHz = endCutoff;
            params.resonance = endRes;
            filter.setParams(params);
        }
        const float x = useSaw ? saw(n, 220.0) : sine(n, 220.0);
        y[static_cast<size_t>(n)] = filter.processSample(x);
        if (! std::isfinite(y[static_cast<size_t>(n)]))
            ++report.invalid;
        report.peak = std::max(report.peak, std::abs(y[static_cast<size_t>(n)]));
    }

    const int start = std::max(settle, jumpAt - 256);
    for (int n = start + 1; n < jumpAt; ++n)
        report.baselineMaxDy = std::max(report.baselineMaxDy,
            std::abs(y[static_cast<size_t>(n)] - y[static_cast<size_t>(n - 1)]));

    report.boundaryDy = std::abs(y[static_cast<size_t>(jumpAt)] - y[static_cast<size_t>(jumpAt - 1)]);
    report.nextDy = std::abs(y[static_cast<size_t>(jumpAt + 1)] - y[static_cast<size_t>(jumpAt)]);
    const int followEnd = std::min(static_cast<int>(y.size()), jumpAt + 256);
    for (int n = jumpAt + 2; n < followEnd; ++n)
        report.followMaxDy = std::max(report.followMaxDy,
            std::abs(y[static_cast<size_t>(n)] - y[static_cast<size_t>(n - 1)]));

    const float neighbor = std::max(report.baselineMaxDy, report.followMaxDy);
    report.isolatedClick = report.boundaryDy > 3.0f * std::max(neighbor, 1.0e-6f)
        && report.nextDy < 0.45f * report.boundaryDy
        && report.invalid == 0;
    return report;
}

void printReport(const JumpReport& r)
{
    std::printf("%-32s baseDy=%.5f boundDy=%.5f nextDy=%.5f followDy=%.5f peak=%.4f invalid=%d click=%s\n",
                r.name,
                static_cast<double>(r.baselineMaxDy),
                static_cast<double>(r.boundaryDy),
                static_cast<double>(r.nextDy),
                static_cast<double>(r.followMaxDy),
                static_cast<double>(r.peak),
                r.invalid,
                r.isolatedClick ? "YES" : "no");
}

}  // namespace

int main()
{
    const int settle = 2048;
    const int jumpAt = 4096;
    std::vector<JumpReport> reports;

    const struct CutCase { const char* name; float a; float b; float res; } cuts[] = {
        { "sine c 20->20k r0", 20.0f, 20000.0f, 0.0f },
        { "sine c 20k->20 r0", 20000.0f, 20.0f, 0.0f },
        { "sine c 100->1k r0", 100.0f, 1000.0f, 0.0f },
        { "sine c 1k->8k r0", 1000.0f, 8000.0f, 0.0f },
        { "sine c 8k->500 r0", 8000.0f, 500.0f, 0.0f },
        { "sine c 20->20k r0.5", 20.0f, 20000.0f, 0.50f },
        { "sine c 20->20k r0.75", 20.0f, 20000.0f, 0.75f },
        { "sine c 20->20k r0.92", 20.0f, 20000.0f, 0.92f },
        { "sine c 20k->20 r0.92", 20000.0f, 20.0f, 0.92f },
        { "saw c 20->20k r0", 20.0f, 20000.0f, 0.0f },
        { "saw c 20k->20 r0", 20000.0f, 20.0f, 0.0f },
        { "saw c 100->1k r0.5", 100.0f, 1000.0f, 0.50f },
        { "saw c 1k->8k r0.75", 1000.0f, 8000.0f, 0.75f },
        { "saw c 8k->500 r0.92", 8000.0f, 500.0f, 0.92f },
        { "saw c 20->20k r0.95", 20.0f, 20000.0f, 0.95f },
    };
    for (const auto& c : cuts)
        reports.push_back(measureJump(c.name, std::string(c.name).rfind("saw", 0) == 0,
                                      c.a, c.b, c.res, c.res, settle, jumpAt));

    const struct ResCase { const char* name; float cutoff; float a; float b; } res[] = {
        { "sine r 0->0.25 c200", 200.0f, 0.00f, 0.25f },
        { "sine r 0.25->0.5 c1k", 1000.0f, 0.25f, 0.50f },
        { "sine r 0.5->0.75 c1k", 1000.0f, 0.50f, 0.75f },
        { "sine r 0.75->0.95 c1k", 1000.0f, 0.75f, 0.95f },
        { "sine r 0.95->0 c1k", 1000.0f, 0.95f, 0.00f },
        { "sine r 0->0.95 c200", 200.0f, 0.00f, 0.95f },
        { "sine r 0.95->0 c200", 200.0f, 0.95f, 0.00f },
        { "sine r 0->0.95 c8k", 8000.0f, 0.00f, 0.95f },
        { "sine r 0.95->0 c8k", 8000.0f, 0.95f, 0.00f },
        { "saw r 0->0.95 c1k", 1000.0f, 0.00f, 0.95f },
        { "saw r 0.95->0 c1k", 1000.0f, 0.95f, 0.00f },
        { "saw r 0->0.95 c200", 200.0f, 0.00f, 0.95f },
        { "saw r 0->0.95 c8k", 8000.0f, 0.00f, 0.95f },
    };
    for (const auto& c : res)
        reports.push_back(measureJump(c.name, std::string(c.name).rfind("saw", 0) == 0,
                                      c.cutoff, c.cutoff, c.a, c.b, settle, jumpAt));

    // Combined jumps
    reports.push_back(measureJump("saw both low->high", true, 200.0f, 8000.0f, 0.1f, 0.9f, settle, jumpAt));
    reports.push_back(measureJump("saw both high->low", true, 8000.0f, 200.0f, 0.9f, 0.1f, settle, jumpAt));

    auto runSteppedSweep = [](const char* name, bool cutoffNotRes, int hostBlock, double hostHz) {
        const double internalPerHost = kFs / hostHz;
        const int updateEvery = std::max(1, static_cast<int>(std::lround(static_cast<double>(hostBlock) * internalPerHost)));
        const int duration = static_cast<int>(kFs * 0.5); // 500 ms
        swaraxt::SwaraXtFilter filter;
        filter.prepare(kFs);
        swaraxt::SwaraXtFilterParams params;
        params.cutoffHz = cutoffNotRes ? 100.0f : 1000.0f;
        params.resonance = cutoffNotRes ? 0.50f : 0.00f;
        filter.setParams(params);
        float prev = 0.0f;
        float maxDy = 0.0f;
        float maxBoundaryDy = 0.0f;
        float maxInteriorDy = 0.0f;
        int invalid = 0;
        for (int n = 0; n < duration; ++n)
        {
            if (n % updateEvery == 0)
            {
                const float t = static_cast<float>(n) / static_cast<float>(duration - 1);
                if (cutoffNotRes)
                    params.cutoffHz = 100.0f * std::pow(80.0f, t); // 100 Hz -> 8 kHz
                else
                    params.resonance = t * 0.95f;
                filter.setParams(params);
            }
            const float y = filter.processSample(saw(n, 220.0));
            if (! std::isfinite(y))
                ++invalid;
            if (n > 0)
            {
                const float dy = std::abs(y - prev);
                maxDy = std::max(maxDy, dy);
                if (n % updateEvery == 0)
                    maxBoundaryDy = std::max(maxBoundaryDy, dy);
                else
                    maxInteriorDy = std::max(maxInteriorDy, dy);
            }
            prev = y;
        }
        const float ratio = maxBoundaryDy / std::max(maxInteriorDy, 1.0e-6f);
        std::printf("%-40s host=%d @%.1fkHz step=%d maxDy=%.5f bound=%.5f interior=%.5f ratio=%.2f invalid=%d\n",
                    name,
                    hostBlock,
                    hostHz / 1000.0,
                    updateEvery,
                    static_cast<double>(maxDy),
                    static_cast<double>(maxBoundaryDy),
                    static_cast<double>(maxInteriorDy),
                    static_cast<double>(ratio),
                    invalid);
        return ratio;
    };

    std::printf("\n=== HOST-BLOCK STEPPED SWEEPS (kernel, saw 220 Hz) ===\n");
    const int blocks[] = { 64, 256, 1024 };
    const double rates[] = { 44100.0, 48000.0, 96000.0 };
    float cutoffRatioMin = 1.0e9f;
    float cutoffRatioMax = 0.0f;
    float resRatioMin = 1.0e9f;
    float resRatioMax = 0.0f;
    for (double rate : rates)
    {
        for (int block : blocks)
        {
            char name[64];
            std::snprintf(name, sizeof(name), "cutoff 100->8k r0.50");
            const float cr = runSteppedSweep(name, true, block, rate);
            cutoffRatioMin = std::min(cutoffRatioMin, cr);
            cutoffRatioMax = std::max(cutoffRatioMax, cr);
            std::snprintf(name, sizeof(name), "res 0->0.95 c1k");
            const float rr = runSteppedSweep(name, false, block, rate);
            resRatioMin = std::min(resRatioMin, rr);
            resRatioMax = std::max(resRatioMax, rr);
        }
    }
    std::printf("cutoff boundary/interior ratio range: %.2f .. %.2f\n",
                static_cast<double>(cutoffRatioMin), static_cast<double>(cutoffRatioMax));
    std::printf("resonance boundary/interior ratio range: %.2f .. %.2f\n",
                static_cast<double>(resRatioMin), static_cast<double>(resRatioMax));

    // Rapid native-rate sweep (40-sample Shruthi control block)
    {
        swaraxt::SwaraXtFilter filter;
        filter.prepare(kFs);
        swaraxt::SwaraXtFilterParams params;
        params.cutoffHz = 1000.0f;
        params.resonance = 0.6f;
        filter.setParams(params);
        float prev = 0.0f;
        float maxDy = 0.0f;
        int invalid = 0;
        for (int n = 0; n < 64 * 40; ++n)
        {
            if (n % 40 == 0)
            {
                const float t = static_cast<float>(n / 40) / 63.0f;
                params.cutoffHz = 1000.0f * std::pow(8.0f, t);
                filter.setParams(params);
            }
            const float y = filter.processSample(saw(n, 220.0));
            if (! std::isfinite(y))
                ++invalid;
            if (n > 0)
                maxDy = std::max(maxDy, std::abs(y - prev));
            prev = y;
        }
        std::printf("%-32s sweep max|dy|=%.5f invalid=%d\n",
                    "saw cutoff sweep 1k->8k r0.6", static_cast<double>(maxDy), invalid);
    }

    int clicks = 0;
    int finiteFails = 0;
    std::printf("\n=== FILTER KERNEL TRANSITION AUDIT ===\n");
    for (const auto& r : reports)
    {
        printReport(r);
        if (r.isolatedClick)
            ++clicks;
        if (r.invalid != 0)
            ++finiteFails;
    }
    std::printf("isolated-click cases: %d / %d\n", clicks, static_cast<int>(reports.size()));
    std::printf("non-finite cases: %d\n", finiteFails);
    return (finiteFails == 0) ? 0 : 1;
}
