// Copyright 2026 MontroneDSP.
// SPDX-License-Identifier: GPL-3.0-or-later
//
// Whole-plugin side of the SRC quality study: deterministic production renders,
// engine CPU breakdown, dormancy behaviour, host block-partition invariance and
// MIDI onset timing.
//
// One source, built twice: once with the fallback Hermite reader and once with
// the polyphase FIR (SWARAXT_SRC_FIR_TAPS).

#include <JuceHeader.h>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <string>
#include <vector>

#include "Plugin/PluginProcessor.h"
#include "Plugin/SwaraXtParameterLayout.h"

namespace {

int failures = 0;

void expect(bool condition, const std::string& message)
{
    if (! condition)
    {
        std::fprintf(stderr, "FAIL: %s\n", message.c_str());
        ++failures;
    }
}

std::string converterName()
{
#if SWARAXT_SRC_FIR_TAPS > 0
    return "fir" + std::to_string(SWARAXT_SRC_FIR_TAPS);
#else
    return "hermite";
#endif
}

std::string artifactRoot()
{
    if (const char* configured = std::getenv("SWARAXT_SRC_ARTIFACT_DIR");
        configured != nullptr && *configured != '\0')
        return configured;

    return (std::filesystem::current_path() / "artifacts" / "src-quality").string();
}

void setParameter(SwaraXtAudioProcessor& processor, const char* id, float value)
{
    auto* parameter = processor.getApvts().getParameter(id);
    if (parameter != nullptr)
        parameter->setValueNotifyingHost(parameter->convertTo0to1(value));
}

// ------------------------------------------------------------------- patches

struct Patch {
    std::string name;
    int osc1Shape = 1;      // 1 = analog saw
    int osc1Param = 0;
    int note = 60;
    float cutoff = 20000.0f;
    float resonance = 0.0f;
    float envAmount = 0.0f;
    int subLevel = 0;
    int noiseLevel = 0;
};

// The renders deliberately keep the filter wide open (or only lightly closed)
// so the high-frequency content that exposes SRC images survives to the output.
void applyPatch(SwaraXtAudioProcessor& processor, const Patch& patch)
{
    setParameter(processor, swaraxt::IDs::osc1Shape, static_cast<float>(patch.osc1Shape));
    setParameter(processor, swaraxt::IDs::osc1Param, static_cast<float>(patch.osc1Param));
    setParameter(processor, swaraxt::IDs::osc2Shape, 0.0f);
    setParameter(processor, swaraxt::IDs::mixBalance, 0.0f);
    setParameter(processor, swaraxt::IDs::mixSub, static_cast<float>(patch.subLevel));
    setParameter(processor, swaraxt::IDs::mixNoise, static_cast<float>(patch.noiseLevel));
    setParameter(processor, swaraxt::IDs::filterCutoff, patch.cutoff);
    setParameter(processor, swaraxt::IDs::filterResonance, patch.resonance);
    setParameter(processor, swaraxt::IDs::filterEnvAmount, patch.envAmount);
    setParameter(processor, swaraxt::IDs::filterKeyTracking, 0.0f);
    setParameter(processor, swaraxt::IDs::filterModAmount, 0.0f);
    // Flat, immediate amplitude envelope: the render is a steady tone so the
    // spectrum is not contaminated by envelope motion.
    setParameter(processor, swaraxt::IDs::env2Attack, 0.0f);
    setParameter(processor, swaraxt::IDs::env2Decay, 0.0f);
    setParameter(processor, swaraxt::IDs::env2Sustain, 127.0f);
    setParameter(processor, swaraxt::IDs::env2Release, 0.0f);
    setParameter(processor, swaraxt::IDs::lfo1Rate, 0.0f);
    setParameter(processor, swaraxt::IDs::lfo2Rate, 0.0f);
    setParameter(processor, swaraxt::IDs::perfGlide, 0.0f);
}

std::vector<float> render(const Patch& patch, double sampleRate, int blockSize, double seconds)
{
    SwaraXtAudioProcessor processor;
    applyPatch(processor, patch);
    processor.prepareToPlay(sampleRate, blockSize);

    const int totalSamples = static_cast<int>(std::lround(seconds * sampleRate));
    std::vector<float> out;
    out.reserve(static_cast<std::size_t>(totalSamples));

    juce::AudioBuffer<float> buffer(2, blockSize);
    juce::MidiBuffer midi;
    midi.addEvent(juce::MidiMessage::noteOn(1, patch.note, static_cast<juce::uint8>(100)), 0);

    while (static_cast<int>(out.size()) < totalSamples)
    {
        buffer.clear();
        processor.processBlock(buffer, midi);
        midi.clear();
        const float* left = buffer.getReadPointer(0);
        const int take = std::min(blockSize, totalSamples - static_cast<int>(out.size()));
        out.insert(out.end(), left, left + take);
    }
    return out;
}

bool writeWavFloat32(const std::string& path, const std::vector<float>& mono, double sampleRate)
{
    juce::File file(path);
    file.getParentDirectory().createDirectory();
    file.deleteFile();
    juce::WavAudioFormat format;
    std::unique_ptr<juce::FileOutputStream> stream(file.createOutputStream());
    if (stream == nullptr)
        return false;
    std::unique_ptr<juce::AudioFormatWriter> writer(
        format.createWriterFor(stream.get(), sampleRate, 1, 32, {}, 0));
    if (writer == nullptr)
        return false;
    stream.release();
    const float* channels[1] { mono.data() };
    writer->writeFromFloatArrays(channels, 1, static_cast<int>(mono.size()));
    return true;
}

// --------------------------------------------------------------- production

struct Case { std::string tag; Patch patch; };

std::vector<Case> productionCases()
{
    std::vector<Case> cases;

    const auto note = [](const char* label, int midi) { return std::make_pair(std::string(label), midi); };
    const std::vector<std::pair<std::string, int>> notes {
        note("c2", 36), note("c4", 60), note("c6", 84), note("c7", 96) };

    for (const auto& [label, midiNote] : notes)
    {
        cases.push_back({ "saw_" + label, Patch { "saw", 1, 0, midiNote, 20000.0f, 0.0f, 0.0f, 0, 0 } });
        cases.push_back({ "pwm_" + label, Patch { "pwm", 2, 64, midiNote, 20000.0f, 0.0f, 0.0f, 0, 0 } });
    }
    cases.push_back({ "triangle_c4", Patch { "tri", 3, 0, 60, 20000.0f, 0.0f, 0.0f, 0, 0 } });
    cases.push_back({ "triangle_c7", Patch { "tri", 3, 0, 96, 20000.0f, 0.0f, 0.0f, 0, 0 } });
    cases.push_back({ "wavetable1_c4", Patch { "wt", 11, 64, 60, 20000.0f, 0.0f, 0.0f, 0, 0 } });
    cases.push_back({ "wavetable1_c6", Patch { "wt", 11, 64, 84, 20000.0f, 0.0f, 0.0f, 0, 0 } });
    cases.push_back({ "wavetable1_c7", Patch { "wt", 11, 64, 96, 20000.0f, 0.0f, 0.0f, 0, 0 } });
    cases.push_back({ "fm_c4", Patch { "fm", 10, 80, 60, 20000.0f, 0.0f, 0.0f, 0, 0 } });
    // Filtered production cases: bright enough to still expose SRC differences.
    cases.push_back({ "sawfilt_lowres_c4", Patch { "saw", 1, 0, 60, 12000.0f, 0.15f, 0.0f, 0, 0 } });
    cases.push_back({ "sawfilt_modres_c4", Patch { "saw", 1, 0, 60, 12000.0f, 0.55f, 0.0f, 0, 0 } });
    cases.push_back({ "pwmfilt_lowres_c4", Patch { "pwm", 2, 64, 60, 12000.0f, 0.15f, 0.0f, 0, 0 } });
    // PWM timbre sweep at three registers.
    for (int timbre : { 16, 32, 64, 96, 127 })
        for (const auto& [label, midiNote] : { note("c4", 60), note("c6", 84), note("c7", 96) })
            cases.push_back({ "pwmT" + std::to_string(timbre) + "_" + label,
                              Patch { "pwm", 2, timbre, midiNote, 20000.0f, 0.0f, 0.0f, 0, 0 } });

    return cases;
}

void runRenders()
{
    const std::string root = artifactRoot() + "/renders/" + converterName();
    juce::File(root).createDirectory();

    const std::vector<Case> cases = productionCases();

    for (double rate : { 44100.0, 96000.0 })
    {
        const std::string rateTag = std::to_string(static_cast<long>(std::lround(rate)));
        for (const Case& c : cases)
        {
            const std::vector<float> audio = render(c.patch, rate, 512, 3.0);
            writeWavFloat32(root + "/" + c.tag + "_" + rateTag + "_" + converterName() + ".wav",
                            audio, rate);
        }
        std::printf("rendered %zu cases at %.0f Hz (%s)\n", cases.size(), rate, converterName().c_str());
    }
}

// ------------------------------------------------------- native reference tap

#if SWARAXT_ENABLE_SHRUTHI_DEBUG_TAPS
// Captures the post-VCA native stream, i.e. exactly what the converter is asked
// to reconstruct. Section 17 of the study brief needs this: without it there is
// no way to tell a native Shruthi alias apart from an image the host-rate
// reconstruction invented. Converter-independent, so it is dumped once.
void runNativeDump()
{
    const std::string root = artifactRoot() + "/renders/native";
    juce::File(root).createDirectory();

    for (const Case& c : productionCases())
    {
        SwaraXtAudioProcessor processor;
        applyPatch(processor, c.patch);
        processor.prepareToPlay(44100.0, 512);

        std::vector<float> native;
        native.reserve(3 * 40000);
        processor.engineForTests().setDebugTapSink(
            &native, [](void* context, const swaraxt::SwaraXtEngine::DebugBlockCapture& capture) {
                auto* sink = static_cast<std::vector<float>*>(context);
                sink->insert(sink->end(), capture.postVca, capture.postVca + capture.samples);
            });

        juce::AudioBuffer<float> buffer(2, 512);
        juce::MidiBuffer midi;
        midi.addEvent(juce::MidiMessage::noteOn(1, c.patch.note, static_cast<juce::uint8>(100)), 0);
        for (int block = 0; block < 3 * 44100 / 512 + 2; ++block)
        {
            buffer.clear();
            processor.processBlock(buffer, midi);
            midi.clear();
        }
        processor.engineForTests().setDebugTapSink(nullptr, nullptr);

        writeWavFloat32(root + "/" + c.tag + "_native.wav", native,
                        swaraxt::SwaraXtEngine::kInternalSampleRate);
    }
    std::printf("dumped native pre-SRC reference streams\n");
}
#endif

// --------------------------------------------------------------- engine CPU

void runEngineCpu()
{
#if SWARAXT_ENABLE_IDLE_CPU_TESTS
    const std::string csvPath = artifactRoot() + "/measurements/engine-cpu-" + converterName() + ".csv";
    FILE* csv = std::fopen(csvPath.c_str(), "w");
    std::fprintf(csv, "converter,host_rate,block_size,total_ms,voice_ms,filter_ms,midi_ms,"
                      "src_output_ms,voice_pct,filter_pct,src_output_pct,"
                      "ns_per_host_sample,realtime_percent,native_blocks,filter_samples,peak\n");

    constexpr int kBlockSize = 256;
    constexpr int kBlocks = 4000;

    for (double rate : { 44100.0, 48000.0, 88200.0, 96000.0, 176400.0, 192000.0 })
    {
        SwaraXtAudioProcessor processor;
        applyPatch(processor, Patch { "saw", 1, 0, 72, 20000.0f, 0.0f, 0.0f, 0, 0 });
        processor.prepareToPlay(rate, kBlockSize);

        juce::AudioBuffer<float> buffer(2, kBlockSize);
        juce::MidiBuffer midi;
        midi.addEvent(juce::MidiMessage::noteOn(1, 72, static_cast<juce::uint8>(100)), 0);
        processor.processBlock(buffer, midi);
        midi.clear();
        for (int i = 0; i < 200; ++i)
            processor.processBlock(buffer, midi);

        auto& engine = processor.engineForTests();
        engine.resetCpuProfileForTests();
        double measuredPeak = 0.0;
        for (int block = 0; block < kBlocks; ++block)
        {
            processor.processBlock(buffer, midi);
            for (int i = 0; i < kBlockSize; ++i)
                measuredPeak = std::max(measuredPeak, std::abs(static_cast<double>(buffer.getSample(0, i))));
        }

        const auto& p = engine.cpuProfileForTests();
        const double total = static_cast<double>(p.processNanoseconds);
        const auto pct = [total](uint64_t v) { return total > 0.0 ? 100.0 * static_cast<double>(v) / total : 0.0; };
        const double nsPerHostSample = total / static_cast<double>(p.hostSamplesProduced);

        std::fprintf(csv, "%s,%.1f,%d,%.3f,%.3f,%.3f,%.3f,%.3f,%.2f,%.2f,%.2f,%.4f,%.4f,%llu,%llu,%.9f\n",
                     converterName().c_str(), rate, kBlockSize,
                     total / 1.0e6,
                     static_cast<double>(p.nativeVoiceNanoseconds) / 1.0e6,
                     static_cast<double>(p.filterNanoseconds) / 1.0e6,
                     static_cast<double>(p.midiAndClockNanoseconds) / 1.0e6,
                     static_cast<double>(p.srcAndOutputNanoseconds) / 1.0e6,
                     pct(p.nativeVoiceNanoseconds), pct(p.filterNanoseconds), pct(p.srcAndOutputNanoseconds),
                     nsPerHostSample, 100.0 * nsPerHostSample * rate / 1.0e9,
                     static_cast<unsigned long long>(p.nativeBlocksRendered),
                     static_cast<unsigned long long>(p.filterSamplesProcessed), measuredPeak);
        expect(measuredPeak > 0.0, "held note produces audio");
    }
    std::fclose(csv);
    std::printf("wrote %s\n", csvPath.c_str());
#else
    std::printf("engine cpu requires SWARAXT_ENABLE_IDLE_CPU_TESTS\n");
#endif
}

// ---------------------------------------------------------------- dormancy

void runDormancy()
{
#if SWARAXT_ENABLE_IDLE_CPU_TESTS
    const std::string csvPath = artifactRoot() + "/measurements/dormancy-" + converterName() + ".csv";
    FILE* csv = std::fopen(csvPath.c_str(), "w");
    std::fprintf(csv, "converter,host_rate,block_size,scenario,native_blocks,filter_samples,"
                      "skipped_host_samples,peak,exact_zero_or_onset_step,dormant\n");

    const auto scenario = [&](double rate, int blockSize, const char* name,
                              bool noteOn, bool noteOff, int settleBlocks) {
        SwaraXtAudioProcessor processor;
        applyPatch(processor, Patch { "saw", 1, 0, 60, 20000.0f, 0.0f, 0.0f, 0, 0 });
        processor.prepareToPlay(rate, blockSize);
        juce::AudioBuffer<float> buffer(2, blockSize);
        juce::MidiBuffer midi;

        if (noteOn)
        {
            midi.addEvent(juce::MidiMessage::noteOn(1, 60, static_cast<juce::uint8>(100)), 0);
            processor.processBlock(buffer, midi);
            midi.clear();
            for (int i = 0; i < 100; ++i)
                processor.processBlock(buffer, midi);
        }
        if (noteOff)
        {
            midi.addEvent(juce::MidiMessage::noteOff(1, 60), 0);
            processor.processBlock(buffer, midi);
            midi.clear();
        }
        for (int i = 0; i < settleBlocks; ++i)
            processor.processBlock(buffer, midi);

        auto& engine = processor.engineForTests();
        engine.resetCpuProfileForTests();
        double measuredPeak = 0.0;
        bool exactZero = true;
        constexpr int kMeasureBlocks = 500;
        for (int block = 0; block < kMeasureBlocks; ++block)
        {
            processor.processBlock(buffer, midi);
            for (int i = 0; i < blockSize; ++i)
            {
                const float v = buffer.getSample(0, i);
                if (v != 0.0f || ! std::isfinite(v))
                    exactZero = false;
                measuredPeak = std::max(measuredPeak, std::abs(static_cast<double>(v)));
            }
        }
        const auto& p = engine.cpuProfileForTests();
        std::fprintf(csv, "%s,%.1f,%d,%s,%llu,%llu,%llu,%.9f,%d,%d\n",
                     converterName().c_str(), rate, blockSize, name,
                     static_cast<unsigned long long>(p.nativeBlocksRendered),
                     static_cast<unsigned long long>(p.filterSamplesProcessed),
                     static_cast<unsigned long long>(p.dormantHostSamplesSkipped),
                     measuredPeak, exactZero ? 1 : 0, engine.dormantForTests() ? 1 : 0);

        expect(p.nativeBlocksRendered == 0, std::string(name) + ": no native blocks while idle");
        expect(p.filterSamplesProcessed == 0, std::string(name) + ": no filter samples while idle");
        expect(exactZero, std::string(name) + ": output is exact zero");
        expect(engine.dormantForTests(), std::string(name) + ": engine is dormant");
    };

    for (double rate : { 44100.0, 96000.0, 192000.0 })
        for (int blockSize : { 64, 512 })
        {
            scenario(rate, blockSize, "fresh_idle", false, false, 4);
            scenario(rate, blockSize, "post_release", true, true, 400);
            scenario(rate, blockSize, "long_dormant", true, true, 4000);
        }

    // Wake after a long dormant period must produce audio again with no stale
    // tail from the previous note.
    {
        SwaraXtAudioProcessor processor;
        applyPatch(processor, Patch { "saw", 1, 0, 60, 20000.0f, 0.0f, 0.0f, 0, 0 });
        processor.prepareToPlay(48000.0, 256);
        juce::AudioBuffer<float> buffer(2, 256);
        juce::MidiBuffer midi;
        midi.addEvent(juce::MidiMessage::noteOn(1, 60, static_cast<juce::uint8>(100)), 0);
        processor.processBlock(buffer, midi);
        midi.clear();
        for (int i = 0; i < 200; ++i)
            processor.processBlock(buffer, midi);
        midi.addEvent(juce::MidiMessage::noteOff(1, 60), 0);
        processor.processBlock(buffer, midi);
        midi.clear();
        for (int i = 0; i < 5000; ++i)
            processor.processBlock(buffer, midi);

        midi.addEvent(juce::MidiMessage::noteOn(1, 84, static_cast<juce::uint8>(100)), 0);
        double firstBlockPeak = 0.0;
        double firstSample = 0.0;
        processor.processBlock(buffer, midi);
        midi.clear();
        firstSample = buffer.getSample(0, 0);
        for (int i = 0; i < 256; ++i)
            firstBlockPeak = std::max(firstBlockPeak, std::abs(static_cast<double>(buffer.getSample(0, i))));
        double laterPeak = 0.0;
        for (int b = 0; b < 20; ++b)
        {
            processor.processBlock(buffer, midi);
            for (int i = 0; i < 256; ++i)
                laterPeak = std::max(laterPeak, std::abs(static_cast<double>(buffer.getSample(0, i))));
        }
        // The wake must start from the exact silence the dormant state left
        // behind: a stale tail would show up as a large first sample or as a
        // first block louder than the settled note.
        const double onsetStep = laterPeak > 0.0 ? std::abs(firstSample) / laterPeak : 0.0;
        std::fprintf(csv, "%s,48000.0,256,long_idle_wake,0,0,0,%.9f,%.6f,0\n",
                     converterName().c_str(), laterPeak, onsetStep);
        expect(laterPeak > 0.0, "long-idle wake produces audio");
        expect(onsetStep < 0.25, "wake starts from silence, not a stale tail");
        expect(firstBlockPeak <= laterPeak * 1.5, "wake has no onset burst");
    }

    std::fclose(csv);
    std::printf("wrote %s\n", csvPath.c_str());
#else
    std::printf("dormancy requires SWARAXT_ENABLE_IDLE_CPU_TESTS\n");
#endif
}

// ------------------------------------------------- block partition + onset

void runBlockPartition()
{
    const std::string csvPath = artifactRoot() + "/measurements/engine-blocks-" + converterName() + ".csv";
    FILE* csv = std::fopen(csvPath.c_str(), "w");
    std::fprintf(csv, "converter,host_rate,block_size,samples_compared,max_abs_difference,bit_exact\n");

    const Patch patch { "saw", 1, 0, 72, 20000.0f, 0.0f, 0.0f, 0, 0 };

    for (double rate : { 44100.0, 48000.0, 96000.0, 192000.0 })
    {
        const std::vector<float> reference = render(patch, rate, 512, 1.0);
        for (int blockSize : { 63, 64, 65, 127, 128, 129, 257 })
        {
            const std::vector<float> candidate = render(patch, rate, blockSize, 1.0);
            const std::size_t n = std::min(reference.size(), candidate.size());
            double worst = 0.0;
            for (std::size_t i = 0; i < n; ++i)
                worst = std::max(worst, std::abs(static_cast<double>(reference[i]) - candidate[i]));
            std::fprintf(csv, "%s,%.1f,%d,%zu,%.3e,%d\n",
                         converterName().c_str(), rate, blockSize, n, worst, worst == 0.0 ? 1 : 0);
            expect(worst == 0.0, "block partition " + std::to_string(blockSize)
                                     + " is sample identical at " + std::to_string(rate));
        }
    }
    std::fclose(csv);
    std::printf("wrote %s\n", csvPath.c_str());
}

// Time from a note-on landing in a process block to the first non-zero output
// sample. A longer reconstruction kernel keeps more already-rendered native
// samples queued ahead of the read point, which shows up here.
void runOnset()
{
    const std::string csvPath = artifactRoot() + "/measurements/onset-" + converterName() + ".csv";
    FILE* csv = std::fopen(csvPath.c_str(), "w");
    std::fprintf(csv, "converter,host_rate,onset_host_samples,onset_ms,onset_native_samples,"
                      "peak_host_samples,peak_ms\n");

    constexpr double kNativeRate = 20000000.0 / 510.0;

    for (double rate : { 44100.0, 48000.0, 88200.0, 96000.0, 176400.0, 192000.0 })
    {
        SwaraXtAudioProcessor processor;
        applyPatch(processor, Patch { "saw", 1, 0, 60, 20000.0f, 0.0f, 0.0f, 0, 0 });
        processor.prepareToPlay(rate, 256);
        juce::AudioBuffer<float> buffer(2, 256);
        juce::MidiBuffer midi;

        // Settle the dormant state first so the measurement starts from the
        // documented idle condition.
        for (int i = 0; i < 8; ++i)
            processor.processBlock(buffer, midi);

        midi.addEvent(juce::MidiMessage::noteOn(1, 60, static_cast<juce::uint8>(100)), 0);
        long onset = -1;
        long peakIndex = -1;
        double peakValue = 0.0;
        long produced = 0;
        for (int block = 0; block < 64; ++block)
        {
            processor.processBlock(buffer, midi);
            midi.clear();
            for (int i = 0; i < 256; ++i)
            {
                const double v = std::abs(static_cast<double>(buffer.getSample(0, i)));
                if (onset < 0 && v > 1.0e-7)
                    onset = produced;
                if (v > peakValue)
                {
                    peakValue = v;
                    peakIndex = produced;
                }
                ++produced;
            }
        }
        std::fprintf(csv, "%s,%.1f,%ld,%.4f,%.2f,%ld,%.4f\n",
                     converterName().c_str(), rate, onset, 1000.0 * static_cast<double>(onset) / rate,
                     static_cast<double>(onset) * kNativeRate / rate,
                     peakIndex, 1000.0 * static_cast<double>(peakIndex) / rate);
        expect(onset >= 0, "note-on produces audio");
    }
    std::fclose(csv);
    std::printf("wrote %s\n", csvPath.c_str());
}

// Note-on from dormancy costs nothing because the queue is rebuilt from the
// new note. While the engine is already sounding, however, the queue holds
// audio rendered ahead of the read point, and a longer kernel keeps more of it.
// This measures that: how long after a note-off (zero release) the output
// actually falls silent, and how long after a retrigger the new note appears.
void runControlLatency()
{
    const std::string csvPath = artifactRoot() + "/measurements/control-latency-" + converterName() + ".csv";
    FILE* csv = std::fopen(csvPath.c_str(), "w");
    std::fprintf(csv, "converter,host_rate,event,latency_host_samples,latency_ms,latency_native_samples\n");

    constexpr double kNativeRate = 20000000.0 / 510.0;

    for (double rate : { 44100.0, 48000.0, 88200.0, 96000.0, 176400.0, 192000.0 })
    {
        SwaraXtAudioProcessor processor;
        applyPatch(processor, Patch { "saw", 1, 0, 48, 20000.0f, 0.0f, 0.0f, 0, 0 });
        processor.prepareToPlay(rate, 256);
        juce::AudioBuffer<float> buffer(2, 256);
        juce::MidiBuffer midi;

        midi.addEvent(juce::MidiMessage::noteOn(1, 48, static_cast<juce::uint8>(100)), 0);
        processor.processBlock(buffer, midi);
        midi.clear();
        for (int i = 0; i < 200; ++i)
            processor.processBlock(buffer, midi);

        // Retrigger: a note three octaves up changes the waveform period
        // immediately in the native stream, so the first sample whose slope
        // differs marks the arrival of the new note.
        midi.addEvent(juce::MidiMessage::noteOff(1, 48), 0);
        midi.addEvent(juce::MidiMessage::noteOn(1, 84, static_cast<juce::uint8>(100)), 0);
        long produced = 0;
        long retrigger = -1;
        double previous = 0.0;
        for (int block = 0; block < 32 && retrigger < 0; ++block)
        {
            processor.processBlock(buffer, midi);
            midi.clear();
            for (int i = 0; i < 256; ++i)
            {
                const double v = buffer.getSample(0, i);
                // A three-octave jump makes the saw ramp eight times steeper.
                if (produced > 0 && std::abs(v - previous) > 0.02 && retrigger < 0)
                    retrigger = produced;
                previous = v;
                ++produced;
            }
        }
        std::fprintf(csv, "%s,%.1f,retrigger,%ld,%.4f,%.2f\n",
                     converterName().c_str(), rate, retrigger,
                     1000.0 * static_cast<double>(retrigger) / rate,
                     static_cast<double>(retrigger) * kNativeRate / rate);

        // Note-off to silence, with the amplitude release already at zero.
        SwaraXtAudioProcessor releaseProcessor;
        applyPatch(releaseProcessor, Patch { "saw", 1, 0, 48, 20000.0f, 0.0f, 0.0f, 0, 0 });
        releaseProcessor.prepareToPlay(rate, 256);
        juce::MidiBuffer releaseMidi;
        releaseMidi.addEvent(juce::MidiMessage::noteOn(1, 48, static_cast<juce::uint8>(100)), 0);
        releaseProcessor.processBlock(buffer, releaseMidi);
        releaseMidi.clear();
        for (int i = 0; i < 200; ++i)
            releaseProcessor.processBlock(buffer, releaseMidi);

        releaseMidi.addEvent(juce::MidiMessage::noteOff(1, 48), 0);
        produced = 0;
        long silence = -1;
        for (int block = 0; block < 64 && silence < 0; ++block)
        {
            releaseProcessor.processBlock(buffer, releaseMidi);
            releaseMidi.clear();
            for (int i = 0; i < 256; ++i)
            {
                if (std::abs(static_cast<double>(buffer.getSample(0, i))) < 1.0e-4 && silence < 0)
                    silence = produced;
                else if (std::abs(static_cast<double>(buffer.getSample(0, i))) >= 1.0e-4)
                    silence = -1;
                ++produced;
            }
        }
        std::fprintf(csv, "%s,%.1f,note_off_to_silence,%ld,%.4f,%.2f\n",
                     converterName().c_str(), rate, silence,
                     1000.0 * static_cast<double>(silence) / rate,
                     static_cast<double>(silence) * kNativeRate / rate);
    }
    std::fclose(csv);
    std::printf("wrote %s\n", csvPath.c_str());
}

// Pitch must not move when the converter changes.
void runPitch()
{
    const std::string csvPath = artifactRoot() + "/measurements/pitch-" + converterName() + ".csv";
    FILE* csv = std::fopen(csvPath.c_str(), "w");
    std::fprintf(csv, "converter,host_rate,note,zero_cross_hz,rms,peak,dc_mean\n");

    for (double rate : { 44100.0, 96000.0 })
        for (int note : { 36, 60, 84, 96 })
        {
            const std::vector<float> audio =
                render(Patch { "saw", 1, 0, note, 20000.0f, 0.0f, 0.0f, 0, 0 }, rate, 512, 4.0);

            // Count upward zero crossings over a whole number of seconds of the
            // steady portion; a saw has exactly one per period.
            const std::size_t begin = static_cast<std::size_t>(rate);
            std::size_t crossings = 0;
            std::size_t first = 0;
            std::size_t last = 0;
            double sum = 0.0;
            double sumSquares = 0.0;
            double peak = 0.0;
            for (std::size_t i = begin + 1; i < audio.size(); ++i)
            {
                if (audio[i - 1] <= 0.0f && audio[i] > 0.0f)
                {
                    if (crossings == 0)
                        first = i;
                    last = i;
                    ++crossings;
                }
                sum += audio[i];
                sumSquares += static_cast<double>(audio[i]) * audio[i];
                peak = std::max(peak, std::abs(static_cast<double>(audio[i])));
            }
            const double span = static_cast<double>(last - first);
            const double hz = crossings > 1 && span > 0.0
                ? rate * static_cast<double>(crossings - 1) / span : 0.0;
            const double n = static_cast<double>(audio.size() - begin - 1);
            std::fprintf(csv, "%s,%.1f,%d,%.5f,%.8f,%.8f,%.3e\n",
                         converterName().c_str(), rate, note, hz,
                         std::sqrt(sumSquares / n), peak, sum / n);
        }
    std::fclose(csv);
    std::printf("wrote %s\n", csvPath.c_str());
}

}  // namespace

int main(int argc, char** argv)
{
    juce::ScopedJuceInitialiser_GUI juceInit;
    const std::string mode = argc > 1 ? argv[1] : "all";

    juce::File(artifactRoot() + "/measurements").createDirectory();

    if (mode == "renders" || mode == "all")
        runRenders();
#if SWARAXT_ENABLE_SHRUTHI_DEBUG_TAPS
    if (mode == "nativedump")
        runNativeDump();
#endif
    if (mode == "cpu" || mode == "all")
        runEngineCpu();
    if (mode == "dormancy" || mode == "all")
        runDormancy();
    if (mode == "blocks" || mode == "all")
        runBlockPartition();
    if (mode == "onset" || mode == "all")
        runOnset();
    if (mode == "latency" || mode == "all")
        runControlLatency();
    if (mode == "pitch" || mode == "all")
        runPitch();

    std::printf("%s: %d failure(s)\n", converterName().c_str(), failures);
    return failures == 0 ? 0 : 1;
}
