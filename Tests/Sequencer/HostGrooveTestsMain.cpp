// Copyright 2026 MontroneDSP.
// SPDX-License-Identifier: GPL-3.0-or-later
// Independent Shruthi groove oracle and HOST alignment regression.
// The expected PPQ values are computed from the firmware LUT and fixed-point
// rule. They do not call SwaraXtEngine::hostTickPpq.

#include <JuceHeader.h>

#include <cmath>
#include <cstdint>
#include <cstdio>
#include <vector>

#include "Engine/SequenceState.h"
#include "Plugin/PluginProcessor.h"
#include "Plugin/SwaraXtParameterLayout.h"
#include "shruthi/part.h"

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

constexpr int kGrooveLut[6][16] = {
    { 127, 127, -127, -127, 127, 127, -127, -127, 127, 127, -127, -127, 127, 127, -127, -127 },
    { 127, -127, 127, -127, 127, -127, 127, -127, 127, -127, 127, -127, 127, -127, 127, -127 },
    { -63, -63, 127, 0, -127, 0, 0, 88, 0, 0, 88, -50, -88, 0, 88, 0 },
    { 19, 44, 93, -4, 32, -53, -90, -127, 117, 32, -102, -53, 105, -53, 93, -53 },
    { 88, -101, 107, -95, 88, -88, 50, -38, 65, -88, 101, -95, 101, -127, 63, -31 },
    { 70, -84, 84, -112, 84, -98, 112, -98, 54, -70, 127, -84, 127, -112, 84, -84 }
};

int firmwareSwing(int groove, int index, int amount)
{
    const int product = kGrooveLut[groove][index] * amount;
    if (product >= 0)
        return product / 512;
    return -((-product + 511) / 512);
}

int scaleNum(int groove, int index, int amount)
{
    return 128 + firmwareSwing(groove, index, amount);
}

int stepIndex(uint64_t step, int length)
{
    if (step == 0)
        return 15;
    return static_cast<int>((step - 1) % static_cast<uint64_t>(length));
}

int64_t scaleSum(uint64_t stepCount, int length, int groove, int amount)
{
    if (stepCount == 0)
        return 0;
    int64_t sum = scaleNum(groove, 15, amount);
    const uint64_t periodic = stepCount - 1;
    const uint64_t cycles = periodic / static_cast<uint64_t>(length);
    const int remainder = static_cast<int>(periodic % static_cast<uint64_t>(length));
    int64_t cycle = 0;
    int prefix = 0;
    for (int index = 0; index < length; ++index)
    {
        const int scale = scaleNum(groove, index, amount);
        cycle += scale;
        if (index < remainder)
            prefix += scale;
    }
    return sum + static_cast<int64_t>(cycles) * cycle + prefix;
}

double oraclePpq(uint64_t tick, uint64_t stepTicks, int length, int groove, int amount)
{
    const uint64_t step = tick / stepTicks;
    const uint64_t position = tick % stepTicks;
    const int64_t units = scaleSum(step, length, groove, amount) * static_cast<int64_t>(stepTicks)
        + static_cast<int64_t>(position) * scaleNum(groove, stepIndex(step, length), amount);
    return static_cast<double>(units) / 3072.0;
}

// The pre-correction HOST scheduler. It ignores the groove template.
double legacyHostPpq(uint64_t tick, uint64_t stepTicks, int amount)
{
    const uint64_t pairTicks = stepTicks * 2;
    const uint64_t pair = tick / pairTicks;
    const uint64_t position = tick % pairTicks;
    const double stepBeats = static_cast<double>(stepTicks) / 24.0;
    const double swing = static_cast<double>(amount) / 127.0 * 0.45;
    const double pairStart = static_cast<double>(pair) * stepBeats * 2.0;
    if (position < stepTicks)
        return pairStart + static_cast<double>(position) / 24.0 * (1.0 + swing);
    return pairStart + stepBeats * (1.0 + swing)
        + static_cast<double>(position - stepTicks) / 24.0 * (1.0 - swing);
}

uint8_t legacyAlignStep(uint64_t ticks, uint8_t duration)
{
    if (ticks == 0)
        return 0xff;
    return static_cast<uint8_t>(((ticks - 1) / duration) & 0x0f);
}

void setInt(SwaraXtAudioProcessor& processor, const char* id, int value)
{
    auto* parameter = processor.getApvts().getParameter(id);
    expect(parameter != nullptr, id);
    if (parameter != nullptr)
        parameter->setValueNotifyingHost(parameter->convertTo0to1(static_cast<float>(value)));
}

class TestPlayHead : public juce::AudioPlayHead {
 public:
    juce::Optional<juce::AudioPlayHead::PositionInfo> getPosition() const override
    {
        juce::AudioPlayHead::PositionInfo position;
        position.setBpm(bpm);
        if (providePpq)
            position.setPpqPosition(ppq);
        position.setIsPlaying(playing);
        return position;
    }

    double bpm = 120.0;
    double ppq = 0.0;
    bool playing = true;
    bool providePpq = true;
};

