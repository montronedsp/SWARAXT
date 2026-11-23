// Copyright 2026 MontroneDSP. SPDX-License-Identifier: GPL-3.0-or-later
#include <JuceHeader.h>
#include "Plugin/PluginProcessor.h"
#include <cmath>
#include <cstdio>
#include <string>

// The annotation defines a genuine callback context while keeping production
// source unchanged. Inputs, parameter writes and fixtures are prepared outside.
static void callback(SwaraXtAudioProcessor& processor, juce::AudioBuffer<float>& audio,
                     juce::MidiBuffer& midi) [[clang::nonblocking]]
{
    processor.processBlock(audio, midi);
}
int main(int argc, char** argv)
{
    juce::ScopedJuceInitialiser_GUI init;
    if (argc > 2) return 2;
    const std::string scenario(argc == 2 ? argv[1] : "notes");
    if (scenario != "notes" && scenario != "sequencer" && scenario != "restore") return 2;
    SwaraXtAudioProcessor processor;
    processor.prepareToPlay(48000, 128);
    if (scenario == "sequencer") {
        auto* mode = processor.getApvts().getParameter("seq.mode");
        if (mode == nullptr) return 1;
        mode->setValueNotifyingHost(mode->convertTo0to1(2));
    }
    juce::MemoryBlock state;
    processor.getStateInformation(state);
    juce::AudioBuffer<float> audio(2, 128);
    juce::MidiBuffer midi;
    midi.ensureSize(4096);
    float peak = 0;
    for (int block = 0; block < 512; ++block) {
        midi.clear();
        if (block % 32 == 0)
            midi.addEvent(juce::MidiMessage::noteOn(1, (block / 32) % 2 == 0 ? 0 : 127, juce::uint8(100)), 0);
        if (block % 32 == 30)
            midi.addEvent(juce::MidiMessage::allNotesOff(1), 127);
        if (scenario == "restore" && block == 256)
            processor.setStateInformation(state.getData(), static_cast<int>(state.getSize()));
        callback(processor, audio, midi);
        for (int channel = 0; channel < 2; ++channel)
        for (int sample = 0; sample < 128; ++sample) {
            const float value = audio.getSample(channel, sample);
            if (!std::isfinite(value)) return 1;
            peak = std::max(peak, std::abs(value));
        }
    }
    processor.releaseResources();
    std::printf("RTSan callback scenario=%s peak=%g\n", scenario.c_str(), static_cast<double>(peak));
    return peak > 0 ? 0 : 1;
}
