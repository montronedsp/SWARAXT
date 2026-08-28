// Copyright 2026 MontroneDSP.
// SPDX-License-Identifier: GPL-3.0-or-later
// Shruthi native timebase, raw oscillator, modulation, SRC, and chunk tests.

#include <JuceHeader.h>

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

#include "Engine/SwaraXtEngine.h"
#include "Engine/SampleRate/InternalSampleQueue.h"
#include "Plugin/SwaraXtParameterLayout.h"
#include "Plugin/PluginProcessor.h"

#include "avrlib/random.h"
#include "shruthi/audio_out.h"
#include "shruthi/midi_dispatcher.h"
#include "shruthi/part.h"
#include "shruthi/storage.h"

#ifndef SWARAXT_SHRUTHI_TIMING_TEST_MODE
#define SWARAXT_SHRUTHI_TIMING_TEST_MODE 0
#endif

#if !SWARAXT_ENABLE_SHRUTHI_DEBUG_TAPS
#error SWARAXT_ENABLE_SHRUTHI_DEBUG_TAPS must be enabled for these tests.
#endif

namespace {

namespace fs = std::filesystem;

constexpr double kInternalRate = swaraxt::SwaraXtEngine::kInternalSampleRate;
constexpr double kControlRateHz = kInternalRate / static_cast<double>(shruthi::kAudioBlockSize);
constexpr double kHostRate = 44100.0;
constexpr int kNativeBlock = shruthi::kAudioBlockSize;
constexpr int kOneSecondHostSamples = 44100;

int gFailures = 0;

struct Metrics {
    double peak = 0.0;
    double rms = 0.0;
    double mean = 0.0;
    double fundamentalHz = 0.0;
    double movingRmsCv = 0.0;
    int invalidCount = 0;
};

struct VcaBoundaryMetrics {
    double heldAttackDelta = 0.0;
    double interpolatedAttackDelta = 0.0;
    double heldReleaseDelta = 0.0;
    double interpolatedReleaseDelta = 0.0;
    double maximumGainStep = 0.0;
    double heldMaximumGainStep = 0.0;
    int transitionCount = 0;
};

struct ControlTrace {
    std::vector<int> lfo1;
    std::vector<int> lfo2;
    std::vector<int> env1;
    std::vector<int> env2;
    std::vector<int> oscCoarse;
    std::vector<int> oscFine;
    std::vector<int> cutoff;
    std::vector<int> resonance;
    std::vector<int> vca;
};

struct ProcessorCapture {
    std::vector<float> rawOsc1;
    std::vector<float> rawOsc2;
    std::vector<float> mixer;
    std::vector<float> filter;
    std::vector<float> postVca;
    std::vector<float> vcaGain;
    std::vector<float> finalHost;
    ControlTrace controls;
    int nativeBlocks = 0;
};

VcaBoundaryMetrics measureVcaBoundaries(const ProcessorCapture& capture)
{
    VcaBoundaryMetrics result;
    const size_t blocks = std::min(capture.controls.vca.size(),
                                   capture.filter.size() / static_cast<size_t>(kNativeBlock));
    for (size_t block = 1; block < blocks; ++block)
    {
        const float previousTarget = static_cast<float>(capture.controls.vca[block - 1]) / 255.0f;
        const float target = static_cast<float>(capture.controls.vca[block]) / 255.0f;
        if (target == previousTarget)
            continue;
        ++result.transitionCount;

        const size_t sample = block * static_cast<size_t>(kNativeBlock);
        const double previousOutput = static_cast<double>(capture.filter[sample - 1]) * previousTarget;
        const double heldOutput = static_cast<double>(capture.filter[sample]) * target;
        const double interpolatedOutput = static_cast<double>(capture.postVca[sample]);
        const double heldDelta = std::abs(heldOutput - previousOutput);
        const double interpolatedDelta = std::abs(interpolatedOutput - previousOutput);
        const double gainStep = std::abs(static_cast<double>(capture.vcaGain[sample]) - previousTarget);
        result.maximumGainStep = std::max(result.maximumGainStep, gainStep);
        result.heldMaximumGainStep = std::max(
            result.heldMaximumGainStep,
            std::abs(static_cast<double>(target) - static_cast<double>(previousTarget)));
        if (target > previousTarget)
        {
            result.heldAttackDelta = std::max(result.heldAttackDelta, heldDelta);
            result.interpolatedAttackDelta = std::max(result.interpolatedAttackDelta, interpolatedDelta);
        }
        else
        {
            result.heldReleaseDelta = std::max(result.heldReleaseDelta, heldDelta);
            result.interpolatedReleaseDelta = std::max(result.interpolatedReleaseDelta, interpolatedDelta);
        }
    }
    return result;
}

struct ShruthiRuntime {
    shruthi::HostAudioRing ring;
    avrlib::Random random;
    shruthi::MidiDispatcher midi;
    shruthi::Storage storage;
    shruthi::Part part;

    void init()
    {
        random.Seed(0x21);
        ring.Init();
        part.Init(ring, random, midi, storage);
        part.ProcessBlock();
        ring.Init();
    }
};

void expect(bool ok, const char* message)
{
    if (! ok)
    {
        std::fprintf(stderr, "FAIL: %s\n", message);
        ++gFailures;
    }
}

fs::path artifactRoot()
{
    return fs::current_path() / "artifacts" / "periodic-audio-debug";
}

void ensureArtifacts()
{
    fs::create_directories(artifactRoot());
}

float u8ToFloat(uint8_t value)
{
    return (static_cast<float>(value) - 128.0f) / 128.0f;
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
        const auto q = static_cast<int16_t>(std::lround(
            std::clamp(finite, -1.0f, 1.0f) * 32767.0f));
        writeLe16(out, static_cast<uint16_t>(q));
    }
}

std::vector<float> tailAfter(const std::vector<float>& samples, size_t skip)
{
    if (skip >= samples.size())
        return {};
    return { samples.begin() + static_cast<std::ptrdiff_t>(skip), samples.end() };
}

double estimateZeroCrossHz(const std::vector<float>& samples, double sampleRate, size_t skip)
{
    if (samples.size() < skip + 16)
        return 0.0;

    double mean = 0.0;
    for (size_t i = skip; i < samples.size(); ++i)
        mean += std::isfinite(samples[i]) ? samples[i] : 0.0f;
    mean /= static_cast<double>(samples.size() - skip);

    double first = -1.0;
    double last = -1.0;
    int crossings = 0;
    double prev = static_cast<double>(samples[skip]) - mean;
    for (size_t i = skip + 1; i < samples.size(); ++i)
    {
        const double current = static_cast<double>(samples[i]) - mean;
        if (prev < 0.0 && current >= 0.0)
        {
            const double denom = std::max(1.0e-12, current - prev);
            const double frac = -prev / denom;
            const double crossing = static_cast<double>(i - 1) + frac;
            if (first < 0.0)
                first = crossing;
            last = crossing;
            ++crossings;
        }
        prev = current;
    }

    if (crossings < 2 || last <= first)
        return 0.0;

    return static_cast<double>(crossings - 1) * sampleRate / (last - first);
}

double estimateTraceHz(const std::vector<int>& values, double sampleRate, int center, size_t skip)
{
    if (values.size() < skip + 16)
        return 0.0;

    double first = -1.0;
    double last = -1.0;
    int crossings = 0;
    double prev = static_cast<double>(values[skip] - center);
    for (size_t i = skip + 1; i < values.size(); ++i)
    {
        const double current = static_cast<double>(values[i] - center);
        if (prev < 0.0 && current >= 0.0)
        {
            const double denom = std::max(1.0e-12, current - prev);
            const double frac = -prev / denom;
            const double crossing = static_cast<double>(i - 1) + frac;
            if (first < 0.0)
                first = crossing;
            last = crossing;
            ++crossings;
        }
        prev = current;
    }

    if (crossings < 2 || last <= first)
        return 0.0;
    return static_cast<double>(crossings - 1) * sampleRate / (last - first);
}