void processBlock(SwaraXtAudioProcessor& processor, int samples, juce::MidiBuffer* midi = nullptr)
{
    juce::AudioBuffer<float> buffer(2, samples);
    buffer.clear();
    juce::MidiBuffer local;
    processor.processBlock(buffer, midi != nullptr ? *midi : local);
}

void testLegacyTemplateCollision()
{
    constexpr uint64_t kStepTicks = 6;
    const double swing = legacyHostPpq(kStepTicks * 2 + 3, kStepTicks, 127);
    const double shuffle = legacyHostPpq(kStepTicks * 2 + 3, kStepTicks, 127);
    expect(swing == shuffle, "legacy HOST swing ignores the groove template");
    const double oracleSwing = oraclePpq(kStepTicks * 2 + 3, kStepTicks, 16, 0, 127);
    const double oracleShuffle = oraclePpq(kStepTicks * 2 + 3, kStepTicks, 16, 1, 127);
    expect(std::abs(oracleSwing - oracleShuffle) > 0.01,
           "firmware swing and shuffle differ where their LUT rows differ");
    expect(std::abs(swing - oracleShuffle) > 0.01,
           "legacy HOST timing is not the shuffle profile");
}

void testHostMatchesOracle(SwaraXtAudioProcessor& processor)
{
    setInt(processor, swaraxt::IDs::seqClockMode, 1);
    setInt(processor, swaraxt::IDs::seqMode, shruthi::SEQUENCER_MODE_SEQ);
    processBlock(processor, 64);

    auto& part = processor.engineForTests().shruthiPart();
    auto* settings = part.mutable_sequencer_settings();
    settings->arp_clock_division = 7;
    const uint64_t stepTicks = part.clock_ticks_per_step();
    expect(stepTicks == 6, "division 7 is six MIDI clocks per step");

    const int amounts[] = { 0, 1, 16, 32, 63, 64, 96, 126, 127 };
    for (int groove = 0; groove < 6; ++groove)
    {
        settings->seq_groove_template = static_cast<uint8_t>(groove);
        for (int length = 1; length <= 16; ++length)
        {
            settings->pattern_size = static_cast<uint8_t>(length);
            for (int amount : amounts)
            {
                setInt(processor, swaraxt::IDs::seqSwing, amount);
                processBlock(processor, 32);
                settings->seq_groove_template = static_cast<uint8_t>(groove);
                settings->pattern_size = static_cast<uint8_t>(length);
                settings->arp_clock_division = 7;
                double previous = -1.0;
                for (uint64_t tick = 0; tick < 48; ++tick)
                {
                    const double expected = oraclePpq(tick, stepTicks, length, groove, amount);
                    const double actual = processor.engineForTests().hostTickPpqForTests(tick);
                    if (std::abs(actual - expected) > 1.0e-9)
                    {
                        std::printf("FAIL: groove %d amount %d length %d tick %llu expected %.8f actual %.8f\n",
                                    groove, amount, length,
                                    static_cast<unsigned long long>(tick), expected, actual);
                        ++failures;
                        return;
                    }
                    if (tick > 0 && !(actual > previous))
                    {
                        std::printf("FAIL: groove timing went backwards at tick %llu\n",
                                    static_cast<unsigned long long>(tick));
                        ++failures;
                        return;
                    }
                    previous = actual;
                    if (amount == 0 && std::abs(actual - static_cast<double>(tick) / 24.0) > 1.0e-12)
                    {
                        std::printf("FAIL: amount 0 is not straight time at tick %llu\n",
                                    static_cast<unsigned long long>(tick));
                        ++failures;
                        return;
                    }
                }
            }
        }
    }

    settings->pattern_size = 16;
    settings->seq_groove_template = 5;
    setInt(processor, swaraxt::IDs::seqSwing, 127);
    processBlock(processor, 32);
    settings->pattern_size = 16;
    settings->seq_groove_template = 5;
    settings->arp_clock_division = 7;
    const uint64_t lateTick = 1000003;
    const double lateExpected = oraclePpq(lateTick, stepTicks, 16, 5, 127);
    const double lateActual = processor.engineForTests().hostTickPpqForTests(lateTick);
    expect(std::abs(lateActual - lateExpected) < 1.0e-6, "long monkey timeline matches the oracle");
    const double legacy = legacyHostPpq(3, stepTicks, 127);
    expect(std::abs(processor.engineForTests().hostTickPpqForTests(3) - legacy) > 0.001,
           "monkey no longer follows the generic swing curve");
}

