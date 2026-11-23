// Copyright 2026 MontroneDSP. SPDX-License-Identifier: GPL-3.0-or-later
#include "Plugin/PluginProcessor.h"
#include "Plugin/SwaraXtParameterLayout.h"

#include "shruthi/patch.h"
#include "shruthi/sequencer_settings.h"

#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <cmath>
#include <functional>
#include <memory>
#include <string>
#include <vector>

namespace {
int gFailures = 0;

void fail(const char* message)
{
    ++gFailures;
    std::printf("FAIL: %s\n", message);
}

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
        fail(id);
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
    explicit Capture(juce::FileOutputStream& stream) : stream_(&stream) {}

    bool writeBytes(const void* data, std::size_t bytes)
    {
        if (stream_ == nullptr || ! stream_->write(data, bytes))
        {
            fail("capture write");
            return false;
        }
        return true;
    }

    void add(const char* name, double rate, int block, int expectedSamples, const std::vector<float>& samples)
    {
        float peak = 0.0f;
        for (float sample : samples)
        {
            if (! std::isfinite(sample))
                fail(name);
            peak = std::max(peak, std::abs(sample));
        }
        if (static_cast<int>(samples.size()) != expectedSamples || name == nullptr || name[0] == '\0')
            fail(name == nullptr ? "scenario" : name);
        if (peak <= 1.0e-8f)
            fail(name);
        const auto hash = hashSamples(samples);
        std::printf("scenario=%s rate=%.0f block=%d samples=%zu hash=%016llx\n",
            name, rate, block, samples.size(), static_cast<unsigned long long>(hash));
        const auto length = static_cast<std::uint32_t>(std::strlen(name));
        const auto count = static_cast<std::uint32_t>(samples.size());
        const auto blockSize = static_cast<std::int32_t>(block);
        writeBytes(&length, 4);
        writeBytes(name, length);
        writeBytes(&rate, sizeof(rate));
        writeBytes(&blockSize, 4);
        writeBytes(&count, 4);
        if (count != 0)
            writeBytes(samples.data(), sizeof(float) * count);
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
        add(name, rate, block, blocks * block, out);
    }

    int rows() const { return rows_; }

private:
    juce::FileOutputStream* stream_ = nullptr;
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
    const juce::File output(juce::String::fromUTF8(argv[1]));
    std::unique_ptr<juce::FileOutputStream> stream(output.createOutputStream());
    if (stream == nullptr || ! stream->openedOk())
    {
        std::printf("FAIL open %s\n", argv[1]);
        return 2;
    }
    const char magic[] = "SWC1";
    Capture capture(*stream);
    if (! capture.writeBytes(magic, 4))
        return 1;
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
    if (programs != 57)
        fail("factory program count");
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
        capture.add("quality-reprepare", 48000.0, 128, static_cast<int>(samples.size()), samples);
        processor.setNonRealtime(true);
        processor.prepareToPlay(48000.0, 128);
        processor.setFilterQuality(swaraxt::FilterQuality::normal);
        std::vector<float> live;
        auto appendLive = [&](int blocks, const std::vector<Timed>& events) {
            juce::AudioBuffer<float> audio(2, 128);
            for (int index = 0; index < blocks; ++index)
            {
                juce::MidiBuffer midi;
                const int origin = index * 128;
                for (const auto& event : events)
                    if (event.sample >= origin && event.sample < origin + 128)
                        midi.addEvent(event.message, event.sample - origin);
                audio.clear();
                processor.processBlock(audio, midi);
                const float* left = audio.getReadPointer(0);
                live.insert(live.end(), left, left + 128);
            }
        };
        appendLive(8, held);
        processor.setFilterQuality(swaraxt::FilterQuality::high);
        appendLive(8, {});
        processor.setFilterQuality(swaraxt::FilterQuality::eco);
        appendLive(8, {});
        capture.add("quality-live", 48000.0, 128, 3072, live);
        processor.reset();
        processor.releaseResources();
        const auto reprepared = render(processor, 48000.0, 64, 12, note(67));
        capture.add("reprepare", 48000.0, 64, 768, reprepared);
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

    if (capture.rows() != 109)
        fail("scenario count");
#if defined(__clang__)
    std::printf("compiler=clang %d.%d.%d rows=%d failures=%d\n",
        __clang_major__, __clang_minor__, __clang_patchlevel__, capture.rows(), gFailures);
#elif defined(_MSC_VER)
    std::printf("compiler=msvc %d rows=%d failures=%d\n", _MSC_VER, capture.rows(), gFailures);
#elif defined(__GNUC__)
    std::printf("compiler=gcc %d.%d.%d rows=%d failures=%d\n",
        __GNUC__, __GNUC_MINOR__, __GNUC_PATCHLEVEL__, capture.rows(), gFailures);
#else
    std::printf("compiler=unknown rows=%d failures=%d\n", capture.rows(), gFailures);
#endif
    stream->flush();
    if (! stream->getStatus().wasOk())
        fail("capture flush");
    return gFailures == 0 ? 0 : 1;
}
