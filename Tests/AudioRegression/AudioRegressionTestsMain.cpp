// Copyright 2026 MontroneDSP.
// SPDX-License-Identifier: GPL-3.0-or-later
// Deterministic audio-regression tests and listening artifact renderer.

#include <JuceHeader.h>

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <limits>
#include <new>
#include <numeric>
#include <string>
#include <type_traits>
#include <vector>

#include "Engine/Filter/SwaraXtFilter.h"
#include "Engine/SwaraXtEngine.h"
#include "Engine/SampleRate/InternalSampleQueue.h"
#include "Plugin/SwaraXtParameterLayout.h"
#include "Plugin/PluginProcessor.h"

#include "avrlib/random.h"
#include "shruthi/audio_out.h"
#include "shruthi/midi_dispatcher.h"
#include "shruthi/part.h"
#include "shruthi/storage.h"

#ifndef SWARAXT_AUDIO_TEST_MODE
#define SWARAXT_AUDIO_TEST_MODE 0
#endif

namespace {

namespace fs = std::filesystem;

constexpr double kHostRate = 44100.0;
constexpr int kBlockSize = 128;
constexpr int kRenderSamples = 44100;
constexpr int kInternalBlockSize = 40;
int gFailures = 0;

struct Metrics {
    float min = 0.0f;
    float max = 0.0f;
    float peak = 0.0f;
    float rms = 0.0f;
    float mean = 0.0f;
    float zeroCrossingRate = 0.0f;
    float spectralCentroid = 0.0f;
    float highFrequencyEnergy = 0.0f;
    int invalidCount = 0;
    uint64_t hash = 0;
};

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
    return fs::current_path() / "artifacts" / "audio-regression";
}

void ensureArtifacts()
{
    fs::create_directories(artifactRoot());
    fs::create_directories(artifactRoot() / "final");
}

uint64_t hashSamples(const std::vector<float>& samples)
{
    uint64_t h = 1469598103934665603ull;
    for (float sample : samples)
    {
        const float finite = std::isfinite(sample) ? sample : 0.0f;
        const auto q = static_cast<int32_t>(std::lround(
            std::clamp(finite, -8.0f, 8.0f) * 8388608.0f));
        uint8_t bytes[sizeof(q)];
        std::memcpy(bytes, &q, sizeof(q));
        for (uint8_t b : bytes)
        {
            h ^= b;
            h *= 1099511628211ull;
        }
    }
    return h;
}