double movingRmsCv(const std::vector<float>& samples, size_t window)
{
    if (samples.size() < window * 4 || window == 0)
        return 0.0;

    std::vector<double> rmsValues;
    for (size_t offset = samples.size() / 4; offset + window <= samples.size(); offset += window)
    {
        double sumSq = 0.0;
        for (size_t i = 0; i < window; ++i)
        {
            const float s = std::isfinite(samples[offset + i]) ? samples[offset + i] : 0.0f;
            sumSq += static_cast<double>(s) * static_cast<double>(s);
        }
        rmsValues.push_back(std::sqrt(sumSq / static_cast<double>(window)));
    }

    if (rmsValues.empty())
        return 0.0;

    const double mean = std::accumulate(rmsValues.begin(), rmsValues.end(), 0.0)
        / static_cast<double>(rmsValues.size());
    if (mean <= 1.0e-12)
        return 0.0;

    double variance = 0.0;
    for (double value : rmsValues)
    {
        const double d = value - mean;
        variance += d * d;
    }
    variance /= static_cast<double>(rmsValues.size());
    return std::sqrt(variance) / mean;
}

Metrics measure(const std::vector<float>& samples, double sampleRate)
{
    Metrics m;
    if (samples.empty())
        return m;

    double sum = 0.0;
    double sumSq = 0.0;
    for (float sample : samples)
    {
        if (! std::isfinite(sample))
        {
            ++m.invalidCount;
            sample = 0.0f;
        }
        m.peak = std::max(m.peak, static_cast<double>(std::fabs(sample)));
        sum += sample;
        sumSq += static_cast<double>(sample) * static_cast<double>(sample);
    }

    m.mean = sum / static_cast<double>(samples.size());
    m.rms = std::sqrt(sumSq / static_cast<double>(samples.size()));
    m.fundamentalHz = estimateZeroCrossHz(samples, sampleRate, samples.size() / 4);
    m.movingRmsCv = movingRmsCv(samples, 1024);
    return m;
}

double maxAbsDiff(const std::vector<float>& a, const std::vector<float>& b, size_t skip)
{
    const size_t n = std::min(a.size(), b.size());
    if (skip >= n)
        return 0.0;

    double maxDiff = 0.0;
    for (size_t i = skip; i < n; ++i)
        maxDiff = std::max(maxDiff, static_cast<double>(std::fabs(a[i] - b[i])));
    return maxDiff;
}

double rmsDiff(const std::vector<float>& a, const std::vector<float>& b, size_t skip)
{
    const size_t n = std::min(a.size(), b.size());
    if (skip >= n)
        return 0.0;

    double sumSq = 0.0;
    for (size_t i = skip; i < n; ++i)
    {
        const double d = static_cast<double>(a[i]) - static_cast<double>(b[i]);
        sumSq += d * d;
    }
    return std::sqrt(sumSq / static_cast<double>(n - skip));
}

double exactRepeatFraction(const std::vector<float>& samples, int lag, size_t skip)
{
    if (lag <= 0 || samples.size() <= skip + static_cast<size_t>(lag))
        return 0.0;

    int matches = 0;
    int count = 0;
    for (size_t i = skip; i + static_cast<size_t>(lag) < samples.size(); ++i)
    {
        if (std::fabs(samples[i] - samples[i + static_cast<size_t>(lag)]) < 1.0e-8f)
            ++matches;
        ++count;
    }
    return count > 0 ? static_cast<double>(matches) / static_cast<double>(count) : 0.0;
}

void writeMetricsHeader(std::ofstream& out)
{
    out << "name,sample_rate,peak,rms,mean,fundamental_hz,moving_rms_cv,invalid_count,native_blocks,lfo_hz,notes\n";
}

void writeMetricsRow(std::ofstream& out,
                     const std::string& name,
                     double sampleRate,
                     const Metrics& m,
                     int nativeBlocks,
                     double lfoHz,
                     const std::string& notes)
{
    out << name << ','
        << sampleRate << ','
        << m.peak << ','
        << m.rms << ','
        << m.mean << ','
        << m.fundamentalHz << ','
        << m.movingRmsCv << ','
        << m.invalidCount << ','
        << nativeBlocks << ','
        << lfoHz << ','
        << notes << '\n';
}

template <typename T>
std::pair<T, T> minMax(const std::vector<T>& values)
{
    if (values.empty())
        return { T {}, T {} };
    const auto result = std::minmax_element(values.begin(), values.end());
    return { *result.first, *result.second };
}

int firstIndexAtLeast(const std::vector<int>& values, int threshold)
{
    for (size_t i = 0; i < values.size(); ++i)
        if (values[i] >= threshold)
            return static_cast<int>(i);
    return -1;
}

void setInt(SwaraXtAudioProcessor& proc, const char* id, int value)
{
    if (auto* param = dynamic_cast<juce::AudioParameterInt*>(proc.getApvts().getParameter(id)))
        *param = value;
    else
        expect(false, id);
}

void setFloat(SwaraXtAudioProcessor& proc, const char* id, float value)
{
    if (auto* param = dynamic_cast<juce::AudioParameterFloat*>(proc.getApvts().getParameter(id)))
        *param = value;
    else
        expect(false, id);
}

void setModRow(SwaraXtAudioProcessor& proc, int row, int source, int destination, int amount)
{
    const std::string prefix = "mod.row" + std::to_string(row) + ".";
    setInt(proc, (prefix + "source").c_str(), source);
    setInt(proc, (prefix + "destination").c_str(), destination);
    setInt(proc, (prefix + "amount").c_str(), amount);
}

void zeroProcessorMatrix(SwaraXtAudioProcessor& proc)
{
    for (int row = 1; row <= shruthi::kModulationMatrixSize; ++row)
        setModRow(proc, row, shruthi::MOD_SRC_OFFSET, shruthi::MOD_DST_VCA, 0);
}

void configureNeutralProcessor(SwaraXtAudioProcessor& proc,
                               int waveform,
                               float cutoffHz = 18000.0f,
                               int env2Attack = 0)
{
    setFloat(proc, swaraxt::IDs::master, 1.0f);
    setInt(proc, swaraxt::IDs::osc1Shape, waveform);
    setInt(proc, swaraxt::IDs::osc1Param, waveform == shruthi::WAVEFORM_SQUARE ? 64 : 0);
    setInt(proc, swaraxt::IDs::osc1Range, 0);
    setInt(proc, swaraxt::IDs::osc1Option, 0);
    setInt(proc, swaraxt::IDs::osc2Shape, shruthi::WAVEFORM_NONE);
    setInt(proc, swaraxt::IDs::osc2Param, 0);
    setInt(proc, swaraxt::IDs::osc2Range, 0);
    setInt(proc, swaraxt::IDs::osc2Option, 0);
    setInt(proc, swaraxt::IDs::mixBalance, 0);
    setInt(proc, swaraxt::IDs::mixSub, 0);
    setInt(proc, swaraxt::IDs::mixNoise, 0);
    setInt(proc, swaraxt::IDs::mixSubShape, 0);
    setFloat(proc, swaraxt::IDs::filterCutoff, cutoffHz);
    setFloat(proc, swaraxt::IDs::filterResonance, 0.0f);
    setFloat(proc, swaraxt::IDs::filterEnvAmount, 0.0f);
    setFloat(proc, swaraxt::IDs::filterKeyTracking, 0.0f);
    setFloat(proc, swaraxt::IDs::filterModAmount, 0.0f);
    setInt(proc, swaraxt::IDs::env1Attack, 0);
    setInt(proc, swaraxt::IDs::env1Decay, 0);
    setInt(proc, swaraxt::IDs::env1Sustain, 127);
    setInt(proc, swaraxt::IDs::env1Release, 0);
    setInt(proc, swaraxt::IDs::env2Attack, env2Attack);
    setInt(proc, swaraxt::IDs::env2Decay, 0);
    setInt(proc, swaraxt::IDs::env2Sustain, 127);
    setInt(proc, swaraxt::IDs::env2Release, 0);
    setInt(proc, swaraxt::IDs::lfo1Wave, shruthi::LFO_WAVEFORM_TRIANGLE);
    setInt(proc, swaraxt::IDs::lfo1Rate, 80);
    setInt(proc, swaraxt::IDs::lfo1Attack, 0);
    setInt(proc, swaraxt::IDs::lfo1Retrig, shruthi::LFO_MODE_FREE);
    setInt(proc, swaraxt::IDs::lfo2Wave, shruthi::LFO_WAVEFORM_TRIANGLE);
    setInt(proc, swaraxt::IDs::lfo2Rate, 3);
    setInt(proc, swaraxt::IDs::lfo2Attack, 0);
    setInt(proc, swaraxt::IDs::lfo2Retrig, shruthi::LFO_MODE_FREE);
    setInt(proc, swaraxt::IDs::perfGlide, 0);
    setInt(proc, swaraxt::IDs::perfLegato, 0);
    setInt(proc, swaraxt::IDs::midiChannel, 1);
    setInt(proc, swaraxt::IDs::seqMode, shruthi::SEQUENCER_MODE_STEP);
    setInt(proc, swaraxt::IDs::seqTempo, 120);
    zeroProcessorMatrix(proc);
}

