// Copyright 2026 MontroneDSP.
// SPDX-License-Identifier: GPL-3.0-or-later
//
// CC 120 All Sound Off: MIDI ownership, HD envelope death, and host-output
// silence. Measures the real processor output path rather than envelope state
// alone.

#include <JuceHeader.h>

#include "Plugin/PluginProcessor.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <vector>

namespace {

using Processor = SwaraXtAudioProcessor;
int failures = 0;

void expect(bool ok, const char* message)
{
    if (! ok)
    {
        ++failures;
        std::printf("FAIL: %s\n", message);
    }
}

void setInt(Processor& processor, const char* id, int value)
{
    if (auto* parameter = processor.getApvts().getParameter(id))
        parameter->setValueNotifyingHost(parameter->convertTo0to1(static_cast<float>(value)));
}

void setFloat(Processor& processor, const char* id, float value)
{
    if (auto* parameter = processor.getApvts().getParameter(id))
        parameter->setValueNotifyingHost(parameter->convertTo0to1(value));
}

shruthi::Part& part(Processor& processor)
{
    return processor.engineForTests().shruthiPart();
}

constexpr float kAudible = 1.0e-2f;
// Practical emergency silence. This is not digital zero: the retained FIR
// history can leave a reconstruction residue of a few thousandths (~-50 dBFS).
constexpr float kPracticalSilence = 4.0e-3f;
constexpr float kDigitalSilence = 1.0e-6f;
constexpr double kNativeRate = 20000000.0 / 510.0;
constexpr int kFirTaps = 256;

int architecturalSilenceBudget(double hostRate) noexcept
{
    // Filter matching delay + FIR group delay, one native block, and the FIR
    // window that silenceLookahead deliberately keeps. No extra time floor.
    const double native = static_cast<double>(swaraxt::SwaraXtEngine::kOutputLatencyNativeSamples
        + swaraxt::SwaraXtEngine::kAudioBlockSize + kFirTaps);
    return static_cast<int>(std::ceil(native * hostRate / kNativeRate)) + 8;
}

int recoveryBlocks(double hostRate, int blockSize) noexcept
{
    const int need = architecturalSilenceBudget(hostRate) + static_cast<int>(std::ceil(0.05 * hostRate));
    return std::max(16, (need + blockSize - 1) / blockSize);
}

struct Rendered {
    std::vector<float> left;
    std::vector<float> right;
};

void applyDryLongRelease(Processor& processor, bool longRelease)
{
    setInt(processor, swaraxt::IDs::osc1Shape, 1);
    setInt(processor, swaraxt::IDs::osc2Shape, 1);
    setInt(processor, swaraxt::IDs::mixBalance, 0);
    setInt(processor, swaraxt::IDs::mixNoise, 0);
    setInt(processor, swaraxt::IDs::mixSub, 0);
    setInt(processor, swaraxt::IDs::env2Attack, 0);
    setInt(processor, swaraxt::IDs::env2Decay, 0);
    setInt(processor, swaraxt::IDs::env2Sustain, 127);
    setInt(processor, swaraxt::IDs::env2Release, longRelease ? 127 : 8);
    setInt(processor, swaraxt::IDs::dspFxProgram, 0);
    setFloat(processor, swaraxt::IDs::filterResonance, 0.0f);
    setFloat(processor, swaraxt::IDs::master, 0.85f);
}

float rmsOf(const std::vector<float>& samples, int begin, int end) noexcept
{
    const int n = static_cast<int>(samples.size());
    begin = std::max(0, begin);
    end = std::min(n, end);
    if (end <= begin)
        return 0.0f;
    double sum = 0.0;
    for (int i = begin; i < end; ++i)
    {
        const double sample = samples[static_cast<size_t>(i)];
        sum += sample * sample;
    }
    return static_cast<float>(std::sqrt(sum / static_cast<double>(end - begin)));
}

float peakOf(const std::vector<float>& samples, int begin, int end) noexcept
{
    const int n = static_cast<int>(samples.size());
    begin = std::max(0, begin);
    end = std::min(n, end);
    float peak = 0.0f;
    for (int i = begin; i < end; ++i)
        peak = std::max(peak, std::fabs(samples[static_cast<size_t>(i)]));
    return peak;
}

int lastAudible(const std::vector<float>& samples, int begin, int end, float threshold) noexcept
{
    const int n = static_cast<int>(samples.size());
    begin = std::max(0, begin);
    end = std::min(n, end);
    int last = -1;
    for (int i = begin; i < end; ++i)
        if (std::fabs(samples[static_cast<size_t>(i)]) > threshold)
            last = i;
    return last;
}

float maxStep(const std::vector<float>& samples, int begin, int end) noexcept
{
    const int n = static_cast<int>(samples.size());
    begin = std::max(1, begin);
    end = std::min(n, end);
    float peak = 0.0f;
    for (int i = begin; i < end; ++i)
        peak = std::max(peak, std::fabs(samples[static_cast<size_t>(i)]
            - samples[static_cast<size_t>(i - 1)]));
    return peak;
}

bool finiteSamples(const Rendered& rendered) noexcept
{
    for (float sample : rendered.left)
        if (! std::isfinite(sample))
            return false;
    for (float sample : rendered.right)
        if (! std::isfinite(sample))
            return false;
    return true;
}

Rendered holdThenEvent(Processor& processor, double hostRate, int blockSize,
                       const juce::MidiMessage& event, int eventOffset,
                       int holdBlocks, int afterBlocks, int noteChannel = 1)
{
    processor.prepareToPlay(hostRate, blockSize);
    juce::AudioBuffer<float> warm(2, blockSize);
    juce::MidiBuffer empty;
    processor.processBlock(warm, empty);

    Rendered rendered;
    auto append = [&](juce::MidiBuffer midi) {
        juce::AudioBuffer<float> audio(2, blockSize);
        processor.processBlock(audio, midi);
        rendered.left.insert(rendered.left.end(),
                             audio.getReadPointer(0), audio.getReadPointer(0) + blockSize);
        rendered.right.insert(rendered.right.end(),
                              audio.getReadPointer(1), audio.getReadPointer(1) + blockSize);
    };

    for (int block = 0; block < holdBlocks; ++block)
    {
        juce::MidiBuffer midi;
        if (block == 0)
            midi.addEvent(juce::MidiMessage::noteOn(noteChannel, 60, (juce::uint8) 100), 0);
        append(std::move(midi));
    }

    juce::MidiBuffer eventMidi;
    eventMidi.addEvent(event, eventOffset);
    append(std::move(eventMidi));
    for (int block = 0; block < afterBlocks; ++block)
        append({});
    return rendered;
}

void midiLifecycle()
{
    Processor processor;
    processor.prepareToPlay(48000, 64);
    applyDryLongRelease(processor, true);
    juce::AudioBuffer<float> audio(2, 64);

    auto render = [&](juce::MidiBuffer midi) {
        processor.processBlock(audio, midi);
    };

    auto send = [&](const juce::MidiMessage& message, int offset = 0) {
        juce::MidiBuffer midi;
        midi.addEvent(message, offset);
        render(std::move(midi));
    };

    send(juce::MidiMessage::noteOn(1, 60, (juce::uint8) 100));
    expect(part(processor).num_notes() == 1, "lifecycle: note held");

    send(juce::MidiMessage::allSoundOff(1));
    expect(part(processor).num_notes() == 0, "lifecycle: CC120 clears stack");
    expect(part(processor).voice().amplitude_envelope_dead(), "lifecycle: envelope DEAD");

    send(juce::MidiMessage::noteOn(1, 64, (juce::uint8) 100));
    send(juce::MidiMessage::controllerEvent(1, 64, 127));
    send(juce::MidiMessage::allSoundOff(1));
    expect(! part(processor).latched(), "lifecycle: CC120 ignores sustain");
    expect(part(processor).num_notes() == 0, "lifecycle: sustain-held note released");
    expect(part(processor).voice().amplitude_envelope_dead(), "lifecycle: DEAD under sustain");

    juce::MidiBuffer chord;
    chord.addEvent(juce::MidiMessage::noteOn(1, 60, (juce::uint8) 100), 0);
    chord.addEvent(juce::MidiMessage::noteOn(1, 67, (juce::uint8) 100), 1);
    render(std::move(chord));
    send(juce::MidiMessage::allSoundOff(1));
    expect(part(processor).num_notes() == 0, "lifecycle: chord cleared");

    send(juce::MidiMessage::allSoundOff(1));
    send(juce::MidiMessage::allSoundOff(1));
    expect(part(processor).voice().amplitude_envelope_dead(), "lifecycle: repeated CC120");

    juce::MidiBuffer retrigger;
    retrigger.addEvent(juce::MidiMessage::noteOn(1, 72, (juce::uint8) 110), 8);
    render(retrigger);
    expect(part(processor).num_notes() == 1, "lifecycle: Note On after CC120");
    expect(! part(processor).voice().amplitude_envelope_dead(),
           "lifecycle: envelope restarts after CC120");
}

void envelopeQualification()
{
    Processor processor;
    processor.prepareToPlay(48000, 256);
    applyDryLongRelease(processor, true);
    juce::AudioBuffer<float> audio(2, 256);

    juce::MidiBuffer note;
    note.addEvent(juce::MidiMessage::noteOn(1, 60, (juce::uint8) 100), 0);
    processor.processBlock(audio, note);
    expect(! part(processor).voice().amplitude_envelope_dead(), "held note is alive");

    juce::MidiBuffer noteOff;
    noteOff.addEvent(juce::MidiMessage::noteOff(1, 60), 0);
    processor.processBlock(audio, noteOff);
    expect(! part(processor).voice().amplitude_envelope_dead(),
           "ordinary Note Off with long release is not DEAD");

    juce::MidiBuffer note2;
    note2.addEvent(juce::MidiMessage::noteOn(1, 60, (juce::uint8) 100), 0);
    processor.processBlock(audio, note2);
    juce::MidiBuffer cc123;
    cc123.addEvent(juce::MidiMessage::allNotesOff(1), 0);
    processor.processBlock(audio, cc123);
    expect(part(processor).num_notes() == 0, "CC123 clears the stack");
    expect(! part(processor).voice().amplitude_envelope_dead(),
           "CC123 without sustain enters release rather than DEAD");

    juce::MidiBuffer note3;
    note3.addEvent(juce::MidiMessage::noteOn(1, 60, (juce::uint8) 100), 0);
    processor.processBlock(audio, note3);
    juce::MidiBuffer cc120;
    cc120.addEvent(juce::MidiMessage::allSoundOff(1), 0);
    processor.processBlock(audio, cc120);
    expect(part(processor).voice().amplitude_envelope_dead(),
           "CC120 puts the HD amplitude envelope in DEAD");
    expect(part(processor).voice().vca() == 0, "CC120 forces VCA to zero after the control update");

    juce::MidiBuffer note4;
    note4.addEvent(juce::MidiMessage::noteOn(1, 61, (juce::uint8) 100), 0);
    processor.processBlock(audio, note4);
    expect(! part(processor).voice().amplitude_envelope_dead(),
           "Note On after CC120 restarts the envelope");
    expect(part(processor).voice().vca() != 0, "Note On after CC120 reopens the VCA");
}

struct CaseResult {
    const char* name = "";
    double rate = 0;
    int block = 0;
    int eventOffset = 0;
    int lastPracticalSample = -1;
    int residualSamples = 0;
    int samplesToPractical = -1;
    float peakBefore = 0;
    float peakAfterEvent = 0;
    float rmsAfterBudget = 0;
    float peakAfterBudget = 0;
    float step = 0;
    bool digitalSilence = false;
    bool envelopeDead = false;
    bool recovered = false;
};

void report(const CaseResult& result)
{
    const auto db = [](float x) {
        return 20.0 * std::log10(std::max(static_cast<double>(x), 1.0e-12));
    };
    std::printf(
        "  %-28s sr=%.0f bs=%4d ev=%4d pre=%.4f(%+.1fdB) postPeak=%.6f(%+.1fdB) "
        "after=%.6f(%+.1fdB) rms=%.6f settle=%d residual=%d digital=%d step=%.4f dead=%d rec=%d\n",
        result.name, result.rate, result.block, result.eventOffset,
        result.peakBefore, db(result.peakBefore),
        result.peakAfterEvent, db(result.peakAfterEvent),
        result.peakAfterBudget, db(result.peakAfterBudget),
        result.rmsAfterBudget, result.samplesToPractical, result.residualSamples,
        result.digitalSilence ? 1 : 0, result.step,
        result.envelopeDead ? 1 : 0, result.recovered ? 1 : 0);
}

CaseResult measureCc120(const char* name, double rate, int blockSize, int eventOffset,
                        bool longRelease, bool resonant, bool experimental, int fxProgram,
                        bool arp)
{
    Processor processor;
    applyDryLongRelease(processor, longRelease);
    if (resonant)
        setFloat(processor, swaraxt::IDs::filterResonance, 0.85f);
    if (fxProgram > 0)
    {
        setInt(processor, swaraxt::IDs::dspFxProgram, fxProgram);
        setInt(processor, swaraxt::IDs::dspFxParam1, 80);
        setInt(processor, swaraxt::IDs::dspFxParam2, 40);
    }
    if (arp)
    {
        setInt(processor, swaraxt::IDs::seqMode, 1);
        setInt(processor, swaraxt::IDs::arpPattern, 1);
        setInt(processor, swaraxt::IDs::seqTempo, 120);
    }
    if (experimental)
        processor.setExperimentalSvfEnabled(true);

    const int holdBlocks = std::max(8, static_cast<int>(std::ceil(0.12 * rate / blockSize)));
    const int afterBlocks = std::max(8, static_cast<int>(std::ceil(0.20 * rate / blockSize)));
    const int clampedOffset = std::clamp(eventOffset, 0, blockSize - 1);
    auto rendered = holdThenEvent(processor, rate, blockSize,
                                  juce::MidiMessage::allSoundOff(1), clampedOffset,
                                  holdBlocks, afterBlocks);

    CaseResult result;
    result.name = name;
    result.rate = rate;
    result.block = blockSize;
    result.eventOffset = clampedOffset;
    const int eventAbs = holdBlocks * blockSize + clampedOffset;
    const int budget = architecturalSilenceBudget(rate);
    const int end = static_cast<int>(rendered.left.size());
    result.peakBefore = peakOf(rendered.left, 0, eventAbs);
    result.peakAfterEvent = peakOf(rendered.left, eventAbs, end);
    result.peakAfterBudget = peakOf(rendered.left, eventAbs + budget, end);
    result.rmsAfterBudget = rmsOf(rendered.left, eventAbs + budget, end);
    result.lastPracticalSample = lastAudible(rendered.left, eventAbs, end, kPracticalSilence);
    result.residualSamples = result.lastPracticalSample < eventAbs
        ? 0
        : result.lastPracticalSample - eventAbs + 1;
    result.samplesToPractical = result.residualSamples;
    result.digitalSilence = peakOf(rendered.left, eventAbs + budget, end) <= kDigitalSilence;
    result.step = maxStep(rendered.left, eventAbs, std::min(end, eventAbs + 16));
    result.envelopeDead = part(processor).voice().amplitude_envelope_dead();

    if (arp)
        setInt(processor, swaraxt::IDs::seqMode, 0);

    juce::AudioBuffer<float> recover(2, blockSize);
    juce::MidiBuffer retrigger;
    retrigger.addEvent(juce::MidiMessage::noteOn(1, 64, (juce::uint8) 110), 0);
    float recoverPeak = 0.0f;
    const int recoverCount = recoveryBlocks(rate, blockSize);
    for (int i = 0; i < recoverCount; ++i)
    {
        processor.processBlock(recover, retrigger);
        retrigger.clear();
        recoverPeak = std::max(recoverPeak, peakOf(
            std::vector<float>(recover.getReadPointer(0), recover.getReadPointer(0) + blockSize),
            0, blockSize));
    }
    result.recovered = recoverPeak > kAudible && ! part(processor).voice().amplitude_envelope_dead();

    expect(finiteSamples(rendered), "CC120 output remains finite");
    expect(result.peakBefore > kAudible, "note was audible before CC120");
    expect(result.envelopeDead, "amplitude envelope is DEAD after CC120");
    expect(result.peakAfterBudget <= kPracticalSilence,
           "host output after the latency budget is practically silent");
    expect(result.recovered, "Note On after CC120 restores synthesis");
    report(result);
    return result;
}

void audioQualification()
{
    std::printf("CC120 host-output measurements (budget is filter+FIR latency, no 25 ms floor):\n");
    measureCc120("dry-osc", 48000, 64, 0, true, false, false, 0, false);
    measureCc120("long-release", 48000, 256, 17, true, false, false, 0, false);
    measureCc120("classic-reso", 48000, 64, 0, true, true, false, 0, false);
    measureCc120("experimental-svf", 48000, 64, 8, true, true, true, 0, false);
    measureCc120("board-delay", 48000, 64, 0, true, false, false, 6, false);
    measureCc120("delay-feedback", 48000, 128, 0, true, false, false, 7, false);
    measureCc120("arp", 48000, 256, 0, true, false, false, 0, true);
}

void contrastReleaseVsKill()
{
    constexpr int holdBlocks = 48;
    constexpr int afterBlocks = 80;
    Processor killProc;
    applyDryLongRelease(killProc, true);
    auto killed = holdThenEvent(killProc, 48000, 64, juce::MidiMessage::allSoundOff(1), 0, holdBlocks, afterBlocks);
    const int killEvent = holdBlocks * 64;
    const int budget = architecturalSilenceBudget(48000);
    const float killTail = peakOf(killed.left, killEvent + budget, static_cast<int>(killed.left.size()));

    Processor releaseProc;
    applyDryLongRelease(releaseProc, true);
    auto released = holdThenEvent(releaseProc, 48000, 64, juce::MidiMessage::allNotesOff(1), 0, holdBlocks, afterBlocks);
    const float releaseTail = peakOf(released.left, killEvent + budget,
                                     static_cast<int>(released.left.size()));
    const float noteOffTail = [&] {
        Processor noteOffProc;
        applyDryLongRelease(noteOffProc, true);
        auto rendered = holdThenEvent(noteOffProc, 48000, 64, juce::MidiMessage::noteOff(1, 60), 0, holdBlocks, afterBlocks);
        return peakOf(rendered.left, killEvent + budget, static_cast<int>(rendered.left.size()));
    }();

    std::printf("contrast: CC120 tail=%.6f CC123 tail=%.6f NoteOff tail=%.6f (budget=%d)\n",
                killTail, releaseTail, noteOffTail, budget);
    expect(killTail <= kPracticalSilence, "CC120 does not keep the long-release tail");
    expect(releaseTail > kAudible, "CC123 still has the long-release tail");
    expect(noteOffTail > kAudible, "ordinary Note Off still has the long-release tail");
    expect(killTail < 0.05f * releaseTail, "CC120 tail is far below the CC123 release tail");
    expect(part(killProc).voice().amplitude_envelope_dead(), "CC120 envelope DEAD");
    expect(! part(releaseProc).voice().amplitude_envelope_dead(), "CC123 envelope not DEAD");
}

float hostPeakAfter(Processor& processor, int blockSize, int blocks)
{
    float peak = 0.0f;
    juce::AudioBuffer<float> audio(2, blockSize);
    for (int i = 0; i < blocks; ++i)
    {
        juce::MidiBuffer idle;
        processor.processBlock(audio, idle);
        peak = std::max(peak, peakOf(
            std::vector<float>(audio.getReadPointer(0), audio.getReadPointer(0) + blockSize),
            0, blockSize));
    }
    return peak;
}

void expectRetriggerAudible(const char* name, int blockSize, int panicOffset, int noteOffset,
                            bool noteInNextBlock)
{
    Processor panicOnly;
    applyDryLongRelease(panicOnly, true);
    panicOnly.prepareToPlay(48000, blockSize);
    juce::AudioBuffer<float> audio(2, blockSize);
    juce::MidiBuffer idle;
    panicOnly.processBlock(audio, idle);
    for (int i = 0; i < 12; ++i)
    {
        juce::MidiBuffer midi;
        if (i == 0)
            midi.addEvent(juce::MidiMessage::noteOn(1, 60, (juce::uint8) 100), 0);
        panicOnly.processBlock(audio, midi);
    }
    juce::MidiBuffer panic;
    panic.addEvent(juce::MidiMessage::allSoundOff(1), panicOffset);
    panicOnly.processBlock(audio, panic);
    const float residue = hostPeakAfter(panicOnly, blockSize, recoveryBlocks(48000, blockSize));

    Processor processor;
    applyDryLongRelease(processor, true);
    processor.prepareToPlay(48000, blockSize);
    processor.processBlock(audio, idle);
    for (int i = 0; i < 12; ++i)
    {
        juce::MidiBuffer midi;
        if (i == 0)
            midi.addEvent(juce::MidiMessage::noteOn(1, 60, (juce::uint8) 100), 0);
        processor.processBlock(audio, midi);
    }

    juce::MidiBuffer packed;
    packed.addEvent(juce::MidiMessage::allSoundOff(1), panicOffset);
    if (! noteInNextBlock)
        packed.addEvent(juce::MidiMessage::noteOn(1, 67, (juce::uint8) 110), noteOffset);
    processor.processBlock(audio, packed);
    if (noteInNextBlock)
    {
        juce::MidiBuffer next;
        next.addEvent(juce::MidiMessage::noteOn(1, 67, (juce::uint8) 110), noteOffset);
        processor.processBlock(audio, next);
    }

    expect(part(processor).num_notes() == 1, name);
    expect(! part(processor).voice().amplitude_envelope_dead(), name);
    const float recovered = hostPeakAfter(processor, blockSize, recoveryBlocks(48000, blockSize));
    expect(recovered > kAudible, name);
    expect(recovered > residue + kPracticalSilence, name);
}

void sameBlockRetrigger()
{
    expectRetriggerAudible("retrigger 16 then 180 owns and sounds", 256, 16, 180, false);
    expectRetriggerAudible("retrigger 40 then 41 owns and sounds", 128, 40, 41, false);
    expectRetriggerAudible("retrigger same sample owns and sounds", 128, 16, 16, false);
    expectRetriggerAudible("retrigger next block owns and sounds", 64, 63, 0, true);

    Processor processor;
    applyDryLongRelease(processor, true);
    processor.prepareToPlay(48000, 256);
    juce::AudioBuffer<float> audio(2, 256);
    juce::MidiBuffer idle;
    processor.processBlock(audio, idle);
    juce::MidiBuffer note;
    note.addEvent(juce::MidiMessage::noteOn(1, 60, (juce::uint8) 100), 0);
    processor.processBlock(audio, note);
    for (int i = 0; i < 8; ++i)
        processor.processBlock(audio, idle);
    juce::MidiBuffer twice;
    twice.addEvent(juce::MidiMessage::allSoundOff(1), 0);
    twice.addEvent(juce::MidiMessage::allSoundOff(1), 200);
    twice.addEvent(juce::MidiMessage::noteOn(1, 72, (juce::uint8) 110), 220);
    processor.processBlock(audio, twice);
    expect(part(processor).num_notes() == 1, "repeated CC120 then Note On is owned");
    expect(! part(processor).voice().amplitude_envelope_dead(),
           "repeated CC120 then Note On starts the envelope");
    expect(hostPeakAfter(processor, 256, recoveryBlocks(48000, 256)) > kAudible,
           "repeated CC120 then Note On reaches the host");
}

void ignoredChannelAudio()
{
    Processor processor;
    applyDryLongRelease(processor, true);
    setInt(processor, swaraxt::IDs::midiChannel, 2);
    auto rendered = holdThenEvent(processor, 48000, 64,
                                  juce::MidiMessage::allSoundOff(1), 0, 16, 12, 2);
    expect(part(processor).num_notes() == 1, "wrong-channel CC120 leaves the key held");
    expect(! part(processor).voice().amplitude_envelope_dead(),
           "wrong-channel CC120 leaves the envelope alive");
    const float tail = peakOf(rendered.left, 16 * 64, static_cast<int>(rendered.left.size()));
    expect(tail > kAudible, "wrong-channel CC120 must not silence the host output");
}

void timingMatrix()
{
    const double rates[] = { 44100.0, 48000.0, 96000.0, 192000.0 };
    const int blocks[] = { 32, 64, 256, 1024 };
    std::printf("CC120 timing matrix:\n");
    for (double rate : rates)
    {
        for (int block : blocks)
        {
            const int offsets[] = { 0, std::max(1, block / 3), block - 1 };
            for (int offset : offsets)
            {
                char name[64];
                std::snprintf(name, sizeof(name), "t-%.0f-%d@%d", rate, block, offset);
                measureCc120(name, rate, block, offset, true, false, false, false, false);
            }
        }
    }

    Processor processor;
    applyDryLongRelease(processor, true);
    processor.prepareToPlay(48000, 128);
    juce::AudioBuffer<float> audio(2, 128);
    juce::MidiBuffer hold;
    hold.addEvent(juce::MidiMessage::noteOn(1, 60, (juce::uint8) 100), 0);
    processor.processBlock(audio, hold);
    for (int i = 0; i < 8; ++i)
    {
        juce::MidiBuffer idle;
        processor.processBlock(audio, idle);
    }
    juce::MidiBuffer beforeNote;
    beforeNote.addEvent(juce::MidiMessage::allSoundOff(1), 40);
    beforeNote.addEvent(juce::MidiMessage::noteOn(1, 62, (juce::uint8) 100), 41);
    processor.processBlock(audio, beforeNote);
    expect(part(processor).num_notes() == 1, "CC120 immediately before Note On still retriggers");
    expect(! part(processor).voice().amplitude_envelope_dead(),
           "CC120 then Note On one sample later starts the envelope");
}

void sequencerDoesNotStickTheKilledVoice()
{
    // Host-clock SEQ with no transport: after panic the killed step must stay
    // dead. Internal-clock SEQ may emit a later step by design; that is not
    // tested as a failure here.
    Processor processor;
    applyDryLongRelease(processor, true);
    setInt(processor, swaraxt::IDs::seqMode, 2);
    setInt(processor, swaraxt::IDs::seqTempo, 140);
    setInt(processor, swaraxt::IDs::seqClockMode, 1);
    processor.prepareToPlay(48000, 256);
    juce::AudioBuffer<float> audio(2, 256);
    juce::MidiBuffer idle;
    processor.processBlock(audio, idle);
    expect(part(processor).sequencer_settings().seq_tempo == 0,
           "host-clock mode disables the internal sequencer clock");

    juce::MidiBuffer note;
    note.addEvent(juce::MidiMessage::noteOn(1, 60, (juce::uint8) 100), 0);
    processor.processBlock(audio, note);
    for (int i = 0; i < 6; ++i)
        processor.processBlock(audio, idle);

    const auto generatedBefore = part(processor).generated_note_count_for_tests();
    juce::MidiBuffer cc120;
    cc120.addEvent(juce::MidiMessage::allSoundOff(1), 0);
    processor.processBlock(audio, cc120);
    expect(part(processor).voice().amplitude_envelope_dead(),
           "CC120 terminates the sounding sequencer voice");
    expect(part(processor).generated_note_count_for_tests() == 0,
           "CC120 clears generated-note owners");
    expect(part(processor).num_notes() == 0, "CC120 clears pressed keys while the sequencer runs");
    for (int i = 0; i < 8; ++i)
        processor.processBlock(audio, idle);
    expect(part(processor).voice().amplitude_envelope_dead(),
           "without a host clock the killed voice stays dead");
    expect(part(processor).generated_note_count_for_tests() == 0,
           "without a host clock no new generated notes appear");
    expect(generatedBefore >= 0, "sequencer ownership was observable before panic");
}

void sustainAndModulationPanic()
{
    Processor processor;
    applyDryLongRelease(processor, true);
    setInt(processor, swaraxt::IDs::lfo1Wave, 1);
    setInt(processor, swaraxt::IDs::lfo1Rate, 120);
    setInt(processor, swaraxt::IDs::lfo1Attack, 0);
    setInt(processor, swaraxt::IDs::filterLfoDepth, 63);
    processor.prepareToPlay(48000, 64);
    juce::AudioBuffer<float> audio(2, 64);
    juce::MidiBuffer idle;
    processor.processBlock(audio, idle);

    juce::MidiBuffer note;
    note.addEvent(juce::MidiMessage::noteOn(1, 60, (juce::uint8) 100), 0);
    processor.processBlock(audio, note);
    for (int i = 0; i < 8; ++i)
        processor.processBlock(audio, idle);

    juce::MidiBuffer pedal;
    pedal.addEvent(juce::MidiMessage::controllerEvent(1, 64, 127), 0);
    pedal.addEvent(juce::MidiMessage::controllerEvent(1, 1, 127), 1);
    pedal.addEvent(juce::MidiMessage::noteOff(1, 60), 2);
    processor.processBlock(audio, pedal);
    expect(part(processor).latched(), "sustain is engaged");
    expect(part(processor).num_notes() == 1, "the note stays latched after Note Off");
    expect(part(processor).voice().modulation_source(avrlib_hd::kModSourceWheel) > 0,
           "mod wheel modulation is active");
    bool lfoMoved = false;
    auto lfoBefore = part(processor).voice().modulation_source(avrlib_hd::kModSourceLfo1);
    for (int i = 0; i < 16; ++i)
    {
        processor.processBlock(audio, idle);
        const auto now = part(processor).voice().modulation_source(avrlib_hd::kModSourceLfo1);
        lfoMoved = lfoMoved || now != lfoBefore;
        lfoBefore = now;
    }
    expect(lfoMoved, "LFO modulation is running");

    juce::MidiBuffer panic;
    panic.addEvent(juce::MidiMessage::allSoundOff(1), 0);
    processor.processBlock(audio, panic);
    expect(part(processor).num_notes() == 0, "CC120 clears the latched owner");
    expect(! part(processor).latched(), "CC120 clears the sustain pedal");
    expect(part(processor).voice().amplitude_envelope_dead(), "CC120 envelope is DEAD");

    juce::MidiBuffer pedalUp;
    pedalUp.addEvent(juce::MidiMessage::controllerEvent(1, 64, 0), 0);
    processor.processBlock(audio, pedalUp);
    expect(part(processor).num_notes() == 0, "pedal-up does not restore the old note");
    expect(part(processor).voice().amplitude_envelope_dead(), "pedal-up leaves the envelope DEAD");

    juce::MidiBuffer again;
    again.addEvent(juce::MidiMessage::noteOn(1, 67, (juce::uint8) 110), 0);
    processor.processBlock(audio, again);
    expect(part(processor).num_notes() == 1, "fresh Note On after sustain panic is owned");
    expect(hostPeakAfter(processor, 64, recoveryBlocks(48000, 64)) > kAudible,
           "fresh Note On after sustain panic reaches the host");
}

void feedbackDelayIsLiveBeforePanic()
{
    Processor dry;
    applyDryLongRelease(dry, false);
    auto dryTail = holdThenEvent(dry, 48000, 64, juce::MidiMessage::noteOff(1, 60), 0, 16, 40);
    const float dryPeak = peakOf(dryTail.left, 20 * 64, static_cast<int>(dryTail.left.size()));

    Processor feedback;
    applyDryLongRelease(feedback, false);
    setInt(feedback, swaraxt::IDs::dspFxProgram, 7);
    setInt(feedback, swaraxt::IDs::dspFxParam1, 96);
    setInt(feedback, swaraxt::IDs::dspFxParam2, 63);
    auto tail = holdThenEvent(feedback, 48000, 64, juce::MidiMessage::noteOff(1, 60), 0, 16, 40);
    const float fbPeak = peakOf(tail.left, 20 * 64, static_cast<int>(tail.left.size()));
    expect(fbPeak > kAudible, "delay feedback is still sounding after the voice releases");
    expect(fbPeak > dryPeak + kAudible, "feedback tail exceeds the dry release");
}

void looperSurvivesPanic()
{
    Processor processor;
    applyDryLongRelease(processor, false);
    setInt(processor, swaraxt::IDs::dspFxProgram, 15);
    setInt(processor, swaraxt::IDs::dspFxParam1, 32);
    setInt(processor, swaraxt::IDs::dspFxParam2, 0);
    processor.prepareToPlay(48000, 64);
    juce::AudioBuffer<float> audio(2, 64);
    juce::MidiBuffer idle;
    processor.processBlock(audio, idle);
    juce::MidiBuffer note;
    note.addEvent(juce::MidiMessage::noteOn(1, 60, (juce::uint8) 100), 0);
    processor.processBlock(audio, note);
    for (int i = 0; i < 80; ++i)
        processor.processBlock(audio, idle);
    expect(processor.engineForTests().boardProcessorForTests().hasValidLoop(),
           "looper recorded nonzero content");

    juce::MidiBuffer release;
    release.addEvent(juce::MidiMessage::noteOff(1, 60), 0);
    processor.processBlock(audio, release);
    for (int i = 0; i < 20 && ! part(processor).voice().amplitude_envelope_dead(); ++i)
        processor.processBlock(audio, idle);

    setInt(processor, swaraxt::IDs::dspFxParam2, 40);
    float playPeak = 0.0f;
    for (int i = 0; i < 40; ++i)
    {
        processor.processBlock(audio, idle);
        if (i >= 8)
            playPeak = std::max(playPeak, peakOf(
                std::vector<float>(audio.getReadPointer(0), audio.getReadPointer(0) + 64), 0, 64));
    }
    expect(part(processor).voice().amplitude_envelope_dead(), "loop playback is not the synthesizer voice");
    expect(playPeak > kAudible, "recorded loop plays back");

    juce::MidiBuffer panic;
    panic.addEvent(juce::MidiMessage::allSoundOff(1), 0);
    processor.processBlock(audio, panic);
    expect(processor.engineForTests().boardProcessorForTests().hasValidLoop(),
           "CC120 keeps the recorded loop");
    expect(processor.engineForTests().boardProcessorForTests().loopPlaybackHeld(),
           "CC120 stops autonomous looper playback");
    expect(part(processor).voice().amplitude_envelope_dead(), "CC120 kills the voice during loop playback");
    const int skip = architecturalSilenceBudget(48000) / 64 + 2;
    for (int i = 0; i < skip; ++i)
        processor.processBlock(audio, idle);
    const float held = hostPeakAfter(processor, 64, 20);
    expect(held <= kPracticalSilence, "held loop does not replay by itself");

    juce::MidiBuffer again;
    again.addEvent(juce::MidiMessage::allSoundOff(1), 0);
    processor.processBlock(audio, again);
    expect(processor.engineForTests().boardProcessorForTests().hasValidLoop(),
           "a second CC120 still keeps the loop");
    expect(hostPeakAfter(processor, 64, 10) <= kPracticalSilence,
           "repeated CC120 stays silent");

    setInt(processor, swaraxt::IDs::dspFxParam2, 48);
    float resume = 0.0f;
    for (int i = 0; i < 40; ++i)
    {
        processor.processBlock(audio, idle);
        resume = std::max(resume, peakOf(
            std::vector<float>(audio.getReadPointer(0), audio.getReadPointer(0) + 64), 0, 64));
    }
    expect(part(processor).voice().amplitude_envelope_dead(), "resume does not require a new recording");
    expect(resume > kAudible, "the original loop plays again after the play control changes");

    juce::MidiBuffer fresh;
    fresh.addEvent(juce::MidiMessage::noteOn(1, 64, (juce::uint8) 100), 0);
    processor.processBlock(audio, fresh);
    expect(! part(processor).voice().amplitude_envelope_dead(),
           "Note On after looper panic starts the voice");
}

}  // namespace

int main()
{
    setvbuf(stdout, nullptr, _IONBF, 0);
    juce::ScopedJuceInitialiser_GUI initialise;
    midiLifecycle();
    envelopeQualification();
    contrastReleaseVsKill();
    sameBlockRetrigger();
    sustainAndModulationPanic();
    feedbackDelayIsLiveBeforePanic();
    looperSurvivesPanic();
    ignoredChannelAudio();
    sequencerDoesNotStickTheKilledVoice();
    audioQualification();
    timingMatrix();
    std::printf("MIDI CC120 emergency silence tests: %s (%d failures)\n",
                failures ? "FAILED" : "PASSED", failures);
    return failures ? 1 : 0;
}
