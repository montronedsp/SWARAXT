// Copyright 2026 MontroneDSP.
// SPDX-License-Identifier: GPL-3.0-or-later

#include "Engine/Filter/SwaraXtFilter.h"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <numeric>
#include <string>
#include <vector>

namespace {

using Clock = std::chrono::steady_clock;

struct Scene
{
    const char* name = "";
    double cutoffHz = 1000.0;
    double resonance = 0.0;
    bool modulate = false;
    double envHz = 0.0;
};

struct Stats
{
    double medianNsPerSample = 0.0;
    double maxAbs = 0.0;
    double rms = 0.0;
    double dc = 0.0;
    int nanCount = 0;
};

double median(std::vector<double> values)
{
    if (values.empty())
        return 0.0;
    std::sort(values.begin(), values.end());
    const size_t n = values.size();
    if ((n % 2) == 1)
        return values[n / 2];
    return 0.5 * (values[n / 2 - 1] + values[n / 2]);
}

std::vector<float> makeInput(int samples, double sampleRate, double freqHz, double amplitude)
{
    std::vector<float> input(static_cast<size_t>(samples));
    const double w = 2.0 * swaraxt::kPi * freqHz / sampleRate;
    for (int i = 0; i < samples; ++i)
        input[static_cast<size_t>(i)] = static_cast<float>(amplitude * std::sin(w * static_cast<double>(i)));
    return input;
}

std::vector<float> makeSaw(int samples, double sampleRate, double freqHz, double amplitude)
{
    std::vector<float> input(static_cast<size_t>(samples));
    const double phaseInc = freqHz / sampleRate;
    double phase = 0.0;
    for (int i = 0; i < samples; ++i)
    {
        input[static_cast<size_t>(i)] = static_cast<float>(amplitude * (2.0 * phase - 1.0));
        phase += phaseInc;
        if (phase >= 1.0)
            phase -= 1.0;
    }
    return input;
}

Stats render(swaraxt::SwaraXtFilter& filter,
             const std::vector<float>& input,
             const Scene& scene,
             double sampleRate,
             int repeats,
             std::vector<float>* captured)
{
    const int n = static_cast<int>(input.size());
    std::vector<double> nsPerSample;
    nsPerSample.reserve(static_cast<size_t>(repeats));
    std::vector<float> last(static_cast<size_t>(n));

    for (int rep = 0; rep < repeats; ++rep)
    {
        filter.reset();
        swaraxt::SwaraXtFilterParams params;
        params.cutoffHz = static_cast<float>(scene.cutoffHz);
        params.resonance = static_cast<float>(scene.resonance);
        params.envAmount = scene.modulate ? 4.0f : 0.0f;
        params.envValue = 0.0f;
        filter.setParams(params);

        const auto start = Clock::now();
        for (int i = 0; i < n; ++i)
        {
            if (scene.modulate)
            {
                const double t = static_cast<double>(i) / sampleRate;
                params.envValue = static_cast<float>(0.5 + 0.5 * std::sin(2.0 * swaraxt::kPi * scene.envHz * t));
                filter.setParams(params);
            }
            last[static_cast<size_t>(i)] = filter.processSample(input[static_cast<size_t>(i)]);
        }
        const auto elapsed = Clock::now() - start;
        const double ns = static_cast<double>(
            std::chrono::duration_cast<std::chrono::nanoseconds>(elapsed).count());
        nsPerSample.push_back(ns / static_cast<double>(n));
    }

    Stats s;
    s.medianNsPerSample = median(std::move(nsPerSample));
    double sum = 0.0;
    double sumSq = 0.0;
    for (float y : last)
    {
        if (! std::isfinite(y))
        {
            ++s.nanCount;
            continue;
        }
        const double v = static_cast<double>(y);
        s.maxAbs = std::max(s.maxAbs, std::abs(v));
        sum += v;
        sumSq += v * v;
    }
    const double count = static_cast<double>(last.size());
    s.dc = sum / count;
    s.rms = std::sqrt(sumSq / count);
    if (captured != nullptr)
        *captured = std::move(last);
    return s;
}

void compareToReference(const std::vector<float>& reference, const std::vector<float>& candidate,
                        double& maxErr, double& rmsErr)
{
    maxErr = 0.0;
    double sumSq = 0.0;
    const size_t n = std::min(reference.size(), candidate.size());
    for (size_t i = 0; i < n; ++i)
    {
        const double e = static_cast<double>(candidate[i]) - static_cast<double>(reference[i]);
        maxErr = std::max(maxErr, std::abs(e));
        sumSq += e * e;
    }
    rmsErr = n > 0 ? std::sqrt(sumSq / static_cast<double>(n)) : 0.0;
}

}  // namespace