void configureLfoPitchProcessor(SwaraXtAudioProcessor& proc, int waveform)
{
    configureNeutralProcessor(proc, waveform);
    setModRow(proc,
              1,
              shruthi::MOD_SRC_LFO_1,
              shruthi::MOD_DST_VCO_1_2_COARSE,
              16);
}

void zeroPartMatrix(shruthi::Part& part)
{
    auto* patch = part.mutable_patch();
    for (int row = 0; row < shruthi::kModulationMatrixSize; ++row)
    {
        patch->modulation_matrix.modulation[row].source = shruthi::MOD_SRC_OFFSET;
        patch->modulation_matrix.modulation[row].destination = shruthi::MOD_DST_VCA;
        patch->modulation_matrix.modulation[row].amount = 0;
    }
}

void configureNeutralPart(shruthi::Part& part, int waveform, int env2Attack = 0)
{
    auto* patch = part.mutable_patch();
    patch->osc[0].shape = static_cast<uint8_t>(waveform);
    patch->osc[0].parameter = waveform == shruthi::WAVEFORM_SQUARE ? 64 : 0;
    patch->osc[0].range = 0;
    patch->osc[0].option = 0;
    patch->osc[1].shape = shruthi::WAVEFORM_NONE;
    patch->osc[1].parameter = 0;
    patch->osc[1].range = 0;
    patch->osc[1].option = 0;
    patch->mix_balance = 0;
    patch->mix_sub_osc = 0;
    patch->mix_noise = 0;
    patch->mix_sub_osc_shape = 0;
    patch->filter_cutoff = 127;
    patch->filter_resonance = 0;
    patch->filter_env = 0;
    patch->filter_lfo = 0;
    patch->env[0].attack = 0;
    patch->env[0].decay = 0;
    patch->env[0].sustain = 127;
    patch->env[0].release = 0;
    patch->env[1].attack = static_cast<uint8_t>(env2Attack);
    patch->env[1].decay = 0;
    patch->env[1].sustain = 127;
    patch->env[1].release = 0;
    part.mutable_voice()->RefreshEnvelopeRatesFromPatch();
    patch->lfo[0].waveform = shruthi::LFO_WAVEFORM_TRIANGLE;
    patch->lfo[0].rate = 80;
    patch->lfo[0].attack = 0;
    patch->lfo[0].retrigger_mode = shruthi::LFO_MODE_FREE;
    patch->lfo[1].waveform = shruthi::LFO_WAVEFORM_TRIANGLE;
    patch->lfo[1].rate = 3;
    patch->lfo[1].attack = 0;
    patch->lfo[1].retrigger_mode = shruthi::LFO_MODE_FREE;
    zeroPartMatrix(part);
    auto* seq = part.mutable_sequencer_settings();
    seq->seq_mode = shruthi::SEQUENCER_MODE_STEP;
    seq->seq_tempo = 120;
    part.Touch(false);
}

void configureLfoPitchPart(shruthi::Part& part, int waveform)
{
    configureNeutralPart(part, waveform);
    auto* patch = part.mutable_patch();
    patch->modulation_matrix.modulation[0].source = shruthi::MOD_SRC_LFO_1;
    patch->modulation_matrix.modulation[0].destination = shruthi::MOD_DST_VCO_1_2_COARSE;
    patch->modulation_matrix.modulation[0].amount = 16;
    part.Touch(false);
}

std::vector<float> renderDirectRawOsc1(ShruthiRuntime& runtime, int blocks, int note)
{
    runtime.ring.Init();
    runtime.part.NoteOn(0, static_cast<uint8_t>(note), 100);
    std::vector<float> out;
    out.reserve(static_cast<size_t>(blocks * kNativeBlock));
    for (int block = 0; block < blocks; ++block)
    {
        runtime.part.ProcessBlock();
        const auto* raw = runtime.part.voice().debug_osc1_buffer();
        for (int i = 0; i < kNativeBlock; ++i)
            out.push_back(u8ToFloat(raw[i]));
    }
    return out;
}

ControlTrace renderDirectControls(ShruthiRuntime& runtime, int blocks, int note)
{
    runtime.ring.Init();
    runtime.part.NoteOn(0, static_cast<uint8_t>(note), 100);
    ControlTrace trace;
    for (int block = 0; block < blocks; ++block)
    {
        runtime.part.ProcessBlock();
        const auto& voice = runtime.part.voice();
        trace.lfo1.push_back(voice.modulation_source(shruthi::MOD_SRC_LFO_1));
        trace.lfo2.push_back(voice.modulation_source(shruthi::MOD_SRC_LFO_2));
        trace.env1.push_back(voice.modulation_source(shruthi::MOD_SRC_ENV_1));
        trace.env2.push_back(voice.modulation_source(shruthi::MOD_SRC_ENV_2));
        trace.oscCoarse.push_back(voice.modulation_destination(shruthi::MOD_DST_VCO_1_2_COARSE));
        trace.oscFine.push_back(voice.modulation_destination(shruthi::MOD_DST_VCO_1_2_FINE));
        trace.cutoff.push_back(voice.cutoff());
        trace.resonance.push_back(voice.resonance());
        trace.vca.push_back(voice.vca());
    }
    return trace;
}

void debugSink(void* context, const swaraxt::SwaraXtEngine::DebugBlockCapture& block)
{
    auto* capture = static_cast<ProcessorCapture*>(context);
    ++capture->nativeBlocks;
    capture->controls.lfo1.push_back(block.lfo1);
    capture->controls.lfo2.push_back(block.lfo2);
    capture->controls.env1.push_back(block.env1);
    capture->controls.env2.push_back(block.env2);
    capture->controls.oscCoarse.push_back(block.oscCoarse);
    capture->controls.oscFine.push_back(block.oscFine);
    capture->controls.cutoff.push_back(block.cutoff);
    capture->controls.resonance.push_back(block.resonance);
    capture->controls.vca.push_back(block.vca);
    for (int i = 0; i < block.samples; ++i)
    {
        capture->rawOsc1.push_back(block.rawOsc1[i]);
        capture->rawOsc2.push_back(block.rawOsc2[i]);
        capture->mixer.push_back(block.postShruthiMixer[i]);
        capture->filter.push_back(block.filterOutput[i]);
        capture->postVca.push_back(block.postVca[i]);
        capture->vcaGain.push_back(block.vcaGain[i]);
    }
}

ProcessorCapture renderProcessorCapture(double hostRate,
                                        const std::vector<int>& schedule,
                                        int totalSamples,
                                        bool noteOn,
                                        void (*configure)(SwaraXtAudioProcessor&))
{
    SwaraXtAudioProcessor proc;
    configure(proc);

    ProcessorCapture capture;
    proc.engineForTests().setDebugTapSink(&capture, debugSink);
    const int maxBlock = *std::max_element(schedule.begin(), schedule.end());
    proc.prepareToPlay(hostRate, maxBlock);

    int rendered = 0;
    size_t scheduleIndex = 0;
    while (rendered < totalSamples)
    {
        int n = schedule[scheduleIndex % schedule.size()];
        if (rendered + n > totalSamples)
            n = totalSamples - rendered;

        juce::AudioBuffer<float> buffer(2, n);
        juce::MidiBuffer midi;
        if (noteOn && rendered == 0)
            midi.addEvent(juce::MidiMessage::noteOn(1, 69, (juce::uint8) 100), 0);

        buffer.clear();
        proc.processBlock(buffer, midi);
        for (int i = 0; i < n; ++i)
            capture.finalHost.push_back(buffer.getSample(0, i));

        rendered += n;
        ++scheduleIndex;
    }
    proc.releaseResources();
    return capture;
}

