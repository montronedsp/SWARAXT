// Copyright 2026 MontroneDSP.
// SPDX-License-Identifier: GPL-3.0-or-later

#include "SonicQuality/SonicQualityMetrics.h"
#include "SonicQuality/SonicQualityRenderer.h"
#include "SonicQuality/SonicQualityReport.h"
#include "SonicQuality/SonicQualityThresholds.h"

#include "Engine/SwaraXtEngine.h"

#include <JuceHeader.h>

#include <cmath>
#include <filesystem>
#include <iostream>
#include <sstream>

#ifndef SWARAXT_SONIC_QUALITY_COMMIT
#define SWARAXT_SONIC_QUALITY_COMMIT "unknown"
#endif

namespace {
namespace sonic = swaraxt::sonic;

std::string num(double value)
{
    std::ostringstream stream;
    stream.setf(std::ios::fmtflags(0), std::ios::floatfield);
    stream.precision(8);
    stream << value;
    return stream.str();
}

std::string buildName()
{
#if defined(_WIN32)
    std::string name = "windows-msvc";
#elif defined(__APPLE__)
    std::string name = "macos";
#else
    std::string name = "linux";
#endif
#if defined(NDEBUG)
    name += "-release";
#else
    name += "-debug";
#endif
    return name;
}

const char* kFrequencyHeader =
    "case,path,frequency_hz,amplitude,resonance,quality,sample_rate,fundamental_gain_db,"
    "relative_to_1khz_db,phase_rad,rms,sample_peak,schematic_gain_db,gain_error_db,result";
const char* kDistortionHeader =
    "case,path,frequency_hz,amplitude_dbfs,resonance,quality,sample_rate,thd,thd_n,h2,h3,h4,h5,"
    "alias_db,alias_hz,fundamental_db,result";
const char* kAliasHeader =
    "case,path,fundamental_hz,harmonic_or_image,folded_hz,amplitude_db,fundamental_db,quality,sample_rate,result";
const char* kFxHeader =
    "case,path,effect,cv1,cv2,rms_difference,gain_difference_db,max_difference,lag_samples,"
    "dry_rms,wet_rms,above_unity,result";
const char* kAutomationHeader =
    "case,host_rate,block_size,law,rms,max_step,step_ratio,compared_with,rms_difference,lag_samples,result";
const char* kPeakHeader =
    "case,path,host_rate,block_size,quality,sample_peak_dbfs,true_peak_dbtp,rms_dbfs,crest_db,dc,"
    "above_unity,longest_near_full_scale,nonfinite,cpu_seconds,result";
const char* kQualityHeader =
    "case,host_rate,reference_quality,other_quality,lag_samples,max_difference,rms_difference,"
    "rms_delta_db,cpu_reference_s,cpu_other_s,result";

struct Suite
{
    sonic::SonicReport report;
    bool extended = false;
    std::string directory;
    std::string commit = SWARAXT_SONIC_QUALITY_COMMIT;

    void add(sonic::Measurement row)
    {
        if (row.result == sonic::Classification::fail)
            std::cerr << "FAIL " << row.testCase << ": " << row.observed << "\n";
        report.add(std::move(row));
    }

