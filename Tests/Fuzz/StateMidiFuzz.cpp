// Copyright 2026 MontroneDSP. SPDX-License-Identifier: GPL-3.0-or-later
#include <JuceHeader.h>
#include "Plugin/PluginProcessor.h"
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <utility>

namespace {
struct Harness {
    juce::ScopedJuceInitialiser_GUI init;
    SwaraXtAudioProcessor processor;
    juce::MemoryBlock initial;
    juce::AudioBuffer<float> audio {2, 128};
    Harness() {
        processor.getStateInformation(initial);
        if (initial.getSize() == 0) std::abort();
        processor.prepareToPlay(48000, 128);
    }
};
}

extern "C" int LLVMFuzzerTestOneInput(const uint8_t* data, size_t size)
{
    if (size == 0 || size > 65536) return 0;
    static Harness harness;
    auto& processor = harness.processor;
    processor.setStateInformation(harness.initial.getData(), static_cast<int>(harness.initial.getSize()));
    const bool beforePrepare = (data[0] & 128) != 0;
    if (beforePrepare) processor.releaseResources();
    juce::MidiBuffer midi;
    switch (data[0] % 3) {
    case 0:
        processor.setStateInformation(data + 1, static_cast<int>(size - 1));
        break;
    case 1: {
        // Mutate a genuine state so the XML, sequence, modulation and tuning
        // restoration paths remain reachable beyond the binary envelope.
        auto state = harness.initial;
        auto* bytes = static_cast<uint8_t*>(state.getData());
        for (size_t index = 1; index + 2 < size; index += 3) {
            const size_t offset = (static_cast<size_t>(data[index]) * 256 + data[index + 1]) % state.getSize();
            bytes[offset] ^= data[index + 2];
        }
        size_t length = state.getSize();
        if (size > 2 && (data[0] & 64) != 0)
            length = (static_cast<size_t>(data[1]) * 256 + data[2]) % (length + 1);
        processor.setStateInformation(state.getData(), static_cast<int>(length));
        break;
    }
    case 2:
        // Bounded dense event queues, including MIDI endpoints and releases.
        for (const auto& setting : {std::make_pair("seq.mode", static_cast<float>((data[0] >> 2) % 3)),
                                   std::make_pair("seq.midiOut", (data[0] & 32) != 0 ? 1.0f : 0.0f)}) {
            auto* parameter = processor.getApvts().getParameter(setting.first);
            if (parameter == nullptr) std::abort();
            parameter->setValueNotifyingHost(parameter->convertTo0to1(setting.second));
        }
        midi.ensureSize(65536);
        for (size_t index = 1, events = 0; index + 2 < size && events < 4096; index += 3, ++events) {
            const int channel = 1 + (data[index] >> 4);
            const int note = data[index + 1] % 128;
            const auto velocity = static_cast<juce::uint8>(data[index + 2] % 128);
            const auto message = data[index] % 4 == 0 ? juce::MidiMessage::noteOn(channel, note, velocity)
                : data[index] % 4 == 1 ? juce::MidiMessage::noteOff(channel, note)
                : data[index] % 4 == 2 ? juce::MidiMessage::controllerEvent(channel, note, velocity)
                : juce::MidiMessage::allNotesOff(channel);
            midi.addEvent(message, static_cast<int>(events % 128));
        }
        break;
    }
    if (beforePrepare) processor.prepareToPlay(48000, 128);
    for (int block = 0; block < 4; ++block) {
        if (block != 0) midi.clear();
        processor.processBlock(harness.audio, midi);
        for (int channel = 0; channel < 2; ++channel)
            for (int sample = 0; sample < 128; ++sample)
                if (!std::isfinite(harness.audio.getSample(channel, sample))) std::abort();
    }
    return 0;
}