ProcessorCapture renderEnvelopeEventCapture(double hostRate,
                                            const std::vector<int>& schedule,
                                            int waveform,
                                            int noteOffSample,
                                            int retriggerSample)
{
    SwaraXtAudioProcessor proc;
    configureNeutralProcessor(proc, waveform);
    setModRow(proc, 9, shruthi::MOD_SRC_ENV_2, shruthi::MOD_DST_VCA, 63);

    ProcessorCapture capture;
    proc.engineForTests().setDebugTapSink(&capture, debugSink);
    proc.prepareToPlay(hostRate, *std::max_element(schedule.begin(), schedule.end()));

    constexpr int noteOnSample = 512;
    const int totalSamples = std::max(noteOffSample, retriggerSample) + 1024;
    int rendered = 0;
    size_t scheduleIndex = 0;
    while (rendered < totalSamples)
    {
        int n = schedule[scheduleIndex % schedule.size()];
        if (rendered + n > totalSamples)
            n = totalSamples - rendered;

        juce::AudioBuffer<float> buffer(2, n);
        juce::MidiBuffer midi;
        const auto addIfInBlock = [&](int eventSample, const juce::MidiMessage& message) {
            if (eventSample >= rendered && eventSample < rendered + n)
                midi.addEvent(message, eventSample - rendered);
        };
        addIfInBlock(noteOnSample, juce::MidiMessage::noteOn(1, 69, (juce::uint8) 100));
        if (noteOffSample > noteOnSample)
            addIfInBlock(noteOffSample, juce::MidiMessage::noteOff(1, 69));
        if (retriggerSample > noteOnSample)
            addIfInBlock(retriggerSample, juce::MidiMessage::noteOn(1, 69, (juce::uint8) 100));

        buffer.clear();
        proc.processBlock(buffer, midi);
        for (int i = 0; i < n; ++i)
            capture.finalHost.push_back(buffer.getSample(0, i));
        rendered += n;
        ++scheduleIndex;
    }
    proc.releaseResources();
    return capture;
}

ProcessorCapture renderRapidEnvelopeCapture(double hostRate,
                                             const std::vector<int>& schedule,
                                             int waveform)
{
    SwaraXtAudioProcessor proc;
    configureNeutralProcessor(proc, waveform);
    setModRow(proc, 9, shruthi::MOD_SRC_ENV_2, shruthi::MOD_DST_VCA, 63);

    ProcessorCapture capture;
    proc.engineForTests().setDebugTapSink(&capture, debugSink);
    proc.prepareToPlay(hostRate, *std::max_element(schedule.begin(), schedule.end()));

    constexpr int firstNoteSample = 512;
    constexpr int cycleSamples = 768;
    constexpr int gateSamples = 384;
    constexpr int cycles = 16;
    constexpr int totalSamples = firstNoteSample + cycleSamples * cycles + 512;
    int rendered = 0;
    size_t scheduleIndex = 0;
    while (rendered < totalSamples)
    {
        int n = schedule[scheduleIndex % schedule.size()];
        if (rendered + n > totalSamples)
            n = totalSamples - rendered;
        juce::AudioBuffer<float> buffer(2, n);
        juce::MidiBuffer midi;
        for (int cycle = 0; cycle < cycles; ++cycle)
        {
            const int noteOnSample = firstNoteSample + cycle * cycleSamples;
            const int noteOffSample = noteOnSample + gateSamples;
            if (noteOnSample >= rendered && noteOnSample < rendered + n)
                midi.addEvent(juce::MidiMessage::noteOn(1, 69, (juce::uint8) 100), noteOnSample - rendered);
            if (noteOffSample >= rendered && noteOffSample < rendered + n)
                midi.addEvent(juce::MidiMessage::noteOff(1, 69), noteOffSample - rendered);
        }
        buffer.clear();
        proc.processBlock(buffer, midi);
        for (int i = 0; i < n; ++i)
            capture.finalHost.push_back(buffer.getSample(0, i));
        rendered += n;
        ++scheduleIndex;
    }
    proc.releaseResources();
    return capture;
}

void configureNeutralSaw(SwaraXtAudioProcessor& proc) { configureNeutralProcessor(proc, shruthi::WAVEFORM_SAW); }
void configureNeutralSquare(SwaraXtAudioProcessor& proc) { configureNeutralProcessor(proc, shruthi::WAVEFORM_SQUARE); }
void configureMediumSaw(SwaraXtAudioProcessor& proc) { configureNeutralProcessor(proc, shruthi::WAVEFORM_SAW, 1200.0f); }
void configureAttackSaw(SwaraXtAudioProcessor& proc) { configureNeutralProcessor(proc, shruthi::WAVEFORM_SAW, 18000.0f, 64); }
void configureLfoSaw(SwaraXtAudioProcessor& proc) { configureLfoPitchProcessor(proc, shruthi::WAVEFORM_SAW); }
void configureShortFilterEnvelope(SwaraXtAudioProcessor& proc)
{
    configureNeutralProcessor(proc, shruthi::WAVEFORM_SAW, 500.0f);
    setInt(proc, swaraxt::IDs::env1Attack, 0);
    setInt(proc, swaraxt::IDs::env1Decay, 0);
    setInt(proc, swaraxt::IDs::env1Sustain, 0);
    setFloat(proc, swaraxt::IDs::filterEnvAmount, 1.0f);
}

void configureShortResonantFilterEnvelope(SwaraXtAudioProcessor& proc)
{
    configureShortFilterEnvelope(proc);
    setFloat(proc, swaraxt::IDs::filterResonance, 0.95f);
}

void configureNegativeAttackModulation(SwaraXtAudioProcessor& proc)
{
    configureNeutralProcessor(proc, shruthi::WAVEFORM_SAW, 18000.0f, 64);
    setModRow(proc, 1, shruthi::MOD_SRC_OFFSET, shruthi::MOD_DST_ATTACK, -63);
}

void configureGatedSilenceSaw(SwaraXtAudioProcessor& proc)
{
    configureNeutralProcessor(proc, shruthi::WAVEFORM_SAW);
    setModRow(proc, 9, shruthi::MOD_SRC_ENV_2, shruthi::MOD_DST_VCA, 63);
}

void renderNativeSineBlock(swaraxt::InternalSampleQueue& queue, int& index, double frequency)
{
    for (int i = 0; i < kNativeBlock; ++i)
    {
        const double phase = 2.0 * juce::MathConstants<double>::pi
            * frequency * static_cast<double>(index++) / kInternalRate;
        queue.push(static_cast<float>(std::sin(phase) * 0.5));
    }
}

std::vector<float> renderSineThroughQueue(double hostRate,
                                          const std::vector<int>& schedule,
                                          int totalSamples,
                                          double frequency)
{
    swaraxt::InternalSampleQueue queue;
    queue.reset();
    queue.setStep(kInternalRate, hostRate);
    int internalIndex = 0;
    std::vector<float> out;
    out.reserve(static_cast<size_t>(totalSamples));

    int rendered = 0;
    size_t scheduleIndex = 0;
    while (rendered < totalSamples)
    {
        while (queue.size() < std::max(8, queue.minimumReadableSize()))
            renderNativeSineBlock(queue, internalIndex, frequency);

        int n = schedule[scheduleIndex % schedule.size()];
        if (rendered + n > totalSamples)
            n = totalSamples - rendered;

        for (int i = 0; i < n; ++i)
        {
            while (queue.size() < std::max(8, queue.minimumReadableSize()))
                renderNativeSineBlock(queue, internalIndex, frequency);
            out.push_back(queue.readInterpolated());
        }

        rendered += n;
        ++scheduleIndex;
    }
    return out;
}

