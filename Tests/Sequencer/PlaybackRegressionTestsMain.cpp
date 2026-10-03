// Copyright 2026 MontroneDSP. SPDX-License-Identifier: GPL-3.0-or-later
#include "Plugin/PluginProcessor.h"
#include <cmath>
#include <cstdio>
namespace {
int failures = 0;
void check(bool ok, const char* message) { if (!ok) { ++failures; std::printf("FAIL: %s\n", message); } }
void set(SwaraXtAudioProcessor& p, const char* id, int value) {
    auto* a = p.getApvts().getParameter(id);
    a->setValueNotifyingHost(a->convertTo0to1(static_cast<float>(value)));
}
struct Host final : juce::AudioPlayHead {
    double ppq = 0; bool playing = true;
    juce::Optional<PositionInfo> getPosition() const override {
        PositionInfo position; position.setBpm(240.0); position.setPpqPosition(ppq);
        position.setIsPlaying(playing); return position;
    }
};
float render(SwaraXtAudioProcessor& p, Host& host, int blocks, int midiNote = -1) {
    float peak = 0;
    for (int i = 0; i < blocks; ++i) {
        juce::AudioBuffer<float> audio(2, 128); juce::MidiBuffer midi;
        if (i == 0 && midiNote >= 0) midi.addEvent(juce::MidiMessage::noteOn(1, midiNote, static_cast<juce::uint8>(100)), 0);
        p.processBlock(audio, midi);
        for (int channel = 0; channel < 2; ++channel)
            for (int sample = 0; sample < 128; ++sample) check(std::isfinite(audio.getSample(channel, sample)), "finite playback");
        peak = std::max(peak, audio.getMagnitude(0, 128));
        if (host.playing) host.ppq += 128.0 / 12000.0;
    }
    return peak;
}
void configure(SwaraXtAudioProcessor& p, Host& host) {
    p.setPlayHead(&host); p.prepareToPlay(48000, 128);
    set(p, swaraxt::IDs::seqMode, 2); set(p, swaraxt::IDs::seqClockMode, 1);
    set(p, swaraxt::IDs::arpGate, 11); set(p, swaraxt::IDs::seqGate, 100);
}
void zeroPitch() {
    for (bool tie : {false, true}) {
        SwaraXtAudioProcessor p; Host host; configure(p, host);
        auto pattern = swaraxt::SequenceState::defaultSnapshot(); pattern.length = 1;
        for (auto& step : pattern.steps) step = static_cast<uint16_t>(tie ? 0xf080 : 0x7080);
        p.sequenceState().store(pattern);
        check(render(p, host, 100) == 0.0f, "legacy pitch zero retains upstream mute semantics");
        p.setRandomNoteRange({-48, 0}); juce::Random rng(63);
        p.sequenceState().randomize(swaraxt::SequenceState::Randomize::notes, rng, p.randomNoteRange());
        for (auto step : p.sequenceState().snapshot().steps) check((step & 127) == 1, "random range excludes native mute sentinel");
        check(render(p, host, 100) > 0.00001f, "randomized lower endpoint starts without range edit or reset");
        for (int restart = 0; restart < 12; ++restart) {
            host.playing = false; render(p, host, 48);
            host.playing = true; host.ppq = 0;
            check(render(p, host, 100) > 0.00001f, "unchanged low-note pattern restarts after transport stop");
        }
    }
}
void transitions() {
    SwaraXtAudioProcessor p; Host host; configure(p, host); p.setSequenceLocked(true);
    auto pattern = swaraxt::SequenceState::defaultSnapshot(); pattern.length = 4;
    for (auto& step : pattern.steps) step = 0x708c;
    p.sequenceState().store(pattern);
    check(render(p, host, 48, 60) > 0.00001f, "start held-note sequence");
    for (int iteration = 0; iteration < 128; ++iteration) {
        const auto saved = p.sequenceState().snapshot();
        p.setCurrentProgram(iteration % p.getNumPrograms());
        render(p, host, 32);
        check(p.engineForTests().shruthiPart().running(), "locked preset swap preserves running sequence");
        check(p.engineForTests().shruthiPart().has_held_notes(), "locked preset does not erase held key");
        check(saved.steps == p.sequenceState().snapshot().steps, "locked preset preserves pattern");
        host.playing = false; render(p, host, 2);
        set(p, swaraxt::IDs::seqClockMode, 0);
        render(p, host, 32);
        check(p.engineForTests().shruthiPart().running(), "stopped-host to free handoff starts held key");
        set(p, swaraxt::IDs::seqClockMode, 1); host.playing = true;
        set(p, swaraxt::IDs::seqMode, 1); render(p, host, 16);
        set(p, swaraxt::IDs::seqMode, 2); render(p, host, 16);
        check(p.engineForTests().shruthiPart().running(), "arp/seq remains running");
        p.sequenceState().store(saved); render(p, host, 16);
        check(p.engineForTests().shruthiPart().sequencer_settings().steps[0].data_[0] == swaraxt::SequenceSnapshot::dataA(saved.steps[0]), "unchanged pattern publication reaches engine");
    }
    std::puts("Completed 128 preset/lock/transport/free-host/arp-seq/unchanged-publication cycles");
}
void oldPrograms() {
    SwaraXtAudioProcessor p; Host host; configure(p, host);
    const char* names[] = {"1996 Sub Bass", "Ibiza Bass", "Pong Fun", "Sport Sub", "Unwanted Truths", "Vowel drone"};
    const int oldIndices[] = {51, 63, 71, 72, 74, 75};
    for (int i = 0; i < 6; ++i) {
        check(p.getProgramName(51+i) == names[i], "retained deterministic index");
        p.setCurrentProgram(51+i); render(p, host, 1);
        juce::MemoryBlock saved; p.getStateInformation(saved);
        auto xml = juce::AudioProcessor::getXmlFromBinary(saved.getData(), static_cast<int>(saved.getSize()));
        xml->setAttribute("currentProgram", oldIndices[i]);
        juce::MemoryBlock legacy; juce::AudioProcessor::copyXmlToBinary(*xml, legacy);
        p.setStateInformation(legacy.getData(), static_cast<int>(legacy.getSize()));
        check(p.getCurrentProgram() == 51+i, "old retained index remaps by saved name");
        xml->setAttribute("currentProgram", 9000); xml->setAttribute("presetName", "Deleted factory preset");
        juce::AudioProcessor::copyXmlToBinary(*xml, legacy);
        p.setStateInformation(legacy.getData(), static_cast<int>(legacy.getSize())); render(p, host, 1);
        check(p.getCurrentProgram() >= 0 && p.getCurrentProgram() < 57, "obsolete/out-of-range session index is safe");
    }
    check(p.getProgramName(50) == "lazrBird", "canonical factory boundary unchanged");
    check(p.getProgramName(-1).isEmpty() && p.getProgramName(57).isEmpty(), "bank boundaries safe");
}
}
int main() { juce::ScopedJuceInitialiser_GUI gui; zeroPitch(); transitions(); oldPrograms();
    std::printf("PlaybackRegression: %s (%d failures)\n", failures ? "FAIL" : "PASS", failures); return failures ? 1 : 0; }
