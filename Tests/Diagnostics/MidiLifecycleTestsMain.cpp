// Copyright 2026 MontroneDSP. SPDX-License-Identifier: GPL-3.0-or-later
#include "Plugin/PluginProcessor.h"
#include <JuceHeader.h>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <fstream>
#include <string>

namespace
{
int failures = 0;
void check(bool value, const char *message)
{
    if (!value)
    {
        std::fprintf(stderr, "FAIL: %s\n", message);
        ++failures;
    }
}
void set(SwaraXtAudioProcessor &p, const char *id, float value)
{
    auto *parameter = p.getApvts().getParameter(id);
    check(parameter != nullptr, "parameter exists");
    if (parameter)
        parameter->setValueNotifyingHost(parameter->convertTo0to1(value));
}
static void callback(SwaraXtAudioProcessor &p, juce::AudioBuffer<float> &audio,
                     juce::MidiBuffer &midi) noexcept
#if defined(__clang__)
    [[clang::nonblocking]]
#endif
{
    p.processBlock(audio, midi);
}
struct Host : juce::AudioPlayHead
{
    double ppq = 0;
    juce::Optional<PositionInfo> getPosition() const override
    {
        PositionInfo info;
        info.setBpm(120);
        info.setIsPlaying(true);
        info.setPpqPosition(ppq);
        return info;
    }
};
int capture(const char *path)
{
    std::ofstream output(path, std::ios::binary);
    if (!output)
        return 2;
    const auto write = [&](const void *data, size_t bytes)
    {
        output.write(static_cast<const char *>(data), static_cast<std::streamsize>(bytes));
        return static_cast<bool>(output);
    };
    for (int scenario = 0; scenario < 10; ++scenario)
    {
        SwaraXtAudioProcessor p;
        Host host;
        const double rate = scenario % 2 == 0 ? 44100 : 48000;
        const int block = scenario % 3 == 0 ? 257 : 128;
        p.prepareToPlay(rate, block);
        if (scenario >= 4 && scenario < 8)
        {
            set(p, "seq.mode", 1); // Internal clock advances before first note.
            set(p, "arp.pattern", static_cast<float>(scenario - 4));
        }
        if (scenario >= 8)
        {
            set(p, "seq.mode", 2);
            set(p, "seq.midiOut", 1);
        }
        if (scenario == 7 || scenario == 9)
        {
            p.setPlayHead(&host);
            set(p, "seq.clockMode", 1);
        }
        set(p, "dsp.fx", static_cast<float>(scenario % 4));
        set(p, "filter_resonance", .25f);
        juce::AudioBuffer<float> audio(2, block);
        juce::MidiBuffer midi;
        midi.ensureSize(8192);
        for (int index = 0; index < 512; ++index)
        {
            midi.clear();
            if (index == 8 || index == 128)
                midi.addEvent(juce::MidiMessage::noteOn(1, index == 8 ? 48 : 72, juce::uint8(100)),
                              17);
            if (index == 96 || index == 400)
                midi.addEvent(juce::MidiMessage::allNotesOff(1), block - 1);
            callback(p, audio, midi);
            for (int channel = 0; channel < 2; ++channel)
                if (!write(audio.getReadPointer(channel),
                           sizeof(float) * static_cast<size_t>(block)))
                    return 2;
            const int events = midi.getNumEvents();
            if (!write(&events, sizeof(events)))
                return 2;
            for (const auto event : midi)
            {
                const int header[]{event.samplePosition, event.numBytes};
                if (!write(header, sizeof(header)) ||
                    !write(event.data, static_cast<size_t>(event.numBytes)))
                    return 2;
            }
            host.ppq += static_cast<double>(block) * 2 / rate;
        }
    }
    output.close();
    return output ? 0 : 2;
}
void lifecycle()
{
    SwaraXtAudioProcessor p;
    for (int preparation = 0; preparation < 3; ++preparation)
    {
        p.prepareToPlay(preparation == 1 ? 96000 : 48000, 128);
        juce::AudioBuffer<float> audio(2, 128);
        juce::MidiBuffer midi;
        callback(p, audio, midi);
        check(midi.isEmpty(), "empty input remains empty with MIDI output off");
        midi.ensureSize(8192);
        const auto storage = reinterpret_cast<std::uintptr_t>(midi.data.begin());
        for (int block = 0; block < 64; ++block)
        {
            midi.clear();
            midi.addEvent(juce::MidiMessage::noteOn(1, 60, juce::uint8(100)), 0);
            for (int event = 0; event < 510; ++event)
                midi.addEvent(juce::MidiMessage::controllerEvent(1, 1, event % 128), event % 127);
            midi.addEvent(juce::MidiMessage::noteOff(1, 60), 127);
            callback(p, audio, midi);
            check(reinterpret_cast<std::uintptr_t>(midi.data.begin()) == storage,
                  "callback retains the host allocation");
            check(midi.isEmpty(), "input is consumed rather than passed through");
            check(p.engineForTests().shruthiPart().num_notes() == 0,
                  "release at full input queue is delivered");
            for (int channel = 0; channel < 2; ++channel)
                for (int sample = 0; sample < 128; ++sample)
                    check(std::isfinite(audio.getSample(channel, sample)),
                          "finite dense-block output");
        }
        juce::MemoryBlock state;
        p.getStateInformation(state);
        p.setStateInformation(state.getData(), static_cast<int>(state.getSize()));
        midi.clear();
        callback(p, audio, midi);
        check(reinterpret_cast<std::uintptr_t>(midi.data.begin()) == storage,
              "state restore retains host storage");
        p.releaseResources();
    }
}
// A headless device descriptor drives the real JUCE standalone player.
// Audio processing, MIDI collection and processor preparation are not mocked.
struct TestDevice : juce::AudioIODevice
{
    TestDevice() : AudioIODevice("MIDI contract test", "Test") {}
    double rate = 48000;
    juce::StringArray getOutputChannelNames() override { return {"L", "R"}; }
    juce::StringArray getInputChannelNames() override { return {}; }
    juce::Array<double> getAvailableSampleRates() override { return {48000, 96000}; }
    juce::Array<int> getAvailableBufferSizes() override { return {4096}; }
    int getDefaultBufferSize() override { return 4096; }
    juce::String open(const juce::BigInteger &, const juce::BigInteger &, double, int) override
    {
        return {};
    }
    void close() override {}
    bool isOpen() override { return true; }
    void start(juce::AudioIODeviceCallback *) override {}
    void stop() override {}
    bool isPlaying() override { return true; }
    juce::String getLastError() override { return {}; }
    int getCurrentBufferSizeSamples() override { return 4096; }
    double getCurrentSampleRate() override { return rate; }
    int getCurrentBitDepth() override { return 32; }
    juce::BigInteger getActiveOutputChannels() const override { return 3; }
    juce::BigInteger getActiveInputChannels() const override { return {}; }
    int getOutputLatencyInSamples() override { return 0; }
    int getInputLatencyInSamples() override { return 0; }
};
struct ObservedProcessor : SwaraXtAudioProcessor
{
    using SwaraXtAudioProcessor::processBlock;
    bool stable = true;
    int blocks = 0, ons = 0, offs = 0;
    void processBlock(juce::AudioBuffer<float> &audio, juce::MidiBuffer &midi) noexcept
#if defined(__clang__)
        [[clang::nonblocking]]
#endif
        override
    {
        const auto before = reinterpret_cast<std::uintptr_t>(midi.data.begin());
        SwaraXtAudioProcessor::processBlock(audio, midi);
        stable = stable && before == reinterpret_cast<std::uintptr_t>(midi.data.begin());
        ++blocks;
        for (const auto event : midi)
        {
            const auto message = event.getMessage();
            if (message.isNoteOn())
                ++ons;
            if (message.isNoteOff())
                ++offs;
        }
    }
};
void standalone()
{
    TestDevice device;
    ObservedProcessor processor;
    juce::AudioProcessorPlayer player;
    player.audioDeviceAboutToStart(&device);
    player.setProcessor(&processor);
    set(processor, "seq.mode", 2);
    set(processor, "seq.midiOut", 1);
    set(processor, "seq.tempo", 240);
    set(processor, "arp.gate", 11);
    set(processor, "seq.gate", 50);
    juce::AudioBuffer<float> audio(2, 4096);
    for (int preparation = 0; preparation < 3; ++preparation)
    {
        if (preparation != 0)
        {
            player.audioDeviceStopped();
            device.rate = preparation == 1 ? 96000 : 48000;
            player.audioDeviceAboutToStart(&device);
        }
        for (int block = 0; block < 128; ++block)
        {
            if (block == 0 || block == 96)
            {
                auto event = block == 0 ? juce::MidiMessage::noteOn(1, 60, juce::uint8(100))
                                        : juce::MidiMessage::allNotesOff(1);
                event.setTimeStamp(juce::Time::getMillisecondCounterHiRes() * .001);
                player.getMidiMessageCollector().addMessageToQueue(event);
            }
            player.audioDeviceIOCallbackWithContext(nullptr, 0, audio.getArrayOfWritePointers(), 2,
                                                    4096, {});
        }
    }
    check(processor.blocks == 384, "real standalone player invokes every processor block");
    check(processor.stable, "standalone input capacity accommodates generated output");
    check(processor.ons > 0 && processor.offs >= processor.ons,
          "standalone preserves generated releases");
    player.audioDeviceStopped();
    player.setProcessor(nullptr);
}
void saturation()
{
    SwaraXtAudioProcessor actual, reference;
    actual.prepareToPlay(48000, 128);
    reference.prepareToPlay(48000, 128);
    juce::AudioBuffer<float> a(2, 128), b(2, 128);
    juce::MidiBuffer dense, simple;
    dense.ensureSize(32768);
    simple.ensureSize(2048);
    for (int count : {512, 1024, 2048})
    {
        dense.clear();
        simple.clear();
        dense.addEvent(juce::MidiMessage::noteOn(1, 60, juce::uint8(100)), 0);
        simple.addEvent(juce::MidiMessage::noteOn(1, 60, juce::uint8(100)), 0);
        callback(actual, a, dense);
        callback(reference, b, simple);
        dense.clear();
        simple.clear();
        for (int event = 0; event < count; ++event)
            dense.addEvent(juce::MidiMessage::controllerEvent(1, 1, 0), event % 127);
        dense.addEvent(juce::MidiMessage::noteOff(1, 60), 127);
        simple.addEvent(juce::MidiMessage::noteOff(1, 60), 127);
        callback(actual, a, dense);
        callback(reference, b, simple);
        check(actual.engineForTests().shruthiPart().num_notes() == 0,
              "release after full input chunks is delivered");
        bool exact = true;
        for (int channel = 0; channel < 2; ++channel)
            for (int sample = 0; sample < 128; ++sample)
                exact = exact && a.getSample(channel, sample) == b.getSample(channel, sample);
        check(
            exact,
            "neutral dense events preserve exact release timing against independent sparse input");
    }
    dense.clear();
    for (int event = 0; event < 511; ++event)
        dense.addEvent(juce::MidiMessage::controllerEvent(1, 1, 0), 0);
    dense.addEvent(juce::MidiMessage::noteOff(1, 60), 127);
    dense.addEvent(juce::MidiMessage::noteOn(1, 60, juce::uint8(100)), 127);
    callback(actual, a, dense);
    check(actual.engineForTests().shruthiPart().num_notes() == 1,
          "equal-offset release/on order crosses input chunk boundary");
    dense.clear();
    dense.addEvent(juce::MidiMessage::noteOff(1, 60), 0);
    callback(actual, a, dense);
    check(actual.engineForTests().shruthiPart().num_notes() == 0,
          "overflow note can subsequently release");
}
void output(bool reserve)
{
    const int blockSize = 4096;
    SwaraXtAudioProcessor p;
    p.prepareToPlay(48000, blockSize);
    set(p, "seq.mode", 2);
    set(p, "seq.midiOut", 1);
    set(p, "seq.tempo", 240);
    set(p, "arp.gate", 11);
    set(p, "seq.gate", 50);
    juce::AudioBuffer<float> audio(2, blockSize);
    juce::MidiBuffer midi;
    if (reserve)
        midi.ensureSize(swaraxt::kMidiOutputReserveBytes); // Reserve before realtime context.
    const auto storage = reinterpret_cast<std::uintptr_t>(midi.data.begin());
    int ons = 0, offs = 0;
    for (int block = 0; block < 128; ++block)
    {
        juce::MidiBuffer returnedStorage;
        if (!reserve)
            midi.swapWith(returnedStorage); // Caller may provide a fresh empty buffer.
        midi.clear();
        if (block == 0)
            midi.addEvent(juce::MidiMessage::noteOn(1, 60, juce::uint8(100)), 0);
        if (block == 96)
            midi.addEvent(juce::MidiMessage::allNotesOff(1), 127);
        callback(p, audio, midi);
        if (reserve)
            check(reinterpret_cast<std::uintptr_t>(midi.data.begin()) == storage,
                  "bounded MIDI output fits the VST3 reserve");
        for (const auto event : midi)
        {
            const auto message = event.getMessage();
            check(event.samplePosition >= 0 && event.samplePosition < blockSize,
                  "output offsets remain inside the block");
            if (message.isNoteOn())
                ++ons;
            if (message.isNoteOff())
                ++offs;
        }
    }
    check(ons > 0 && offs >= ons, "generated MIDI includes matching releases");
}
} // namespace
int main(int argc, char **argv)
{
    juce::ScopedJuceInitialiser_GUI init;
    if (argc == 3 && std::string(argv[1]) == "--capture")
        return capture(argv[2]);
    if (argc > 2)
        return 2;
    const std::string mode = argc == 2 ? argv[1] : "lifecycle";
    if (mode == "lifecycle")
        lifecycle();
    else if (mode == "standalone")
        standalone();
    else if (mode == "saturation")
        saturation();
    else if (mode == "output")
        output(true);
    else if (mode == "unreserved-output")
        output(false);
    else
        return 2;
    std::printf("MIDI lifecycle scenario=%s failures=%d\n", mode.c_str(), failures);
    return failures == 0 ? 0 : 1;
}