void runShruthiTiming()
{
    ensureArtifacts();
    std::ofstream metrics(artifactRoot() / "timing_metrics.csv", std::ios::trunc);
    writeMetricsHeader(metrics);

    expect(std::fabs(kInternalRate - (20000000.0 / 510.0)) < 1.0e-9,
           "Shruthi internal sample rate is fixed");
    expect(shruthi::kControlRate == 40, "Shruthi control rate is 40 samples");
    expect(shruthi::kAudioBlockSize == 40, "Shruthi audio block is 40 samples");

    ShruthiRuntime direct;
    direct.init();
    configureNeutralPart(direct.part, shruthi::WAVEFORM_SAW);
    const auto raw = renderDirectRawOsc1(direct, 128, 69);
    expect(raw.size() == 128u * static_cast<size_t>(kNativeBlock),
           "direct Part::ProcessBlock produces 40 raw samples per call");
    expect(direct.part.voice().debug_process_block_count() >= 128,
           "voice debug block counter advances per native block");
    writeMetricsRow(metrics, "direct_native_raw_saw", kInternalRate, measure(raw, kInternalRate),
                    128, 0.0, "128 explicit native blocks");

    const std::vector<std::vector<int>> schedules = {
        { 64 },
        { 128 },
        { 257 },
        { 31, 33, 64 }
    };
    int expectedBlocks = -1;
    for (const auto& schedule : schedules)
    {
        const auto capture = renderProcessorCapture(kHostRate, schedule, kOneSecondHostSamples, true, configureNeutralSaw);
        const auto m = measure(capture.rawOsc1, kInternalRate);
        const double lfoHz = estimateTraceHz(capture.controls.lfo1, kControlRateHz, 128, 32);
        writeMetricsRow(metrics,
                        "host_block_" + std::to_string(schedule.front()),
                        kInternalRate,
                        m,
                        capture.nativeBlocks,
                        lfoHz,
                        "native blocks rendered by real processor path");
        expect(m.invalidCount == 0, "host raw capture finite");
        expect(m.fundamentalHz > 435.0 && m.fundamentalHz < 445.0,
               "host raw oscillator pitch remains A4");
        if (expectedBlocks < 0)
            expectedBlocks = capture.nativeBlocks;
        expect(std::abs(capture.nativeBlocks - expectedBlocks) <= 1,
               "native block count is host-block invariant");
    }

    const double hostRates[] = { 44100.0, 48000.0, 88200.0, 96000.0, 176400.0, 192000.0 };
    const int hostBlocks[] = { 63, 64, 65, 127, 128, 129, 257, 512 };
    int matrixExpectedBlocks = -1;
    for (double hostRate : hostRates)
    {
        for (int hostBlock : hostBlocks)
        {
            const auto capture = renderProcessorCapture(
                hostRate, { hostBlock }, static_cast<int>(hostRate), true, configureNeutralSaw);
            const auto rawMetrics = measure(capture.rawOsc1, kInternalRate);
            const auto hostMetrics = measure(capture.finalHost, hostRate);
            writeMetricsRow(metrics,
                            "matrix_" + std::to_string(static_cast<int>(hostRate))
                                + "_block_" + std::to_string(hostBlock),
                            kInternalRate,
                            rawMetrics,
                            capture.nativeBlocks,
                            estimateTraceHz(capture.controls.lfo1, kControlRateHz, 128, 32),
                            "real processor sample-rate/block-size matrix");
            expect(rawMetrics.invalidCount == 0 && hostMetrics.invalidCount == 0,
                   "processor matrix render remains finite");
            expect(rawMetrics.fundamentalHz > 435.0 && rawMetrics.fundamentalHz < 445.0,
                   "processor matrix raw oscillator pitch remains A4");
            expect(hostMetrics.peak > 1.0e-4,
                   "processor matrix final output remains non-silent");
            expect(std::fabs(hostMetrics.mean) < 0.05,
                   "processor matrix final output has controlled DC");
            if (matrixExpectedBlocks < 0)
                matrixExpectedBlocks = capture.nativeBlocks;
            expect(std::abs(capture.nativeBlocks - matrixExpectedBlocks) <= 1,
                   "native block count is sample-rate and host-block invariant");
        }
    }
}

void runRawOscillatorParity()
{
    ensureArtifacts();
    std::ofstream metrics(artifactRoot() / "raw_oscillator_metrics.csv", std::ios::trunc);
    writeMetricsHeader(metrics);

    ShruthiRuntime sawDirect;
    sawDirect.init();
    configureNeutralPart(sawDirect.part, shruthi::WAVEFORM_SAW);
    const auto directSaw = renderDirectRawOsc1(sawDirect, 1200, 69);
    const auto directSawMetrics = measure(directSaw, kInternalRate);
    writeMetricsRow(metrics, "direct_neutral_raw_saw", kInternalRate, directSawMetrics, 1200, 0.0, "adapted native Part harness");
    expect(directSawMetrics.invalidCount == 0, "direct raw saw finite");
    expect(directSawMetrics.fundamentalHz > 435.0 && directSawMetrics.fundamentalHz < 445.0,
           "direct raw saw pitch is A4");

    ShruthiRuntime squareDirect;
    squareDirect.init();
    configureNeutralPart(squareDirect.part, shruthi::WAVEFORM_SQUARE);
    const auto directSquare = renderDirectRawOsc1(squareDirect, 1200, 69);
    const auto directSquareMetrics = measure(directSquare, kInternalRate);
    writeMetricsRow(metrics, "direct_neutral_raw_square", kInternalRate, directSquareMetrics, 1200, 0.0, "adapted native Part harness");
    expect(directSquareMetrics.invalidCount == 0, "direct raw square finite");
    expect(directSquareMetrics.fundamentalHz > 435.0 && directSquareMetrics.fundamentalHz < 445.0,
           "direct raw square pitch is A4");

    const auto before = renderProcessorCapture(kHostRate, { 128 }, kOneSecondHostSamples, true, configureLfoSaw);
    const auto afterSaw = renderProcessorCapture(kHostRate, { 128 }, kOneSecondHostSamples, true, configureNeutralSaw);
    const auto afterSquare = renderProcessorCapture(kHostRate, { 128 }, kOneSecondHostSamples, true, configureNeutralSquare);
    const auto medium = renderProcessorCapture(kHostRate, { 128 }, kOneSecondHostSamples, true, configureMediumSaw);
    const auto silence = renderProcessorCapture(kHostRate, { 128 }, kOneSecondHostSamples, false, configureGatedSilenceSaw);

    writeWav(artifactRoot() / "before_raw_saw.wav", before.rawOsc1, static_cast<int>(kInternalRate));
    writeWav(artifactRoot() / "after_raw_saw.wav", afterSaw.rawOsc1, static_cast<int>(kInternalRate));
    writeWav(artifactRoot() / "after_raw_square.wav", afterSquare.rawOsc1, static_cast<int>(kInternalRate));
    writeWav(artifactRoot() / "after_mixer.wav", afterSaw.mixer, static_cast<int>(kInternalRate));
    writeWav(artifactRoot() / "after_filter_open.wav", afterSaw.filter, static_cast<int>(kInternalRate));
    writeWav(artifactRoot() / "after_filter_medium.wav", medium.filter, static_cast<int>(kInternalRate));
    writeWav(artifactRoot() / "silence.wav", silence.finalHost, static_cast<int>(kHostRate));

    const auto beforeM = measure(before.rawOsc1, kInternalRate);
    const auto afterM = measure(afterSaw.rawOsc1, kInternalRate);
    const auto squareM = measure(afterSquare.rawOsc1, kInternalRate);
    writeMetricsRow(metrics, "before_raw_saw_lfo_pitch_route", kInternalRate, beforeM, before.nativeBlocks,
                    estimateTraceHz(before.controls.lfo1, kControlRateHz, 128, 32), "LFO1 to oscillator coarse amount 16");
    writeMetricsRow(metrics, "after_raw_saw_neutral", kInternalRate, afterM, afterSaw.nativeBlocks,
                    estimateTraceHz(afterSaw.controls.lfo1, kControlRateHz, 128, 32), "all matrix amounts zero");
    writeMetricsRow(metrics, "after_raw_square_neutral", kInternalRate, squareM, afterSquare.nativeBlocks,
                    estimateTraceHz(afterSquare.controls.lfo1, kControlRateHz, 128, 32), "all matrix amounts zero");
    writeMetricsRow(metrics, "silence_final_host", kHostRate, measure(silence.finalHost, kHostRate),
                    silence.nativeBlocks, 0.0, "no note");

    const auto coarseBefore = minMax(before.controls.oscCoarse);
    const auto coarseAfter = minMax(afterSaw.controls.oscCoarse);
    expect(coarseBefore.first != coarseBefore.second,
           "explicit LFO pitch route moves oscillator coarse destination");
    expect(coarseAfter.first == coarseAfter.second,
           "neutral raw saw has constant oscillator coarse destination");
    expect(afterM.fundamentalHz > 435.0 && afterM.fundamentalHz < 445.0,
           "processor neutral raw saw pitch is A4");
    expect(squareM.fundamentalHz > 435.0 && squareM.fundamentalHz < 445.0,
           "processor neutral raw square pitch is A4");
    expect(measure(silence.finalHost, kHostRate).peak < 1.0e-5,
           "no-note final host output remains silent");
}

