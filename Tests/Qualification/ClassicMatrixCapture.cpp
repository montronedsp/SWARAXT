// Copyright 2026 MontroneDSP. SPDX-License-Identifier: GPL-3.0-or-later
#include "Plugin/PluginProcessor.h"
#include "Plugin/SwaraXtParameterLayout.h"

#include "shruthi/patch.h"
#include "shruthi/sequencer_settings.h"

#include <cstdint>
#include <cstdio>
#include <cstring>
#include <cmath>
#include <functional>
#include <string>
#include <vector>

namespace {
struct Timed
{
    int sample = 0;
    juce::MidiMessage message;
};

std::uint64_t hashSamples(const std::vector<float>& samples)
{
    std::uint64_t hash = 14695981039346656037ull;
    for (float sample : samples)
    {
        std::uint32_t bits = 0;
        std::memcpy(&bits, &sample, sizeof(bits));
        for (int byte = 0; byte < 4; ++byte)
        {
            hash ^= (bits >> (8 * byte)) & 0xffu;
            hash *= 1099511628211ull;
        }
    }
    return hash;
}

void setParam(SwaraXtAudioProcessor& processor, const char* id, float value)
{
    if (auto* parameter = processor.getApvts().getParameter(id))
        parameter->setValueNotifyingHost(parameter->convertTo0to1(value));
    else
        std::printf("FAIL missing %s\n", id);
}

void neutral(SwaraXtAudioProcessor& processor)
{
    setParam(processor, swaraxt::IDs::osc1Shape, 1.0f);
    setParam(processor, swaraxt::IDs::osc1Param, 0.0f);
    setParam(processor, swaraxt::IDs::osc1Range, 0.0f);
    setParam(processor, swaraxt::IDs::osc1Option, 0.0f);
    setParam(processor, swaraxt::IDs::osc2Shape, 0.0f);
    setParam(processor, swaraxt::IDs::mixBalance, 0.0f);
    setParam(processor, swaraxt::IDs::mixSub, 0.0f);
    setParam(processor, swaraxt::IDs::mixNoise, 0.0f);
    setParam(processor, swaraxt::IDs::filterCutoff, 1000.0f);
    setParam(processor, swaraxt::IDs::filterResonance, 0.0f);
    setParam(processor, swaraxt::IDs::filterEnvDepth, 0.0f);
    setParam(processor, swaraxt::IDs::filterLfoDepth, 0.0f);
    setParam(processor, swaraxt::IDs::filterKeyTracking, 0.5f);
    setParam(processor, swaraxt::IDs::perfGlide, 0.0f);
    setParam(processor, swaraxt::IDs::master, 0.5f);
    setParam(processor, swaraxt::IDs::dspFxProgram, 0.0f);
    setParam(processor, swaraxt::IDs::dspFxParam1, 0.0f);
    setParam(processor, swaraxt::IDs::dspFxParam2, 0.0f);
    setParam(processor, swaraxt::IDs::env1Sustain, 127.0f);
    setParam(processor, swaraxt::IDs::env2Sustain, 127.0f);
    setParam(processor, "mod.row1.amount", 0.0f);
}

std::vector<float> render(SwaraXtAudioProcessor& processor, double rate, int block, int blocks,
                          const std::vector<Timed>& events)
{
    processor.setNonRealtime(true);
    processor.prepareToPlay(rate, block);
    juce::AudioBuffer<float> audio(2, block);
    std::vector<float> out;
    out.reserve(static_cast<std::size_t>(blocks) * static_cast<std::size_t>(block));
    for (int index = 0; index < blocks; ++index)
    {
        juce::MidiBuffer midi;
        const int origin = index * block;
        for (const auto& event : events)
            if (event.sample >= origin && event.sample < origin + block)
                midi.addEvent(event.message, event.sample - origin);
        audio.clear();
        processor.processBlock(audio, midi);
        const float* left = audio.getReadPointer(0);
        out.insert(out.end(), left, left + block);
    }
    return out;
}

std::vector<Timed> note(int midiNote, int sample = 0, int velocity = 100)
{
    return {Timed{sample, juce::MidiMessage::noteOn(1, midiNote, static_cast<juce::uint8>(velocity))}};
}

class Capture
{
public:
    explicit Capture(FILE* file) : file_(file) {}

