// Copyright 2026 MontroneDSP.
// SPDX-License-Identifier: GPL-3.0-or-later

#include <JuceHeader.h>

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <vector>

#include "Engine/MidiInputTuning.h"
#include "Plugin/PluginEditor.h"
#include "Plugin/PluginProcessor.h"
#include "Plugin/SwaraXtParameterLayout.h"
#include "Tuning/ScalePitch.h"
#include "Tuning/TuningTable.h"
#include "Ui/SwaraXtPanels.h"
#include "shruthi/sequencer_settings.h"

namespace {

int failures = 0;

void expect(bool condition, const char* message)
{
    if (! condition)
    {
        std::printf("FAIL: %s\n", message);
        ++failures;
    }
}

void setInt(SwaraXtAudioProcessor& processor, const char* id, int value)
{
    if (auto* parameter = processor.getApvts().getParameter(id))
        parameter->setValueNotifyingHost(parameter->convertTo0to1(static_cast<float>(value)));
}

void setNormalized(SwaraXtAudioProcessor& processor, const char* id, float normalized)
{
    if (auto* parameter = processor.getApvts().getParameter(id))
        parameter->setValueNotifyingHost(juce::jlimit(0.0f, 1.0f, normalized));
}

double estimateFundamentalHz(const std::vector<float>& mono,
                             double sampleRate,
                             double hintHz = 0.0)
{
    if (mono.size() < 2048)
        return 0.0;
    const int n = static_cast<int>(mono.size());
    double mean = 0.0;
    for (float s : mono)
        mean += s;
    mean /= static_cast<double>(n);

    // Saw autocorrelation often prefers subharmonics; bias the search around a
    // pitch-word hint when available, otherwise keep a wide musical window.
    int minLag = static_cast<int>(sampleRate / 900.0);
    int maxLag = static_cast<int>(sampleRate / 80.0);
    if (hintHz > 80.0 && hintHz < 900.0)
    {
        const int centerLag = static_cast<int>(std::lround(sampleRate / hintHz));
        minLag = std::max(8, centerLag - centerLag / 5);
        maxLag = std::min(n / 2 - 1, centerLag + centerLag / 5);
        if (minLag >= maxLag)
        {
            minLag = static_cast<int>(sampleRate / 900.0);
            maxLag = static_cast<int>(sampleRate / 80.0);
        }
    }

    double bestCorr = -1.0e300;
    int bestLag = 0;
    for (int lag = minLag; lag <= maxLag; ++lag)
    {
        double corr = 0.0;
        double energy0 = 0.0;
        double energy1 = 0.0;
        const int count = n - lag;
        for (int i = 0; i < count; ++i)
        {
            const double a = mono[static_cast<size_t>(i)] - mean;
            const double b = mono[static_cast<size_t>(i + lag)] - mean;
            corr += a * b;
            energy0 += a * a;
            energy1 += b * b;
        }
        const double denom = std::sqrt(energy0 * energy1);
        if (denom <= 1.0e-20)
            continue;
        corr /= denom;
        if (corr > bestCorr)
        {
            bestCorr = corr;
            bestLag = lag;
        }
    }
    if (bestLag <= 0 || bestCorr < 0.2)
        return 0.0;
    return sampleRate / static_cast<double>(bestLag);
}

double centsBetween(double aHz, double bHz)
{
    if (! (aHz > 0.0 && bHz > 0.0))
        return 0.0;
    return 1200.0 * std::log2(bHz / aHz);
}

// Default PB -> VCO_1_2_COARSE amount 32 yields (signed_src * 32) >> 4 pitch units.
constexpr double kExpectedBendUpCents = 100.0 * (254.0 / 128.0);   // MIDI 16383
constexpr double kExpectedBendDownCents = 100.0 * (-256.0 / 128.0); // MIDI 0
constexpr double kExpectedBendHalfCents = 100.0 * (128.0 / 128.0);  // MIDI 12288

void setStepMode(SwaraXtAudioProcessor& processor);
double hzFromWord(int word);

void prepareStableBendPatch(SwaraXtAudioProcessor& processor)
{
    setInt(processor, swaraxt::IDs::osc1Shape, 1); // SAW
    setInt(processor, swaraxt::IDs::osc2Shape, 0); // NONE
    setInt(processor, swaraxt::IDs::mixBalance, 0);
    setInt(processor, swaraxt::IDs::mixSub, 0);
    setInt(processor, swaraxt::IDs::mixNoise, 0);
    setInt(processor, swaraxt::IDs::env1Attack, 0);
    setInt(processor, swaraxt::IDs::env1Decay, 0);
    setInt(processor, swaraxt::IDs::env1Sustain, 127);
    setInt(processor, swaraxt::IDs::env1Release, 0);
    setInt(processor, swaraxt::IDs::env2Attack, 0);
    setInt(processor, swaraxt::IDs::env2Decay, 0);
    setInt(processor, swaraxt::IDs::env2Sustain, 127);
    setInt(processor, swaraxt::IDs::env2Release, 0);
    setNormalized(processor, swaraxt::IDs::filterCutoff, 1.0f);
    setNormalized(processor, swaraxt::IDs::filterResonance, 0.0f);
    setInt(processor, swaraxt::IDs::filterEnvDepth, 0);
    setInt(processor, swaraxt::IDs::filterLfoDepth, 0);
    setInt(processor, swaraxt::IDs::dspFxProgram, 0);
    setInt(processor, swaraxt::IDs::perfGlide, 0);
    setInt(processor, swaraxt::IDs::perfLegato, 0);
    setNormalized(processor, swaraxt::IDs::master, 0.75f);
    // Keep init-patch PB -> VCO coarse 32; zero LFO1 -> VCO coarse vibrato.
    setInt(processor, "mod.row11.source", 8);
    setInt(processor, "mod.row11.destination", 6);
    setInt(processor, "mod.row11.amount", 32);
    setInt(processor, "mod.row12.amount", 0);
    setInt(processor, "mod.row1.amount", 0);
    setInt(processor, "mod.row2.amount", 0);
    setInt(processor, "mod.row3.amount", 0);
    setInt(processor, "mod.row4.amount", 0);
    setInt(processor, "mod.row5.amount", 0);
    setInt(processor, "mod.row6.amount", 0);
    setInt(processor, "mod.row7.amount", 0);
    setInt(processor, "mod.row8.amount", 0);
}

void processIdle(SwaraXtAudioProcessor& processor, juce::AudioBuffer<float>& buffer, int blocks)
{
    for (int i = 0; i < blocks; ++i)
    {
        juce::MidiBuffer midi;
        processor.processBlock(buffer, midi);
    }
}

std::vector<float> captureAudio(SwaraXtAudioProcessor& processor,
                                juce::AudioBuffer<float>& buffer,
                                int totalSamples)
{
    std::vector<float> mono;
    mono.reserve(static_cast<size_t>(totalSamples));
    int generated = 0;
    while (generated < totalSamples)
    {
        const int n = std::min(buffer.getNumSamples(), totalSamples - generated);
        juce::AudioBuffer<float> slice(2, n);
        juce::MidiBuffer midi;
        processor.processBlock(slice, midi);
        for (int i = 0; i < n; ++i)
            mono.push_back(0.5f * (slice.getSample(0, i) + slice.getSample(1, i)));
        generated += n;
    }
    return mono;
}

void sendBend(SwaraXtAudioProcessor& processor, juce::AudioBuffer<float>& buffer, int value)
{
    juce::MidiBuffer midi;
    midi.addEvent(juce::MidiMessage::pitchWheel(1, value), 0);
    processor.processBlock(buffer, midi);
}

struct BendResult
{
    double center = 0.0;
    double positive = 0.0;
    double half = 0.0;
    double negative = 0.0;
    double returned = 0.0;
    int baseWord = 0;
    int wordDuringBend = 0;
};

BendResult measureAudibleBend(SwaraXtAudioProcessor& processor, int note)
{
    constexpr double sampleRate = 48000.0;
    constexpr int blockSize = 64;
    constexpr int warmup = 12000;
    constexpr int capture = 24000;

    prepareStableBendPatch(processor);
    setStepMode(processor);
    processor.prepareToPlay(sampleRate, blockSize);
    juce::AudioBuffer<float> buffer(2, blockSize);

    {
        juce::MidiBuffer clear;
        clear.addEvent(juce::MidiMessage::allNotesOff(1), 0);
        clear.addEvent(juce::MidiMessage::pitchWheel(1, 8192), 0);
        processor.processBlock(buffer, clear);
    }
    {
        juce::MidiBuffer on;
        on.addEvent(juce::MidiMessage::noteOn(1, note, static_cast<juce::uint8>(110)), 0);
        processor.processBlock(buffer, on);
    }
    processIdle(processor, buffer, warmup / blockSize);

    BendResult result;
    result.baseWord = processor.engineForTests().shruthiPart().voice().pitch_value();
    const double baseHint = hzFromWord(result.baseWord);
    // Matrix amount 32: MIDI 16383 -> +254 pitch units; MIDI 0 -> -256; 12288 -> +128.
    const double upHint = hzFromWord(result.baseWord + 254);
    const double halfHint = hzFromWord(result.baseWord + 128);
    const double downHint = hzFromWord(result.baseWord - 256);

    auto centerMono = captureAudio(processor, buffer, capture);
    result.center = estimateFundamentalHz(centerMono, sampleRate, baseHint);

    sendBend(processor, buffer, 16383);
    processIdle(processor, buffer, warmup / blockSize);
    result.wordDuringBend = processor.engineForTests().shruthiPart().voice().pitch_value();
    auto posMono = captureAudio(processor, buffer, capture);
    result.positive = estimateFundamentalHz(posMono, sampleRate, upHint);

    sendBend(processor, buffer, 12288);
    processIdle(processor, buffer, warmup / blockSize);
    auto halfMono = captureAudio(processor, buffer, capture);
    result.half = estimateFundamentalHz(halfMono, sampleRate, halfHint);

    sendBend(processor, buffer, 8192);
    processIdle(processor, buffer, warmup / blockSize);

    sendBend(processor, buffer, 0);
    processIdle(processor, buffer, warmup / blockSize);
    auto negMono = captureAudio(processor, buffer, capture);
    result.negative = estimateFundamentalHz(negMono, sampleRate, downHint);

    sendBend(processor, buffer, 8192);
    processIdle(processor, buffer, warmup / blockSize);
    auto retMono = captureAudio(processor, buffer, capture);
    result.returned = estimateFundamentalHz(retMono, sampleRate, baseHint);

    juce::MidiBuffer off;
    off.addEvent(juce::MidiMessage::noteOff(1, note), 0);
    off.addEvent(juce::MidiMessage::allNotesOff(1), 0);
    processor.processBlock(buffer, off);
    return result;
}

double hzFromWord(int word)
{
    const double semis = static_cast<double>(word) / 128.0;
    return 440.0 * std::pow(2.0, (semis - 69.0) / 12.0);
}

uint16_t expectedTuned(SwaraXtAudioProcessor& processor, int note)
{
    int16_t degrees[128];
    processor.copyTuningDegrees(degrees, 128);
    if (note < 0 || note > 127 || degrees[note] < 0)
        return 0;
    return swaraxt::ScalePitch::toVoicePitchFromHz(processor.hzForActiveDegree(degrees[note]));
}

uint16_t playNote(SwaraXtAudioProcessor& processor, int note, int channel = 1, int blocks = 4)
{
    processor.prepareToPlay(48000.0, 128);
    juce::AudioBuffer<float> buffer(2, 128);
    {
        juce::MidiBuffer off;
        off.addEvent(juce::MidiMessage::allNotesOff(channel), 0);
        processor.processBlock(buffer, off);
    }
    uint16_t word = 0;
    for (int block = 0; block < blocks; ++block)
    {
        juce::MidiBuffer midi;
        if (block == 0)
            midi.addEvent(juce::MidiMessage::noteOn(channel, note, static_cast<juce::uint8>(100)), 0);
        processor.processBlock(buffer, midi);
        word = static_cast<uint16_t>(processor.engineForTests().shruthiPart().voice().pitch_value());
    }
    return word;
}

void setStepMode(SwaraXtAudioProcessor& processor)
{
    setInt(processor, swaraxt::IDs::seqMode, shruthi::SEQUENCER_MODE_STEP);
    setInt(processor, swaraxt::IDs::perfGlide, 0);
    setInt(processor, swaraxt::IDs::perfLegato, 0);
    setInt(processor, swaraxt::IDs::midiChannel, 0);
}

void testOffIsOriginalTune()
{
    SwaraXtAudioProcessor processor;
    setStepMode(processor);
    setInt(processor, swaraxt::IDs::midiInputTuning, 0);
    const auto word = playNote(processor, 60);
    expect(word == 60 * 128, "OFF MIDI 60 uses the original 128-unit Tune word");
    std::printf("OFF MIDI 60 word=%u hz=%.6f\n", word, hzFromWord(word));

    processor.engineForTests().shruthiPart().mutable_system_settings()->raga = 1;
    const auto ragaWord = playNote(processor, 61);
    expect(ragaWord == 61 * 128 + 15, "OFF applies the just-raga offset from Tune()");
    std::printf("OFF raga MIDI 61 word=%u hz=%.6f\n", ragaWord, hzFromWord(ragaWord));

    setInt(processor, swaraxt::IDs::midiInputTuning, 1);
    expect(processor.loadBuiltinTuning(0), "12-EDO over raga");
    const auto tuned = playNote(processor, 61);
    expect(tuned == expectedTuned(processor, 61), "TUNED ignores the stored raga offset");
    expect(tuned != ragaWord, "TUNED pitch is not the raga pitch");
    expect(processor.engineForTests().shruthiPart().system_settings().raga == 1,
           "switching to TUNED keeps the raga setting stored");
    setInt(processor, swaraxt::IDs::midiInputTuning, 0);
    processor.engineForTests().shruthiPart().mutable_system_settings()->raga = 0;
    expect(playNote(processor, 61) == 61 * 128, "clearing raga restores the original Tune word");
}

void testTunedMapping()
{
    SwaraXtAudioProcessor processor;
    setStepMode(processor);
    setInt(processor, swaraxt::IDs::midiInputTuning, 1);

    expect(processor.loadBuiltinTuning(0), "12-EDO");
    const auto a4 = playNote(processor, 69);
    expect(a4 == expectedTuned(processor, 69), "TUNED A4 follows the SWT1 keyboard degree");
    std::printf("TUNED 12-EDO A4 word=%u hz=%.6f\n", a4, hzFromWord(a4));

    expect(processor.loadBuiltinTuning(1), "19-EDO");
    const auto key = playNote(processor, 61);
    expect(key == expectedTuned(processor, 61), "TUNED 19-EDO uses the mapped degree");
    expect(key != 61 * 128, "TUNED 19-EDO is not ordinary 12-TET");
    std::printf("TUNED 19-EDO key 61 word=%u hz=%.6f\n", key, hzFromWord(key));

    expect(processor.loadBuiltinTuning(2), "24-EDO");
    const auto edo24 = playNote(processor, 63);
    expect(edo24 == expectedTuned(processor, 63), "TUNED 24-EDO");

    expect(processor.loadBuiltinTuning(3), "31-EDO");
    const auto edo31 = playNote(processor, 70);
    expect(edo31 == expectedTuned(processor, 70), "TUNED 31-EDO");
    std::printf("TUNED 31-EDO key 70 word=%u hz=%.6f\n", edo31, hzFromWord(edo31));

    expect(processor.loadBuiltinTuning(4), "just");
    const auto justWord = playNote(processor, 67);
    expect(justWord == expectedTuned(processor, 67), "TUNED just intonation follows the keyboard map");
    std::printf("TUNED just key 67 word=%u hz=%.6f\n", justWord, hzFromWord(justWord));

    expect(processor.loadBuiltinTuning(5), "tritave");
    const auto tritaveWord = playNote(processor, 62);
    expect(tritaveWord == expectedTuned(processor, 62), "TUNED tritave follows its map");
    std::printf("TUNED tritave key 62 word=%u hz=%.6f\n", tritaveWord, hzFromWord(tritaveWord));
}

void testQuantizedInput()
{
    SwaraXtAudioProcessor processor;
    setStepMode(processor);
    setInt(processor, swaraxt::IDs::midiInputTuning, 2);
    const int tunings[] = { 1, 2, 3, 4, 5 };
    const char* names[] = { "19-EDO", "24-EDO", "31-EDO", "just", "tritave" };
    const int notes[] = { 60, 61, 63, 67, 70 };
    for (int index = 0; index < 5; ++index)
    {
        expect(processor.loadBuiltinTuning(tunings[index]), names[index]);
        const auto word = playNote(processor, notes[index]);
        const double source = 440.0 * std::pow(2.0, (static_cast<double>(notes[index]) - 69.0) / 12.0);
        const auto chosen = processor.quantizeActiveHz(source);
        const auto expected = swaraxt::ScalePitch::toVoicePitchFromHz(chosen.hz);
        expect(chosen.valid && word == expected, names[index]);
        std::printf("QUANTIZED %s MIDI %d -> %.6f Hz word=%u rendered=%.6f\n",
                    names[index], notes[index], chosen.hz, word, hzFromWord(word));
    }

    expect(processor.loadBuiltinTuning(4), "just");
    std::printf("QUANTIZED families complete\n");
}

void testUnmappedAndAllocation()
{
    const char* scale =
        "! partial\n"
        "partial map\n"
        "12\n"
        "100.0\n200.0\n300.0\n400.0\n500.0\n600.0\n"
        "700.0\n800.0\n900.0\n1000.0\n1100.0\n1200.0\n";
    const char* kbm =
        "2\n60\n127\n60\n60\n261.6255653006\n12\n0\nx\n";
    swaraxt::tuning::ParseError error;
    swaraxt::tuning::TuningSnapshot snapshot;
    expect(swaraxt::tuning::parseScalaAndKeyboard(scale, kbm, snapshot, error), "partial map parses");
    std::vector<uint8_t> bytes;
    expect(swaraxt::tuning::writeTuningTable(snapshot, bytes, error), "partial map encodes");

    SwaraXtAudioProcessor processor;
    setStepMode(processor);
    setInt(processor, swaraxt::IDs::midiInputTuning, 1);
    expect(processor.loadTuningBytes(bytes.data(), bytes.size()), "partial map loads");
    processor.prepareToPlay(48000.0, 128);
    juce::AudioBuffer<float> buffer(2, 128);
    const auto mapped = expectedTuned(processor, 60);
    expect(expectedTuned(processor, 61) == 0, "key 61 is unmapped");
    uint16_t held = 0;
    {
        juce::MidiBuffer midi;
        midi.addEvent(juce::MidiMessage::noteOn(1, 60, static_cast<juce::uint8>(100)), 0);
        processor.processBlock(buffer, midi);
        held = static_cast<uint16_t>(processor.engineForTests().shruthiPart().voice().pitch_value());
    }
    expect(held == mapped, "mapped key attacks at its SWT1 pitch");
    {
        juce::MidiBuffer midi;
        midi.addEvent(juce::MidiMessage::noteOn(1, 61, static_cast<juce::uint8>(90)), 0);
        processor.processBlock(buffer, midi);
    }
    expect(processor.engineForTests().shruthiPart().voice().pitch_value() == held,
           "unmapped Note On does not steal the sounding note");
    {
        juce::MidiBuffer midi;
        midi.addEvent(juce::MidiMessage::noteOff(1, 61), 0);
        processor.processBlock(buffer, midi);
        expect(processor.engineForTests().shruthiPart().voice().pitch_value() == held,
               "unmapped Note Off is harmless");
        midi.clear();
        midi.addEvent(juce::MidiMessage::noteOff(1, 60), 0);
        processor.processBlock(buffer, midi);
    }

    expect(processor.loadBuiltinTuning(1), "19-EDO allocation");
    const auto c = expectedTuned(processor, 60);
    const auto e = expectedTuned(processor, 64);
    uint16_t words[3] {};
    {
        juce::MidiBuffer midi;
        midi.addEvent(juce::MidiMessage::noteOn(1, 60, static_cast<juce::uint8>(100)), 0);
        processor.processBlock(buffer, midi);
        words[0] = static_cast<uint16_t>(processor.engineForTests().shruthiPart().voice().pitch_value());
    }
    {
        juce::MidiBuffer midi;
        midi.addEvent(juce::MidiMessage::noteOn(1, 64, static_cast<juce::uint8>(100)), 0);
        processor.processBlock(buffer, midi);
        words[1] = static_cast<uint16_t>(processor.engineForTests().shruthiPart().voice().pitch_value());
    }
    {
        juce::MidiBuffer midi;
        midi.addEvent(juce::MidiMessage::noteOff(1, 64), 0);
        processor.processBlock(buffer, midi);
        words[2] = static_cast<uint16_t>(processor.engineForTests().shruthiPart().voice().pitch_value());
    }
    expect(words[0] == c && words[1] == e && words[2] == c, "returning to held C restores its tuned pitch");
    {
        juce::MidiBuffer midi;
        midi.addEvent(juce::MidiMessage::noteOff(1, 60), 0);
        processor.processBlock(buffer, midi);
    }
}

void testSustainBendAndPortamento()
{
    SwaraXtAudioProcessor processor;
    setStepMode(processor);
    setInt(processor, swaraxt::IDs::midiInputTuning, 2);
    expect(processor.loadBuiltinTuning(1), "19-EDO lifecycle");
    setInt(processor, swaraxt::IDs::perfGlide, 70);
    processor.prepareToPlay(48000.0, 64);
    juce::AudioBuffer<float> buffer(2, 64);
    juce::MidiBuffer first;
    first.addEvent(juce::MidiMessage::noteOn(1, 60, static_cast<juce::uint8>(100)), 0);
    processor.processBlock(buffer, first);
    const auto start = processor.engineForTests().shruthiPart().voice().pitch_value();
    juce::MidiBuffer second;
    second.addEvent(juce::MidiMessage::noteOn(1, 67, static_cast<juce::uint8>(100)), 0);
    processor.processBlock(buffer, second);
    const auto mid = processor.engineForTests().shruthiPart().voice().pitch_value();
    const double source = 440.0 * std::pow(2.0, (67.0 - 69.0) / 12.0);
    const auto chosen = processor.quantizeActiveHz(source);
    const auto target = swaraxt::ScalePitch::toVoicePitchFromHz(chosen.hz);
    expect(start != target, "portamento has two different endpoints");
    expect(mid != start && mid != target, "portamento moves between tuned endpoints");
    std::printf("portamento start=%d mid=%d target=%u\n", start, mid, target);

    setInt(processor, swaraxt::IDs::perfGlide, 0);
    juce::MidiBuffer clear;
    clear.addEvent(juce::MidiMessage::allNotesOff(1), 0);
    processor.processBlock(buffer, clear);
    juce::MidiBuffer fresh;
    fresh.addEvent(juce::MidiMessage::noteOn(1, 60, static_cast<juce::uint8>(100)), 0);
    processor.processBlock(buffer, fresh);
    int stable = processor.engineForTests().shruthiPart().voice().pitch_value();
    for (int block = 0; block < 6; ++block)
    {
        juce::MidiBuffer idle;
        processor.processBlock(buffer, idle);
        stable = processor.engineForTests().shruthiPart().voice().pitch_value();
    }
    juce::MidiBuffer bend;
    bend.addEvent(juce::MidiMessage::pitchWheel(1, 14000), 0);
    processor.processBlock(buffer, bend);
    const auto bent = processor.engineForTests().shruthiPart().voice().pitch_value();
    juce::MidiBuffer center;
    center.addEvent(juce::MidiMessage::pitchWheel(1, 8192), 0);
    processor.processBlock(buffer, center);
    int returned = stable;
    for (int block = 0; block < 4; ++block)
    {
        juce::MidiBuffer idle;
        processor.processBlock(buffer, idle);
        returned = processor.engineForTests().shruthiPart().voice().pitch_value();
    }
    expect(returned == stable, "pitch bend release restores the tuned note target");
    std::printf("pitch bend stable=%d bent=%d returned=%d\n", stable, bent, returned);

    juce::MidiBuffer sustain;
    sustain.addEvent(juce::MidiMessage::controllerEvent(1, 64, 127), 0);
    sustain.addEvent(juce::MidiMessage::noteOff(1, 67), 0);
    processor.processBlock(buffer, sustain);
    const auto sustained = processor.engineForTests().shruthiPart().voice().pitch_value();
    juce::MidiBuffer release;
    release.addEvent(juce::MidiMessage::controllerEvent(1, 64, 0), 0);
    processor.processBlock(buffer, release);
    expect(sustained != 0, "sustain keeps a pitched note until the pedal is released");

    juce::MidiBuffer panic;
    panic.addEvent(juce::MidiMessage::allNotesOff(1), 0);
    processor.processBlock(buffer, panic);
}

void testHeldPitchAndSequencerIsolation()
{
    SwaraXtAudioProcessor processor;
    setStepMode(processor);
    setInt(processor, swaraxt::IDs::midiInputTuning, 1);
    expect(processor.loadBuiltinTuning(1), "19-EDO hold");
    processor.prepareToPlay(48000.0, 128);
    juce::AudioBuffer<float> buffer(2, 128);
    juce::MidiBuffer on;
    on.addEvent(juce::MidiMessage::noteOn(1, 60, static_cast<juce::uint8>(110)), 0);
    processor.processBlock(buffer, on);
    const auto held = processor.engineForTests().shruthiPart().voice().pitch_value();
    setInt(processor, swaraxt::IDs::midiInputTuning, 2);
    expect(processor.loadBuiltinTuning(3), "31-EDO while held");
    juce::MidiBuffer idle;
    processor.processBlock(buffer, idle);
    expect(processor.engineForTests().shruthiPart().voice().pitch_value() == held,
           "held notes keep their attack pitch across tuning and mode changes");
    juce::MidiBuffer off;
    off.addEvent(juce::MidiMessage::noteOff(1, 60), 0);
    processor.processBlock(buffer, off);
}

void testLegacySequenceIgnoresInputTuning()
{
    SwaraXtAudioProcessor processor;
    processor.prepareToPlay(48000.0, 256);
    setInt(processor, swaraxt::IDs::seqMode, shruthi::SEQUENCER_MODE_SEQ);
    setInt(processor, swaraxt::IDs::seqClockMode, 0);
    setInt(processor, swaraxt::IDs::seqTempo, 180);
    setInt(processor, swaraxt::IDs::seqGate, 100);
    setInt(processor, swaraxt::IDs::perfGlide, 0);
    setInt(processor, swaraxt::IDs::midiInputTuning, 2);
    expect(processor.loadBuiltinTuning(1), "19-EDO must not retune LEGACY");
    processor.sequenceState().setPitchMode(0);
    for (int step = 0; step < 16; ++step)
        processor.sequenceState().setStep(step, static_cast<uint8_t>(0x80 | 72), 0x70);
    auto snapshot = processor.sequenceState().snapshot();
    snapshot.length = 1;
    snapshot.pitchMode = 0;
    processor.sequenceState().store(snapshot);
    juce::AudioBuffer<float> buffer(2, 256);
    bool sawLegacy = false;
    uint16_t sequenceWord = 0;
    for (int block = 0; block < 24; ++block)
    {
        juce::MidiBuffer midi;
        if (block == 0)
            midi.addEvent(juce::MidiMessage::noteOn(1, 60, static_cast<juce::uint8>(100)), 0);
        processor.processBlock(buffer, midi);
        sequenceWord = static_cast<uint16_t>(processor.engineForTests().shruthiPart().voice().pitch_value());
        if (sequenceWord == 72 * 128)
            sawLegacy = true;
    }
    expect(sawLegacy, "LEGACY sequence notes ignore input tuning");
    std::printf("LEGACY under QUANTIZED input word=%u\n", sequenceWord);
}

void testArpeggiatorUsesInputTuning()
{
    SwaraXtAudioProcessor processor;
    processor.prepareToPlay(48000.0, 128);
    setInt(processor, swaraxt::IDs::seqMode, shruthi::SEQUENCER_MODE_ARP);
    setInt(processor, swaraxt::IDs::seqClockMode, 0);
    setInt(processor, swaraxt::IDs::seqTempo, 200);
    setInt(processor, swaraxt::IDs::arpOctaves, 1);
    setInt(processor, swaraxt::IDs::perfGlide, 0);
    setInt(processor, swaraxt::IDs::midiInputTuning, 1);
    expect(processor.loadBuiltinTuning(1), "19-EDO arp");
    const auto expected = expectedTuned(processor, 64);
    juce::AudioBuffer<float> buffer(2, 128);
    bool matched = false;
    uint16_t word = 0;
    for (int block = 0; block < 20; ++block)
    {
        juce::MidiBuffer midi;
        if (block == 0)
            midi.addEvent(juce::MidiMessage::noteOn(1, 64, static_cast<juce::uint8>(100)), 0);
        processor.processBlock(buffer, midi);
        word = static_cast<uint16_t>(processor.engineForTests().shruthiPart().voice().pitch_value());
        if (word == expected)
            matched = true;
    }
    expect(matched && expected != 64 * 128, "ARP notes use the input-tuning pitch of the held key");
    std::printf("ARP TUNED key 64 word=%u expected=%u hz=%.6f\n", word, expected, hzFromWord(word));
}

void testStateDefaultsOff()
{
    SwaraXtAudioProcessor processor;
    setInt(processor, swaraxt::IDs::midiInputTuning, 2);
    juce::MemoryBlock state;
    processor.getStateInformation(state);
    auto xml = juce::parseXML(juce::String::fromUTF8(static_cast<const char*>(state.getData()) + 8,
                                                     static_cast<int>(state.getSize()) - 9));
    expect(xml != nullptr, "state xml parses");
    if (xml == nullptr)
        return;
    auto tree = juce::ValueTree::fromXml(*xml);
    for (int index = tree.getNumChildren(); --index >= 0;)
    {
        const auto child = tree.getChild(index);
        if (child.hasType("PARAM") && child["id"].toString() == swaraxt::IDs::midiInputTuning)
            tree.removeChild(index, nullptr);
    }
    auto strippedXml = tree.createXml();
    juce::MemoryBlock stripped;
    {
        juce::MemoryOutputStream out(stripped, false);
        out.writeInt(0x21324356);
        out.writeInt(0);
        strippedXml->writeTo(out, juce::XmlElement::TextFormat().singleLine());
        out.writeByte(0);
    }
    static_cast<uint32_t*>(stripped.getData())[1] = juce::ByteOrder::swapIfBigEndian(
        static_cast<uint32_t>(stripped.getSize() - 9));
    processor.setStateInformation(stripped.getData(), static_cast<int>(stripped.getSize()));
    const int restored = static_cast<int>(std::lround(
        processor.getApvts().getRawParameterValue(swaraxt::IDs::midiInputTuning)->load()));
    expect(restored == 0, "missing input-tuning state restores OFF");
}

void testLayout()
{
    SwaraXtAudioProcessor processor;
    SwaraXtAudioProcessorEditor editor(processor);
    for (auto size : { swaraxt::ui::GuiSize::small, swaraxt::ui::GuiSize::medium, swaraxt::ui::GuiSize::large })
    {
        editor.setGuiSizeForTests(size);
        editor.resized();
        auto& panel = editor.mainPanelForTests();
        panel.reflow();
        auto* combo = &panel.inputTuneBoxForTests();
        auto* selector = combo->getParentComponent();
        const auto bounds = panel.getLocalArea(selector, selector->getLocalBounds());
        expect(panel.getLocalBounds().contains(bounds), "INPUT TUNE stays inside the editor");
        expect(bounds.getWidth() > 20 && bounds.getHeight() > 20, "INPUT TUNE has a visible hit area");
    }
    const float previous = juce::Desktop::getInstance().getGlobalScaleFactor();
    juce::Desktop::getInstance().setGlobalScaleFactor(1.5f);
    editor.setGuiSizeForTests(swaraxt::ui::GuiSize::medium);
    editor.resized();
    editor.mainPanelForTests().reflow();
    auto* combo = &editor.mainPanelForTests().inputTuneBoxForTests();
    const auto hidpi = editor.mainPanelForTests().getLocalArea(
        combo->getParentComponent(), combo->getParentComponent()->getLocalBounds());
    expect(editor.mainPanelForTests().getLocalBounds().contains(hidpi), "HiDPI INPUT TUNE stays inside");
    juce::Desktop::getInstance().setGlobalScaleFactor(previous);
    expect(editor.mainPanelForTests().inputTuneBoxForTests().getNumItems() == 3,
           "INPUT TUNE has OFF, TUNED and QUANTIZED");
}

void expectBendGate(const char* modeName, const BendResult& r)
{
    const double upCents = centsBetween(r.center, r.positive);
    const double halfCents = centsBetween(r.center, r.half);
    const double downCents = centsBetween(r.center, r.negative);
    const double returnCents = centsBetween(r.center, r.returned);
    std::printf("| %s | %.3f | %.3f | %.3f | %.3f |",
                modeName, r.center, r.positive, r.negative, r.returned);
    std::printf(" up=%.1fc (exp %.1f) half=%.1fc (exp %.1f) down=%.1fc (exp %.1f) ret=%.1fc |\n",
                upCents, kExpectedBendUpCents,
                halfCents, kExpectedBendHalfCents,
                downCents, kExpectedBendDownCents,
                returnCents);

    expect(r.center > 100.0 && r.center < 600.0, "center Hz in pitched range");
    expect(r.positive > r.center, "positive bend raises audible frequency");
    expect(r.negative < r.center, "negative bend lowers audible frequency");
    expect(r.half > r.center && r.half < r.positive, "intermediate bend is between center and max");
    expect(std::abs(upCents - kExpectedBendUpCents) < 25.0, "positive bend interval near matrix depth");
    expect(std::abs(halfCents - kExpectedBendHalfCents) < 25.0, "half bend interval near matrix depth");
    expect(std::abs(downCents - kExpectedBendDownCents) < 25.0, "negative bend interval near matrix depth");
    expect(std::abs(returnCents) < 15.0, "return to center restores audible pitch");
    expect(r.wordDuringBend == r.baseWord, "pitch bend does not replace the stored base pitch word");
}

void testAudiblePitchBendGate()
{
    std::printf("\n=== GATE 1 audible pitch bend ===\n");
    std::printf("| Input mode | Center Hz | Positive Hz | Negative Hz | Return Hz | Result |\n");

    {
        SwaraXtAudioProcessor processor;
        setInt(processor, swaraxt::IDs::midiInputTuning, 0);
        const auto r = measureAudibleBend(processor, 60);
        expectBendGate("OFF", r);
        expect(r.baseWord == 60 * 128, "OFF center uses original Tune word");
    }
    {
        SwaraXtAudioProcessor processor;
        setInt(processor, swaraxt::IDs::midiInputTuning, 1);
        expect(processor.loadBuiltinTuning(1), "19-EDO for TUNED bend");
        const auto r = measureAudibleBend(processor, 60);
        expectBendGate("TUNED", r);
        expect(r.baseWord == static_cast<int>(expectedTuned(processor, 60)),
               "TUNED bends around its SWT1 base word");
        expect(r.baseWord != 60 * 128, "TUNED 19-EDO base differs from 12-TET");
    }
    {
        SwaraXtAudioProcessor processor;
        setInt(processor, swaraxt::IDs::midiInputTuning, 2);
        expect(processor.loadBuiltinTuning(1), "19-EDO for QUANTIZED bend");
        const double source = 440.0 * std::pow(2.0, (60.0 - 69.0) / 12.0);
        const auto chosen = processor.quantizeActiveHz(source);
        const auto expected = swaraxt::ScalePitch::toVoicePitchFromHz(chosen.hz);
        const auto r = measureAudibleBend(processor, 60);
        expectBendGate("QUANTIZED", r);
        expect(r.baseWord == static_cast<int>(expected),
               "QUANTIZED bends around its quantized SWT1 base word");
    }
}

bool waitForPitchTarget(SwaraXtAudioProcessor& processor,
                        juce::AudioBuffer<float>& buffer,
                        int target,
                        int maxBlocks,
                        std::vector<int>& trajectory)
{
    for (int block = 0; block < maxBlocks; ++block)
    {
        juce::MidiBuffer idle;
        processor.processBlock(buffer, idle);
        const int word = processor.engineForTests().shruthiPart().voice().pitch_value();
        if (trajectory.empty() || trajectory.back() != word)
            trajectory.push_back(word);
        if (word == target)
            return true;
    }
    return false;
}

void testCompletePortamentoGate()
{
    std::printf("\n=== GATE 2 complete portamento ===\n");

    auto runGlide = [](int mode, const char* label) {
        SwaraXtAudioProcessor processor;
        prepareStableBendPatch(processor);
        setStepMode(processor);
        setInt(processor, swaraxt::IDs::midiInputTuning, mode);
        expect(processor.loadBuiltinTuning(1), "19-EDO glide");
        setInt(processor, swaraxt::IDs::perfGlide, 0);
        setInt(processor, swaraxt::IDs::perfLegato, 0);
        processor.prepareToPlay(48000.0, 64);
        juce::AudioBuffer<float> buffer(2, 64);

        const int noteA = 60;
        const int noteB = 67;
        uint16_t targetA = 0;
        uint16_t targetB = 0;
        if (mode == 0)
        {
            targetA = static_cast<uint16_t>(noteA * 128);
            targetB = static_cast<uint16_t>(noteB * 128);
        }
        else if (mode == 1)
        {
            targetA = expectedTuned(processor, noteA);
            targetB = expectedTuned(processor, noteB);
        }
        else
        {
            const double srcA = 440.0 * std::pow(2.0, (noteA - 69.0) / 12.0);
            const double srcB = 440.0 * std::pow(2.0, (noteB - 69.0) / 12.0);
            targetA = swaraxt::ScalePitch::toVoicePitchFromHz(processor.quantizeActiveHz(srcA).hz);
            targetB = swaraxt::ScalePitch::toVoicePitchFromHz(processor.quantizeActiveHz(srcB).hz);
        }

        {
            juce::MidiBuffer on;
            on.addEvent(juce::MidiMessage::noteOn(1, noteA, static_cast<juce::uint8>(100)), 0);
            processor.processBlock(buffer, on);
        }
        processIdle(processor, buffer, 8);
        const int start = processor.engineForTests().shruthiPart().voice().pitch_value();
        expect(start == static_cast<int>(targetA), "glide source is the tuned A pitch");
        setInt(processor, swaraxt::IDs::perfGlide, 70);
        processor.engineForTests().applyParameters();

        {
            juce::MidiBuffer on;
            on.addEvent(juce::MidiMessage::noteOn(1, noteB, static_cast<juce::uint8>(100)), 0);
            processor.processBlock(buffer, on);
        }
        std::vector<int> trajectory;
        const bool reached = waitForPitchTarget(processor, buffer, static_cast<int>(targetB), 4000, trajectory);
        const int finalWord = processor.engineForTests().shruthiPart().voice().pitch_value();
        int maxJump = 0;
        for (size_t i = 1; i < trajectory.size(); ++i)
            maxJump = std::max(maxJump, std::abs(trajectory[i] - trajectory[i - 1]));
        std::printf("%s glide start=%d target=%u final=%d steps=%zu maxJump=%d reached=%d\n",
                    label, start, targetB, finalWord, trajectory.size(), maxJump, reached ? 1 : 0);
        expect(reached && finalWord == static_cast<int>(targetB), "portamento reaches destination B");
        expect(trajectory.size() > 3, "portamento records an intermediate trajectory");
        expect(maxJump < std::abs(static_cast<int>(targetB) - start),
               "glide advances without jumping the full interval in one step");

        processIdle(processor, buffer, 32);
        expect(processor.engineForTests().shruthiPart().voice().pitch_value() == static_cast<int>(targetB),
               "destination remains stable after settling");

        {
            juce::MidiBuffer off;
            off.addEvent(juce::MidiMessage::noteOff(1, noteB), 0);
            processor.processBlock(buffer, off);
        }
        std::vector<int> retTrajectory;
        const bool returned = waitForPitchTarget(processor, buffer, static_cast<int>(targetA), 4000, retTrajectory);
        expect(returned, "release of B returns to held A");
        expect(processor.engineForTests().shruthiPart().voice().pitch_value() == static_cast<int>(targetA),
               "held A pitch restored after glide return");
        {
            juce::MidiBuffer off;
            off.addEvent(juce::MidiMessage::noteOff(1, noteA), 0);
            off.addEvent(juce::MidiMessage::allNotesOff(1), 0);
            processor.processBlock(buffer, off);
        }
    };

    runGlide(1, "TUNED");
    runGlide(2, "QUANTIZED");

    // Held-note return after active tuning change (19-EDO capture, switch to 31-EDO).
    {
        SwaraXtAudioProcessor processor;
        prepareStableBendPatch(processor);
        setStepMode(processor);
        setInt(processor, swaraxt::IDs::midiInputTuning, 1);
        expect(processor.loadBuiltinTuning(1), "19-EDO hold");
        setInt(processor, swaraxt::IDs::perfGlide, 0);
        processor.prepareToPlay(48000.0, 64);
        juce::AudioBuffer<float> buffer(2, 64);
        const auto capturedC = expectedTuned(processor, 60);
        {
            juce::MidiBuffer on;
            on.addEvent(juce::MidiMessage::noteOn(1, 60, static_cast<juce::uint8>(100)), 0);
            processor.processBlock(buffer, on);
        }
        setInt(processor, swaraxt::IDs::perfGlide, 40);
        processor.engineForTests().applyParameters();
        {
            juce::MidiBuffer on;
            on.addEvent(juce::MidiMessage::noteOn(1, 64, static_cast<juce::uint8>(100)), 0);
            processor.processBlock(buffer, on);
        }
        std::vector<int> traj;
        waitForPitchTarget(processor, buffer, static_cast<int>(expectedTuned(processor, 64)), 4000, traj);
        expect(processor.loadBuiltinTuning(3), "switch to 31-EDO while held");
        {
            juce::MidiBuffer off;
            off.addEvent(juce::MidiMessage::noteOff(1, 64), 0);
            processor.processBlock(buffer, off);
        }
        traj.clear();
        const bool restored = waitForPitchTarget(processor, buffer, static_cast<int>(capturedC), 4000, traj);
        expect(restored, "return to C restores originally captured 19-EDO pitch");
        expect(processor.engineForTests().shruthiPart().voice().pitch_value() == static_cast<int>(capturedC),
               "held C ignores live 31-EDO until retrigger");
        {
            juce::MidiBuffer off;
            off.addEvent(juce::MidiMessage::noteOff(1, 60), 0);
            processor.processBlock(buffer, off);
        }
        setInt(processor, swaraxt::IDs::perfGlide, 0);
        processor.engineForTests().applyParameters();
        processIdle(processor, buffer, 4);
        {
            juce::MidiBuffer on;
            on.addEvent(juce::MidiMessage::noteOn(1, 60, static_cast<juce::uint8>(100)), 0);
            processor.processBlock(buffer, on);
        }
        const auto fresh = expectedTuned(processor, 60);
        expect(processor.engineForTests().shruthiPart().voice().pitch_value() == static_cast<int>(fresh),
               "retrigger C uses the current 31-EDO tuning");
        expect(fresh != capturedC, "31-EDO C differs from captured 19-EDO C");
    }

    // Mode changes while held: pitch remains until a new attack.
    {
        SwaraXtAudioProcessor processor;
        prepareStableBendPatch(processor);
        setStepMode(processor);
        setInt(processor, swaraxt::IDs::midiInputTuning, 0);
        processor.prepareToPlay(48000.0, 64);
        juce::AudioBuffer<float> buffer(2, 64);
        {
            juce::MidiBuffer on;
            on.addEvent(juce::MidiMessage::noteOn(1, 60, static_cast<juce::uint8>(100)), 0);
            processor.processBlock(buffer, on);
        }
        const int offWord = processor.engineForTests().shruthiPart().voice().pitch_value();
        setInt(processor, swaraxt::IDs::midiInputTuning, 1);
        expect(processor.loadBuiltinTuning(1), "OFF->TUNED while held");
        processIdle(processor, buffer, 4);
        expect(processor.engineForTests().shruthiPart().voice().pitch_value() == offWord,
               "OFF->TUNED keeps sounding pitch until release");
        setInt(processor, swaraxt::IDs::midiInputTuning, 2);
        processIdle(processor, buffer, 4);
        expect(processor.engineForTests().shruthiPart().voice().pitch_value() == offWord,
               "TUNED->QUANTIZED keeps sounding pitch until release");
        setInt(processor, swaraxt::IDs::midiInputTuning, 0);
        processIdle(processor, buffer, 4);
        expect(processor.engineForTests().shruthiPart().voice().pitch_value() == offWord,
               "QUANTIZED->OFF keeps sounding pitch until release");
        {
            juce::MidiBuffer off;
            off.addEvent(juce::MidiMessage::noteOff(1, 60), 0);
            processor.processBlock(buffer, off);
        }
    }

    // Sustain + legato + bend during glide.
    {
        SwaraXtAudioProcessor processor;
        prepareStableBendPatch(processor);
        setStepMode(processor);
        setInt(processor, swaraxt::IDs::midiInputTuning, 1);
        expect(processor.loadBuiltinTuning(1), "19-EDO sustain/legato");
        setInt(processor, swaraxt::IDs::perfGlide, 55);
        setInt(processor, swaraxt::IDs::perfLegato, 1);
        processor.prepareToPlay(48000.0, 64);
        juce::AudioBuffer<float> buffer(2, 64);
        const auto c = expectedTuned(processor, 60);
        const auto e = expectedTuned(processor, 64);
        {
            juce::MidiBuffer on;
            on.addEvent(juce::MidiMessage::noteOn(1, 60, static_cast<juce::uint8>(100)), 0);
            processor.processBlock(buffer, on);
        }
        {
            juce::MidiBuffer on;
            on.addEvent(juce::MidiMessage::controllerEvent(1, 64, 127), 0);
            on.addEvent(juce::MidiMessage::noteOn(1, 64, static_cast<juce::uint8>(100)), 0);
            processor.processBlock(buffer, on);
        }
        sendBend(processor, buffer, 14000);
        std::vector<int> traj;
        waitForPitchTarget(processor, buffer, static_cast<int>(e), 4000, traj);
        const int midWord = processor.engineForTests().shruthiPart().voice().pitch_value();
        expect(midWord == static_cast<int>(e), "legato glide still reaches E target");
        sendBend(processor, buffer, 8192);
        {
            juce::MidiBuffer off;
            off.addEvent(juce::MidiMessage::noteOff(1, 64), 0);
            processor.processBlock(buffer, off);
        }
        // Sustain pedal still holding both notes via latch; release pedal after C note-off flag.
        {
            juce::MidiBuffer off;
            off.addEvent(juce::MidiMessage::noteOff(1, 60), 0);
            processor.processBlock(buffer, off);
        }
        expect(processor.engineForTests().shruthiPart().voice().pitch_value() != 0,
               "sustain keeps a sounding voice after deferred note-offs");
        {
            juce::MidiBuffer pedal;
            pedal.addEvent(juce::MidiMessage::controllerEvent(1, 64, 0), 0);
            processor.processBlock(buffer, pedal);
        }
        processIdle(processor, buffer, 8);
        std::printf("sustain/legato path completed (c=%u e=%u)\n", c, e);
    }
}

bool loadPartialMap(SwaraXtAudioProcessor& processor)
{
    const char* scale =
        "! partial\n"
        "partial map\n"
        "12\n"
        "100.0\n200.0\n300.0\n400.0\n500.0\n600.0\n"
        "700.0\n800.0\n900.0\n1000.0\n1100.0\n1200.0\n";
    const char* kbm =
        "2\n60\n127\n60\n60\n261.6255653006\n12\n0\nx\n";
    swaraxt::tuning::ParseError error;
    swaraxt::tuning::TuningSnapshot snapshot;
    if (! swaraxt::tuning::parseScalaAndKeyboard(scale, kbm, snapshot, error))
        return false;
    std::vector<uint8_t> bytes;
    if (! swaraxt::tuning::writeTuningTable(snapshot, bytes, error))
        return false;
    return processor.loadTuningBytes(bytes.data(), bytes.size());
}

void testUnmappedKeySafetyGate()
{
    std::printf("\n=== GATE 3 unmapped key safety ===\n");

    // A/B: mapped held + unmapped press; unmapped first then mapped.
    {
        SwaraXtAudioProcessor processor;
        prepareStableBendPatch(processor);
        setStepMode(processor);
        setInt(processor, swaraxt::IDs::midiInputTuning, 1);
        expect(loadPartialMap(processor), "partial map");
        processor.prepareToPlay(48000.0, 64);
        juce::AudioBuffer<float> buffer(2, 64);
        const auto mapped = expectedTuned(processor, 60);
        expect(expectedTuned(processor, 61) == 0, "61 unmapped");

        {
            juce::MidiBuffer on;
            on.addEvent(juce::MidiMessage::noteOn(1, 60, static_cast<juce::uint8>(100)), 0);
            processor.processBlock(buffer, on);
        }
        expect(processor.engineForTests().shruthiPart().voice().pitch_value() == static_cast<int>(mapped),
               "mapped sounds");
        {
            juce::MidiBuffer on;
            on.addEvent(juce::MidiMessage::noteOn(1, 61, static_cast<juce::uint8>(100)), 0);
            processor.processBlock(buffer, on);
        }
        expect(processor.engineForTests().shruthiPart().voice().pitch_value() == static_cast<int>(mapped),
               "unmapped does not steal sounding pitch");
        expect(processor.engineForTests().shruthiPart().num_notes() >= 2,
               "unmapped remains in pressed-key list");
        {
            juce::MidiBuffer off;
            off.addEvent(juce::MidiMessage::noteOff(1, 61), 0);
            processor.processBlock(buffer, off);
        }
        expect(processor.engineForTests().shruthiPart().voice().pitch_value() == static_cast<int>(mapped),
               "release unmapped keeps mapped ownership");
        {
            juce::MidiBuffer off;
            off.addEvent(juce::MidiMessage::noteOff(1, 60), 0);
            processor.processBlock(buffer, off);
        }
        processIdle(processor, buffer, 4);

        {
            juce::MidiBuffer on;
            on.addEvent(juce::MidiMessage::noteOn(1, 61, static_cast<juce::uint8>(100)), 0);
            processor.processBlock(buffer, on);
        }
        expect(processor.engineForTests().shruthiPart().num_notes() == 1,
               "unmapped-first still enters pressed keys");
        processIdle(processor, buffer, 8);
        expect(processor.engineForTests().shruthiPart().voice().amplitude_envelope_dead(),
               "unmapped-first leaves the voice silent");
        {
            juce::MidiBuffer on;
            on.addEvent(juce::MidiMessage::noteOn(1, 60, static_cast<juce::uint8>(100)), 0);
            processor.processBlock(buffer, on);
        }
        expect(processor.engineForTests().shruthiPart().voice().pitch_value() == static_cast<int>(mapped),
               "mapped after unmapped-first sounds");
        {
            juce::MidiBuffer off;
            off.addEvent(juce::MidiMessage::noteOff(1, 60), 0);
            processor.processBlock(buffer, off);
        }
        processIdle(processor, buffer, 8);
        // Unmapped never entered the mono allocator, so release must not re-open the envelope.
        expect(processor.engineForTests().shruthiPart().voice().amplitude_envelope_dead(),
               "no unintended fallback to unmapped after mapped release");
        expect(processor.engineForTests().shruthiPart().has_held_notes(),
               "unmapped key can remain pressed without sounding");
        {
            juce::MidiBuffer off;
            off.addEvent(juce::MidiMessage::noteOff(1, 61), 0);
            off.addEvent(juce::MidiMessage::allNotesOff(1), 0);
            processor.processBlock(buffer, off);
        }
    }

    // C: sustain with mixed mapped/unmapped.
    {
        SwaraXtAudioProcessor processor;
        prepareStableBendPatch(processor);
        setStepMode(processor);
        setInt(processor, swaraxt::IDs::midiInputTuning, 1);
        expect(loadPartialMap(processor), "partial map sustain");
        processor.prepareToPlay(48000.0, 64);
        juce::AudioBuffer<float> buffer(2, 64);
        const auto mapped = expectedTuned(processor, 60);
        {
            juce::MidiBuffer midi;
            midi.addEvent(juce::MidiMessage::controllerEvent(1, 64, 127), 0);
            midi.addEvent(juce::MidiMessage::noteOn(1, 60, static_cast<juce::uint8>(100)), 0);
            midi.addEvent(juce::MidiMessage::noteOn(1, 61, static_cast<juce::uint8>(90)), 0);
            processor.processBlock(buffer, midi);
        }
        expect(processor.engineForTests().shruthiPart().voice().pitch_value() == static_cast<int>(mapped),
               "sustain mixed: mapped sounds");
        {
            juce::MidiBuffer off;
            off.addEvent(juce::MidiMessage::noteOff(1, 60), 0);
            off.addEvent(juce::MidiMessage::noteOff(1, 61), 0);
            processor.processBlock(buffer, off);
        }
        expect(processor.engineForTests().shruthiPart().voice().pitch_value() == static_cast<int>(mapped),
               "deferred offs keep sounding under sustain");
        {
            juce::MidiBuffer pedal;
            pedal.addEvent(juce::MidiMessage::controllerEvent(1, 64, 0), 0);
            processor.processBlock(buffer, pedal);
        }
        processIdle(processor, buffer, 8);
        expect(processor.engineForTests().shruthiPart().num_notes() == 0,
               "pedal release clears latched pressed keys");
    }

    // D: arpeggiator with mixed mapped/unmapped — unmapped steps stay silent, rhythm continues.
    {
        SwaraXtAudioProcessor processor;
        prepareStableBendPatch(processor);
        setInt(processor, swaraxt::IDs::seqMode, shruthi::SEQUENCER_MODE_ARP);
        setInt(processor, swaraxt::IDs::seqClockMode, 0);
        setInt(processor, swaraxt::IDs::seqTempo, 240);
        setInt(processor, swaraxt::IDs::arpOctaves, 1);
        setInt(processor, swaraxt::IDs::perfGlide, 0);
        setInt(processor, swaraxt::IDs::midiInputTuning, 1);
        expect(loadPartialMap(processor), "partial map arp");
        const auto mapped = expectedTuned(processor, 60);
        processor.prepareToPlay(48000.0, 64);
        juce::AudioBuffer<float> buffer(2, 64);
        bool sawMapped = false;
        {
            juce::MidiBuffer on;
            on.addEvent(juce::MidiMessage::noteOn(1, 60, static_cast<juce::uint8>(100)), 0);
            on.addEvent(juce::MidiMessage::noteOn(1, 61, static_cast<juce::uint8>(100)), 0);
            processor.processBlock(buffer, on);
        }
        for (int block = 0; block < 64; ++block)
        {
            juce::MidiBuffer idle;
            processor.processBlock(buffer, idle);
            const int word = processor.engineForTests().shruthiPart().voice().pitch_value();
            if (word == static_cast<int>(mapped))
                sawMapped = true;
        }
        expect(sawMapped, "arp still sounds the mapped key");
        expect(processor.engineForTests().shruthiPart().num_notes() == 2,
               "arp keeps both mapped and unmapped pressed keys");
        std::printf("arp mixed mapped/unmapped: pressed=%u sawMapped=%d\n",
                    processor.engineForTests().shruthiPart().num_notes(),
                    sawMapped ? 1 : 0);
        juce::MidiBuffer panic;
        panic.addEvent(juce::MidiMessage::allNotesOff(1), 0);
        processor.processBlock(buffer, panic);
    }

    // E: LEGACY sequencer transposition still uses pressed unmapped key chromatically.
    {
        SwaraXtAudioProcessor processor;
        prepareStableBendPatch(processor);
        setInt(processor, swaraxt::IDs::seqMode, shruthi::SEQUENCER_MODE_SEQ);
        setInt(processor, swaraxt::IDs::seqClockMode, 0);
        setInt(processor, swaraxt::IDs::seqTempo, 200);
        setInt(processor, swaraxt::IDs::seqGate, 100);
        setInt(processor, swaraxt::IDs::perfGlide, 0);
        setInt(processor, swaraxt::IDs::midiInputTuning, 1);
        expect(loadPartialMap(processor), "partial map seq transpose");
        processor.sequenceState().setPitchMode(0);
        for (int step = 0; step < 16; ++step)
            processor.sequenceState().setStep(step, static_cast<uint8_t>(0x80 | 60), 0x70);
        auto snap = processor.sequenceState().snapshot();
        snap.length = 1;
        snap.pitchMode = 0;
        processor.sequenceState().store(snap);
        processor.prepareToPlay(48000.0, 128);
        juce::AudioBuffer<float> buffer(2, 128);
        bool sawTransposed = false;
        for (int block = 0; block < 32; ++block)
        {
            juce::MidiBuffer midi;
            if (block == 0)
                midi.addEvent(juce::MidiMessage::noteOn(1, 61, static_cast<juce::uint8>(100)), 0);
            processor.processBlock(buffer, midi);
            // Unmapped 61 is silent as a voice attack, but LEGACY transposition is note-60.
            if (processor.engineForTests().shruthiPart().voice().pitch_value() == 61 * 128)
                sawTransposed = true;
        }
        expect(sawTransposed, "unmapped pressed key still chromatically transposes LEGACY seq");
        std::printf("LEGACY transpose via unmapped key 61 -> word %d\n",
                    processor.engineForTests().shruthiPart().voice().pitch_value());
    }

    // F: tuning change mapped<->unmapped while held.
    {
        SwaraXtAudioProcessor processor;
        prepareStableBendPatch(processor);
        setStepMode(processor);
        setInt(processor, swaraxt::IDs::midiInputTuning, 1);
        expect(processor.loadBuiltinTuning(1), "19-EDO then partial");
        processor.prepareToPlay(48000.0, 64);
        juce::AudioBuffer<float> buffer(2, 64);
        const auto held = expectedTuned(processor, 61);
        {
            juce::MidiBuffer on;
            on.addEvent(juce::MidiMessage::noteOn(1, 61, static_cast<juce::uint8>(100)), 0);
            processor.processBlock(buffer, on);
        }
        expect(processor.engineForTests().shruthiPart().voice().pitch_value() == static_cast<int>(held),
               "61 mapped under 19-EDO");
        expect(loadPartialMap(processor), "switch to partial while held");
        processIdle(processor, buffer, 4);
        expect(processor.engineForTests().shruthiPart().voice().pitch_value() == static_cast<int>(held),
               "held note keeps captured pitch after becoming unmapped in new table");
        {
            juce::MidiBuffer off;
            off.addEvent(juce::MidiMessage::noteOff(1, 61), 0);
            processor.processBlock(buffer, off);
        }
    }

    // G: panic / reset controllers.
    {
        SwaraXtAudioProcessor processor;
        prepareStableBendPatch(processor);
        setStepMode(processor);
        setInt(processor, swaraxt::IDs::midiInputTuning, 1);
        expect(loadPartialMap(processor), "partial map panic");
        processor.prepareToPlay(48000.0, 64);
        juce::AudioBuffer<float> buffer(2, 64);
        {
            juce::MidiBuffer on;
            on.addEvent(juce::MidiMessage::noteOn(1, 60, static_cast<juce::uint8>(100)), 0);
            on.addEvent(juce::MidiMessage::noteOn(1, 61, static_cast<juce::uint8>(100)), 0);
            processor.processBlock(buffer, on);
        }
        {
            juce::MidiBuffer panic;
            panic.addEvent(juce::MidiMessage::controllerEvent(1, 123, 0), 0); // All Notes Off
            processor.processBlock(buffer, panic);
        }
        processIdle(processor, buffer, 4);
        expect(processor.engineForTests().shruthiPart().num_notes() == 0,
               "All Notes Off clears pressed keys");
        {
            juce::MidiBuffer on;
            on.addEvent(juce::MidiMessage::noteOn(1, 60, static_cast<juce::uint8>(100)), 0);
            processor.processBlock(buffer, on);
        }
        {
            juce::MidiBuffer panic;
            panic.addEvent(juce::MidiMessage::controllerEvent(1, 120, 0), 0); // All Sound Off
            processor.processBlock(buffer, panic);
        }
        {
            juce::MidiBuffer resetCc;
            resetCc.addEvent(juce::MidiMessage::controllerEvent(1, 121, 0), 0); // Reset All Controllers
            processor.processBlock(buffer, resetCc);
        }
        processor.engineForTests().reset();
        processIdle(processor, buffer, 2);
        expect(processor.engineForTests().shruthiPart().num_notes() == 0,
               "engine reset leaves no stuck pressed keys");
        std::printf("panic/reset path completed\n");
    }
}

}  // namespace

int main()
{
    juce::ScopedJuceInitialiser_GUI juce;
    testOffIsOriginalTune();
    testTunedMapping();
    testQuantizedInput();
    testUnmappedAndAllocation();
    testSustainBendAndPortamento();
    testHeldPitchAndSequencerIsolation();
    testLegacySequenceIgnoresInputTuning();
    testArpeggiatorUsesInputTuning();
    testStateDefaultsOff();
    testLayout();
    testAudiblePitchBendGate();
    testCompletePortamentoGate();
    testUnmappedKeySafetyGate();
    if (failures == 0)
    {
        std::printf("midi input tuning tests: PASSED\n");
        return 0;
    }
    std::printf("midi input tuning tests: FAILED (%d)\n", failures);
    return 1;
}