void runControlRate()
{
    ensureArtifacts();
    std::ofstream metrics(artifactRoot() / "control_rate_metrics.csv", std::ios::trunc);
    metrics << "name,blocks,lfo_hz,env2_attack_block,osc_coarse_min,osc_coarse_max,vca_min,vca_max\n";

    ShruthiRuntime directLfo;
    directLfo.init();
    configureNeutralPart(directLfo.part, shruthi::WAVEFORM_SAW);
    const auto directTrace = renderDirectControls(directLfo, 4000, 69);
    const double directLfoHz = estimateTraceHz(directTrace.lfo1, kControlRateHz, 128, 32);

    const auto capture64 = renderProcessorCapture(kHostRate, { 64 }, kOneSecondHostSamples * 2, true, configureNeutralSaw);
    const auto capture128 = renderProcessorCapture(kHostRate, { 128 }, kOneSecondHostSamples * 2, true, configureNeutralSaw);
    const auto capture257 = renderProcessorCapture(kHostRate, { 257 }, kOneSecondHostSamples * 2, true, configureNeutralSaw);
    const double lfo64 = estimateTraceHz(capture64.controls.lfo1, kControlRateHz, 128, 32);
    const double lfo128 = estimateTraceHz(capture128.controls.lfo1, kControlRateHz, 128, 32);
    const double lfo257 = estimateTraceHz(capture257.controls.lfo1, kControlRateHz, 128, 32);

    auto writeTraceRow = [&](const char* name, const ProcessorCapture& capture, double lfoHz) {
        const auto coarse = minMax(capture.controls.oscCoarse);
        const auto vca = minMax(capture.controls.vca);
        metrics << name << ','
                << capture.nativeBlocks << ','
                << lfoHz << ','
                << -1 << ','
                << coarse.first << ','
                << coarse.second << ','
                << vca.first << ','
                << vca.second << '\n';
    };
    writeTraceRow("host64_neutral", capture64, lfo64);
    writeTraceRow("host128_neutral", capture128, lfo128);
    writeTraceRow("host257_neutral", capture257, lfo257);

    expect(directLfoHz > 0.1, "direct LFO frequency can be measured");
    expect(std::fabs(lfo64 - directLfoHz) / directLfoHz < 0.02, "host64 LFO rate matches native");
    expect(std::fabs(lfo128 - directLfoHz) / directLfoHz < 0.02, "host128 LFO rate matches native");
    expect(std::fabs(lfo257 - directLfoHz) / directLfoHz < 0.02, "host257 LFO rate matches native");

    ShruthiRuntime directEnv;
    directEnv.init();
    configureNeutralPart(directEnv.part, shruthi::WAVEFORM_SAW, 64);
    const auto directEnvTrace = renderDirectControls(directEnv, 800, 69);
    const auto attackCapture = renderProcessorCapture(kHostRate, { 257 }, kOneSecondHostSamples, true, configureAttackSaw);
    const int directAttack = firstIndexAtLeast(directEnvTrace.env2, 200);
    const int hostAttack = firstIndexAtLeast(attackCapture.controls.env2, 200);
    metrics << "direct_env_attack," << directEnvTrace.env2.size() << ",0," << directAttack << ",0,0,0,0\n";
    metrics << "host257_env_attack," << attackCapture.nativeBlocks << ",0," << hostAttack << ",0,0,0,0\n";
    expect(directAttack >= 0 && hostAttack >= 0, "Env2 attack timing can be measured");
    expect(std::abs(directAttack - hostAttack) <= 1, "Env2 attack advances on native control blocks");
}

void runModulationNeutrality()
{
    ensureArtifacts();
    std::ofstream metrics(artifactRoot() / "modulation_neutrality_metrics.csv", std::ios::trunc);
    writeMetricsHeader(metrics);

    const auto neutral = renderProcessorCapture(kHostRate, { 31, 33, 64 }, kOneSecondHostSamples, true, configureNeutralSaw);
    const auto m = measure(neutral.rawOsc1, kInternalRate);
    const auto coarse = minMax(neutral.controls.oscCoarse);
    const auto fine = minMax(neutral.controls.oscFine);
    const auto cutoff = minMax(neutral.controls.cutoff);
    const auto resonance = minMax(neutral.controls.resonance);
    const auto vca = minMax(neutral.controls.vca);
    writeMetricsRow(metrics, "neutral_raw_osc1", kInternalRate, m, neutral.nativeBlocks,
                    estimateTraceHz(neutral.controls.lfo1, kControlRateHz, 128, 32), "all matrix amounts zero");

    expect(coarse.first == coarse.second, "neutral oscillator coarse destination is constant");
    expect(fine.first == fine.second, "neutral oscillator fine destination is constant");
    expect(cutoff.first == cutoff.second, "neutral filter cutoff destination is constant");
    expect(resonance.first == resonance.second, "neutral resonance destination is constant");
    expect(vca.first == vca.second, "neutral VCA destination is constant");
    expect(m.fundamentalHz > 435.0 && m.fundamentalHz < 445.0,
           "neutral modulation raw oscillator stays at A4");
    expect(m.movingRmsCv < 0.08, "neutral raw oscillator has no large cyclic RMS movement");
}

void runStreamingSrc()
{
    ensureArtifacts();
    std::ofstream metrics(artifactRoot() / "streaming_src_metrics.csv", std::ios::trunc);
    writeMetricsHeader(metrics);

    const double rates[] = { 44100.0, 48000.0, 88200.0, 96000.0, 176400.0, 192000.0 };
    for (double rate : rates)
    {
        const int samples = static_cast<int>(rate);
        const auto rendered = renderSineThroughQueue(rate, { 64, 65, 257 }, samples, 440.0);
        const auto m = measure(rendered, rate);
        writeMetricsRow(metrics, "queue_sine_" + std::to_string(static_cast<int>(rate)),
                        rate, m, 0, 0.0, "internal 440 Hz sine through InternalSampleQueue");
        expect(m.invalidCount == 0, "queue sine output finite");
        expect(m.fundamentalHz > 438.0 && m.fundamentalHz < 442.0,
               "queue sine output pitch remains 440 Hz");
        expect(m.movingRmsCv < 0.03, "queue sine has no cyclic amplitude modulation");

        swaraxt::DcBlocker blocker;
        blocker.prepare(rate);
        float dcResponse = blocker.process(1.0f);
        const int responseSamples = static_cast<int>(std::lround(rate * 0.1));
        for (int i = 0; i < responseSamples; ++i)
            dcResponse = blocker.process(1.0f);
        static float referenceDcResponse = 0.0f;
        if (rate == rates[0])
            referenceDcResponse = dcResponse;
        expect(std::fabs(dcResponse - referenceDcResponse) < 1.0e-4f,
               "DC blocker has a sample-rate-invariant physical response");
    }

    const auto a = renderSineThroughQueue(44100.0, { 128 }, 8192, 440.0);
    const auto b = renderSineThroughQueue(44100.0, { 64, 64 }, 8192, 440.0);
    const auto c = renderSineThroughQueue(44100.0, { 31, 33, 64 }, 8192, 440.0);
    const auto d = renderSineThroughQueue(44100.0, { 63, 65 }, 8192, 440.0);
    expect(maxAbsDiff(a, b, 512) < 1.0e-6, "queue 128 and 64+64 renders are sample-identical");
    expect(maxAbsDiff(a, c, 512) < 1.0e-6, "queue 128 and 31+33+64 renders are sample-identical");
    expect(maxAbsDiff(a, d, 512) < 1.0e-6, "queue 128 and 63+65 renders are sample-identical");
}