    void add(const char* name, double rate, int block, const std::vector<float>& samples)
    {
        const auto hash = hashSamples(samples);
        std::printf("scenario=%s rate=%.0f block=%d samples=%zu hash=%016llx\n",
            name, rate, block, samples.size(), static_cast<unsigned long long>(hash));
        const auto length = static_cast<std::uint32_t>(std::strlen(name));
        const auto count = static_cast<std::uint32_t>(samples.size());
        const auto blockSize = static_cast<std::int32_t>(block);
        std::fwrite(&length, 4, 1, file_);
        std::fwrite(name, 1, length, file_);
        std::fwrite(&rate, sizeof(rate), 1, file_);
        std::fwrite(&blockSize, 4, 1, file_);
        std::fwrite(&count, 4, 1, file_);
        if (count != 0)
            std::fwrite(samples.data(), sizeof(float), count, file_);
        ++rows_;
    }

    void run(const char* name, double rate, int block, int blocks, const std::vector<Timed>& events,
             const std::function<void(SwaraXtAudioProcessor&)>& setup,
             const std::function<void(SwaraXtAudioProcessor&, int)>& during = {})
    {
        SwaraXtAudioProcessor processor;
        neutral(processor);
        setup(processor);
        processor.setNonRealtime(true);
        processor.prepareToPlay(rate, block);
        juce::AudioBuffer<float> audio(2, block);
        std::vector<float> out;
        out.reserve(static_cast<std::size_t>(blocks) * static_cast<std::size_t>(block));
        for (int index = 0; index < blocks; ++index)
        {
            if (during)
                during(processor, index);
            juce::MidiBuffer midi;
            const int origin = index * block;
            for (const auto& event : events)
                if (event.sample >= origin && event.sample < origin + block)
                    midi.addEvent(event.message, event.sample - origin);
            audio.clear();
            processor.processBlock(audio, midi);
            const float* left = audio.getReadPointer(0);
            out.insert(out.end(), left, left + block);
        }
        add(name, rate, block, out);
    }

    int rows() const { return rows_; }

private:
    FILE* file_ = nullptr;
    int rows_ = 0;
};
}

