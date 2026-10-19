// Copyright 2026 MontroneDSP. SPDX-License-Identifier: GPL-3.0-or-later
#include <JuceHeader.h>
#include "Plugin/PluginProcessor.h"
#include <atomic>
#include <cmath>
#include <condition_variable>
#include <mutex>
#include <thread>

int main()
{
    juce::ScopedJuceInitialiser_GUI init;
    SwaraXtAudioProcessor processor;
    processor.prepareToPlay(48000, 128);
    std::mutex mutex;
    std::condition_variable cv;
    bool start = false;
    bool firstBlock = false;
    bool controlStarted = false;
    std::atomic<int> rendered {0};
    std::atomic<bool> finite {true};
    std::thread audio([&] {
        juce::AudioBuffer<float> samples(2, 128);
        juce::MidiBuffer midi;
        midi.ensureSize(1024);
        { std::unique_lock<std::mutex> lock(mutex); cv.wait(lock, [&] { return start; }); }
        for (int block = 0; block < 1024; ++block) {
            midi.clear();
            if (block % 64 == 0) midi.addEvent(juce::MidiMessage::noteOn(1, block % 128, juce::uint8(100)), 0);
            if (block % 64 == 63) midi.addEvent(juce::MidiMessage::allNotesOff(1), 127);
            processor.processBlock(samples, midi);
            for (int channel = 0; channel < 2; ++channel)
            for (int sample = 0; sample < 128; ++sample)
                if (!std::isfinite(samples.getSample(channel, sample))) finite.store(false);
            rendered.store(block + 1, std::memory_order_release);
            if (block == 0) {
                std::unique_lock<std::mutex> lock(mutex);
                firstBlock = true;
                cv.notify_one();
                cv.wait(lock, [&] { return controlStarted; });
            }
        }
    });
    { std::lock_guard<std::mutex> lock(mutex); start = true; }
    cv.notify_one();
    { std::unique_lock<std::mutex> lock(mutex); cv.wait(lock, [&] { return firstBlock; }); }
    const int tuningCount = processor.builtinTuningCount();
    int publications = 0;
    for (int iteration = 0; iteration < 64; ++iteration) {
        const int progress = rendered.load(std::memory_order_acquire);
        if (progress > 0 && progress < 1024) ++publications;
        if (iteration == 0) {
            { std::lock_guard<std::mutex> lock(mutex); controlStarted = true; }
            cv.notify_one();
        }
        if (tuningCount <= 0 || !processor.loadBuiltinTuning(iteration % std::max(1, tuningCount))) finite.store(false);
        auto* parameter = processor.getApvts().getParameter("filter_cutoff");
        if (parameter == nullptr) { finite.store(false); break; }
        parameter->setValueNotifyingHost(static_cast<float>(iteration) / 64);
        juce::MemoryBlock state;
        processor.getStateInformation(state);
        processor.setStateInformation(state.getData(), static_cast<int>(state.getSize()));
    }
    audio.join();
    processor.releaseResources(); // Shutdown follows the audio-thread join.
    std::printf("Publication stress: blocks=%d publications during render lifetime=%d finite=%d\n",
                rendered.load(), publications, static_cast<int>(finite.load()));
    return finite.load() && publications > 0 && rendered.load() == 1024 ? 0 : 1;
}