int main()
{
    constexpr double kInternal = 20000000.0 / 510.0;
    constexpr int kSamples = 16384;
    constexpr int kRepeats = 7;

    const Scene scenes[] = {
        { "low_res", 1200.0, 0.00, false, 0.0 },
        { "high_res", 1200.0, 0.85, false, 0.0 },
        { "worst_cutoff", 8000.0, 0.90, false, 0.0 },
        { "env_sweep", 800.0, 0.55, true, 18.0 },
        { "lfo_mod", 1500.0, 0.40, true, 6.0 },
        { "self_osc", 440.0, 1.00, false, 0.0 },
    };

    const auto saw = makeSaw(kSamples, kInternal, 220.0, 0.35);
    const auto sine = makeInput(kSamples, kInternal, 440.0, 1.0e-4);

    std::printf("SWARA XT filter CPU bench  fs=%.3f  samples=%d  repeats=%d\n",
                kInternal, kSamples, kRepeats);
    std::printf("candidate,scene,os,iters,ns_per_sample,max_abs,rms,dc,nan,max_err,rms_err\n");

    struct Candidate
    {
        const char* name;
        int oversample;
        int iterations;
    };
    const Candidate candidates[] = {
        { "HIGH", 4, 8 },
        { "os2_i8", 2, 8 },
        { "os1_i8", 1, 8 },
        { "os4_i4", 4, 4 },
        { "os2_i4", 2, 4 },
        { "os1_i4", 1, 4 },
    };

    for (const auto& scene : scenes)
    {
        const auto& input = (scene.resonance >= 0.99) ? sine : saw;
        swaraxt::SwaraXtFilter reference;
        reference.prepare(kInternal);
        reference.setOversampleFactorForTests(4);
        reference.setSolverIterationLimitForTests(8);
        std::vector<float> refOut;
        const Stats refStats = render(reference, input, scene, kInternal, kRepeats, &refOut);

        std::printf("%s,%s,%d,%d,%.3f,%.6f,%.6f,%.6e,%d,0,0\n",
                    "HIGH", scene.name, 4, 8,
                    refStats.medianNsPerSample, refStats.maxAbs, refStats.rms, refStats.dc,
                    refStats.nanCount);

        for (const auto& candidate : candidates)
        {
            if (candidate.oversample == 4 && candidate.iterations == 8)
                continue;

            swaraxt::SwaraXtFilter filter;
            filter.prepare(kInternal);
            filter.setOversampleFactorForTests(candidate.oversample);
            filter.setSolverIterationLimitForTests(candidate.iterations);
            std::vector<float> out;
            const Stats stats = render(filter, input, scene, kInternal, kRepeats, &out);
            double maxErr = 0.0;
            double rmsErr = 0.0;
            compareToReference(refOut, out, maxErr, rmsErr);
            std::printf("%s,%s,%d,%d,%.3f,%.6f,%.6f,%.6e,%d,%.6f,%.6f\n",
                        candidate.name, scene.name, candidate.oversample, candidate.iterations,
                        stats.medianNsPerSample, stats.maxAbs, stats.rms, stats.dc,
                        stats.nanCount, maxErr, rmsErr);
        }
    }

    return 0;
}