    sonic::Measurement base(const std::string& test, const std::string& family) const
    {
        sonic::Measurement row;
        row.testCase = test;
        row.family = family;
        row.commit = commit;
        row.result = sonic::Classification::observed;
        return row;
    }
};

void addSafety(Suite& suite, const std::string& test, const std::string& path,
               const sonic::LevelStats& levels, bool boardClamp)
{
    auto row = suite.base(test + ".safety", "safety");
    row.sourceClass = path;
    row.observed = "nonfinite=" + std::to_string(levels.nonfinite)
        + " peak=" + num(levels.samplePeak)
        + " above_unity=" + std::to_string(levels.aboveUnity);
    row.reference = boardClamp ? "finite and |y|<=8 on the instrument clamp" : "finite and |y|<100";
    row.tolerance = boardClamp ? "abs(y)<=8" : "abs(y)<100";
    row.result = sonic::Classification::pass;
    if (levels.nonfinite != 0 || levels.aboveUnbounded != 0
        || (boardClamp && levels.aboveBoardLimit != 0))
        row.result = sonic::Classification::fail;
    else if (!boardClamp && levels.aboveUnity > 0)
    {
        row.result = sonic::Classification::observed;
        row.notes = "Samples above full scale can clip a downstream consumer. This is not a numeric-safety failure.";
    }
    suite.add(row);
}

int steadyOrigin(const std::vector<float>& samples, int latency, double sampleRate)
{
    const int skip = latency + static_cast<int>(std::lround(0.03 * sampleRate));
    if (samples.size() < static_cast<size_t>(skip + 512))
        return 0;
    return skip;
}

void measureBoardFrequency(Suite& suite, const sonic::BoardTone& tone, const std::string& test,
                           double referenceGain)
{
    const auto render = sonic::renderBoardTone(tone);
    const auto levels = sonic::measureLevels(render.samples.data(), static_cast<int>(render.samples.size()));
    addSafety(suite, test, "production-ir3109-vca", levels, true);
    const auto window = sonic::nearlyIntegerCycles(render.sampleRate, tone.frequencyHz,
                                                   static_cast<int>(render.samples.size()))
        ? sonic::AnalysisWindow::rectangular
        : sonic::AnalysisWindow::hann;
    const auto fundamental = sonic::projectTone(render.samples.data(), static_cast<int>(render.samples.size()),
                                                render.sampleRate, tone.frequencyHz, window);
    const double measuredGain = tone.amplitude > 0.0f ? fundamental.peakAmplitude / tone.amplitude : 0.0;
    const double schematic = sonic::schematicOpenFilterGain(tone.frequencyHz);
    const double relative = referenceGain > 0.0 ? measuredGain / referenceGain : 0.0;
    auto row = suite.base(test, "frequency_response");
    row.sourceClass = tone.instrumentPath ? "production-processInstrumentSample" : "diagnostic-processSample";
    row.input = "sine " + num(tone.frequencyHz) + " Hz amp " + num(tone.amplitude);
    row.parameters = "cutoff " + num(tone.cutoffHz) + " resonance " + num(tone.resonance)
        + " oversample " + std::to_string(render.oversample);
    row.quality = sonic::qualityName(tone.quality);
    row.hostRate = render.sampleRate;
    row.reference = "schematic small-signal gain " + num(schematic)
        + "; 1 kHz measured gain " + num(referenceGain);
    row.observed = "gain " + num(measuredGain) + " (" + num(sonic::db(measuredGain)) + " dB)";
    const bool schematicCheck = tone.instrumentPath && tone.oversampleOverride == 0
        && tone.quality == swaraxt::FilterQuality::normal && tone.resonance == 0.0f
        && tone.amplitude <= sonic::kSchematicCheckAmplitude * 1.01f
        && std::abs(tone.frequencyHz - sonic::kSchematicCheckFrequencyHz) < 0.1
        && std::abs(tone.cutoffHz - 20000.0f) < 0.1f;
    if (!render.finite || levels.nonfinite != 0)
        row.result = sonic::Classification::fail;
    else if (schematicCheck)
    {
        row.tolerance = "relative error 0.015, same condition as SwaraXTIR3109BoardTests";
        row.result = std::abs(measuredGain / schematic - 1.0) <= sonic::kSchematicGainTolerance
            ? sonic::Classification::pass
            : sonic::Classification::fail;
        row.notes = "Validated small-signal cross-check. Not a flat-response requirement.";
    }
    else if (tone.instrumentPath && tone.oversampleOverride == 0
             && tone.amplitude <= 1.0e-3f && tone.resonance == 0.0f && tone.frequencyHz <= 4000.0
             && std::abs(measuredGain / schematic - 1.0) > 0.05)
    {
        row.result = sonic::Classification::needsReview;
        row.notes = "Screening bound of 5 percent versus the schematic estimate. Not an approved sonic limit.";
    }
    else if (!tone.instrumentPath)
    {
        row.result = sonic::Classification::observed;
        row.notes = "processSample is the diagnostic core. It is not the production instrument path and the schematic VCA estimate does not apply.";
    }
    else
    {
        row.result = sonic::Classification::observed;
        row.notes = "Open IR3109 response includes the intended OTA and coupling curve.";
    }
    suite.add(row);
    std::ostringstream line;
    line << test << ',' << row.sourceClass << ',' << tone.frequencyHz << ',' << tone.amplitude << ','
         << tone.resonance << ',' << row.quality << ',' << render.sampleRate << ','
         << sonic::db(measuredGain) << ',' << sonic::db(relative) << ',' << fundamental.phaseRadians << ','
         << levels.rms << ',' << levels.samplePeak << ',' << sonic::db(schematic) << ','
         << sonic::db(measuredGain / std::max(schematic, 1.0e-15)) << ','
         << sonic::classificationName(row.result);
    suite.report.addTableRow("frequency_response.csv", kFrequencyHeader, line.str());
}

void measureBoardDistortion(Suite& suite, const sonic::BoardTone& tone, const std::string& test)
{
    auto capture = tone;
    capture.captureSamples = sonic::coherentLength(swaraxt::SwaraXtEngine::kInternalSampleRate,
                                                   tone.frequencyHz, std::max(2048, tone.captureSamples));
    const auto render = sonic::renderBoardTone(capture);
    const auto levels = sonic::measureLevels(render.samples.data(), static_cast<int>(render.samples.size()));
    addSafety(suite, test, "production-ir3109-vca", levels, true);
    const auto distortion = sonic::measureDistortion(render.samples.data(), static_cast<int>(render.samples.size()),
                                                     render.sampleRate, capture.frequencyHz,
                                                     sonic::AnalysisWindow::rectangular);
    auto row = suite.base(test, "distortion");
    row.sourceClass = capture.oversampleOverride > 0 ? "numerical-oversample-reference" : "production-ir3109-vca";
    row.input = "sine " + num(capture.frequencyHz) + " Hz";
    row.parameters = "amplitude " + num(sonic::db(capture.amplitude)) + " dBFS resonance " + num(capture.resonance);
    row.quality = sonic::qualityName(capture.quality);
    row.hostRate = render.sampleRate;
    row.reference = "no approved THD limit; analog tanh harmonics are expected";
    row.observed = "THD " + num(distortion.thd)
        + (distortion.thdNValid ? " THD+N " + num(distortion.thdN) : " THD+N n/a")
        + " alias " + num(sonic::db(distortion.aliasPeak));
    row.result = (!render.finite || levels.nonfinite != 0) ? sonic::Classification::fail
        : (capture.resonance == 0.0f && capture.amplitude <= 0.01f && distortion.thd > 0.5)
            ? sonic::Classification::needsReview
            : sonic::Classification::observed;
    if (row.result == sonic::Classification::needsReview)
        row.notes = "Quiet zero-resonance THD above 50 percent. Screening bound only.";
    else if (capture.oversampleOverride > 0)
        row.notes = "8x oversampling is a numerical reference, not a hardware oracle.";
    else
        row.notes = "Harmonics can be the IR3109 tanh model, the native-rate image, or both.";
    suite.add(row);
    std::ostringstream line;
    line << test << ',' << row.sourceClass << ',' << capture.frequencyHz << ',' << sonic::db(capture.amplitude)
         << ',' << capture.resonance << ',' << row.quality << ',' << render.sampleRate << ','
         << distortion.thd << ',' << (distortion.thdNValid ? num(distortion.thdN) : "") << ','
         << distortion.harmonicPeak[0] << ',' << distortion.harmonicPeak[1] << ','
         << distortion.harmonicPeak[2] << ',' << distortion.harmonicPeak[3] << ','
         << (distortion.aliasPeak > 0.0 ? num(sonic::db(distortion.aliasPeak)) : "") << ','
         << distortion.aliasFrequencyHz << ','
         << sonic::db(distortion.fundamental.peakAmplitude) << ','
         << sonic::classificationName(row.result);
    suite.report.addTableRow("harmonic_distortion.csv", kDistortionHeader, line.str());
    if (distortion.aliasPeak > 0.0)
    {
        std::ostringstream alias;
        alias << test << ',' << row.sourceClass << ',' << capture.frequencyHz << ",folded-harmonic,"
              << distortion.aliasFrequencyHz << ',' << sonic::db(distortion.aliasPeak) << ','
              << sonic::db(distortion.fundamental.peakAmplitude) << ',' << row.quality << ','
              << render.sampleRate << ',' << sonic::classificationName(row.result);
        suite.report.addTableRow("alias_spectrum.csv", kAliasHeader, alias.str());
        if (capture.resonance == 0.0f && capture.amplitude <= 0.25f && distortion.fundamental.peakAmplitude > 1.0e-8
            && distortion.aliasPeak > distortion.fundamental.peakAmplitude * 0.1)
        {
            auto review = row;
            review.testCase += ".alias-screen";
            review.family = "alias";
            review.result = sonic::Classification::needsReview;
            review.notes = "Folded component within 20 dB of the fundamental at zero resonance. Screening bound only.";
            suite.add(review);
        }
    }
}

void compareFx(Suite& suite, const sonic::FxRender& dry, const sonic::FxRender& wet,
               const sonic::FxInsert& insert, const std::string& path)
{
    const std::string test = std::string(path) + "." + sonic::effectName(insert.effectChoice);
    double correlation = 0.0;
    const int lag = sonic::bestIntegerLag(dry.samples, wet.samples, 40, correlation);
    const double maxDifference = sonic::maxAbsDifference(dry.samples, wet.samples, lag);
    const double rmsDifference = sonic::rmsDifference(dry.samples, wet.samples, lag);
    const auto dryLevel = sonic::measureLevels(dry.samples.data(), static_cast<int>(dry.samples.size()));
    const auto wetLevel = sonic::measureLevels(wet.samples.data(), static_cast<int>(wet.samples.size()));
    auto row = suite.base(test, "fx_dry");
    row.sourceClass = path;
    row.input = "sine " + num(insert.frequencyHz) + " Hz amp " + num(insert.amplitude);
    row.parameters = "cv1 " + std::to_string(insert.cv1) + " cv2 " + std::to_string(insert.cv2);
    row.quality = "n/a";
    row.hostRate = dry.sampleRate;
    row.blockSize = 40;
    row.observed = "rms-diff " + num(rmsDifference) + " max-diff " + num(maxDifference)
        + " lag " + std::to_string(lag);
    if (!dry.finite || !wet.finite || dryLevel.nonfinite != 0 || wetLevel.nonfinite != 0)
    {
        row.result = sonic::Classification::fail;
        row.reference = "finite FX insert";
    }
    else if (insert.effectChoice == 0)
    {
        row.reference = "Effect::off is an identity copy";
        row.tolerance = "max abs difference 0";
        row.result = maxDifference == 0.0 ? sonic::Classification::pass : sonic::Classification::fail;
        row.notes = "Off bypasses 12-bit quantization and the post-DAC filter.";
    }
    else if (!sonic::effectCanBeTransparent(insert.effectChoice))
    {
        row.reference = "no zero-wet equivalent";
        row.result = sonic::Classification::notApplicable;
        row.notes = "processClassicFx quantizes every enabled program to 12 bits and then runs the post-DAC Chebyshev. "
                    "The measured difference is recorded and is not a transparency failure.";
        auto observed = row;
        observed.testCase += ".coloration";
        observed.result = sonic::Classification::observed;
        observed.reference = "intentional 12-bit FX plus post-DAC coloration";
        suite.add(observed);
    }
    suite.add(row);
    const double gain = dryLevel.rms > 1.0e-12 ? sonic::db(wetLevel.rms / dryLevel.rms) : 0.0;
    std::ostringstream line;
    line << test << ',' << path << ',' << sonic::effectName(insert.effectChoice) << ','
         << static_cast<int>(insert.cv1) << ',' << static_cast<int>(insert.cv2) << ','
         << rmsDifference << ',' << gain << ',' << maxDifference << ',' << lag << ','
         << dryLevel.rms << ',' << wetLevel.rms << ',' << wetLevel.aboveUnity << ','
         << sonic::classificationName(row.result);
    suite.report.addTableRow("fx_dry_comparison.csv", kFxHeader, line.str());
}

void measurePlugin(Suite& suite, const sonic::PluginTone& tone, const std::string& test,
                   bool expectAudio)
{
    const auto render = sonic::renderPluginTone(tone);
    const auto levels = sonic::measureLevels(render.left.data(), static_cast<int>(render.left.size()));
    const auto peaks = sonic::measureTruePeak(render.left.data(), static_cast<int>(render.left.size()));
    addSafety(suite, test, "complete-plugin", levels, false);
    auto latency = suite.base(test + ".latency", "latency");
    latency.sourceClass = "complete-plugin";
    latency.quality = sonic::qualityName(tone.quality);
    latency.hostRate = tone.hostRate;
    latency.blockSize = tone.blockSize;
    latency.reference = std::to_string(render.expectedLatency);
    latency.observed = std::to_string(render.reportedLatency);
    latency.tolerance = "exact documented native latency converted to host samples";
    latency.result = render.reportedLatency == render.expectedLatency
        ? sonic::Classification::pass : sonic::Classification::fail;
    latency.notes = "kOutputLatencyNativeSamples converted with ceil(native * host / internal).";
    suite.add(latency);

    auto stereo = suite.base(test + ".stereo", "routing");
    stereo.sourceClass = "mono-sum-to-both-channels";
    stereo.hostRate = tone.hostRate;
    stereo.blockSize = tone.blockSize;
    stereo.quality = sonic::qualityName(tone.quality);
    stereo.reference = "left equals right";
    stereo.observed = render.rightMatchesLeft ? "exact" : "mismatch";
    stereo.result = render.rightMatchesLeft && render.finite
        ? sonic::Classification::pass : sonic::Classification::fail;
    suite.add(stereo);

    if (expectAudio && levels.rms < 1.0e-6 && tone.note >= 0 && tone.master > 0.1f)
    {
        auto silent = suite.base(test + ".silence", "continuity");
        silent.sourceClass = "complete-plugin";
        silent.hostRate = tone.hostRate;
        silent.blockSize = tone.blockSize;
        silent.quality = sonic::qualityName(tone.quality);
        silent.reference = "a held note produces energy";
        silent.observed = "rms " + num(levels.rms);
        silent.result = sonic::Classification::fail;
        silent.notes = "The render was silent. This is a processing-continuity failure, not a tonal judgement.";
        suite.add(silent);
    }

    auto peak = suite.base(test + ".peaks", "peaks");
    peak.sourceClass = "complete-plugin";
    peak.input = "midi note " + std::to_string(tone.note);
    peak.parameters = "cutoff " + num(tone.cutoffHz) + " resonance " + num(tone.resonance)
        + " fx " + sonic::effectName(tone.fxChoice) + " master " + num(tone.master)
        + " postMixer " + num(tone.postMixerDb);
    peak.quality = sonic::qualityName(tone.quality);
    peak.hostRate = tone.hostRate;
    peak.blockSize = tone.blockSize;
    peak.reference = "no approved peak ceiling; |y|>1 is downstream clip risk";
    peak.observed = "sample " + num(sonic::db(peaks.samplePeak)) + " dBFS true "
        + num(sonic::db(peaks.truePeak)) + " dBTP dc " + num(levels.mean);
    peak.result = levels.nonfinite != 0 ? sonic::Classification::fail : sonic::Classification::observed;
    peak.notes = "True peak uses a DC-normalised 4x 48-tap Blackman-sinc, validated to 0.02 on a half-sample cosine.";
    suite.add(peak);
    std::ostringstream line;
    line << test << ",complete-plugin," << tone.hostRate << ',' << tone.blockSize << ','
         << sonic::qualityName(tone.quality) << ',' << sonic::db(peaks.samplePeak) << ','
         << sonic::db(peaks.truePeak) << ',' << sonic::db(levels.rms) << ',' << levels.crestDb << ','
         << levels.mean << ',' << levels.aboveUnity << ',' << levels.longestNearFullScaleRun << ','
         << levels.nonfinite << ',' << render.renderSeconds << ','
         << sonic::classificationName(peak.result);
    suite.report.addTableRow("output_peaks.csv", kPeakHeader, line.str());
}

void comparePluginPartition(Suite& suite, sonic::PluginTone tone, int otherBlock, const std::string& test)
{
    const auto first = sonic::renderPluginTone(tone);
    tone.blockSize = otherBlock;
    const auto second = sonic::renderPluginTone(tone);
    double correlation = 0.0;
    const int lag = sonic::bestIntegerLag(first.left, second.left, 64, correlation);
    const double maxDifference = sonic::maxAbsDifference(first.left, second.left, lag);
    auto row = suite.base(test, "partition");
    row.sourceClass = "complete-plugin";
    row.hostRate = tone.hostRate;
    row.blockSize = otherBlock;
    row.quality = sonic::qualityName(tone.quality);
    row.reference = "equivalent timeline";
    row.observed = "lag " + std::to_string(lag) + " max-diff " + num(maxDifference);
    if (!first.finite || !second.finite)
        row.result = sonic::Classification::fail;
    else if (maxDifference >= 0.25)
    {
        row.result = sonic::Classification::fail;
        row.notes = "Gross disagreement remains after integer alignment.";
    }
    else if (lag != 0 || maxDifference > 1.0e-5)
    {
        row.result = sonic::Classification::needsReview;
        row.notes = "Block-size dependence is visible. No approved partition tolerance exists yet.";
    }
    else
    {
        row.result = sonic::Classification::pass;
        row.tolerance = "lag 0 and max abs difference <= 1e-5";
        row.notes = "Observed agreement on this timeline.";
    }
    suite.add(row);
}

void runSelfTests(Suite& suite)
{
    std::string detail;
    const int failures = sonic::runMetricSelfTests(detail);
    auto row = suite.base("metric-self-test", "measurement");
    row.sourceClass = "analytic-signal";
    row.reference = "known sine, harmonic, DC, clip, alias and true-peak values";
    row.observed = failures == 0 ? "all analytic checks passed" : detail;
    row.tolerance = "see SonicQualityMetrics.cpp";
    row.result = failures == 0 ? sonic::Classification::pass : sonic::Classification::fail;
    row.notes = detail;
    suite.add(row);
}

void runQuick(Suite& suite)
{
    runSelfTests(suite);
    const double native = swaraxt::SwaraXtEngine::kInternalSampleRate;
    sonic::BoardTone reference;
    reference.frequencyHz = 1000.0;
    reference.amplitude = sonic::kSchematicCheckAmplitude;
    reference.cutoffHz = 20000.0f;
    reference.resonance = 0.0f;
    reference.captureSamples = sonic::coherentLength(native, 1000.0, 2048);
    const auto referenceRender = sonic::renderBoardTone(reference);
    const auto referenceTone = sonic::projectTone(referenceRender.samples.data(),
                                                  static_cast<int>(referenceRender.samples.size()),
                                                  referenceRender.sampleRate, 1000.0,
                                                  sonic::AnalysisWindow::rectangular);
    const double referenceGain = referenceTone.peakAmplitude / reference.amplitude;
    measureBoardFrequency(suite, reference, "board.open.1k.minus80", referenceGain);

    sonic::BoardTone loud = reference;
    loud.amplitude = static_cast<float>(std::pow(10.0, -12.0 / 20.0));
    loud.captureSamples = reference.captureSamples;
    measureBoardFrequency(suite, loud, "board.open.1k.minus12", referenceGain);
    measureBoardDistortion(suite, loud, "board.thd.1k.minus12.res0");
    loud.resonance = 0.9f;
    measureBoardDistortion(suite, loud, "board.thd.1k.minus12.res0.9");

    sonic::BoardTone diagnostic = loud;
    diagnostic.resonance = 0.0f;
    diagnostic.amplitude = sonic::kSchematicCheckAmplitude;
    diagnostic.instrumentPath = false;
    diagnostic.captureSamples = reference.captureSamples;
    measureBoardFrequency(suite, diagnostic, "diagnostic.processSample.1k", referenceGain);

    sonic::FxInsert dryInsert;
    const auto dryFx = sonic::renderClassicFxInsert(dryInsert);
    compareFx(suite, dryFx, dryFx, dryInsert, "layer-a-fx");
    for (int choice : {1, 6})
    {
        sonic::FxInsert wet = dryInsert;
        wet.effectChoice = choice;
        compareFx(suite, dryFx, sonic::renderClassicFxInsert(wet), wet, "layer-a-fx");
    }

    sonic::PluginTone plugin;
    measurePlugin(suite, plugin, "plugin.48k.note", true);
    plugin.note = -1;
    comparePluginPartition(suite, plugin, 128, "plugin.silence.block64-vs-128");
    plugin.note = 69;
    plugin.hostRate = 7000.0;
    plugin.totalSamples = 512;
    const auto unsupported = sonic::renderPluginTone(plugin);
    const auto unsupportedLevel = sonic::measureLevels(unsupported.left.data(),
                                                       static_cast<int>(unsupported.left.size()));
    auto unsupportedRow = suite.base("plugin.unsupported-rate", "latency");
    unsupportedRow.sourceClass = "complete-plugin";
    unsupportedRow.hostRate = 7000.0;
    unsupportedRow.reference = "silent output and zero reported latency";
    unsupportedRow.observed = "rms " + num(unsupportedLevel.rms) + " latency "
        + std::to_string(unsupported.reportedLatency);
    unsupportedRow.result = unsupportedLevel.rms == 0.0 && unsupported.reportedLatency == 0
        && unsupported.finite
        ? sonic::Classification::pass : sonic::Classification::fail;
    suite.add(unsupportedRow);

    const auto reprepare = sonic::renderPluginReprepare();
    auto reprepareRow = suite.base("plugin.reprepare.48k-to-96k", "latency");
    reprepareRow.sourceClass = "complete-plugin";
    reprepareRow.reference = "latency tracks the documented conversion and both rates stay finite";
    reprepareRow.observed = "48k " + std::to_string(reprepare.latencyBefore) + "/"
        + std::to_string(reprepare.expectedBefore) + " 96k " + std::to_string(reprepare.latencyAfter)
        + "/" + std::to_string(reprepare.expectedAfter);
    reprepareRow.result = reprepare.finite && reprepare.audibleBefore && reprepare.audibleAfter
            && reprepare.latencyBefore == reprepare.expectedBefore
            && reprepare.latencyAfter == reprepare.expectedAfter
        ? sonic::Classification::pass : sonic::Classification::fail;
    suite.add(reprepareRow);

    sonic::PluginTone sweep;
    sweep.cutoffHz = 200.0f;
    sweep.cutoffEndHz = 8000.0f;
    sweep.cutoffLaw = sonic::CutoffLaw::linear;
    sweep.totalSamples = 4096;
    const auto swept = sonic::renderPluginTone(sweep);
    const auto sweptLevel = sonic::measureLevels(swept.left.data(), static_cast<int>(swept.left.size()));
    auto sweepRow = suite.base("plugin.cutoff.linear", "automation");
    sweepRow.sourceClass = "complete-plugin";
    sweepRow.hostRate = sweep.hostRate;
    sweepRow.blockSize = sweep.blockSize;
    sweepRow.quality = "normal";
    sweepRow.reference = "finite sweep; zipper energy has no approved limit";
    sweepRow.observed = "rms " + num(sweptLevel.rms) + " max-step " + num(sweptLevel.maxAdjacentDifference);
    sweepRow.result = swept.finite && sweptLevel.nonfinite == 0
        ? sonic::Classification::observed : sonic::Classification::fail;
    suite.add(sweepRow);
    std::ostringstream automation;
    automation << sweepRow.testCase << ',' << sweep.hostRate << ',' << sweep.blockSize
               << ",linear," << sweptLevel.rms << ',' << sweptLevel.maxAdjacentDifference << ','
               << (sweptLevel.medianAdjacentDifference > 0.0
                       ? sweptLevel.maxAdjacentDifference / sweptLevel.medianAdjacentDifference : 0.0)
               << ",none,0,0," << sonic::classificationName(sweepRow.result);
    suite.report.addTableRow("cutoff_automation.csv", kAutomationHeader, automation.str());

    sonic::PluginTone unity;
    unity.cutoffHz = 8000.0f;
    unity.resonance = 0.0f;
    unity.postMixerDb = 0.0f;
    unity.totalSamples = 12000;
    sonic::PluginTone trimmed = unity;
    trimmed.postMixerDb = -18.0f;
    const auto unityRender = sonic::renderPluginTone(unity);
    const auto trimmedRender = sonic::renderPluginTone(trimmed);
    const int skip = std::min(8000, static_cast<int>(unityRender.left.size() / 2));
    const auto unityLevel = sonic::measureLevels(unityRender.left.data() + skip,
                                                static_cast<int>(unityRender.left.size()) - skip);
    const auto trimmedLevel = sonic::measureLevels(trimmedRender.left.data() + skip,
                                                  static_cast<int>(trimmedRender.left.size()) - skip);
    const double ratioDb = sonic::db(trimmedLevel.rms) - sonic::db(unityLevel.rms);
    auto trim = suite.base("plugin.post-mixer-law", "gain");
    trim.sourceClass = "complete-plugin";
    trim.hostRate = 48000.0;
    trim.blockSize = 64;
    trim.quality = "normal";
    trim.input = "saw note, cutoff 8 kHz, resonance 0";
    trim.reference = "-18 dB parameter law before the filter";
    trim.tolerance = "screening window -18 +/- 3 dB; filter coloration is expected";
    trim.observed = num(ratioDb) + " dB";
    if (!unityRender.finite || !trimmedRender.finite)
        trim.result = sonic::Classification::fail;
    else if (std::abs(ratioDb + 18.0) > 3.0)
    {
        trim.result = sonic::Classification::needsReview;
        trim.notes = "Linear-region post-mixer ratio missed the parameter law by more than 3 dB. Screening bound only.";
    }
    else
    {
        trim.result = sonic::Classification::observed;
        trim.notes = "Ratio includes IR3109 gain. It is not an approved regression tolerance.";
    }
    suite.add(trim);
}

void runExtended(Suite& suite, const std::filesystem::path& directory)
{
    const double native = swaraxt::SwaraXtEngine::kInternalSampleRate;
    const double frequencies[] = {
        20, 40, 60, 100, 250, 500, 1000, 2000, 4000, 5000, 8000, 10000, 12000, 16000, 18000, 19000
    };
    sonic::BoardTone probe;
    probe.frequencyHz = 1000.0;
    probe.amplitude = static_cast<float>(std::pow(10.0, -60.0 / 20.0));
    probe.captureSamples = sonic::coherentLength(native, 1000.0, 2048);
    const auto anchorRender = sonic::renderBoardTone(probe);
    const auto anchor = sonic::projectTone(anchorRender.samples.data(), static_cast<int>(anchorRender.samples.size()),
                                           anchorRender.sampleRate, 1000.0, sonic::AnalysisWindow::rectangular);
    const double anchorGain = anchor.peakAmplitude / probe.amplitude;
    for (double frequency : frequencies)
    {
        if (frequency >= native * 0.49)
            continue;
        for (double decibels : {-60.0, -12.0})
        {
            sonic::BoardTone tone = probe;
            tone.frequencyHz = frequency;
            tone.amplitude = static_cast<float>(std::pow(10.0, decibels / 20.0));
            tone.captureSamples = sonic::coherentLength(native, frequency, 2048);
            if (frequency < 100.0)
                tone.warmupSamples = static_cast<int>(std::lround(2.5 * native));
            measureBoardFrequency(suite, tone, "board.open." + num(frequency) + "." + num(decibels), anchorGain);
        }
    }

    const double distortionFrequencies[] = {100.0, 1000.0, 4000.0, 12000.0};
    const double amplitudeDb[] = {-60, -48, -36, -24, -18, -12, -6, 0};
    const float resonances[] = {0.0f, 0.5f, 0.9f};
    for (double frequency : distortionFrequencies)
        for (double decibels : amplitudeDb)
            for (float resonance : resonances)
            {
                if (frequency >= native * 0.45)
                    continue;
                sonic::BoardTone tone;
                tone.frequencyHz = frequency;
                tone.amplitude = static_cast<float>(std::pow(10.0, decibels / 20.0));
                tone.resonance = resonance;
                tone.cutoffHz = 20000.0f;
                measureBoardDistortion(suite, tone,
                    "board.thd." + num(frequency) + "." + num(decibels) + ".r" + num(resonance));
            }

    for (swaraxt::FilterQuality quality : {swaraxt::FilterQuality::high, swaraxt::FilterQuality::normal,
                                           swaraxt::FilterQuality::eco})
    {
        sonic::BoardTone tone;
        tone.quality = quality;
        tone.frequencyHz = 1000.0;
        tone.amplitude = 0.25f;
        tone.resonance = 0.99f;
        tone.cutoffHz = 1000.0f;
        measureBoardDistortion(suite, tone, std::string("board.resonance-near-max.") + sonic::qualityName(quality));
        tone.oversampleOverride = 8;
        tone.resonance = 0.5f;
        tone.cutoffHz = 8000.0f;
        if (quality == swaraxt::FilterQuality::normal)
            measureBoardDistortion(suite, tone, "board.numerical-8x.1k");
    }

    sonic::FxInsert dryInsert;
    dryInsert.blocks = 60;
    const auto dry = sonic::renderClassicFxInsert(dryInsert);
    for (int choice = 0; choice <= 16; ++choice)
    {
        sonic::FxInsert wet = dryInsert;
        wet.effectChoice = choice;
        compareFx(suite, dry, sonic::renderClassicFxInsert(wet), wet, "layer-a-fx");
    }

    const double rates[] = {32000.0, 44100.0, 48000.0, 88200.0, 96000.0, 192000.0};
    for (double rate : rates)
    {
        sonic::PluginTone tone;
        tone.hostRate = rate;
        tone.totalSamples = static_cast<int>(rate * 0.15);
        measurePlugin(suite, tone, "plugin.rate." + num(rate), true);
    }
    const int blocks[] = {1, 32, 64, 127, 256, 512};
    for (int block : blocks)
    {
        sonic::PluginTone tone;
        tone.blockSize = block;
        tone.totalSamples = block == 1 ? 2048 : 8192;
        measurePlugin(suite, tone, "plugin.block." + std::to_string(block), true);
    }
    sonic::PluginTone partition;
    partition.totalSamples = 8192;
    comparePluginPartition(suite, partition, 127, "plugin.note.block64-vs-127");

    for (int choice = 0; choice <= 16; ++choice)
    {
        sonic::PluginTone tone;
        tone.fxChoice = choice;
        tone.fxParam1 = 0;
        tone.fxParam2 = 0;
        tone.totalSamples = 8192;
        measurePlugin(suite, tone, std::string("plugin.fx.") + sonic::effectName(choice), choice != 15);
    }

    sonic::PluginTone loud;
    loud.master = 1.0f;
    loud.resonance = 0.95f;
    loud.cutoffHz = 1500.0f;
    loud.mixSub = 127;
    loud.mixNoise = 63;
    loud.osc2Shape = 1;
    loud.totalSamples = 12000;
    measurePlugin(suite, loud, "plugin.extreme-level", true);
    loud.postMixerDb = -18.0f;
    measurePlugin(suite, loud, "plugin.post-mixer-minus18", true);

    std::vector<sonic::PluginRender> qualities;
    for (swaraxt::FilterQuality quality : {swaraxt::FilterQuality::high, swaraxt::FilterQuality::normal,
                                           swaraxt::FilterQuality::eco})
    {
        sonic::PluginTone tone;
        tone.quality = quality;
        tone.totalSamples = 12000;
        tone.resonance = 0.4f;
        qualities.push_back(sonic::renderPluginTone(tone));
        auto cpu = suite.base(std::string("plugin.cpu.") + sonic::qualityName(quality), "cpu");
        cpu.sourceClass = "complete-plugin";
        cpu.quality = sonic::qualityName(quality);
        cpu.hostRate = tone.hostRate;
        cpu.observed = num(qualities.back().renderSeconds) + " s";
        cpu.reference = "timing is not an audio gate";
        cpu.result = sonic::Classification::observed;
        cpu.notes = "Wall time includes processor construction and is not an isolated DSP benchmark.";
        suite.add(cpu);
    }
    for (size_t i = 1; i < qualities.size(); ++i)
    {
        double correlation = 0.0;
        const int lag = sonic::bestIntegerLag(qualities[0].left, qualities[i].left, 32, correlation);
        const double maxDifference = sonic::maxAbsDifference(qualities[0].left, qualities[i].left, lag);
        const double rmsDifference = sonic::rmsDifference(qualities[0].left, qualities[i].left, lag);
        const auto left = sonic::measureLevels(qualities[0].left.data(), static_cast<int>(qualities[0].left.size()));
        const auto right = sonic::measureLevels(qualities[i].left.data(), static_cast<int>(qualities[i].left.size()));
        const char* other = i == 1 ? "normal" : "eco";
        auto row = suite.base(std::string("plugin.quality.high-vs-") + other, "quality_mode");
        row.sourceClass = "complete-plugin";
        row.quality = other;
        row.hostRate = 48000.0;
        row.reference = "modes are not bit-identical; no approved difference limit";
        row.observed = "lag " + std::to_string(lag) + " rms-diff " + num(rmsDifference);
        row.result = qualities[0].finite && qualities[i].finite
            ? sonic::Classification::observed : sonic::Classification::fail;
        row.notes = "High versus the other mode on one saw note. Difference is characterization.";
        suite.add(row);
        std::ostringstream line;
        line << row.testCase << ",48000,high," << other << ',' << lag << ',' << maxDifference << ','
             << rmsDifference << ',' << sonic::db(right.rms / std::max(left.rms, 1.0e-15)) << ','
             << qualities[0].renderSeconds << ',' << qualities[i].renderSeconds << ','
             << sonic::classificationName(row.result);
        suite.report.addTableRow("quality_mode_comparison.csv", kQualityHeader, line.str());
    }

    sonic::PluginTone fine;
    fine.cutoffHz = 180.0f;
    fine.cutoffEndHz = 6000.0f;
    fine.cutoffLaw = sonic::CutoffLaw::linear;
    fine.blockSize = 1;
    fine.totalSamples = 4096;
    sonic::PluginTone coarse = fine;
    coarse.blockSize = 512;
    const auto fineRender = sonic::renderPluginTone(fine);
    const auto coarseRender = sonic::renderPluginTone(coarse);
    double correlation = 0.0;
    const int lag = sonic::bestIntegerLag(fineRender.left, coarseRender.left, 64, correlation);
    auto zipper = suite.base("plugin.cutoff.block1-vs-512", "automation");
    zipper.sourceClass = "complete-plugin";
    zipper.hostRate = 48000.0;
    zipper.observed = "rms-diff " + num(sonic::rmsDifference(fineRender.left, coarseRender.left, lag))
        + " lag " + std::to_string(lag);
    zipper.reference = "no approved zipper limit";
    zipper.result = fineRender.finite && coarseRender.finite
        ? sonic::Classification::observed : sonic::Classification::fail;
    zipper.notes = "Same linear cutoff law sampled at the host-block boundary.";
    suite.add(zipper);

    for (sonic::CutoffLaw law : {sonic::CutoffLaw::logarithmic, sonic::CutoffLaw::jump})
    {
        sonic::PluginTone tone = fine;
        tone.blockSize = 64;
        tone.cutoffLaw = law;
        const auto render = sonic::renderPluginTone(tone);
        const auto levels = sonic::measureLevels(render.left.data(), static_cast<int>(render.left.size()));
        auto row = suite.base(law == sonic::CutoffLaw::logarithmic ? "plugin.cutoff.log" : "plugin.cutoff.jump",
                              "automation");
        row.sourceClass = "complete-plugin";
        row.hostRate = tone.hostRate;
        row.blockSize = tone.blockSize;
        row.observed = "max-step " + num(levels.maxAdjacentDifference);
        row.result = render.finite ? sonic::Classification::observed : sonic::Classification::fail;
        suite.add(row);
    }
    sonic::PluginTone modulated;
    modulated.filterEnv = 63;
    modulated.cutoffHz = 400.0f;
    modulated.totalSamples = 12000;
    measurePlugin(suite, modulated, "plugin.env-cutoff", true);
    modulated.filterEnv = 0;
    modulated.filterLfo = 63;
    modulated.lfoRate = 40;
    modulated.totalSamples = 48000;
    measurePlugin(suite, modulated, "plugin.lfo-cutoff", true);
    modulated.filterLfo = 0;
    modulated.matrixCutoff = true;
    measurePlugin(suite, modulated, "plugin.matrix-cutoff", true);

    const auto wave = sonic::renderPluginTone(loud);
    sonic::writeFloatWav((directory / "plugin-extreme.wav").string(), wave.left, 48000);
    sonic::writeSpectrogramPpm((directory / "cutoff-sweep.ppm").string(), coarseRender.left, 48000, 512, 128);
}

}  // namespace

