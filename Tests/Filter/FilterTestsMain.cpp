// Copyright 2026 MontroneDSP.
// SPDX-License-Identifier: GPL-3.0-or-later

#include "Engine/Filter/SwaraXtFilter.h"
#include "Engine/Filter/Circuit/ActiveLoadPole.h"
#include "Engine/Filter/Circuit/PassiveLoadPole.h"
#include "Engine/Filter/Control/CutoffMapper.h"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <limits>
#include <numeric>
#include <string>
#include <vector>

#ifndef SWARAXT_FILTER_TEST_MODE
#define SWARAXT_FILTER_TEST_MODE 0
#endif

namespace {

namespace fs = std::filesystem;

int gFailures = 0;

struct Metrics
{
    double min = 0.0;
    double max = 0.0;
    double peak = 0.0;
    double rms = 0.0;
    double mean = 0.0;
    int invalid = 0;
};

void expect(bool cond, const char* name)
{
    if (! cond)
    {
        std::printf("FAIL: %s\n", name);
        ++gFailures;
    }
    else
    {
        std::printf("PASS: %s\n", name);
    }
}

double db(double x)
{
    return swaraxt::linearToDb(std::max(std::abs(x), 1.0e-15));
}

Metrics measure(const std::vector<float>& samples, size_t start = 0)
{
    Metrics m;
    if (samples.empty() || start >= samples.size())
        return m;

    m.min = std::numeric_limits<double>::max();
    m.max = std::numeric_limits<double>::lowest();
    double sum = 0.0;
    double sumSq = 0.0;
    int count = 0;
    for (size_t i = start; i < samples.size(); ++i)
    {
        double s = samples[i];
        if (! std::isfinite(s))
        {
            s = 0.0;
            ++m.invalid;
        }
        m.min = std::min(m.min, s);
        m.max = std::max(m.max, s);
        m.peak = std::max(m.peak, std::abs(s));
        sum += s;
        sumSq += s * s;
        ++count;
    }

    m.mean = sum / static_cast<double>(count);
    m.rms = std::sqrt(sumSq / static_cast<double>(count));
    return m;
}

fs::path artifactRoot()
{
    return fs::current_path() / "artifacts" / "filter-debug";
}

void writeLe16(std::ofstream& out, uint16_t v)
{
    out.put(static_cast<char>(v & 0xff));
    out.put(static_cast<char>((v >> 8) & 0xff));
}

void writeLe32(std::ofstream& out, uint32_t v)
{
    writeLe16(out, static_cast<uint16_t>(v & 0xffff));
    writeLe16(out, static_cast<uint16_t>((v >> 16) & 0xffff));
}

void writeWav(const fs::path& path, const std::vector<float>& samples, int sampleRate)
{
    fs::create_directories(path.parent_path());
    std::ofstream out(path, std::ios::binary);
    const uint16_t channels = 1;
    const uint16_t bits = 16;
    const uint32_t dataBytes = static_cast<uint32_t>(samples.size() * sizeof(int16_t));
    out.write("RIFF", 4);
    writeLe32(out, 36u + dataBytes);
    out.write("WAVEfmt ", 8);
    writeLe32(out, 16);
    writeLe16(out, 1);
    writeLe16(out, channels);
    writeLe32(out, static_cast<uint32_t>(sampleRate));
    writeLe32(out, static_cast<uint32_t>(sampleRate * channels * bits / 8));
    writeLe16(out, static_cast<uint16_t>(channels * bits / 8));
    writeLe16(out, bits);
    out.write("data", 4);
    writeLe32(out, dataBytes);
    for (float s : samples)
    {
        const float finite = std::isfinite(s) ? s : 0.0f;
        const auto q = static_cast<int16_t>(std::lround(std::clamp(finite, -1.0f, 1.0f) * 32767.0f));
        writeLe16(out, static_cast<uint16_t>(q));
    }
}

std::vector<float> renderFilter(double sampleRate,
                                swaraxt::SwaraXtFilterParams params,
                                const std::vector<float>& input)
{
    swaraxt::SwaraXtFilter filter;
    filter.prepare(sampleRate);
    filter.setParams(params);
    std::vector<float> out;
    out.reserve(input.size());
    for (float x : input)
        out.push_back(filter.processSample(x));
    return out;
}

std::vector<float> sine(double sampleRate, double hz, double amp, int samples)
{
    std::vector<float> data;
    data.reserve(static_cast<size_t>(samples));
    for (int i = 0; i < samples; ++i)
        data.push_back(static_cast<float>(amp * std::sin(2.0 * swaraxt::kPi * hz * static_cast<double>(i) / sampleRate)));
    return data;
}

std::vector<float> saw(double sampleRate, double hz, double amp, int samples)
{
    std::vector<float> data;
    data.reserve(static_cast<size_t>(samples));
    double phase = 0.0;
    const double inc = hz / sampleRate;
    for (int i = 0; i < samples; ++i)
    {
        data.push_back(static_cast<float>((phase * 2.0 - 1.0) * amp));
        phase += inc;
        if (phase >= 1.0)
            phase -= 1.0;
    }
    return data;
}

std::vector<float> square(double sampleRate, double hz, double amp, int samples)
{
    std::vector<float> data;
    data.reserve(static_cast<size_t>(samples));
    double phase = 0.0;
    const double inc = hz / sampleRate;
    for (int i = 0; i < samples; ++i)
    {
        data.push_back(phase < 0.5 ? static_cast<float>(amp) : static_cast<float>(-amp));
        phase += inc;
        if (phase >= 1.0)
            phase -= 1.0;
    }
    return data;
}

double amplitudeAt(const std::vector<float>& samples, double sampleRate, double hz, size_t start)
{
    double re = 0.0;
    double im = 0.0;
    int count = 0;
    for (size_t i = start; i < samples.size(); ++i)
    {
        const double phase = 2.0 * swaraxt::kPi * hz * static_cast<double>(i) / sampleRate;
        re += static_cast<double>(samples[i]) * std::cos(phase);
        im += static_cast<double>(samples[i]) * std::sin(phase);
        ++count;
    }
    if (count == 0)
        return 0.0;
    return 2.0 * std::sqrt(re * re + im * im) / static_cast<double>(count);
}

void testExponentialConverter()
{
    swaraxt::CutoffMapper map;
    const double f0 = map.stageCutoffFromControlVoltage(0.0);
    expect(std::fabs(f0 - swaraxt::kAs3109ReferenceCutoffHz) < 1.0, "VC=0 stage cutoff AS3109 prior");
    expect(std::fabs(swaraxt::CutoffMapper::fourPoleMinus3DbRatio() - 0.43497944117144233) < 1.0e-12,
           "four-pole no-resonance -3 dB ratio documented");
}

void testKeyTrackCalibration()
{
    expect(std::fabs(swaraxt::CutoffMapper::keyTrackRatio(1.0) - 2.0) < 1.0e-9, "KT 100% F5/F4 = 2");
    expect(std::fabs(swaraxt::CutoffMapper::keyTrackRatio(0.5) - std::sqrt(2.0)) < 1.0e-9, "KT 50% = sqrt(2)");
    expect(std::fabs(swaraxt::CutoffMapper::keyTrackRatio(0.0) - 1.0) < 1.0e-9, "KT 0% = 1");

    swaraxt::CutoffMapper map;
    const double base = 1000.0;
    const double f4 = map.mapCutoffHz(base, 1.0, 65.0, 0.0);
    const double f5 = map.mapCutoffHz(base, 1.0, 77.0, 0.0);
    expect(std::fabs(f5 / f4 - 2.0) < 1.0e-6, "factory 100% key track at 1 kHz");
}

void testStageTopology()
{
    expect(swaraxt::kPassiveStage1Traits.loadTopology == swaraxt::LoadTopology::passiveResistor,
           "stage 1 passive-load topology retained");
    expect(swaraxt::kActiveStage2Traits.loadTopology == swaraxt::LoadTopology::activeCurrentSource,
           "stage 2 active-load topology retained");
    expect(swaraxt::kPassiveStage3Traits.nominalOffsetVolts == 0.0,
           "stage offsets not injected into audio-domain state");
    expect(swaraxt::kActiveStage4Traits.saturationSoftness != swaraxt::kPassiveStage1Traits.saturationSoftness,
           "passive/active large-signal softness differs");
}

enum Node
{
    kFilterInput,
    kStage1State,
    kStage1Output,
    kStage2State,
    kStage2Output,
    kStage3State,
    kStage3Output,
    kStage4State,
    kStage4Output,
    kResonanceFeedback,
    kPhaseInverted,
    kPhaseNonInverted,
    kDiodeOutput,
    kPostResonanceMix,
    kRawOutput,
    kRecenteredOutput,
    kPostVcaInput,
    kFinalPluginOutput,
    kNodeCount
};

const char* nodeName(Node node)
{
    switch (node)
    {
        case kFilterInput: return "filter_input";
        case kStage1State: return "stage_1_state";
        case kStage1Output: return "stage_1_output";
        case kStage2State: return "stage_2_state";
        case kStage2Output: return "stage_2_output";
        case kStage3State: return "stage_3_state";
        case kStage3Output: return "stage_3_output";
        case kStage4State: return "stage_4_state";
        case kStage4Output: return "stage_4_output";
        case kResonanceFeedback: return "resonance_feedback_signal";
        case kPhaseInverted: return "phase_split_inverted_path";
        case kPhaseNonInverted: return "phase_split_non_inverted_path";
        case kDiodeOutput: return "diode_limiter_output";
        case kPostResonanceMix: return "post_resonance_mix";
        case kRawOutput: return "raw_filter_output_before_recentering";
        case kRecenteredOutput: return "recentered_filter_output";
        case kPostVcaInput: return "post_filter_vca_input";
        case kFinalPluginOutput: return "final_plugin_output";
        default: return "unknown";
    }
}

double traceValue(const swaraxt::SwaraXtFilter::Trace& t, Node node)
{
    switch (node)
    {
        case kFilterInput: return t.filterInput;
        case kStage1State: return t.stageState[0];
        case kStage1Output: return t.stageOutput[0];
        case kStage2State: return t.stageState[1];
        case kStage2Output: return t.stageOutput[1];
        case kStage3State: return t.stageState[2];
        case kStage3Output: return t.stageOutput[2];
        case kStage4State: return t.stageState[3];
        case kStage4Output: return t.stageOutput[3];
        case kResonanceFeedback: return t.resonanceFeedbackSignal;
        case kPhaseInverted: return t.phaseSplitInvertedPath;
        case kPhaseNonInverted: return t.phaseSplitNonInvertedPath;
        case kDiodeOutput: return t.diodeLimiterOutput;
        case kPostResonanceMix: return t.postResonanceMix;
        case kRawOutput: return t.rawFilterOutputBeforeRecentering;
        case kRecenteredOutput: return t.recenteredFilterOutput;
        case kPostVcaInput: return t.postFilterVcaInput;
        case kFinalPluginOutput: return t.finalPluginOutput;
        default: return 0.0;
    }
}

Metrics measureDoubles(const std::vector<double>& samples, size_t start)
{
    std::vector<float> asFloat;
    asFloat.reserve(samples.size());
    for (double x : samples)
        asFloat.push_back(static_cast<float>(x));
    return measure(asFloat, start);
}

int settlingSample(const std::vector<double>& samples, size_t start)
{
    if (samples.empty() || start >= samples.size())
        return 0;
    const Metrics tail = measureDoubles(samples, start);
    const double tolerance = std::max(1.0e-7, tail.rms * 0.001);
    for (size_t i = start; i < samples.size(); ++i)
    {
        if (std::abs(samples[i] - tail.mean) <= tolerance)
            return static_cast<int>(i);
    }
    return static_cast<int>(samples.size());
}

void writeDiagnostics()
{
    fs::create_directories(artifactRoot());
    std::ofstream csv(artifactRoot() / "filter_node_metrics.csv", std::ios::trunc);
    csv << "scenario,resonance,node,mean,rms,peak,min,max,dc_dbfs,settling_sample,drift\n";

    constexpr double sampleRate = 44100.0;
    constexpr int samples = 16384;
    constexpr int settle = 4096;
    const double resonances[] = { 0.0, 0.25, 0.50, 0.75, 0.95, 1.0 };

    struct Scenario
    {
        const char* name;
        std::vector<float> input;
    };

    std::vector<float> zero(static_cast<size_t>(samples), 0.0f);
    std::vector<float> impulse(static_cast<size_t>(samples), 0.0f);
    impulse[0] = 1.0f;
    auto lowSine = sine(sampleRate, 80.0, 0.05, samples);
    auto sine1k = sine(sampleRate, 1000.0, 0.25, samples);
    auto sawWave = saw(sampleRate, 110.0, 0.35, samples);
    auto squareWave = square(sampleRate, 110.0, 0.35, samples);
    auto nominal = saw(sampleRate, 110.0, 0.28, samples);
    auto nominalSquare = square(sampleRate, 220.0, 0.16, samples);
    auto large = saw(sampleRate, 110.0, 1.5, samples);
    for (size_t i = 0; i < nominal.size(); ++i)
        nominal[i] += nominalSquare[i];

    const Scenario scenarios[] = {
        { "zero_input", zero },
        { "impulse", impulse },
        { "low_level_sine", lowSine },
        { "1khz_sine", sine1k },
        { "saw", sawWave },
        { "square", squareWave },
        { "nominal_swaraxt_oscillator_input", nominal },
        { "large_input", large },
    };

    for (const Scenario& scenario : scenarios)
    {
        for (double resonance : resonances)
        {
            swaraxt::SwaraXtFilter filter;
            filter.prepare(sampleRate);
            swaraxt::SwaraXtFilterParams params;
            params.cutoffHz = 1000.0f;
            params.resonance = static_cast<float>(resonance);
            filter.setParams(params);

            std::array<std::vector<double>, kNodeCount> nodes;
            for (auto& node : nodes)
                node.reserve(scenario.input.size());

            for (float x : scenario.input)
            {
                filter.processSample(x);
                const auto& trace = filter.lastTrace();
                for (int node = 0; node < kNodeCount; ++node)
                    nodes[static_cast<size_t>(node)].push_back(traceValue(trace, static_cast<Node>(node)));
            }

            for (int node = 0; node < kNodeCount; ++node)
            {
                const auto& values = nodes[static_cast<size_t>(node)];
                const Metrics m = measureDoubles(values, settle);
                const double firstHalf = measureDoubles(values, settle).mean;
                const double secondHalf = measureDoubles(values, values.size() * 3 / 4).mean;
                csv << scenario.name << ','
                    << resonance << ','
                    << nodeName(static_cast<Node>(node)) << ','
                    << m.mean << ','
                    << m.rms << ','
                    << m.peak << ','
                    << m.min << ','
                    << m.max << ','
                    << db(m.mean) << ','
                    << settlingSample(values, settle) << ','
                    << (secondHalf - firstHalf) << '\n';
            }
        }
    }
}

void testZeroInputDc()
{
    const double rates[] = { 44100.0, 48000.0, 88200.0, 96000.0, 176400.0, 192000.0 };
    const float resonances[] = { 0.0f, 0.25f, 0.50f, 0.75f, 0.95f };
    for (double sampleRate : rates)
    {
        for (float resonance : resonances)
        {
            swaraxt::SwaraXtFilter filter;
            filter.prepare(sampleRate);
            swaraxt::SwaraXtFilterParams params;
            params.cutoffHz = 1000.0f;
            params.resonance = resonance;
            filter.setParams(params);
            std::vector<float> out;
            out.reserve(static_cast<size_t>(sampleRate));
            for (int i = 0; i < static_cast<int>(sampleRate); ++i)
                out.push_back(filter.processSample(0.0f));
            const Metrics m = measure(out, out.size() / 2);
            expect(m.invalid == 0, "zero-input output finite");
            expect(std::abs(m.mean) < 1.0e-8, "zero-input mean below -160 dBFS");
            expect(m.peak < 1.0e-7, "zero-input peak remains silent below self-oscillation");
        }
    }
}

void testLinearLowPassResponse()
{
    constexpr double sampleRate = 44100.0;
    constexpr int samples = 32768;
    constexpr size_t settle = 8192;
    swaraxt::SwaraXtFilterParams params;
    params.cutoffHz = 1000.0f;
    params.resonance = 0.0f;

    auto gainAt = [&](double hz) {
        const auto input = sine(sampleRate, hz, 0.001, samples);
        const auto output = renderFilter(sampleRate, params, input);
        return amplitudeAt(output, sampleRate, hz, settle) / 0.001;
    };

    const double g100 = gainAt(100.0);
    const double g1k = gainAt(1000.0);
    const double g2k = gainAt(2000.0);
    const double g4k = gainAt(4000.0);
    const double g8k = gainAt(8000.0);

    expect(g100 > 0.90, "low frequencies pass near unity");
    expect(g100 > g1k && g1k > g2k && g2k > g4k && g4k > g8k,
           "four-pole response is monotonic");
    expect(db(g4k / g2k) < -13.0, "high-frequency slope approaches 24 dB/octave");
    expect(g8k < 0.01, "far-above-cutoff rejection is strong");
}

void testCutoffAndKeyTracking()
{
    constexpr double sampleRate = 44100.0;
    constexpr int samples = 32768;
    constexpr size_t settle = 8192;
    const double cutoffs[] = { 500.0, 1000.0, 2000.0 };
    std::vector<double> atPanel;

    for (double cutoff : cutoffs)
    {
        swaraxt::SwaraXtFilterParams params;
        params.cutoffHz = static_cast<float>(cutoff);
        params.resonance = 0.0f;
        const auto input = sine(sampleRate, cutoff, 0.001, samples);
        const auto output = renderFilter(sampleRate, params, input);
        atPanel.push_back(amplitudeAt(output, sampleRate, cutoff, settle) / 0.001);
    }

    const double minGain = *std::min_element(atPanel.begin(), atPanel.end());
    const double maxGain = *std::max_element(atPanel.begin(), atPanel.end());
    expect(maxGain / minGain < 1.35, "panel cutoff mapping is consistent across octaves");

    swaraxt::CutoffMapper mapper;
    const double c4 = mapper.mapCutoffHz(1000.0, 1.0, 60.0, 0.0);
    const double c5 = mapper.mapCutoffHz(1000.0, 1.0, 72.0, 0.0);
    expect(std::fabs(c5 / c4 - 2.0) < 1.0e-6, "100% key tracking remains one octave per octave");
}

void testResonanceProgression()
{
    constexpr double sampleRate = 44100.0;
    constexpr int samples = 32768;
    constexpr size_t settle = 8192;
    const float resonances[] = { 0.0f, 0.25f, 0.50f, 0.75f, 0.90f };
    std::vector<double> gains;

    for (float resonance : resonances)
    {
        swaraxt::SwaraXtFilterParams params;
        params.cutoffHz = 1000.0f;
        params.resonance = resonance;
        const auto input = sine(sampleRate, 1000.0, 0.001, samples);
        const auto output = renderFilter(sampleRate, params, input);
        const Metrics m = measure(output, settle);
        gains.push_back(amplitudeAt(output, sampleRate, 1000.0, settle) / 0.001);
        expect(m.invalid == 0 && m.peak < 4.0, "resonant sine remains finite and bounded");
        expect(std::abs(m.mean) < 1.0e-4, "resonant sine remains centered");
    }

    bool mostlyMonotonic = true;
    for (size_t i = 1; i < gains.size(); ++i)
        mostlyMonotonic = mostlyMonotonic && gains[i] >= gains[i - 1] * 0.95;
    expect(mostlyMonotonic, "resonance peak progresses musically");
    expect(gains.back() > gains.front() * 1.8, "high resonance provides useful emphasis");
}

void testSelfOscillation()
{
    constexpr double sampleRate = 44100.0;
    constexpr int samples = 96000;
    swaraxt::SwaraXtFilter filter;
    filter.prepare(sampleRate);
    swaraxt::SwaraXtFilterParams params;
    params.cutoffHz = 880.0f;
    params.resonance = 1.0f;
    filter.setParams(params);

    std::vector<float> out;
    out.reserve(static_cast<size_t>(samples));
    bool finite = true;
    for (int i = 0; i < samples; ++i)
    {
        const float seed = i < 16 ? 1.0e-3f : 0.0f;
        const float y = filter.processSample(seed);
        finite = finite && std::isfinite(y);
        out.push_back(y);
    }

    const Metrics m = measure(out, out.size() * 3 / 4);
    expect(finite && m.invalid == 0, "self-oscillation remains finite");
    expect(m.peak > 0.02 && m.peak < 3.0, "self-oscillation amplitude is controlled");
    expect(std::abs(m.mean) < 2.0e-3, "self-oscillation remains centered");
    expect(filter.isSelfOscillating(), "self-oscillation metric detects sustained oscillation");
}

void testModulationStability()
{
    constexpr double sampleRate = 44100.0;
    constexpr int samples = 44100;
    swaraxt::SwaraXtFilter filter;
    filter.prepare(sampleRate);

    std::vector<float> out;
    out.reserve(static_cast<size_t>(samples));
    bool finite = true;
    for (int i = 0; i < samples; ++i)
    {
        const double t = static_cast<double>(i) / sampleRate;
        swaraxt::SwaraXtFilterParams params;
        params.cutoffHz = static_cast<float>(600.0 * std::pow(2.0, 2.5 * std::sin(2.0 * swaraxt::kPi * 7.0 * t)));
        params.resonance = 0.65f;
        params.modAmount = 0.0f;
        filter.setParams(params);
        const float x = static_cast<float>(0.25 * std::sin(2.0 * swaraxt::kPi * 110.0 * t));
        const float y = filter.processSample(x);
        finite = finite && std::isfinite(y) && filter.lastTrace().solverFallbacks == 0;
        out.push_back(y);
    }

    const Metrics m = measure(out, 1024);
    expect(finite && m.invalid == 0, "fast cutoff modulation remains finite without solver fallback");
    expect(m.peak < 3.0, "fast cutoff modulation remains bounded");
    expect(std::abs(m.mean) < 0.01, "fast cutoff modulation does not create large DC");
}

void testBlockSizeIndependence()
{
    constexpr double sampleRate = 44100.0;
    constexpr int samples = 12000;
    std::vector<float> input;
    input.reserve(static_cast<size_t>(samples));
    for (int i = 0; i < samples; ++i)
    {
        const double t = static_cast<double>(i) / sampleRate;
        const double value = 0.23 * std::sin(2.0 * swaraxt::kPi * 137.0 * t)
                           + 0.11 * std::sin(2.0 * swaraxt::kPi * 1103.0 * t);
        input.push_back(static_cast<float>(value));
    }

    swaraxt::SwaraXtFilterParams params;
    params.cutoffHz = 1800.0f;
    params.resonance = 0.72f;
    const auto reference = renderFilter(sampleRate, params, input);
    const int blockSizes[] = { 1, 7, 16, 31, 32, 63, 64, 65, 128, 257, 512, 1024, 4096 };

    for (int blockSize : blockSizes)
    {
        swaraxt::SwaraXtFilter filter;
        filter.prepare(sampleRate);
        filter.setParams(params);
        std::vector<float> chunked = input;
        for (size_t offset = 0; offset < chunked.size(); offset += static_cast<size_t>(blockSize))
        {
            const int n = static_cast<int>(std::min(static_cast<size_t>(blockSize), chunked.size() - offset));
            filter.processBlock(chunked.data() + offset, n);
        }

        double maxErr = 0.0;
        for (size_t i = 0; i < reference.size(); ++i)
            maxErr = std::max(maxErr, std::abs(static_cast<double>(reference[i] - chunked[i])));
        expect(maxErr < 1.0e-7, "filter output independent of process block size");
    }
}

void testSampleRateAndOversampling()
{
    const double rates[] = { 44100.0, 48000.0, 88200.0, 96000.0, 176400.0, 192000.0 };
    std::vector<double> lowGains;
    for (double sampleRate : rates)
    {
        swaraxt::SwaraXtFilter filter;
        filter.prepare(sampleRate);
        if (sampleRate <= 48001.0)
            expect(filter.oversamplingFactor() == 4, "44.1/48 kHz use 4x oversampling");
        else if (sampleRate <= 96001.0)
            expect(filter.oversamplingFactor() == 2, "88.2/96 kHz use 2x oversampling");
        else
            expect(filter.oversamplingFactor() == 1, "176.4/192 kHz run 1x");

        swaraxt::SwaraXtFilterParams params;
        params.cutoffHz = 1000.0f;
        params.resonance = 0.0f;
        const int samples = static_cast<int>(sampleRate);
        const auto input = sine(sampleRate, 200.0, 0.001, samples);
        const auto output = renderFilter(sampleRate, params, input);
        lowGains.push_back(amplitudeAt(output, sampleRate, 200.0, static_cast<size_t>(samples / 4)) / 0.001);
    }

    const double minGain = *std::min_element(lowGains.begin(), lowGains.end());
    const double maxGain = *std::max_element(lowGains.begin(), lowGains.end());
    expect(maxGain / minGain < 1.20, "sample-rate low-frequency gain is consistent");
}

void testReferenceAgreement()
{
    constexpr double sampleRate = 44100.0;
    constexpr int samples = 32768;
    swaraxt::SwaraXtFilter production;
    swaraxt::Ir3109ReferenceModel reference;
    production.prepare(sampleRate);
    reference.prepare(sampleRate);
    swaraxt::SwaraXtFilterParams params;
    params.cutoffHz = 1400.0f;
    params.resonance = 0.55f;
    production.setParams(params);
    reference.setParams(params);

    const auto input = sine(sampleRate, 330.0, 0.05, samples);
    double errSq = 0.0;
    double refSq = 0.0;
    for (float x : input)
    {
        const double a = production.processSample(x);
        const double b = reference.process(x);
        errSq += (a - b) * (a - b);
        refSq += b * b;
    }
    const double rel = std::sqrt(errSq / std::max(refSq, 1.0e-18));
    expect(rel < 0.18, "production tracks independent high-oversampling reference");
}

void writeListeningRenders()
{
    constexpr double sampleRate = 44100.0;
    constexpr int samples = 44100 * 2;
    const fs::path root = artifactRoot() / "listening";

    struct Render
    {
        const char* file;
        float cutoff;
        float resonance;
        std::vector<float> input;
        bool sweep = false;
    };

    auto openSaw = saw(sampleRate, 110.0, 0.45, samples);
    auto squareBass = square(sampleRate, 55.0, 0.35, samples);
    auto twoOsc = saw(sampleRate, 110.0, 0.30, samples);
    auto second = square(sampleRate, 220.0, 0.18, samples);
    for (size_t i = 0; i < twoOsc.size(); ++i)
        twoOsc[i] += second[i];
    std::vector<float> silence(static_cast<size_t>(samples), 0.0f);

    const Render renders[] = {
        { "01_open_filter_saw.wav", 20000.0f, 0.0f, openSaw, false },
        { "02_half_cutoff_saw.wav", 1800.0f, 0.0f, openSaw, false },
        { "03_low_resonance.wav", 1400.0f, 0.25f, openSaw, false },
        { "04_medium_resonance.wav", 1200.0f, 0.55f, openSaw, false },
        { "05_high_resonance.wav", 1000.0f, 0.88f, openSaw, false },
        { "06_self_oscillation.wav", 880.0f, 1.0f, silence, false },
        { "07_envelope_sweep.wav", 600.0f, 0.45f, openSaw, true },
        { "08_fast_cutoff_modulation.wav", 1400.0f, 0.65f, openSaw, true },
        { "09_low_bass_patch.wav", 520.0f, 0.42f, squareBass, false },
        { "10_two_oscillator_patch.wav", 1600.0f, 0.35f, twoOsc, false },
        { "11_silence.wav", 1000.0f, 0.75f, silence, false },
    };

    for (const Render& render : renders)
    {
        swaraxt::SwaraXtFilter filter;
        filter.prepare(sampleRate);
        swaraxt::SwaraXtFilterParams params;
        params.cutoffHz = render.cutoff;
        params.resonance = render.resonance;
        filter.setParams(params);
        std::vector<float> out;
        out.reserve(render.input.size());
        for (size_t i = 0; i < render.input.size(); ++i)
        {
            if (render.sweep)
            {
                const double t = static_cast<double>(i) / sampleRate;
                params.cutoffHz = static_cast<float>(300.0 * std::pow(2.0, 4.5 * (0.5 + 0.5 * std::sin(2.0 * swaraxt::kPi * 0.35 * t))));
                if (render.file[0] == '0' && render.file[1] == '8')
                    params.cutoffHz = static_cast<float>(1200.0 * std::pow(2.0, 1.5 * std::sin(2.0 * swaraxt::kPi * 18.0 * t)));
                filter.setParams(params);
            }
            float x = render.input[i];
            if (render.file[0] == '0' && render.file[1] == '6' && i < 24)
                x = 1.0e-3f;
            out.push_back(filter.processSample(x));
        }
        writeWav(root / render.file, out, static_cast<int>(sampleRate));
    }

    expect(fs::exists(root / "01_open_filter_saw.wav"), "listening renders written");
}

void runCoreTests()
{
    testExponentialConverter();
    testKeyTrackCalibration();
    testStageTopology();
    testLinearLowPassResponse();
    testCutoffAndKeyTracking();
    testReferenceAgreement();
}

void runDcTests()
{
    writeDiagnostics();
    testZeroInputDc();
}

void runResponseTests()
{
    testLinearLowPassResponse();
    testCutoffAndKeyTracking();
}

void runResonanceTests()
{
    testResonanceProgression();
    testSelfOscillation();
}

void runModulationTests()
{
    testModulationStability();
}

void runOversamplingTests()
{
    testSampleRateAndOversampling();
    testBlockSizeIndependence();
}

void runIntegrationTests()
{
    testReferenceAgreement();
    writeListeningRenders();
}

}  // namespace

int main()
{
    std::printf("SwaraXt filter test mode %d\n", SWARAXT_FILTER_TEST_MODE);

#if SWARAXT_FILTER_TEST_MODE == 1
    runCoreTests();
#elif SWARAXT_FILTER_TEST_MODE == 2
    runDcTests();
#elif SWARAXT_FILTER_TEST_MODE == 3
    runResponseTests();
#elif SWARAXT_FILTER_TEST_MODE == 4
    runResonanceTests();
#elif SWARAXT_FILTER_TEST_MODE == 5
    runModulationTests();
#elif SWARAXT_FILTER_TEST_MODE == 6
    runOversamplingTests();
#elif SWARAXT_FILTER_TEST_MODE == 7
    runIntegrationTests();
#else
    runCoreTests();
    runDcTests();
    runResonanceTests();
    runModulationTests();
    runOversamplingTests();
    runIntegrationTests();
#endif

    std::printf("failures=%d\n", gFailures);
    return gFailures == 0 ? 0 : 1;
}