void runChunkContinuity()
{
    ensureArtifacts();
    std::ofstream metrics(artifactRoot() / "chunk_continuity_metrics.csv", std::ios::trunc);
    metrics << "name,rms_diff,max_abs_diff,lag,exact_repeat_fraction\n";

    const auto one = renderProcessorCapture(kHostRate, { 128 }, 8192, true, configureNeutralSaw);
    const auto two = renderProcessorCapture(kHostRate, { 64, 64 }, 8192, true, configureNeutralSaw);
    const auto three = renderProcessorCapture(kHostRate, { 31, 33, 64 }, 8192, true, configureNeutralSaw);
    const auto four = renderProcessorCapture(kHostRate, { 63, 65 }, 8192, true, configureNeutralSaw);

    const double d12 = rmsDiff(one.finalHost, two.finalHost, 1024);
    const double d13 = rmsDiff(one.finalHost, three.finalHost, 1024);
    const double m12 = maxAbsDiff(one.finalHost, two.finalHost, 1024);
    const double m13 = maxAbsDiff(one.finalHost, three.finalHost, 1024);
    const double d14 = rmsDiff(one.finalHost, four.finalHost, 1024);
    const double m14 = maxAbsDiff(one.finalHost, four.finalHost, 1024);
    metrics << "128_vs_64_64," << d12 << ',' << m12 << ",0,0\n";
    metrics << "128_vs_31_33_64," << d13 << ',' << m13 << ",0,0\n";
    metrics << "128_vs_63_65," << d14 << ',' << m14 << ",0,0\n";
    metrics << "raw_128_vs_64_64," << rmsDiff(one.rawOsc1, two.rawOsc1, 0) << ','
            << maxAbsDiff(one.rawOsc1, two.rawOsc1, 0) << ",0,0\n";
    metrics << "mixer_128_vs_64_64," << rmsDiff(one.mixer, two.mixer, 0) << ','
            << maxAbsDiff(one.mixer, two.mixer, 0) << ",0,0\n";
    metrics << "filter_128_vs_64_64," << rmsDiff(one.filter, two.filter, 0) << ','
            << maxAbsDiff(one.filter, two.filter, 0) << ",0,0\n";
    metrics << "vca_128_vs_64_64," << rmsDiff(one.postVca, two.postVca, 0) << ','
            << maxAbsDiff(one.postVca, two.postVca, 0) << ",0,0\n";
    metrics << "native_block_counts," << one.nativeBlocks << ',' << two.nativeBlocks
            << ',' << three.nativeBlocks << ',' << four.nativeBlocks << "\n";

    const auto mOne = measure(one.finalHost, kHostRate);
    const auto mTwo = measure(two.finalHost, kHostRate);
    const auto mThree = measure(three.finalHost, kHostRate);
    const auto mFour = measure(four.finalHost, kHostRate);

    expect(mOne.invalidCount == 0 && mTwo.invalidCount == 0
               && mThree.invalidCount == 0 && mFour.invalidCount == 0,
           "chunked final renders remain finite");
    expect(mOne.peak > 1.0e-4 && mTwo.peak > 1.0e-4
               && mThree.peak > 1.0e-4 && mFour.peak > 1.0e-4,
           "chunked final renders remain non-silent");
    expect(std::fabs(mOne.rms - mTwo.rms) / std::max(mOne.rms, mTwo.rms) < 0.35,
           "64+64 has comparable energy to 128 for neutral sustained render");
    expect(std::fabs(mOne.rms - mThree.rms) / std::max(mOne.rms, mThree.rms) < 0.35,
           "31+33+64 has comparable energy to 128 for neutral sustained render");
    expect(std::fabs(mOne.rms - mFour.rms) / std::max(mOne.rms, mFour.rms) < 0.35,
           "63+65 has comparable energy to 128 for neutral sustained render");
    expect(d12 < 1.0e-6 && m12 < 1.0e-6,
           "128 and 64+64 final renders are sample-identical");
    expect(d13 < 1.0e-6 && m13 < 1.0e-6,
           "128 and 31+33+64 final renders are sample-identical");
    expect(d14 < 1.0e-6 && m14 < 1.0e-6,
           "128 and 63+65 final renders are sample-identical");
    expect(maxAbsDiff(one.rawOsc1, two.rawOsc1, 0) < 1.0e-6,
           "raw oscillator stream is host-block independent");
    expect(mOne.fundamentalHz > 435.0 && mOne.fundamentalHz < 445.0,
           "128 final render pitch remains A4");
    expect(mTwo.fundamentalHz > 435.0 && mTwo.fundamentalHz < 445.0,
           "64+64 final render pitch remains A4");
    expect(mThree.fundamentalHz > 435.0 && mThree.fundamentalHz < 445.0,
           "31+33+64 final render pitch remains A4");
    expect(mFour.fundamentalHz > 435.0 && mFour.fundamentalHz < 445.0,
           "63+65 final render pitch remains A4");

    const int lags[] = { 8, 16, 32, 40, 64, 128 };
    for (int lag : lags)
    {
        const double repeat = exactRepeatFraction(one.finalHost, lag, 1024);
        metrics << "host_final_repeat,0,0," << lag << ',' << repeat << '\n';
        expect(repeat < 0.02, "host output has no suspicious exact chunk repetition");
    }
}