void testAlignment(SwaraXtAudioProcessor& processor)
{
    auto& part = processor.engineForTests().shruthiPart();
    auto* settings = part.mutable_sequencer_settings();
    settings->arp_clock_division = 7;
    const uint8_t duration = part.clock_ticks_per_step();
    const uint8_t modes[] = { shruthi::SEQUENCER_MODE_SEQ, shruthi::SEQUENCER_MODE_ARP };
    for (uint8_t mode : modes)
    {
        settings->seq_mode = mode;
        settings->arp_pattern = 0;
        for (int length = 1; length <= 16; ++length)
        {
            settings->pattern_size = static_cast<uint8_t>(length);
            settings->pattern_rotation = 3;
            const uint64_t probes[] = {
                0, 1, duration, static_cast<uint64_t>(duration) * static_cast<uint64_t>(length),
                static_cast<uint64_t>(duration) * static_cast<uint64_t>(length) + 1,
                static_cast<uint64_t>(duration) * (static_cast<uint64_t>(length) * 20 + 4) + 3
            };
            for (uint64_t count : probes)
            {
                part.Start(false);
                for (uint64_t tick = 0; tick < count; ++tick)
                    part.Clock(false);
                const uint8_t playedStep = part.step();
                const uint8_t playedPrescaler = part.prescaler_for_tests();
                part.AlignExternalClock(count);
                if (part.step() != playedStep || part.prescaler_for_tests() != playedPrescaler)
                {
                    std::printf("FAIL: align length %d mode %u ticks %llu step %u/%u prescaler %u/%u\n",
                                length, static_cast<unsigned>(mode),
                                static_cast<unsigned long long>(count),
                                part.step(), playedStep,
                                part.prescaler_for_tests(), playedPrescaler);
                    ++failures;
                    return;
                }
            }
        }
    }

    settings->pattern_size = 5;
    settings->seq_mode = shruthi::SEQUENCER_MODE_SEQ;
    part.Start(false);
    const uint64_t barTicks = static_cast<uint64_t>(duration) * 5 * 4;
    for (uint64_t tick = 0; tick < barTicks; ++tick)
        part.Clock(false);
    const uint8_t continuous = part.step();
    expect(legacyAlignStep(barTicks, duration) != continuous,
           "the 16-step mask disagrees with a 5-step pattern");
    part.AlignExternalClock(barTicks);
    expect(part.step() == continuous, "seek into a 5-step pattern matches continuous playback");
    expect(part.prescaler_for_tests() == 0, "bar boundary prescaler is zero");

    settings->pattern_size = 16;
    part.Start(false);
    const uint64_t sixteen = static_cast<uint64_t>(duration) * 18 + 2;
    for (uint64_t tick = 0; tick < sixteen; ++tick)
        part.Clock(false);
    const uint8_t sixteenStep = part.step();
    part.AlignExternalClock(sixteen);
    expect(part.step() == sixteenStep, "16-step alignment matches continuous playback");
    expect(part.step() == legacyAlignStep(sixteen, duration),
           "16-step alignment keeps the original mask");
    part.AlignExternalClock(0);
    expect(part.step() == 0xff && part.prescaler_for_tests() == 0,
           "zero ticks restore the pre-clock sentinel");
}

void testBoundedTransport(SwaraXtAudioProcessor& processor)
{
    TestPlayHead playHead;
    processor.setPlayHead(&playHead);
    setInt(processor, swaraxt::IDs::seqClockMode, 1);
    setInt(processor, swaraxt::IDs::seqMode, shruthi::SEQUENCER_MODE_ARP);
    setInt(processor, swaraxt::IDs::seqSwing, 127);
    auto snapshot = processor.sequenceState().snapshot();
    snapshot.grooveTemplate = 5;
    snapshot.length = 5;
    processor.sequenceState().store(snapshot);

    playHead.ppq = 4294967296.0;
    playHead.bpm = 400.0;
    const auto before = processor.engineForTests().hostClockEventCountForTests();
    processBlock(processor, 8);
    const auto extremeTicks = processor.engineForTests().hostClockEventCountForTests() - before;
    expect(extremeTicks <= 2, "maximum PPQ schedules a bounded number of clocks");

    playHead.ppq = 0.0;
    playHead.bpm = 20.0;
    juce::MidiBuffer note;
    note.addEvent(juce::MidiMessage::noteOn(1, 60, static_cast<juce::uint8>(100)), 0);
    processBlock(processor, 2400, &note);
    expect(processor.engineForTests().shruthiPart().generated_note_count_for_tests() > 0,
           "host playback owns at least one generated note");

    playHead.playing = false;
    processBlock(processor, 128);
    expect(! processor.engineForTests().shruthiPart().running(), "stop ends host sequencer playback");
    expect(processor.engineForTests().shruthiPart().generated_note_count_for_tests() == 0,
           "stop releases every generated note");

    playHead.playing = true;
    playHead.ppq = 20.0 * 4.0;
    playHead.bpm = 120.0;
    processBlock(processor, 256, &note);
    playHead.ppq = 1.5;
    processBlock(processor, 256);
    playHead.playing = false;
    processBlock(processor, 128);
    expect(processor.engineForTests().shruthiPart().generated_note_count_for_tests() == 0,
           "seek and stop leave no generated notes");
}

}  // namespace

int main()
{
    testLegacyTemplateCollision();
    SwaraXtAudioProcessor processor;
    processor.prepareToPlay(48000.0, 256);
    testHostMatchesOracle(processor);
    testAlignment(processor);
    testBoundedTransport(processor);
    processor.releaseResources();
    if (failures != 0)
    {
        std::printf("%d HOST groove checks failed\n", failures);
        return 1;
    }
    std::printf("HOST groove checks passed\n");
    return 0;
}
