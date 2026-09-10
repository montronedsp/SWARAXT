// Copyright 2026 MontroneDSP. SPDX-License-Identifier: GPL-3.0-or-later
#include <JuceHeader.h>
#include "Plugin/PluginProcessor.h"
#include "Plugin/SwaraXtParameterLayout.h"
#include "Plugin/ShruthiFactoryPresets.h"
#include "shruthi/patch.h"
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <set>
#include <vector>

namespace {
using namespace swaraxt;
int failures = 0;
void check(bool ok, const char* message) { if (!ok) { ++failures; std::printf("FAIL: %s\n", message); } }
void set(SwaraXtAudioProcessor& p, const char* id, float value) {
    auto* parameter = p.getApvts().getParameter(id);
    check(parameter != nullptr, id);
    if (parameter) parameter->setValueNotifyingHost(parameter->convertTo0to1(value));
}
void neutral(SwaraXtAudioProcessor& p) {
    set(p, IDs::osc1Shape, 1); set(p, IDs::osc1Param, 0); set(p, IDs::osc1Range, 0);
    set(p, IDs::osc2Shape, 0); set(p, IDs::mixBalance, 0); set(p, IDs::mixSub, 0); set(p, IDs::mixNoise, 0);
    set(p, IDs::filterCutoff, 1000); set(p, IDs::filterResonance, 0);
    set(p, IDs::filterShruthiEnv, 0); set(p, IDs::filterShruthiLfo, 0);
    set(p, IDs::filterEnvAmount, 0); set(p, IDs::filterModAmount, 0); set(p, IDs::filterKeyTracking, .5f);
    set(p, IDs::perfGlide, 0); set(p, IDs::master, .5f);
    for (const auto* id : {IDs::env1Attack, IDs::env1Decay, IDs::env1Release, IDs::env2Attack, IDs::env2Decay, IDs::env2Release}) set(p, id, 0);
    set(p, IDs::env1Sustain, 127); set(p, IDs::env2Sustain, 127);
    for (int row = 1; row <= 12; ++row) {
        const auto id = "mod.row" + std::to_string(row) + ".amount";
        set(p, id.c_str(), row == 9 ? 63.f : 0.f);
    }
}
std::vector<float> render(SwaraXtAudioProcessor& p, int note = 60, int blocks = 80) {
    juce::AudioBuffer<float> audio(2, 128); juce::MidiBuffer midi;
    midi.addEvent(juce::MidiMessage::noteOn(1, note, static_cast<juce::uint8>(127)), 0);
    std::vector<float> result;
    for (int k = 0; k < blocks; ++k) {
        p.processBlock(audio, midi); midi.clear();
        result.insert(result.end(), audio.getReadPointer(0), audio.getReadPointer(0) + 128);
    }
    check(std::all_of(result.begin(), result.end(), [](float x) { return std::isfinite(x) && std::abs(x) < 8; }), "finite bounded output");
    return result;
}
double difference(const std::vector<float>& a, const std::vector<float>& b) {
    double total = 0; for (size_t i = 0; i < std::min(a.size(), b.size()); ++i) total += std::abs(a[i] - b[i]);
    return total / static_cast<double>(std::max<size_t>(1, std::min(a.size(), b.size())));
}
double effectiveCutoff(const SwaraXtAudioProcessor& p) {
    const auto& f = p.engineForTests().filter(); const auto& v = f.paramsForTests();
    return f.cutoffMapper().mapCutoffHz(v.cutoffHz, v.keyTrack, v.noteNumber,
        v.envAmount * v.envValue + v.modAmount * v.modValue + v.matrixCutoffOctaves);
}
void authority(const SwaraXtAudioProcessor& p) {
    const auto cv = p.engineForTests().shruthiPart().voice().cutoff();
    const double expected = std::clamp(20000. * std::exp2((static_cast<double>(cv) - 254.) / 24.), 10., 20000.);
    check(std::abs(effectiveCutoff(p) - expected) < .01, "final Shruthi cutoff CV is authoritative exactly once");
    check(std::abs(p.engineForTests().filter().paramsForTests().boardCutoffCvVolts - cv * (5.0 / 255.0)) < 1.e-12,
        "hardware consumes final native CV directly without panel-Hz clipping");
}
void routingAndState() {
    std::vector<float> before; double previousCutoff = 0;
    for (int depth : {0, 64, 127}) {
        auto owner = std::make_unique<SwaraXtAudioProcessor>(); auto& p = *owner;
        neutral(p); set(p, IDs::filterShruthiEnv, static_cast<float>(depth));
        // Stay below the inherited signed-16 overflow boundary at large depths.
        set(p, IDs::env1Sustain, 20);
        p.prepareToPlay(48000, 128); auto audio = render(p); authority(p);
        const double cutoff = effectiveCutoff(p);
        std::printf("env=%d firmware=%d cutoff=%.3f mean_audio_change=%.8f\n", depth,
            static_cast<int>(p.engineForTests().shruthiPart().voice().cutoff()), cutoff, before.empty() ? 0 : difference(audio, before));
        if (!before.empty()) { check(difference(audio, before) > .001, "original env depth changes audible filtering"); check(cutoff > previousCutoff, "positive env raises cutoff"); }
        before = audio; previousCutoff = cutoff;
        juce::MemoryBlock state; p.getStateInformation(state);
        auto restoredOwner = std::make_unique<SwaraXtAudioProcessor>(); auto& restored = *restoredOwner;
        restored.setStateInformation(state.getData(), static_cast<int>(state.getSize())); restored.prepareToPlay(48000, 128);
        auto freshOwner = std::make_unique<SwaraXtAudioProcessor>(); auto& fresh = *freshOwner;
        fresh.setStateInformation(state.getData(), static_cast<int>(state.getSize())); fresh.prepareToPlay(48000, 128);
        const auto restoredAudio = render(restored);
        check(difference(restoredAudio, render(fresh)) == 0, "serialization restores deterministic audible control result");
        check(difference(restoredAudio, audio) == 0, "serialization preserves original rendered result");
        check(restored.getApvts().getRawParameterValue(IDs::filterShruthiEnv)->load() == depth, "serialized signed env amount retained");
    }
    SwaraXtAudioProcessor base, modulated; neutral(base); neutral(modulated);
    set(modulated, IDs::filterShruthiLfo, 100); set(base, IDs::lfo2Rate, 100); set(modulated, IDs::lfo2Rate, 100);
    base.prepareToPlay(48000, 128); modulated.prepareToPlay(48000, 128);
    check(difference(render(base, 60, 400), render(modulated, 60, 400)) > .001, "original LFO depth changes audible filtering"); authority(modulated);
    // Import an actual factory patch, preserving its source controls and matrix.
    SwaraXtAudioProcessor factory; factory.setCurrentProgram(1); factory.prepareToPlay(48000, 128);
    auto patch = factory.engineForTests().shruthiPart().patch(); patch.filter_env = 64; patch.filter_lfo = 40;
    ShruthiFactoryPresets::applyPatchToApvts(patch, factory.getApvts()); render(factory); authority(factory);
    // Plugin extras augment the final firmware CV rather than rebuilding its envelope.
    set(factory, IDs::filterEnvAmount, .125f); render(factory);
    const auto& fp = factory.engineForTests().filter().paramsForTests();
    check(std::abs(fp.envAmount - .5f) < 1.e-6f, "plugin envelope depth remains an independent extra");
}
void matrixAndExtras() {
    for (int destination : {static_cast<int>(shruthi::MOD_DST_FILTER_CUTOFF), static_cast<int>(shruthi::MOD_DST_FILTER_RESONANCE)}) {
        SwaraXtAudioProcessor base, routed; neutral(base); neutral(routed);
        set(routed, "mod.row1.source", static_cast<float>(shruthi::MOD_SRC_ENV_1));
        set(routed, "mod.row1.destination", static_cast<float>(destination));
        set(routed, "mod.row1.amount", 16);
        base.prepareToPlay(48000, 128); routed.prepareToPlay(48000, 128);
        check(difference(render(base), render(routed)) > .001, "matrix cutoff/resonance route remains audible");
        authority(routed);
        const auto& voice = routed.engineForTests().shruthiPart().voice();
        check(std::abs(routed.engineForTests().filter().paramsForTests().resonance - voice.resonance() / 255.f) < 1.e-7f, "final resonance CV authoritative exactly once");
    }
    for (const auto* extra : {IDs::filterEnvAmount, IDs::filterModAmount}) {
        SwaraXtAudioProcessor base, augmented; neutral(base); neutral(augmented);
        set(base, IDs::filterShruthiEnv, 8); set(augmented, IDs::filterShruthiEnv, 8);
        set(augmented, extra, .5f);
        base.prepareToPlay(48000, 128); augmented.prepareToPlay(48000, 128);
        check(difference(render(base), render(augmented)) > .001, "plugin extra modulation remains audible");
        check(base.engineForTests().shruthiPart().voice().cutoff() == augmented.engineForTests().shruthiPart().voice().cutoff(), "plugin extra does not duplicate or mutate native contribution");
    }
}
void tracking() {
    SwaraXtAudioProcessor p; neutral(p); p.prepareToPlay(48000, 128);
    juce::AudioBuffer<float> audio(2, 128); juce::MidiBuffer midi;
    const auto run = [&](int blocks) { for (int i = 0; i < blocks; ++i) { p.processBlock(audio, midi); midi.clear(); } };
    midi.addEvent(juce::MidiMessage::noteOn(1, 60, static_cast<juce::uint8>(127)), 0); run(20);
    const double lower = effectiveCutoff(p); authority(p);
    midi.addEvent(juce::MidiMessage::noteOn(1, 72, static_cast<juce::uint8>(127)), 0); run(20);
    check(std::abs(effectiveCutoff(p) / lower - 2.) < .001, "original tracking is one octave per octave at neutral trim");
    midi.addEvent(juce::MidiMessage::noteOff(1, 72), 0); run(20); authority(p);
    check(std::abs(effectiveCutoff(p) - lower) < .01, "held-note fallback restores filter tracking");
    check(std::abs(p.engineForTests().filter().paramsForTests().noteNumber - 60) < .01, "filter note source returns to actual voice note");
    midi.addEvent(juce::MidiMessage::noteOn(1, 60, static_cast<juce::uint8>(127)), 0); run(20);
    check(std::abs(effectiveCutoff(p) - lower) < .01, "repeated-note legato keeps tracking");
    set(p, IDs::perfGlide, 80); midi.addEvent(juce::MidiMessage::noteOn(1, 84, static_cast<juce::uint8>(127)), 0);
    std::set<int> pitches; for (int i = 0; i < 600; ++i) { run(1); authority(p); pitches.insert(static_cast<int>(p.engineForTests().filter().paramsForTests().noteNumber * 128)); }
    check(pitches.size() > 8, "filter follows intermediate glide pitches");
    for (float amount : {0.f, .5f, 1.f}) {
        SwaraXtAudioProcessor q; neutral(q); set(q, IDs::filterKeyTracking, amount); q.prepareToPlay(48000, 128); render(q, 60); const auto low = effectiveCutoff(q);
        render(q, 72); const auto ratio = effectiveCutoff(q) / low;
        check(std::abs(ratio - std::exp2(2. * amount)) < .01, "key trim minimum/middle/maximum yields 0x/1x/2x tracking away from clipping");
    }
}
void internalNotes() {
    for (int mode : {static_cast<int>(shruthi::SEQUENCER_MODE_ARP), static_cast<int>(shruthi::SEQUENCER_MODE_SEQ)}) {
        SwaraXtAudioProcessor p; neutral(p); set(p, IDs::seqMode, static_cast<float>(mode)); set(p, IDs::seqTempo, 240);
        auto seq = SequenceState::defaultSnapshot(); seq.length = 4;
        for (int i = 0; i < 4; ++i) seq.steps[static_cast<size_t>(i)] = SequenceSnapshot::pack(static_cast<uint8_t>(0x80 | (48 + 4 * i)), 0x70);
        p.sequenceState().store(seq); p.prepareToPlay(48000, 128);
        juce::AudioBuffer<float> audio(2, 128); juce::MidiBuffer midi;
        for (int note : {48, 60, 67}) midi.addEvent(juce::MidiMessage::noteOn(1, note, static_cast<juce::uint8>(127)), 0);
        std::set<int> notes;
        for (int i = 0; i < 1200; ++i) { p.processBlock(audio, midi); midi.clear(); authority(p); notes.insert(static_cast<int>(p.engineForTests().filter().paramsForTests().noteNumber)); }
        check(notes.size() > 1, "arp/sequencer filter follows internally generated notes");
    }
}
struct VcaTiming {
    int samples = 0, firstTarget = -1, firstGain = -1;
    static void capture(void* context, const SwaraXtEngine::DebugBlockCapture& block) {
        auto& self = *static_cast<VcaTiming*>(context);
        for (int i = 0; i < block.samples; ++i) {
            if (self.firstTarget < 0 && block.vcaTarget[i] > 0) self.firstTarget = self.samples + i;
            if (self.firstGain < 0 && block.vcaGain[i] > 0) self.firstGain = self.samples + i;
        }
        self.samples += block.samples;
    }
};
void latencyAndBuffers() {
    for (double rate : {8000., 16000., 32000., 44100., 48000., 96000., 192000., 384000.}) {
        std::vector<float> reference;
        for (int blockSize : {1, 32, 127, 512}) {
            auto owner = std::make_unique<SwaraXtAudioProcessor>(); auto& p = *owner;
            neutral(p); p.prepareToPlay(rate, blockSize);
            const int expectedLatency = static_cast<int>(std::ceil(SwaraXtEngine::kOutputLatencyNativeSamples * rate / SwaraXtEngine::kInternalSampleRate));
            check(p.getLatencySamples() == expectedLatency, "host latency reports the native FIR delay");
            VcaTiming timing; p.engineForTests().setDebugTapSink(&timing, VcaTiming::capture);
            std::vector<float> output;
            for (int position = 0; position < 8192; position += blockSize) {
                const int count = std::min(blockSize, 8192 - position);
                juce::AudioBuffer<float> audio(2, count); juce::MidiBuffer midi;
                if (position == 0) midi.addEvent(juce::MidiMessage::noteOn(1, 60, static_cast<juce::uint8>(127)), 0);
                if (position <= 2048 && position + count > 2048) midi.addEvent(juce::MidiMessage::noteOff(1, 60), 2048 - position);
                p.processBlock(audio, midi);
                output.insert(output.end(), audio.getReadPointer(0), audio.getReadPointer(0) + count);
            }
            check(timing.firstTarget >= 0 && timing.firstGain - timing.firstTarget == FilterRateConverter::kInputDelay,
                "oversampled VCA CV follows interpolation delay; decimation delays the completed instrument");
            check(std::any_of(output.begin(), output.end(), [](float x) { return std::abs(x) > .001f; }), "short note survives converter startup and VCA delay");
            check(std::all_of(output.begin(), output.end(), [](float x) { return std::isfinite(x) && std::abs(x) < 8; }), "short-note output remains finite and bounded");
            if (!reference.empty()) check(difference(reference, output) == 0, "audio timeline independent of host buffer size");
            else reference = output;
        }
    }
}
void hostRatePolicy() {
    auto p = std::make_unique<SwaraXtAudioProcessor>(); neutral(*p);
    for (double rate : {32000., 48000., 32000., 96000., 44100.}) {
        p->prepareToPlay(rate, 128);
        const auto output = render(*p);
        check(std::any_of(output.begin(), output.end(), [](float x) { return std::abs(x) > .001f; }),
            "downsampling/upsampling rate transitions preserve audible rendering");
    }
    for (double rate : {4000., 0., -1., 1000000., std::numeric_limits<double>::infinity(), std::numeric_limits<double>::quiet_NaN()}) {
        p->prepareToPlay(rate, 128);
        juce::AudioBuffer<float> audio(2, 128); audio.clear();
        for (int i = 0; i < 128; ++i) audio.setSample(0, i, 1);
        juce::MidiBuffer midi; p->processBlock(audio, midi);
        check(audio.getMagnitude(0, 128) == 0 && p->getLatencySamples() == 0,
            "unsupported rates are explicitly rejected to silence");
    }
    p->prepareToPlay(48000, 128);
    const auto recovered = render(*p);
    check(std::any_of(recovered.begin(), recovered.end(), [](float x) { return std::abs(x) > .001f; }),
        "supported prepare recovers after a rejected rate");
}
}
int main() {
    std::setvbuf(stdout, nullptr, _IONBF, 0);
    juce::ScopedJuceInitialiser_GUI init;
    std::printf("Processor bytes=%zu; routing/state\n", sizeof(SwaraXtAudioProcessor));
    routingAndState(); std::puts("matrix/extras"); matrixAndExtras();
    std::puts("tracking"); tracking(); std::puts("internal notes"); internalNotes();
    std::puts("latency/buffers"); latencyAndBuffers();
    std::puts("host rate policy"); hostRatePolicy();
    std::printf("Architecture controls: %d failures\n", failures); return failures ? 1 : 0;
}