Metrics measure(const std::vector<float>& samples, double sampleRate)
{
    Metrics m;
    if (samples.empty())
        return m;

    m.min = std::numeric_limits<float>::max();
    m.max = std::numeric_limits<float>::lowest();
    double sum = 0.0;
    double sumSq = 0.0;
    int crossings = 0;
    int previousSign = 0;
    for (float s : samples)
    {
        if (! std::isfinite(s))
        {
            ++m.invalidCount;
            s = 0.0f;
        }
        m.min = std::min(m.min, s);
        m.max = std::max(m.max, s);
        m.peak = std::max(m.peak, std::fabs(s));
        sum += s;
        sumSq += static_cast<double>(s) * static_cast<double>(s);
        const int sign = s > 0.0f ? 1 : (s < 0.0f ? -1 : previousSign);
        if (previousSign != 0 && sign != 0 && sign != previousSign)
            ++crossings;
        previousSign = sign;
    }
    m.mean = static_cast<float>(sum / static_cast<double>(samples.size()));
    m.rms = static_cast<float>(std::sqrt(sumSq / static_cast<double>(samples.size())));
    m.zeroCrossingRate = static_cast<float>(
        static_cast<double>(crossings) / static_cast<double>(samples.size()));

    const size_t n = std::min<size_t>(samples.size(), 2048);
    if (n > 8)
    {
        double weighted = 0.0;
        double magSum = 0.0;
        double high = 0.0;
        const size_t maxBin = n / 2;
        for (size_t bin = 1; bin < maxBin; ++bin)
        {
            double re = 0.0;
            double im = 0.0;
            for (size_t i = 0; i < n; ++i)
            {
                const double phase = -2.0 * juce::MathConstants<double>::pi
                    * static_cast<double>(bin * i) / static_cast<double>(n);
                const double s = std::isfinite(samples[i]) ? samples[i] : 0.0f;
                re += s * std::cos(phase);
                im += s * std::sin(phase);
            }
            const double mag = std::sqrt(re * re + im * im);
            const double hz = sampleRate * static_cast<double>(bin) / static_cast<double>(n);
            weighted += mag * hz;
            magSum += mag;
            if (bin > maxBin * 7 / 10)
                high += mag * mag;
        }
        if (magSum > 0.0)
            m.spectralCentroid = static_cast<float>(weighted / magSum);
        m.highFrequencyEnergy = static_cast<float>(high / std::max(1.0, magSum * magSum));
    }

    m.hash = hashSamples(samples);
    return m;
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

void writeMetricsHeader(std::ofstream& out)
{
    out << "name,min,max,peak,rms,mean,zero_crossing_rate,spectral_centroid_hz,"
           "high_frequency_energy,invalid_count,hash\n";
}

void writeMetricsRow(std::ofstream& out, const std::string& name, const Metrics& m)
{
    out << name << ','
        << m.min << ','
        << m.max << ','
        << m.peak << ','
        << m.rms << ','
        << m.mean << ','
        << m.zeroCrossingRate << ','
        << m.spectralCentroid << ','
        << m.highFrequencyEnergy << ','
        << m.invalidCount << ','
        << std::hex << m.hash << std::dec << '\n';
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

void setModRow(SwaraXtAudioProcessor& proc, int row, int source, int dest, int amount)
{
    const juce::String prefix = "mod.row" + juce::String(row) + ".";
    setInt(proc, (prefix + "source").toRawUTF8(), source);
    setInt(proc, (prefix + "destination").toRawUTF8(), dest);
    setInt(proc, (prefix + "amount").toRawUTF8(), amount);
}

void configureBasicPatch(SwaraXtAudioProcessor& proc,
                         int osc1Shape,
                         int osc2Shape,
                         int mixBalance,
                         int noise,
                         float cutoff,
                         float resonance)
{
    setFloat(proc, swaraxt::IDs::master, 0.85f);
    setInt(proc, swaraxt::IDs::osc1Shape, osc1Shape);
    setInt(proc, swaraxt::IDs::osc1Param, osc1Shape == shruthi::WAVEFORM_SQUARE ? 64 : 0);
    setInt(proc, swaraxt::IDs::osc1Range, 0);
    setInt(proc, swaraxt::IDs::osc1Option, 0);
    setInt(proc, swaraxt::IDs::osc2Shape, osc2Shape);
    setInt(proc, swaraxt::IDs::osc2Param, 16);
    setInt(proc, swaraxt::IDs::osc2Range, -12);
    setInt(proc, swaraxt::IDs::osc2Option, 12);
    setInt(proc, swaraxt::IDs::mixBalance, mixBalance);
    setInt(proc, swaraxt::IDs::mixSub, 0);
    setInt(proc, swaraxt::IDs::mixNoise, noise);
    setFloat(proc, swaraxt::IDs::filterCutoff, cutoff);
    setFloat(proc, swaraxt::IDs::filterResonance, resonance);
    setFloat(proc, swaraxt::IDs::filterEnvAmount, 0.0f);
    setFloat(proc, swaraxt::IDs::filterKeyTracking, 0.0f);
    setFloat(proc, swaraxt::IDs::filterModAmount, 0.0f);
    setInt(proc, swaraxt::IDs::env2Attack, 0);
    setInt(proc, swaraxt::IDs::env2Decay, 30);
    setInt(proc, swaraxt::IDs::env2Sustain, 110);
    setInt(proc, swaraxt::IDs::env2Release, 25);
    setInt(proc, swaraxt::IDs::midiChannel, 0);
    setModRow(proc, 9, shruthi::MOD_SRC_ENV_2, shruthi::MOD_DST_VCA, 63);
    setModRow(proc, 10, shruthi::MOD_SRC_VELOCITY, shruthi::MOD_DST_VCA, 16);
}

std::vector<float> renderProcessor(SwaraXtAudioProcessor& proc, int note, bool releaseNote)
{
    proc.prepareToPlay(kHostRate, kBlockSize);
    std::vector<float> output;
    output.reserve(kRenderSamples);
    juce::AudioBuffer<float> buffer(2, kBlockSize);
    int rendered = 0;
    while (rendered < kRenderSamples)
    {
        const int n = std::min(kBlockSize, kRenderSamples - rendered);
        buffer.setSize(2, n, false, false, true);
        buffer.clear();
        juce::MidiBuffer midi;
        if (rendered == 0 && note >= 0)
            midi.addEvent(juce::MidiMessage::noteOn(1, note, (juce::uint8) 100), 0);
        if (releaseNote && note >= 0 && rendered <= kRenderSamples * 3 / 5
            && rendered + n > kRenderSamples * 3 / 5)
        {
            midi.addEvent(juce::MidiMessage::noteOff(1, note),
                          kRenderSamples * 3 / 5 - rendered);
        }
        proc.processBlock(buffer, midi);
        for (int i = 0; i < n; ++i)
            output.push_back(buffer.getSample(0, i));
        rendered += n;
    }
    proc.releaseResources();
    return output;
}

void configureDirectPatch(shruthi::Part& part,
                          int osc1Shape,
                          int osc2Shape,
                          int mixBalance,
                          int noise)
{
    auto* patch = part.mutable_patch();
    patch->osc[0].shape = static_cast<uint8_t>(osc1Shape);
    patch->osc[0].parameter = osc1Shape == shruthi::WAVEFORM_SQUARE ? 64 : 0;
    patch->osc[0].range = 0;
    patch->osc[0].option = 0;
    patch->osc[1].shape = static_cast<uint8_t>(osc2Shape);
    patch->osc[1].parameter = 16;
    patch->osc[1].range = -12;
    patch->osc[1].option = 12;
    patch->mix_balance = static_cast<uint8_t>(mixBalance);
    patch->mix_sub_osc = 0;
    patch->mix_noise = static_cast<uint8_t>(noise);
    patch->filter_cutoff = 127;
    patch->filter_resonance = 0;
    patch->filter_env = 0;
    patch->filter_lfo = 0;
    patch->env[1].attack = 0;
    patch->env[1].decay = 30;
    patch->env[1].sustain = 110;
    patch->env[1].release = 25;
    for (int row = 0; row < shruthi::kModulationMatrixSize; ++row)
    {
        patch->modulation_matrix.modulation[row].source = shruthi::MOD_SRC_OFFSET;
        patch->modulation_matrix.modulation[row].destination = shruthi::MOD_DST_VCA;
        patch->modulation_matrix.modulation[row].amount = 0;
    }
    patch->modulation_matrix.modulation[8].source = shruthi::MOD_SRC_ENV_2;
    patch->modulation_matrix.modulation[8].destination = shruthi::MOD_DST_VCA;
    patch->modulation_matrix.modulation[8].amount = 63;
    patch->modulation_matrix.modulation[9].source = shruthi::MOD_SRC_VELOCITY;
    patch->modulation_matrix.modulation[9].destination = shruthi::MOD_DST_VCA;
    patch->modulation_matrix.modulation[9].amount = 16;
    part.Touch(false);
}

std::vector<float> renderShruthiSource(ShruthiRuntime& runtime,
                                       int blocks,
                                       int note,
                                       bool applyVca)
{
    runtime.ring.Init();
    if (note >= 0)
        runtime.part.NoteOn(0, static_cast<uint8_t>(note), 100);
    std::vector<float> out;
    out.reserve(static_cast<size_t>(blocks * kInternalBlockSize));
    for (int block = 0; block < blocks; ++block)
    {
        runtime.part.ProcessBlock();
        const float vca = static_cast<float>(runtime.part.voice().vca()) / 255.0f;
        float temp[kInternalBlockSize] {};
        const int read = runtime.ring.readBlockRaw(temp, kInternalBlockSize);
        for (int i = 0; i < read; ++i)
            out.push_back(temp[i] * (applyVca ? vca : 1.0f));
    }
    return out;
}

struct FilterCapture {
    std::vector<float> input;
    std::vector<float> stage1;
    std::vector<float> stage2;
    std::vector<float> stage3;
    std::vector<float> stage4;
    std::vector<float> output;
};

FilterCapture runFilterCapture(const std::vector<float>& source, float resonance)
{
    swaraxt::SwaraXtFilter filter;
    filter.prepare(swaraxt::SwaraXtEngine::kInternalSampleRate);
    swaraxt::SwaraXtFilterParams params;
    params.cutoffHz = 8000.0f;
    params.resonance = resonance;
    params.keyTrack = 0.0f;
    params.envAmount = 0.0f;
    params.modAmount = 0.0f;
    params.noteNumber = 60.0f;
    filter.setParams(params);

    FilterCapture capture;
    capture.input = source;
    capture.stage1.reserve(source.size());
    capture.stage2.reserve(source.size());
    capture.stage3.reserve(source.size());
    capture.stage4.reserve(source.size());
    capture.output.reserve(source.size());
    for (float s : source)
    {
        const float y = filter.processSample(s);
        capture.stage1.push_back(filter.lastStageOutput(0));
        capture.stage2.push_back(filter.lastStageOutput(1));
        capture.stage3.push_back(filter.lastStageOutput(2));
        capture.stage4.push_back(filter.lastStageOutput(3));
        capture.output.push_back(y);
    }
    return capture;
}

std::vector<float> renderPoisonedEngine()
{
    alignas(swaraxt::SwaraXtEngine) unsigned char storage[sizeof(swaraxt::SwaraXtEngine)];
    std::memset(storage, 0xa5, sizeof(storage));
    auto* engine = new (storage) swaraxt::SwaraXtEngine();
    engine->prepare(kHostRate, kBlockSize);
    engine->reset();
    juce::AudioBuffer<float> buffer(2, kBlockSize);
    std::vector<float> out;
    out.reserve(4096);
    for (int block = 0; block < 32; ++block)
    {
        juce::MidiBuffer midi;
        if (block == 0)
            midi.addEvent(juce::MidiMessage::noteOn(1, 60, (juce::uint8) 100), 0);
        if (block == 24)
            midi.addEvent(juce::MidiMessage::noteOff(1, 60), 0);
        buffer.clear();
        engine->process(midi, buffer, 0, kBlockSize);
        for (int i = 0; i < kBlockSize; ++i)
            out.push_back(buffer.getSample(0, i));
    }
    engine->~SwaraXtEngine();
    return out;
}

void runAudioParity()
{
    ensureArtifacts();
    std::ofstream metrics(artifactRoot() / "metrics.csv", std::ios::trunc);
    writeMetricsHeader(metrics);

    struct Scenario {
        const char* file;
        const char* name;
        int osc1;
        int osc2;
        int balance;
        int noise;
        float cutoff;
        float resonance;
        int note;
        bool release;
    };

    const Scenario scenarios[] = {
        { "01_basic_saw.wav", "Test A basic saw", shruthi::WAVEFORM_SAW, shruthi::WAVEFORM_NONE, 0, 0, 20000.0f, 0.0f, 60, true },
        { "02_basic_square.wav", "Test B basic square", shruthi::WAVEFORM_SQUARE, shruthi::WAVEFORM_NONE, 0, 0, 20000.0f, 0.0f, 60, true },
        { "03_two_osc_mix.wav", "Test C two oscillators", shruthi::WAVEFORM_SAW, shruthi::WAVEFORM_SQUARE, 64, 0, 20000.0f, 0.0f, 60, true },
        { "04_filter_low_res.wav", "Test F filter low resonance", shruthi::WAVEFORM_SAW, shruthi::WAVEFORM_NONE, 0, 0, 1200.0f, 0.25f, 48, true },
        { "05_filter_high_res.wav", "Test G filter high resonance", shruthi::WAVEFORM_SAW, shruthi::WAVEFORM_NONE, 0, 0, 900.0f, 0.82f, 48, true },
        { "06_envelope_bass.wav", "Test I normal envelope", shruthi::WAVEFORM_SAW, shruthi::WAVEFORM_SAW, 32, 0, 650.0f, 0.35f, 36, true },
        { "07_noise_only.wav", "Test D noise only", shruthi::WAVEFORM_NONE, shruthi::WAVEFORM_NONE, 0, 127, 5000.0f, 0.2f, 60, true },
        { "08_silence_test.wav", "Test J silence", shruthi::WAVEFORM_SAW, shruthi::WAVEFORM_NONE, 0, 0, 20000.0f, 0.0f, -1, false },
    };

    for (const auto& scenario : scenarios)
    {
        SwaraXtAudioProcessor proc;
        configureBasicPatch(proc,
                            scenario.osc1,
                            scenario.osc2,
                            scenario.balance,
                            scenario.noise,
                            scenario.cutoff,
                            scenario.resonance);
        const auto audio = renderProcessor(proc, scenario.note, scenario.release);
        const auto m = measure(audio, kHostRate);
        writeWav(artifactRoot() / "final" / scenario.file, audio, static_cast<int>(kHostRate));
        writeMetricsRow(metrics, scenario.name, m);
        expect(m.invalidCount == 0, "final render finite");
        if (scenario.note >= 0)
            expect(m.peak > 1.0e-4f, "final render non-silent");
        else
            expect(m.peak < 1.0e-5f, "silence render remains silent");
    }

    const auto poisonedA = renderPoisonedEngine();
    const auto poisonedB = renderPoisonedEngine();
    expect(hashSamples(poisonedA) == hashSamples(poisonedB),
           "poisoned storage engine renders deterministically");
    writeMetricsRow(metrics, "Test L poisoned placement instance A", measure(poisonedA, kHostRate));
    writeMetricsRow(metrics, "Test L poisoned placement instance B", measure(poisonedB, kHostRate));
}

void runSignalChain()
{
    ensureArtifacts();
    std::ofstream metrics(artifactRoot() / "signal_chain_metrics.csv", std::ios::trunc);
    writeMetricsHeader(metrics);

    ShruthiRuntime saw;
    saw.init();
    configureDirectPatch(saw.part, shruthi::WAVEFORM_SAW, shruthi::WAVEFORM_NONE, 0, 0);
    auto source = renderShruthiSource(saw, 128, 60, false);
    auto postVca = source;
    ShruthiRuntime sawVca;
    sawVca.init();
    configureDirectPatch(sawVca.part, shruthi::WAVEFORM_SAW, shruthi::WAVEFORM_NONE, 0, 0);
    postVca = renderShruthiSource(sawVca, 128, 60, true);
    auto lowFilter = runFilterCapture(source, 0.25f);
    auto highFilter = runFilterCapture(source, 0.82f);

    ShruthiRuntime noiseZero;
    noiseZero.init();
    configureDirectPatch(noiseZero.part, shruthi::WAVEFORM_NONE, shruthi::WAVEFORM_NONE, 0, 0);
    const auto zeroNoise = renderShruthiSource(noiseZero, 64, 60, false);

    ShruthiRuntime noiseOnly;
    noiseOnly.init();
    configureDirectPatch(noiseOnly.part, shruthi::WAVEFORM_NONE, shruthi::WAVEFORM_NONE, 0, 127);
    const auto noise = renderShruthiSource(noiseOnly, 64, 60, false);

    ShruthiRuntime silence;
    silence.init();
    configureDirectPatch(silence.part, shruthi::WAVEFORM_SAW, shruthi::WAVEFORM_NONE, 0, 0);
    const auto silent = renderShruthiSource(silence, 64, -1, true);

    writeMetricsRow(metrics, "raw_oscillator_1_and_post_source_mixer", measure(source, swaraxt::SwaraXtEngine::kInternalSampleRate));
    writeMetricsRow(metrics, "raw_oscillator_2_disabled", measure(zeroNoise, swaraxt::SwaraXtEngine::kInternalSampleRate));
    writeMetricsRow(metrics, "sub_oscillator_disabled", measure(zeroNoise, swaraxt::SwaraXtEngine::kInternalSampleRate));
    writeMetricsRow(metrics, "noise_source_zero", measure(zeroNoise, swaraxt::SwaraXtEngine::kInternalSampleRate));
    writeMetricsRow(metrics, "noise_source_only", measure(noise, swaraxt::SwaraXtEngine::kInternalSampleRate));
    writeMetricsRow(metrics, "filter_input", measure(lowFilter.input, swaraxt::SwaraXtEngine::kInternalSampleRate));
    writeMetricsRow(metrics, "filter_stage_1_output", measure(lowFilter.stage1, swaraxt::SwaraXtEngine::kInternalSampleRate));
    writeMetricsRow(metrics, "filter_stage_2_output", measure(lowFilter.stage2, swaraxt::SwaraXtEngine::kInternalSampleRate));
    writeMetricsRow(metrics, "filter_stage_3_output", measure(lowFilter.stage3, swaraxt::SwaraXtEngine::kInternalSampleRate));
    writeMetricsRow(metrics, "filter_stage_4_output", measure(lowFilter.stage4, swaraxt::SwaraXtEngine::kInternalSampleRate));
    writeMetricsRow(metrics, "post_filter_low_resonance", measure(lowFilter.output, swaraxt::SwaraXtEngine::kInternalSampleRate));
    writeMetricsRow(metrics, "post_resonance_high_resonance", measure(highFilter.output, swaraxt::SwaraXtEngine::kInternalSampleRate));
    writeMetricsRow(metrics, "post_vca", measure(postVca, swaraxt::SwaraXtEngine::kInternalSampleRate));
    writeMetricsRow(metrics, "silence_no_active_notes", measure(silent, swaraxt::SwaraXtEngine::kInternalSampleRate));

    expect(measure(source, swaraxt::SwaraXtEngine::kInternalSampleRate).peak > 0.01f,
           "source mixer has oscillator energy");
    const auto zeroNoiseMetrics = measure(zeroNoise, swaraxt::SwaraXtEngine::kInternalSampleRate);
    expect(zeroNoiseMetrics.peak < 0.03f
              && zeroNoiseMetrics.zeroCrossingRate == 0.0f
              && zeroNoiseMetrics.highFrequencyEnergy < 1.0e-12f,
           "noise zero has only deterministic Shruthi mixer bias, no stochastic energy");
    expect(measure(noise, swaraxt::SwaraXtEngine::kInternalSampleRate).peak > 0.01f,
           "noise-only source produces energy");
    expect(measure(silent, swaraxt::SwaraXtEngine::kInternalSampleRate).peak < 1.0e-6f,
           "direct silence remains silent");
    expect(measure(highFilter.output, swaraxt::SwaraXtEngine::kInternalSampleRate).invalidCount == 0,
           "high-resonance filter remains finite");

    writeWav(artifactRoot() / "test_a_internal_saw_source.wav", source, static_cast<int>(swaraxt::SwaraXtEngine::kInternalSampleRate));
    writeWav(artifactRoot() / "test_d_internal_noise_source.wav", noise, static_cast<int>(swaraxt::SwaraXtEngine::kInternalSampleRate));
    writeWav(artifactRoot() / "test_f_internal_filter_low_res.wav", lowFilter.output, static_cast<int>(swaraxt::SwaraXtEngine::kInternalSampleRate));
    writeWav(artifactRoot() / "test_h_internal_vca_open.wav", source, static_cast<int>(swaraxt::SwaraXtEngine::kInternalSampleRate));
    writeWav(artifactRoot() / "test_i_internal_post_vca.wav", postVca, static_cast<int>(swaraxt::SwaraXtEngine::kInternalSampleRate));
}

void runInitialization()
{
    static_assert(! std::is_copy_constructible_v<swaraxt::SwaraXtEngine>);
    static_assert(! std::is_copy_assignable_v<swaraxt::SwaraXtEngine>);
    static_assert(! std::is_move_constructible_v<swaraxt::SwaraXtEngine>);
    static_assert(! std::is_move_assignable_v<swaraxt::SwaraXtEngine>);

    const auto baseline = renderPoisonedEngine();
    const uint64_t expectedHash = hashSamples(baseline);
    expect(measure(baseline, kHostRate).invalidCount == 0, "poisoned baseline finite");
    for (int i = 0; i < 8; ++i)
    {
        const auto current = renderPoisonedEngine();
        expect(hashSamples(current) == expectedHash, "repeated poisoned engine hash");
    }

    ShruthiRuntime a;
    ShruthiRuntime b;
    a.init();
    b.init();
    configureDirectPatch(a.part, shruthi::WAVEFORM_SAW, shruthi::WAVEFORM_NONE, 0, 0);
    configureDirectPatch(b.part, shruthi::WAVEFORM_SAW, shruthi::WAVEFORM_NONE, 0, 0);
    const auto ra = renderShruthiSource(a, 64, 60, true);
    const auto rb = renderShruthiSource(b, 64, 60, true);
    const auto ma = measure(ra, swaraxt::SwaraXtEngine::kInternalSampleRate);
    const auto mb = measure(rb, swaraxt::SwaraXtEngine::kInternalSampleRate);
    expect(ma.invalidCount == 0 && mb.invalidCount == 0, "direct Shruthi runtimes remain finite");
    expect(std::fabs(ma.rms - mb.rms) / std::max(ma.rms, mb.rms) < 0.01f,
           "direct Shruthi runtime initialized energy is repeatable");
}

void runQueueIntegrity()
{
    swaraxt::InternalSampleQueue queue;
    queue.reset();
    expect(queue.size() == 0, "queue reset empty");
    expect(queue.readInterpolated() == 0.0f, "empty queue reads zero");
    queue.setStep(39215.6862745098, 44100.0);
    for (int i = 0; i < 16; ++i)
        queue.push(static_cast<float>(i) * 0.01f);
    for (int i = 0; i < 8; ++i)
        expect(std::isfinite(queue.readInterpolated()), "queue interpolated sample finite");

    shruthi::HostAudioRing ring;
    float temp[kInternalBlockSize] {};
    ring.Init();
    expect(ring.readBlockRaw(temp, kInternalBlockSize) == 0, "empty host ring has no readable samples");
    for (int i = 0; i < kInternalBlockSize; ++i)
        ring.Overwrite(static_cast<uint8_t>(128 + (i % 7)));
    expect(ring.readBlockRaw(temp, kInternalBlockSize) == kInternalBlockSize,
           "host ring reads exactly written samples");
    ring.Init();
    expect(ring.readBlockRaw(temp, kInternalBlockSize) == 0, "host ring reset drops stale samples");

    SwaraXtAudioProcessor one;
    configureBasicPatch(one, shruthi::WAVEFORM_SAW, shruthi::WAVEFORM_NONE, 0, 0, 20000.0f, 0.0f);
    const auto block128 = renderProcessor(one, 60, true);
    SwaraXtAudioProcessor two;
    configureBasicPatch(two, shruthi::WAVEFORM_SAW, shruthi::WAVEFORM_NONE, 0, 0, 20000.0f, 0.0f);
    two.prepareToPlay(kHostRate, 64);
    juce::AudioBuffer<float> buffer(2, 64);
    std::vector<float> chunked;
    chunked.reserve(kRenderSamples);
    for (int rendered = 0; rendered < kRenderSamples; rendered += 64)
    {
        juce::MidiBuffer midi;
        if (rendered == 0)
            midi.addEvent(juce::MidiMessage::noteOn(1, 60, (juce::uint8) 100), 0);
        if (rendered <= kRenderSamples * 3 / 5 && rendered + 64 > kRenderSamples * 3 / 5)
            midi.addEvent(juce::MidiMessage::noteOff(1, 60), kRenderSamples * 3 / 5 - rendered);
        buffer.clear();
        two.processBlock(buffer, midi);
        for (int i = 0; i < 64 && static_cast<int>(chunked.size()) < kRenderSamples; ++i)
            chunked.push_back(buffer.getSample(0, i));
    }
    two.releaseResources();
    const auto a = measure(block128, kHostRate);
    const auto b = measure(chunked, kHostRate);
    expect(a.invalidCount == 0 && b.invalidCount == 0, "chunked renders finite");
    expect(a.peak > 1.0e-4f && b.peak > 1.0e-4f, "chunked renders non-silent");
    expect(std::fabs(a.rms - b.rms) / std::max(a.rms, b.rms) < 0.35f,
           "128 and 64 sample chunking have comparable energy");
}

}  // namespace

int main()
{
    juce::ScopedJuceInitialiser_GUI juceInit;
    std::printf("SwaraXt audio regression test mode %d start\n", SWARAXT_AUDIO_TEST_MODE);
    std::fflush(stdout);

#if SWARAXT_AUDIO_TEST_MODE == 0
    runAudioParity();
#elif SWARAXT_AUDIO_TEST_MODE == 1
    runSignalChain();
#elif SWARAXT_AUDIO_TEST_MODE == 2
    runInitialization();
#elif SWARAXT_AUDIO_TEST_MODE == 3
    runQueueIntegrity();
#else
    expect(false, "unknown audio regression test mode");
#endif

    std::printf(gFailures == 0 ? "SwaraXt audio regression test: PASSED\n"
                               : "SwaraXt audio regression test: FAILED (%d)\n",
                gFailures);
    return gFailures == 0 ? 0 : 1;
}
