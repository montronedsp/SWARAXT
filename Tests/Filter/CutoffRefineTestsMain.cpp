// Copyright 2026 MontroneDSP.
// SPDX-License-Identifier: GPL-3.0-or-later

#include <JuceHeader.h>

#include "Engine/Filter/Circuit/Ir3109Pole.h"
#include "Engine/Filter/SwaraXtFilter.h"
#include "Engine/SampleRate/HostResampler.h"
#include "Engine/SwaraXtEngine.h"
#include "Plugin/PluginProcessor.h"
#include "Plugin/SwaraXtParameterLayout.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <limits>
#include <string>
#include <vector>

namespace {

constexpr std::array<double, 19> kRequestedCutoffs {
    20.0, 25.0, 30.0, 35.0, 40.0, 50.0, 60.0, 70.0, 80.0, 100.0,
    125.0, 150.0, 200.0, 250.0, 300.0, 400.0, 500.0, 750.0, 1000.0
};

constexpr std::array<double, 4> kHostRates { 44100.0, 48000.0, 96000.0, 192000.0 };
constexpr std::array<double, 10> kNormalizedPoints {
    0.000, 0.005, 0.010, 0.020, 0.030, 0.050, 0.075, 0.100, 0.150, 0.200
};

int failures = 0;
std::vector<double> neutralMeasured;

void expect(bool condition, const char* message)
{
    if (! condition)
    {
        std::printf("FAIL: %s\n", message);
        ++failures;
    }
}

double sineGain(double testHz, double cutoffHz, double resonance = 0.0, double amplitude = 1.0e-4)
{
    constexpr double sampleRate = swaraxt::SwaraXtEngine::kInternalSampleRate;
    swaraxt::SwaraXtFilter filter;
    filter.prepare(sampleRate);
    swaraxt::SwaraXtFilterParams params;
    params.cutoffHz = static_cast<float>(cutoffHz);
    params.resonance = static_cast<float>(resonance);
    params.keyTrack = 0.0f;
    params.envAmount = 0.0f;
    params.modAmount = 0.0f;
    params.envValue = 0.0f;
    params.modValue = 0.0f;
    params.drive = 1.0f;
    filter.setParams(params);

    const int totalSamples = std::max(8192, static_cast<int>(std::ceil(sampleRate * 14.0 / testHz)));
    const int measureStart = totalSamples / 2;
    double re = 0.0;
    double im = 0.0;
    int count = 0;
    for (int i = 0; i < totalSamples; ++i)
    {
        const double phase = 2.0 * swaraxt::kPi * testHz * static_cast<double>(i) / sampleRate;
        const float output = filter.processSample(static_cast<float>(amplitude * std::sin(phase)));
        if (i >= measureStart)
        {
            re += static_cast<double>(output) * std::cos(phase);
            im += static_cast<double>(output) * std::sin(phase);
            ++count;
        }
    }
    return count > 0 ? (2.0 * std::sqrt(re * re + im * im) / static_cast<double>(count)) / amplitude : 0.0;
}

struct SignalStats {
    double inputRms = 0.0;
    double outputMean = 0.0;
    double outputRms = 0.0;
};

SignalStats sineStats(double testHz, double cutoffHz, double amplitude)
{
    constexpr double sampleRate = swaraxt::SwaraXtEngine::kInternalSampleRate;
    swaraxt::SwaraXtFilter filter;
    filter.prepare(sampleRate);
    swaraxt::SwaraXtFilterParams params;
    params.cutoffHz = static_cast<float>(cutoffHz);
    filter.setParams(params);

    const int totalSamples = std::max(8192, static_cast<int>(std::ceil(sampleRate * 14.0 / testHz)));
    const int measureStart = totalSamples / 2;
    double inputSquare = 0.0;
    double outputSum = 0.0;
    double outputSquare = 0.0;
    int count = 0;
    for (int i = 0; i < totalSamples; ++i)
    {
        const double phase = 2.0 * swaraxt::kPi * testHz * static_cast<double>(i) / sampleRate;
        const float input = static_cast<float>(amplitude * std::sin(phase));
        const float output = filter.processSample(input);
        if (i >= measureStart)
        {
            inputSquare += static_cast<double>(input) * input;
            outputSum += output;
            outputSquare += static_cast<double>(output) * output;
            ++count;
        }
    }

    SignalStats result;
    result.inputRms = std::sqrt(inputSquare / static_cast<double>(count));
    result.outputMean = outputSum / static_cast<double>(count);
    result.outputRms = std::sqrt(outputSquare / static_cast<double>(count));
    return result;
}

double measuredCutoff(double requestedHz, double resonance = 0.0, double amplitude = 1.0e-4)
{
    constexpr double target = 0.7071067811865475244;
    double low = std::max(1.0, requestedHz * 0.10);
    double high = requestedHz * 1.10;
    for (int iteration = 0; iteration < 14; ++iteration)
    {
        const double mid = std::sqrt(low * high);
        if (sineGain(mid, requestedHz, resonance, amplitude) > target)
            low = mid;
        else
            high = mid;
    }
    return std::sqrt(low * high);
}

double measuredRelativeCutoff(double requestedHz, double resonance)
{
    const double passbandGain = sineGain(std::max(1.0, requestedHz * 0.05), requestedHz, resonance);
    const double target = passbandGain * 0.7071067811865475244;
    double low = requestedHz * 0.10;
    double high = requestedHz * 4.0;
    for (int iteration = 0; iteration < 14; ++iteration)
    {
        const double mid = std::sqrt(low * high);
        if (sineGain(mid, requestedHz, resonance) > target)
            low = mid;
        else
            high = mid;
    }
    return std::sqrt(low * high);
}

juce::RangedAudioParameter& cutoffParameter(SwaraXtAudioProcessor& processor)
{
    auto* parameter = processor.getApvts().getParameter(swaraxt::IDs::filterCutoff);
    jassert(parameter != nullptr);
    return *parameter;
}

double setAndReadCutoff(SwaraXtAudioProcessor& processor, double requestedHz, double& normalized)
{
    auto& parameter = cutoffParameter(processor);
    normalized = parameter.convertTo0to1(static_cast<float>(requestedHz));
    parameter.setValueNotifyingHost(static_cast<float>(normalized));
    return processor.getApvts().getRawParameterValue(swaraxt::IDs::filterCutoff)->load();
}

double productionCoefficient(double stageHz)
{
    swaraxt::Ir3109Pole pole(swaraxt::kPassiveStage1Traits);
    constexpr double filterRate = swaraxt::SwaraXtEngine::kInternalSampleRate;
    const int oversampling = filterRate <= 48001.0 ? 4 : (filterRate <= 96001.0 ? 2 : 1);
    pole.setSampleRate(filterRate * static_cast<double>(oversampling));
    pole.setCutoffHz(stageHz);
    return pole.tptGain();
}

void writeBaselineCsv(const std::filesystem::path& path)
{
    std::filesystem::create_directories(path.parent_path());
    std::ofstream csv(path, std::ios::trunc);
    csv << "phase,host_rate_hz,requested_hz,host_normalized,apvts_physical_hz,"
           "smoother_target_hz,smoothed_hz,modulation_octaves,pre_clamp_hz,"
           "post_clamp_hz,control_voltage,stage_cutoff_hz,final_tpt_coefficient,"
           "measured_effective_cutoff_hz,measured_to_requested_ratio,absolute_error_hz,"
           "error_cents,adjacent_step_hz\n";

    std::vector<double> measured;
    measured.reserve(kRequestedCutoffs.size());
    for (double requested : kRequestedCutoffs)
        measured.push_back(measuredCutoff(requested));
    neutralMeasured = measured;

    for (double hostRate : kHostRates)
    {
        SwaraXtAudioProcessor processor;
        processor.prepareToPlay(hostRate, 128);
        swaraxt::CutoffMapper mapper;
        double previous = 0.0;
        for (size_t i = 0; i < kRequestedCutoffs.size(); ++i)
        {
            const double requested = kRequestedCutoffs[i];
            double normalized = 0.0;
            const double apvtsHz = setAndReadCutoff(processor, requested, normalized);
            const double preClamp = apvtsHz;
            const double postClamp = mapper.mapCutoffHz(preClamp, 0.0, 69.0, 0.0);
            const double cv = mapper.controlVoltageForCutoff(postClamp);
            const double stageHz = mapper.stageCutoffFromControlVoltage(cv);
            const double coefficient = productionCoefficient(stageHz);
            const double effective = measured[i];
            csv << "AFTER," << hostRate << ',' << requested << ',' << normalized << ','
                << apvtsHz << ',' << apvtsHz << ',' << apvtsHz << ",0,"
                << preClamp << ',' << postClamp << ',' << cv << ',' << stageHz << ','
                << coefficient << ',' << effective << ',' << effective / requested << ','
                << effective - requested << ',' << 1200.0 * std::log2(effective / requested) << ','
                << (i == 0 ? 0.0 : effective - previous) << '\n';
            previous = effective;
        }
        processor.releaseResources();
    }
}

void writeNormalizedCsv(const std::filesystem::path& path)
{
    std::filesystem::create_directories(path.parent_path());
    std::ofstream csv(path, std::ios::trunc);
    csv << "normalized,physical_hz\n";
    SwaraXtAudioProcessor processor;
    auto& parameter = cutoffParameter(processor);
    for (double normalized : kNormalizedPoints)
        csv << normalized << ',' << parameter.convertFrom0to1(static_cast<float>(normalized)) << '\n';
}

void runRoundtripChecks()
{
    SwaraXtAudioProcessor processor;
    auto& parameter = cutoffParameter(processor);
    const double important[] = { 20, 25, 30, 40, 50, 70, 100, 150, 200, 500 };
    for (double hz : important)
    {
        const double normalized = parameter.convertTo0to1(static_cast<float>(hz));
        const double recovered = parameter.convertFrom0to1(static_cast<float>(normalized));
        expect(std::abs(recovered - hz) < 0.01, "cutoff_normalized_roundtrip");
    }

    expect(parameter.getText(parameter.convertTo0to1(500.0f), 16).contains("Hz"),
           "cutoff display uses Hz below 1 kHz");
    expect(parameter.getText(parameter.convertTo0to1(5000.0f), 16).contains("kHz"),
           "cutoff display uses kHz above 1 kHz");
}

void runMonotonicityChecks()
{
    SwaraXtAudioProcessor processor;
    auto& parameter = cutoffParameter(processor);
    swaraxt::CutoffMapper mapper;
    double previousNormalized = -1.0;
    double previousCoefficient = -1.0;
    double minimumCoefficientStep = std::numeric_limits<double>::max();
    int longestPlateau = 0;
    int currentPlateau = 0;

    constexpr int points = 512;
    for (int i = 0; i < points; ++i)
    {
        const double position = static_cast<double>(i) / static_cast<double>(points - 1);
        const double requested = 10.0 * std::pow(2000.0, position);
        const double normalized = parameter.convertTo0to1(static_cast<float>(requested));
        const double physical = parameter.convertFrom0to1(static_cast<float>(normalized));
        const double mapped = mapper.mapCutoffHz(physical, 0.0, 69.0, 0.0);
        const double coefficient = productionCoefficient(mapper.stageCutoffFromMusicalCutoff(mapped));
        if (i > 0)
        {
            expect(normalized > previousNormalized, "cutoff_low_end_monotonicity");
            expect(coefficient > previousCoefficient, "cutoff coefficient monotonicity");
            const double step = coefficient - previousCoefficient;
            minimumCoefficientStep = std::min(minimumCoefficientStep, step);
            currentPlateau = step <= 1.0e-12 ? currentPlateau + 1 : 0;
            longestPlateau = std::max(longestPlateau, currentPlateau);
        }
        previousNormalized = normalized;
        previousCoefficient = coefficient;
    }

    expect(longestPlateau == 0, "cutoff_no_low_end_plateau");
    expect(minimumCoefficientStep > 1.0e-8, "cutoff dense sweep retains coefficient separation");

    bool measuredMonotonic = neutralMeasured.size() == kRequestedCutoffs.size();
    for (size_t i = 1; i < neutralMeasured.size(); ++i)
        measuredMonotonic = measuredMonotonic && neutralMeasured[i] > neutralMeasured[i - 1];
    expect(measuredMonotonic, "cutoff_requested_vs_effective");
}

double approachGain(double initialHz, double targetHz)
{
    constexpr double sampleRate = swaraxt::SwaraXtEngine::kInternalSampleRate;
    const double testHz = targetHz * swaraxt::CutoffMapper::fourPoleMinus3DbRatio();
    swaraxt::SwaraXtFilter filter;
    filter.prepare(sampleRate);
    swaraxt::SwaraXtFilterParams params;
    params.cutoffHz = static_cast<float>(initialHz);
    filter.setParams(params);

    const int warm = std::max(4096, static_cast<int>(sampleRate * 8.0 / testHz));
    double phase = 0.0;
    const double phaseStep = 2.0 * swaraxt::kPi * testHz / sampleRate;
    for (int i = 0; i < warm; ++i)
    {
        filter.processSample(static_cast<float>(1.0e-4 * std::sin(phase)));
        phase += phaseStep;
    }

    params.cutoffHz = static_cast<float>(targetHz);
    filter.setParams(params);
    const int total = std::max(8192, static_cast<int>(sampleRate * 16.0 / testHz));
    double re = 0.0;
    double im = 0.0;
    int count = 0;
    for (int i = 0; i < total; ++i)
    {
        const float output = filter.processSample(static_cast<float>(1.0e-4 * std::sin(phase)));
        if (i >= total / 2)
        {
            re += output * std::cos(phase);
            im += output * std::sin(phase);
            ++count;
        }
        phase += phaseStep;
    }
    return (2.0 * std::sqrt(re * re + im * im) / static_cast<double>(count)) / 1.0e-4;
}

void runHysteresisChecks(const std::filesystem::path& outputRoot)
{
    std::ofstream csv(outputRoot / "cutoff-hysteresis.csv", std::ios::trunc);
    csv << "target_hz,approach,settled_gain_at_expected_cutoff\n";
    for (double target : { 30.0, 50.0, 100.0, 200.0 })
    {
        const double downward = approachGain(5000.0, target);
        const double upward = approachGain(10.0, target);
        csv << target << ",high_to_low," << downward << '\n';
        csv << target << ",minimum_to_high," << upward << '\n';
        expect(std::abs(downward - upward) < 0.002, "cutoff_up_down_hysteresis");
    }
}

void writeInteractionChecks(const std::filesystem::path& outputRoot)
{
    std::ofstream resonanceCsv(outputRoot / "cutoff-resonance-interaction.csv", std::ios::trunc);
    resonanceCsv << "requested_hz,resonance,measured_effective_cutoff_hz,ratio\n";
    for (double cutoff : { 30.0, 50.0, 100.0, 200.0 })
    {
        for (double resonance : { 0.0, 0.25, 0.50, 0.85 })
        {
            const double measured = measuredRelativeCutoff(cutoff, resonance);
            resonanceCsv << cutoff << ',' << resonance << ',' << measured << ',' << measured / cutoff << '\n';
        }
    }

    std::ofstream levelCsv(outputRoot / "cutoff-input-level-interaction.csv", std::ios::trunc);
    levelCsv << "requested_hz,input_peak,input_rms,measured_effective_cutoff_hz,"
                "output_dc,output_rms,ratio\n";
    for (double cutoff : { 30.0, 50.0, 100.0, 200.0 })
    {
        for (double amplitude : { 1.0e-5, 1.0e-3, 0.1 })
        {
            const double measured = measuredCutoff(cutoff, 0.0, amplitude);
            const auto stats = sineStats(measured, cutoff, amplitude);
            levelCsv << cutoff << ',' << amplitude << ',' << stats.inputRms << ',' << measured << ','
                     << stats.outputMean << ',' << stats.outputRms << ',' << measured / cutoff << '\n';
        }
    }

    std::ofstream transitionCsv(outputRoot / "cutoff-transition-control.csv", std::ios::trunc);
    transitionCsv << "from_hz,to_hz,sample,target_hz,effective_control_hz\n";
    for (const auto transition : { std::array<double, 2> { 500.0, 50.0 },
                                   std::array<double, 2> { 50.0, 500.0 },
                                   std::array<double, 2> { 500.0, 10.0 },
                                   std::array<double, 2> { 10.0, 500.0 },
                                   std::array<double, 2> { 200.0, 30.0 },
                                   std::array<double, 2> { 30.0, 200.0 } })
    {
        transitionCsv << transition[0] << ',' << transition[1] << ",0,"
                      << transition[1] << ',' << transition[1] << '\n';
    }
}

void runSampleRateChecks()
{
    for (double hostRate : kHostRates)
    {
        SwaraXtAudioProcessor processor;
        processor.setFilterQuality(swaraxt::FilterQuality::high);
        processor.prepareToPlay(hostRate, 128);
        expect(processor.engineForTests().filter().oversamplingFactor() == 4,
               "cutoff_sample_rate_consistency");
        processor.releaseResources();
    }
}

void runLowEndDcChecks(const std::filesystem::path& outputRoot)
{
    constexpr double sampleRate = swaraxt::SwaraXtEngine::kInternalSampleRate;
    std::ofstream csv(outputRoot / "cutoff-low-end-dc.csv", std::ios::trunc);
    csv << "cutoff_hz,source,pre_blocker_mean,post_blocker_mean,pre_rms,post_rms\n";
    for (double cutoff : { 10.0, 30.0, 50.0, 100.0, 200.0 })
    {
        for (int waveform = 0; waveform < 5; ++waveform)
        {
            swaraxt::SwaraXtFilter filter;
            swaraxt::DcBlocker blocker;
            filter.prepare(sampleRate);
            blocker.reset();
            swaraxt::SwaraXtFilterParams params;
            params.cutoffHz = static_cast<float>(cutoff);
            filter.setParams(params);
            double preSum = 0.0;
            double postSum = 0.0;
            double preSquare = 0.0;
            double postSquare = 0.0;
            int count = 0;
            double phase = 0.0;
            const double increment = 110.0 / sampleRate;
            const int samples = static_cast<int>(sampleRate * 2.0);
            for (int i = 0; i < samples; ++i)
            {
                float input = 0.0f;
                if (waveform == 1)
                    input = static_cast<float>((phase * 2.0 - 1.0) * 0.2);
                else if (waveform == 2)
                    input = phase < 0.5 ? 0.2f : -0.2f;
                else if (waveform == 3 || (waveform == 4 && i < samples / 2))
                    input = static_cast<float>(0.2 * std::sin(2.0 * swaraxt::kPi * phase));
                phase += increment;
                if (phase >= 1.0)
                    phase -= 1.0;
                const float pre = filter.processSample(input);
                const float post = blocker.process(pre);
                if (i >= samples / 2)
                {
                    preSum += pre;
                    postSum += post;
                    preSquare += static_cast<double>(pre) * pre;
                    postSquare += static_cast<double>(post) * post;
                    ++count;
                }
            }
            const double preMean = preSum / static_cast<double>(count);
            const double postMean = postSum / static_cast<double>(count);
            const double preRms = std::sqrt(preSquare / static_cast<double>(count));
            const double postRms = std::sqrt(postSquare / static_cast<double>(count));
            const char* source = waveform == 0 ? "silence"
                                : waveform == 1 ? "saw"
                                : waveform == 2 ? "square"
                                : waveform == 3 ? "sustained_note"
                                                : "released_note";
            csv << cutoff << ',' << source << ',' << preMean << ',' << postMean << ','
                << preRms << ',' << postRms << '\n';
            expect(std::abs(preMean) < 0.01, "cutoff low end pre-blocker DC");
            expect(std::abs(postMean) < 0.002, "cutoff_low_end_dc");
        }
    }
}

}  // namespace

int main(int argc, char** argv)
{
    juce::ScopedJuceInitialiser_GUI juce;
    std::filesystem::path outputRoot = std::filesystem::current_path() / "artifacts" / "filter-refine";
    if (argc > 1)
        outputRoot = argv[1];

    writeBaselineCsv(outputRoot / "requested-vs-effective-cutoff.csv");
    writeNormalizedCsv(outputRoot / "normalized-cutoff-mapping.csv");
    runRoundtripChecks();
    runMonotonicityChecks();
    runHysteresisChecks(outputRoot);
    runSampleRateChecks();
    writeInteractionChecks(outputRoot);
    runLowEndDcChecks(outputRoot);

    std::printf(failures == 0 ? "Swara XT cutoff refine diagnostics: PASSED\n"
                              : "Swara XT cutoff refine diagnostics: FAILED (%d)\n",
                failures);
    return failures == 0 ? 0 : 1;
}