int main(int argc, char** argv)
{
    juce::ScopedJuceInitialiser_GUI juce;
    Suite suite;
    suite.extended = false;
    suite.directory = (std::filesystem::current_path() / "sonic-quality-artifacts").string();
    for (int i = 1; i < argc; ++i)
    {
        const std::string argument = argv[i];
        if (argument == "--extended")
            suite.extended = true;
        else if (argument == "--output" && i + 1 < argc)
            suite.directory = argv[++i];
    }
    suite.report.setContext(suite.commit, suite.extended ? "extended" : "quick", buildName());
    std::cout << "SWARA XT sonic quality " << (suite.extended ? "extended" : "quick")
              << " commit " << suite.commit << "\n";
    runQuick(suite);
    if (suite.extended)
        runExtended(suite, suite.directory);
    const bool wrote = suite.report.write(suite.directory);
    std::cout << "artifacts " << suite.directory << (wrote ? "" : " (write failed)") << "\n"
              << "PASS " << suite.report.count(sonic::Classification::pass)
              << " FAIL " << suite.report.count(sonic::Classification::fail)
              << " OBSERVED " << suite.report.count(sonic::Classification::observed)
              << " NEEDS_REVIEW " << suite.report.count(sonic::Classification::needsReview)
              << " NOT_APPLICABLE " << suite.report.count(sonic::Classification::notApplicable)
              << "\n";
    return wrote && suite.report.failures() == 0 ? 0 : 1;
}