void runEnvelopeRegression()
{
    ensureArtifacts();
    std::ofstream metrics(artifactRoot() / "envelope_regression_metrics.csv", std::ios::trunc);
    metrics << "sample_rate,attack_block,attack_ms,first_env,peak,mean,invalid\n";

    const double rates[] = { 44100.0, 48000.0, 88200.0, 96000.0, 176400.0, 192000.0 };
    int referenceAttackBlock = -1;
    for (double rate : rates)
    {
        const auto capture = renderProcessorCapture(
            rate, { 63, 65, 257 }, static_cast<int>(rate), true, configureAttackSaw);
        const int attackBlock = firstIndexAtLeast(capture.controls.env2, 200);
        const double attackMs = attackBlock >= 0
            ? static_cast<double>(attackBlock) * 1000.0 / kControlRateHz
            : -1.0;
        const auto audio = measure(capture.finalHost, rate);
        const int firstEnv = capture.controls.env2.empty() ? -1 : capture.controls.env2.front();
        metrics << static_cast<int>(rate) << ','
                << attackBlock << ','
                << attackMs << ','
                << firstEnv << ','
                << audio.peak << ','
                << audio.mean << ','
                << audio.invalidCount << '\n';

        expect(firstEnv >= 0 && firstEnv < 16,
               "saved slow attack is active on the first native block");
        expect(attackBlock > 200 && attackBlock < 300,
               "envelope parameter 64 has a gradual native-rate attack");
        expect(audio.invalidCount == 0, "short-envelope regression render remains finite");
        expect(std::fabs(audio.mean) < 0.05, "short-envelope regression render has controlled DC");
        if (referenceAttackBlock < 0)
            referenceAttackBlock = attackBlock;
        expect(std::abs(attackBlock - referenceAttackBlock) <= 1,
               "envelope duration is host-sample-rate invariant");
    }

    const auto minimum = renderProcessorCapture(
        48000.0, { 64 }, 4800, true, configureNeutralSaw);
    expect(! minimum.controls.env2.empty() && minimum.controls.env2.front() >= 200,
           "minimum legal attack intentionally produces a sharp native control-rate edge");
    expect(measure(minimum.finalHost, 48000.0).invalidCount == 0,
           "minimum legal attack remains finite");

    const auto filterEdge = renderProcessorCapture(
        48000.0, { 64 }, 4800, true, configureShortFilterEnvelope);
    const auto resonantFilterEdge = renderProcessorCapture(
        48000.0, { 64 }, 4800, true, configureShortResonantFilterEnvelope);
    const auto negativeAttackMod = renderProcessorCapture(
        48000.0, { 64 }, 4800, true, configureNegativeAttackModulation);
    const auto filterEdgeMetrics = measure(filterEdge.finalHost, 48000.0);
    const auto resonantFilterEdgeMetrics = measure(resonantFilterEdge.finalHost, 48000.0);
    expect(filterEdgeMetrics.invalidCount == 0 && filterEdgeMetrics.peak < 8.0,
           "minimum filter envelope remains finite and bounded at low resonance");
    expect(resonantFilterEdgeMetrics.invalidCount == 0
               && resonantFilterEdgeMetrics.peak < 8.0
               && std::fabs(resonantFilterEdgeMetrics.mean) < 0.1,
           "minimum filter envelope remains stable at high resonance");
    expect(filterEdge.controls.env1 == resonantFilterEdge.controls.env1,
           "VCA interpolation leaves the filter-envelope trajectory unchanged");
    expect(measure(negativeAttackMod.finalHost, 48000.0).invalidCount == 0,
           "negative attack modulation remains defined and finite");

    ShruthiRuntime transitions;
    transitions.init();
    configureNeutralPart(transitions.part, shruthi::WAVEFORM_SAW, 64);
    transitions.part.NoteOn(0, 69, 100);
    for (int block = 0; block < 80; ++block)
        transitions.part.ProcessBlock();
    const int beforeRetrigger = transitions.part.voice().modulation_source(shruthi::MOD_SRC_ENV_2);
    transitions.part.NoteOn(0, 69, 100);
    transitions.part.ProcessBlock();
    const int afterRetrigger = transitions.part.voice().modulation_source(shruthi::MOD_SRC_ENV_2);
    expect(afterRetrigger >= beforeRetrigger && afterRetrigger <= 255,
           "retrigger continues attack from the current bounded value");
    transitions.part.NoteOff(0, 69);
    transitions.part.ProcessBlock();
    transitions.part.ProcessBlock();
    expect(transitions.part.voice().modulation_source(shruthi::MOD_SRC_ENV_2) == 0,
           "note-off during attack reaches zero with minimum release");

    const auto renderSameOffsetNotes = [](int firstNote, int secondNote) {
        SwaraXtAudioProcessor proc;
        configureNeutralSaw(proc);
        ProcessorCapture capture;
        proc.engineForTests().setDebugTapSink(&capture, debugSink);
        proc.prepareToPlay(48000.0, 128);
        for (int block = 0; block < 375; ++block)
        {
            juce::AudioBuffer<float> buffer(2, 128);
            juce::MidiBuffer midi;
            if (block == 0)
            {
                midi.addEvent(juce::MidiMessage::noteOn(
                    1, firstNote, (juce::uint8) 100), 0);
                midi.addEvent(juce::MidiMessage::noteOn(
                    1, secondNote, (juce::uint8) 100), 0);
            }
            proc.processBlock(buffer, midi);
        }
        return measure(capture.rawOsc1, kInternalRate).fundamentalHz;
    };

    const double ascendingPitch = renderSameOffsetNotes(60, 72);
    const double descendingPitch = renderSameOffsetNotes(72, 60);
    metrics << "same_offset_60_72," << ascendingPitch << '\n';
    metrics << "same_offset_72_60," << descendingPitch << '\n';
    expect(ascendingPitch > 520.0 && ascendingPitch < 528.0,
           "same-offset ascending notes preserve insertion order");
    expect(descendingPitch > 259.0 && descendingPitch < 265.0,
           "same-offset descending notes preserve insertion order");

    std::ofstream clickMetrics(artifactRoot() / "envelope_click_metrics.csv", std::ios::trunc);
    clickMetrics << "waveform,held_note_on_delta,interpolated_note_on_delta,"
                    "held_note_off_delta,interpolated_note_off_delta,held_gain_step,"
                    "interpolated_gain_step,retrigger_gain_step\n";
    const int waveforms[] = {
        shruthi::WAVEFORM_SAW,
        shruthi::WAVEFORM_SQUARE,
        shruthi::WAVEFORM_TRIANGLE
    };
    const char* waveformNames[] = { "saw", "square", "triangle" };
    constexpr int eventSample = 4096;
    for (int waveformIndex = 0; waveformIndex < 3; ++waveformIndex)
    {
        const auto noteOff = renderRapidEnvelopeCapture(
            48000.0, { 63, 65, 127, 129, 257 }, waveforms[waveformIndex]);
        const auto retrigger = renderEnvelopeEventCapture(
            48000.0, { 63, 65, 127, 129, 257 }, waveforms[waveformIndex], -1, eventSample);
        const auto boundaries = measureVcaBoundaries(noteOff);
        const auto retriggerBoundaries = measureVcaBoundaries(retrigger);
        clickMetrics << waveformNames[waveformIndex] << ','
                     << boundaries.heldAttackDelta << ','
                     << boundaries.interpolatedAttackDelta << ','
                     << boundaries.heldReleaseDelta << ','
                     << boundaries.interpolatedReleaseDelta << ','
                     << boundaries.heldMaximumGainStep << ','
                     << boundaries.maximumGainStep << ','
                     << retriggerBoundaries.maximumGainStep << '\n';
        expect(boundaries.heldMaximumGainStep > 0.99,
               "minimum envelope exposes the original full-scale VCA control step");
        expect(boundaries.maximumGainStep <= (1.0 / kNativeBlock) + 1.0e-5,
               "VCA gain advances by at most one fortieth per native sample");
        expect(retriggerBoundaries.maximumGainStep <= (1.0 / kNativeBlock) + 1.0e-5,
               "same-note retrigger remains bounded by the native VCA ramp");
    }

    std::ofstream syntheticMetrics(
        artifactRoot() / "envelope_synthetic_click_metrics.csv", std::ios::trunc);
    syntheticMetrics << "signal,held_note_on_delta,interpolated_note_on_delta,"
                        "held_note_off_delta,interpolated_note_off_delta\n";
    const auto recordSynthetic = [&](const char* name, double previousSample, double nextSample) {
        constexpr double firstRampGain = 1.0 / kNativeBlock;
        const double heldOn = std::abs(nextSample);
        const double interpolatedOn = std::abs(nextSample * firstRampGain);
        const double heldOff = std::abs(previousSample);
        const double interpolatedOff = std::abs(nextSample * (1.0 - firstRampGain) - previousSample);
        syntheticMetrics << name << ',' << heldOn << ',' << interpolatedOn << ','
                         << heldOff << ',' << interpolatedOff << '\n';
        expect(interpolatedOn < heldOn * 0.05,
               "controlled signal note-on discontinuity is substantially reduced");
        expect(interpolatedOff < heldOff * 0.1,
               "controlled signal note-off discontinuity is substantially reduced");
    };
    const double sineStep = 2.0 * juce::MathConstants<double>::pi * 220.0 / kInternalRate;
    recordSynthetic("sine_220_hz", 1.0, std::cos(sineStep));
    const double sawPhase = 0.75;
    const double sawNextPhase = std::fmod(sawPhase + 220.0 / kInternalRate, 1.0);
    recordSynthetic("saw_220_hz", sawPhase * 2.0 - 1.0, sawNextPhase * 2.0 - 1.0);
    const double lowSineStep = 2.0 * juce::MathConstants<double>::pi * 5.0 / kInternalRate;
    recordSynthetic("sine_5_hz", 1.0, std::cos(lowSineStep));

    std::ofstream matrixMetrics(
        artifactRoot() / "envelope_click_rate_block_matrix.csv", std::ios::trunc);
    matrixMetrics << "sample_rate,block_size,held_gain_step,interpolated_gain_step,"
                     "peak,invalid\n";
    const int blockSizes[] = { 63, 64, 65, 127, 128, 129, 257 };
    for (double rate : rates)
    {
        for (int blockSize : blockSizes)
        {
            const auto capture = renderEnvelopeEventCapture(
                rate, { blockSize }, shruthi::WAVEFORM_TRIANGLE, 4096, -1);
            const auto boundaries = measureVcaBoundaries(capture);
            const auto audio = measure(capture.finalHost, rate);
            matrixMetrics << static_cast<int>(rate) << ',' << blockSize << ','
                          << boundaries.heldMaximumGainStep << ','
                          << boundaries.maximumGainStep << ','
                          << audio.peak << ',' << audio.invalidCount << '\n';
            expect(boundaries.heldMaximumGainStep > 0.99,
                   "sample-rate/block matrix exercises a full VCA target transition");
            expect(boundaries.maximumGainStep <= (1.0 / kNativeBlock) + 1.0e-5,
                   "VCA interpolation is sample-rate and host-block independent");
            expect(audio.invalidCount == 0,
                   "VCA interpolation matrix remains finite");
        }
    }
}

}  // namespace

int main()
{
    juce::ScopedJuceInitialiser_GUI juceInit;
    std::printf("SwaraXt Shruthi timing test mode %d start\n", SWARAXT_SHRUTHI_TIMING_TEST_MODE);
    std::fflush(stdout);

#if SWARAXT_SHRUTHI_TIMING_TEST_MODE == 0
    runShruthiTiming();
#elif SWARAXT_SHRUTHI_TIMING_TEST_MODE == 1
    runRawOscillatorParity();
#elif SWARAXT_SHRUTHI_TIMING_TEST_MODE == 2
    runControlRate();
#elif SWARAXT_SHRUTHI_TIMING_TEST_MODE == 3
    runModulationNeutrality();
#elif SWARAXT_SHRUTHI_TIMING_TEST_MODE == 4
    runStreamingSrc();
#elif SWARAXT_SHRUTHI_TIMING_TEST_MODE == 5
    runChunkContinuity();
#elif SWARAXT_SHRUTHI_TIMING_TEST_MODE == 6
    runEnvelopeRegression();
#else
    expect(false, "unknown test mode");
#endif

    std::printf(gFailures == 0 ? "SwaraXt Shruthi timing test: PASSED\n"
                               : "SwaraXt Shruthi timing test: FAILED (%d)\n",
                gFailures);
    return gFailures == 0 ? 0 : 1;
}