int main(int argc, char** argv)
{
    juce::ScopedJuceInitialiser_GUI juce;
    if (argc < 2)
    {
        std::printf("usage: ClassicMatrixCapture <output.bin>\n");
        return 2;
    }
    FILE* file = nullptr;
    if (fopen_s(&file, argv[1], "wb") != 0 || file == nullptr)
    {
        std::printf("FAIL open %s\n", argv[1]);
        return 2;
    }
    const char magic[] = "SWC1";
    std::fwrite(magic, 1, 4, file);
    Capture capture(file);
    const auto held = note(60);

    for (int byte : {0, 1, 127, 254, 255})
    {
        const float hz = std::clamp(20000.0f * std::exp2((static_cast<float>(byte) - 254.0f) / 24.0f), 10.0f, 20000.0f);
        const std::string name = "cutoff-byte-" + std::to_string(byte);
        capture.run(name.c_str(), 48000.0, 128, 24, held, [&](SwaraXtAudioProcessor& processor) {
            setParam(processor, swaraxt::IDs::filterCutoff, hz);
        });
    }
    capture.run("resonance-min", 48000.0, 128, 24, held, [](SwaraXtAudioProcessor& processor) {
        setParam(processor, swaraxt::IDs::filterResonance, 0.0f);
    });
    capture.run("resonance-mid", 48000.0, 128, 24, held, [](SwaraXtAudioProcessor& processor) {
        setParam(processor, swaraxt::IDs::filterResonance, 0.5f);
    });
    capture.run("resonance-max", 48000.0, 128, 24, held, [](SwaraXtAudioProcessor& processor) {
        setParam(processor, swaraxt::IDs::filterResonance, 1.0f);
    });
    capture.run("tracking-negative", 48000.0, 128, 24, note(76), [](SwaraXtAudioProcessor& processor) {
        setParam(processor, swaraxt::IDs::filterKeyTracking, 0.0f);
    });
    capture.run("tracking-zero", 48000.0, 128, 24, note(76), [](SwaraXtAudioProcessor& processor) {
        setParam(processor, swaraxt::IDs::filterKeyTracking, 0.5f);
    });
    capture.run("tracking-positive", 48000.0, 128, 24, note(76), [](SwaraXtAudioProcessor& processor) {
        setParam(processor, swaraxt::IDs::filterKeyTracking, 1.0f);
    });
    capture.run("duo-tracking", 48000.0, 128, 24, note(76), [](SwaraXtAudioProcessor& processor) {
        setParam(processor, swaraxt::IDs::osc1Option, static_cast<float>(shruthi::OP_DUO));
        setParam(processor, swaraxt::IDs::filterKeyTracking, 1.0f);
    });
    capture.run("envelope", 48000.0, 128, 32, held, [](SwaraXtAudioProcessor& processor) {
        setParam(processor, swaraxt::IDs::filterEnvDepth, 63.0f);
        setParam(processor, swaraxt::IDs::env1Attack, 0.0f);
        setParam(processor, swaraxt::IDs::env1Decay, 40.0f);
        setParam(processor, swaraxt::IDs::env1Sustain, 20.0f);
    });
    capture.run("lfo", 48000.0, 128, 48, held, [](SwaraXtAudioProcessor& processor) {
        setParam(processor, swaraxt::IDs::filterLfoDepth, 63.0f);
        setParam(processor, swaraxt::IDs::lfo1Rate, 90.0f);
    });
    capture.run("matrix-cutoff", 48000.0, 128, 32, held, [](SwaraXtAudioProcessor& processor) {
        setParam(processor, "mod.row1.source", 0.0f);
        setParam(processor, "mod.row1.destination", 0.0f);
        setParam(processor, "mod.row1.amount", 40.0f);
        setParam(processor, swaraxt::IDs::lfo1Rate, 80.0f);
    });
    capture.run("glide", 48000.0, 128, 40, {
        Timed{0, juce::MidiMessage::noteOn(1, 48, static_cast<juce::uint8>(100))},
        Timed{1280, juce::MidiMessage::noteOn(1, 72, static_cast<juce::uint8>(100))}
    }, [](SwaraXtAudioProcessor& processor) {
        setParam(processor, swaraxt::IDs::perfGlide, 80.0f);
    });
    capture.run("repeated-note", 48000.0, 128, 24, {
        Timed{0, juce::MidiMessage::noteOn(1, 60, static_cast<juce::uint8>(110))},
        Timed{512, juce::MidiMessage::noteOff(1, 60)},
        Timed{768, juce::MidiMessage::noteOn(1, 60, static_cast<juce::uint8>(90))}
    }, [](SwaraXtAudioProcessor&) {});
    capture.run("poly", 48000.0, 128, 24, {
        Timed{0, juce::MidiMessage::noteOn(1, 60, static_cast<juce::uint8>(100))},
        Timed{0, juce::MidiMessage::noteOn(1, 64, static_cast<juce::uint8>(100))},
        Timed{0, juce::MidiMessage::noteOn(1, 67, static_cast<juce::uint8>(80))}
    }, [](SwaraXtAudioProcessor&) {});
    capture.run("sequencer-step", 48000.0, 128, 64, held, [](SwaraXtAudioProcessor& processor) {
        processor.setCurrentProgram(11);
        setParam(processor, swaraxt::IDs::seqMode, static_cast<float>(shruthi::SEQUENCER_MODE_STEP));
        setParam(processor, swaraxt::IDs::seqTempo, 140.0f);
    });
    capture.run("sequencer-seq", 48000.0, 128, 64, held, [](SwaraXtAudioProcessor& processor) {
        processor.setCurrentProgram(11);
        setParam(processor, swaraxt::IDs::seqMode, static_cast<float>(shruthi::SEQUENCER_MODE_SEQ));
        setParam(processor, swaraxt::IDs::seqTempo, 140.0f);
    });
    const int programs = SwaraXtAudioProcessor{}.getNumPrograms();
    for (int program = 0; program < programs; ++program)
    {
        const std::string name = "factory-" + std::to_string(program);
        capture.run(name.c_str(), 48000.0, 128, 12, held, [&](SwaraXtAudioProcessor& processor) {
            processor.setCurrentProgram(program);
        });
    }
    capture.run("state-restore", 48000.0, 128, 16, held, [](SwaraXtAudioProcessor& processor) {
        setParam(processor, swaraxt::IDs::filterCutoff, 3500.0f);
        setParam(processor, swaraxt::IDs::filterResonance, 0.35f);
        processor.prepareToPlay(48000.0, 128);
        juce::AudioBuffer<float> audio(2, 128);
        juce::MidiBuffer midi;
        midi.addEvent(juce::MidiMessage::noteOn(1, 60, static_cast<juce::uint8>(100)), 0);
        processor.processBlock(audio, midi);
        juce::MemoryBlock state;
        processor.getStateInformation(state);
        processor.releaseResources();
        processor.setStateInformation(state.getData(), static_cast<int>(state.getSize()));
    });
    for (double rate : {44100.0, 48000.0, 96000.0})
    {
        for (int block : {64, 128, 512})
        {
            const std::string name = "rate-block-" + std::to_string(static_cast<int>(rate)) + "-" + std::to_string(block);
            capture.run(name.c_str(), rate, block, 16, held, [](SwaraXtAudioProcessor& processor) {
                setParam(processor, swaraxt::IDs::filterCutoff, 2500.0f);
                setParam(processor, swaraxt::IDs::filterResonance, 0.3f);
            });
        }
    }
    {
        SwaraXtAudioProcessor processor;
        neutral(processor);
        setParam(processor, swaraxt::IDs::filterCutoff, 1800.0f);
        setParam(processor, swaraxt::IDs::filterResonance, 0.4f);
        processor.setFilterQuality(swaraxt::FilterQuality::normal);
        auto samples = render(processor, 48000.0, 128, 8, held);
        processor.setFilterQuality(swaraxt::FilterQuality::high);
        auto high = render(processor, 48000.0, 128, 8, {});
        processor.setFilterQuality(swaraxt::FilterQuality::eco);
        auto eco = render(processor, 48000.0, 128, 8, {});
        samples.insert(samples.end(), high.begin(), high.end());
        samples.insert(samples.end(), eco.begin(), eco.end());
        capture.add("quality-transition", 48000.0, 128, samples);
        processor.reset();
        processor.releaseResources();
        capture.add("reprepare", 48000.0, 64, render(processor, 48000.0, 64, 12, note(67)));
    }
    capture.run("saturation", 48000.0, 128, 24, held, [](SwaraXtAudioProcessor& processor) {
        setParam(processor, swaraxt::IDs::filterCutoff, 12000.0f);
        setParam(processor, swaraxt::IDs::filterResonance, 1.0f);
        setParam(processor, swaraxt::IDs::master, 1.0f);
    });
    const std::vector<Timed> fxNote = {
        Timed{0, juce::MidiMessage::noteOn(1, 60, static_cast<juce::uint8>(100))},
        Timed{256, juce::MidiMessage::noteOff(1, 60)}
    };
    for (int fx = 0; fx <= 16; ++fx)
    {
        const std::string name = "board-fx-" + std::to_string(fx);
        capture.run(name.c_str(), 48000.0, 128, 400, fxNote, [&](SwaraXtAudioProcessor& processor) {
            setParam(processor, swaraxt::IDs::dspFxProgram, static_cast<float>(fx));
            setParam(processor, swaraxt::IDs::dspFxParam1, 20.0f);
            setParam(processor, swaraxt::IDs::dspFxParam2, fx == 15 ? 0.0f : 40.0f);
        });
    }
    capture.run("board-looper-replay", 48000.0, 128, 160, held, [](SwaraXtAudioProcessor& processor) {
        setParam(processor, swaraxt::IDs::dspFxProgram, 15.0f);
        setParam(processor, swaraxt::IDs::dspFxParam1, 8.0f);
        setParam(processor, swaraxt::IDs::dspFxParam2, 0.0f);
    }, [](SwaraXtAudioProcessor& processor, int block) {
        if (block == 40)
            setParam(processor, swaraxt::IDs::dspFxParam2, 40.0f);
    });

    std::printf("compiler=_MSC_FULL_VER=%d rows=%d\n", _MSC_FULL_VER, capture.rows());
    std::fclose(file);
    return 0;
}
