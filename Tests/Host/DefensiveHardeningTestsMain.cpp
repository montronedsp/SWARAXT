// Copyright 2026 MontroneDSP.
// SPDX-License-Identifier: GPL-3.0-or-later

#include <JuceHeader.h>

#include "Plugin/PluginProcessor.h"
#include "Ui/SwaraXtSkin.h"
#include "shruthi/system_settings.h"

#include <cmath>
#include <cstdint>
#include <cstdio>
#include <limits>

namespace {
using Processor = SwaraXtAudioProcessor;
int failures = 0;

void expect(bool ok, const char* message)
{
    if (! ok)
    {
        ++failures;
        std::printf("FAIL: %s\n", message);
        std::fflush(stdout);
    }
}

void render(Processor& processor, juce::MidiBuffer& midi, int samples = 64, int channels = 2)
{
    juce::AudioBuffer<float> audio(channels, samples);
    processor.processBlock(audio, midi);
}

bool containsNote(const juce::MidiBuffer& midi, int note, int velocity)
{
    for (const auto metadata : midi)
        if (metadata.getMessage().isNoteOn() && metadata.getMessage().getNoteNumber() == note
            && metadata.getMessage().getVelocity() == velocity)
            return true;
    return false;
}

void midiLifecycle()
{
    Processor processor;
    juce::MidiBuffer midi;
    midi.addEvent(juce::MidiMessage::noteOn(1, 60, (juce::uint8) 17), 0);
    midi.addEvent(juce::MidiMessage::noteOff(1, 60), 10);
    render(processor, midi);
    expect(midi.getNumEvents() == 0, "unprepared processBlock must not pass incoming MIDI through");

    processor.prepareToPlay(1000.0, 64);
    midi.clear();
    midi.addEvent(juce::MidiMessage::noteOn(1, 61, (juce::uint8) 19), 0);
    render(processor, midi);
    expect(! containsNote(midi, 61, 19), "unsupported sample rate must not echo the input note");

    processor.prepareToPlay(48000.0, 64);
    midi.clear();
    midi.addEvent(juce::MidiMessage::noteOn(1, 3, (juce::uint8) 23), 0);
    render(processor, midi);
    expect(! containsNote(midi, 3, 23), "prepared processing does not echo an input note");

    if (auto* parameter = processor.getApvts().getParameter(swaraxt::IDs::seqMidiOut))
        parameter->setValueNotifyingHost(parameter->convertTo0to1(1.0f));
    midi.clear();
    midi.addEvent(juce::MidiMessage::noteOn(1, 3, (juce::uint8) 29), 0);
    render(processor, midi);
    expect(! containsNote(midi, 3, 29), "MIDI Out does not turn input notes into output notes");

    processor.releaseResources();
    midi.clear();
    midi.addEvent(juce::MidiMessage::noteOn(1, 62, (juce::uint8) 31), 0);
    render(processor, midi);
    expect(midi.getNumEvents() == 0, "processBlock after releaseResources clears incoming MIDI");

    processor.prepareToPlay(48000.0, 64);
    midi.clear();
    midi.addEvent(juce::MidiMessage::noteOn(1, 4, (juce::uint8) 11), 0);
    render(processor, midi, 0);
    expect(midi.getNumEvents() == 0, "a zero-sample block clears incoming MIDI");
}

void busContract()
{
    Processor processor;
    auto layout = processor.getBusesLayout();
    expect(processor.isBusesLayoutSupported(layout), "the default stereo layout is supported");
    expect(layout.getMainOutputChannelSet() == juce::AudioChannelSet::stereo(), "the default output is stereo");

    auto rejected = layout;
    rejected.outputBuses.getReference(0) = juce::AudioChannelSet::disabled();
    expect(! processor.isBusesLayoutSupported(rejected), "disabled output is rejected");
    rejected.outputBuses.getReference(0) = juce::AudioChannelSet::mono();
    expect(! processor.isBusesLayoutSupported(rejected), "mono output is not part of the contract");
    rejected.outputBuses.getReference(0) = juce::AudioChannelSet::discreteChannels(3);
    expect(! processor.isBusesLayoutSupported(rejected), "three-channel output is rejected");
    rejected.outputBuses.getReference(0) = juce::AudioChannelSet::create5point1();
    expect(! processor.isBusesLayoutSupported(rejected), "surround output is rejected");
    rejected.outputBuses.add(juce::AudioChannelSet::stereo());
    expect(! processor.isBusesLayoutSupported(rejected), "an extra output bus is rejected");
    rejected = layout;
    rejected.inputBuses.add(juce::AudioChannelSet::stereo());
    expect(! processor.isBusesLayoutSupported(rejected), "an input bus is rejected");

    expect(! processor.setBusesLayout(rejected), "an unsupported layout is not applied");
    expect(processor.getBusesLayout().getMainOutputChannelSet() == juce::AudioChannelSet::stereo(),
           "a rejected layout leaves stereo in place");

    processor.prepareToPlay(48000.0, 32);
    juce::AudioBuffer<float> audio(2, 32);
    juce::MidiBuffer midi;
    audio.setSample(0, 0, 4.0f);
    processor.processBlock(audio, midi);
    expect(std::isfinite(audio.getSample(0, 0)) && std::isfinite(audio.getSample(1, 0)),
           "stereo render writes finite samples on both channels");
    expect(audio.getNumChannels() == 2, "the rendered buffer stays stereo");
}

void presetBounds()
{
    const auto directory = juce::File::getCurrentWorkingDirectory()
                               .getChildFile("mission04-preset-fixtures");
    directory.createDirectory();
    Processor processor;
    processor.prepareToPlay(48000.0, 64);
    if (auto* shape = processor.getApvts().getParameter(swaraxt::IDs::osc1Shape))
        shape->setValueNotifyingHost(shape->convertTo0to1(6.0f));
    const auto* raw = processor.getApvts().getRawParameterValue(swaraxt::IDs::osc1Shape);
    const int before = raw != nullptr ? static_cast<int>(std::lround(raw->load())) : -1;

    auto load = [&](const juce::File& file, juce::String& error) {
        Processor::PresetEntry entry;
        entry.file = file;
        entry.name = file.getFileNameWithoutExtension();
        return processor.loadPresetEntry(entry, error);
    };

    juce::String error;
    expect(! load(directory.getChildFile("missing.swaraxtpreset"), error), "a missing preset is rejected");
    expect(error.isNotEmpty(), "a missing preset reports an error");

    const auto empty = directory.getChildFile("empty.swaraxtpreset");
    empty.replaceWithData("", 0);
    expect(! load(empty, error), "an empty preset is rejected");
    expect(static_cast<int>(std::lround(raw->load())) == before, "an empty preset leaves the current sound");

    const auto junk = directory.getChildFile("junk.swaraxtpreset");
    junk.replaceWithText("not a preset");
    expect(! load(junk, error), "a malformed preset is rejected");
    expect(static_cast<int>(std::lround(raw->load())) == before, "a malformed preset leaves the current sound");

    juce::MemoryBlock state;
    processor.getStateInformation(state);
    const auto valid = directory.getChildFile("valid.swaraxtpreset");
    expect(static_cast<juce::int64>(state.getSize()) < Processor::maxUserPresetFileBytes,
           "a real processor state fits under the preset ceiling");
    valid.replaceWithData(state.getData(), state.getSize());
    Processor restored;
    Processor::PresetEntry entry;
    entry.file = valid;
    entry.name = "valid";
    expect(restored.loadPresetEntry(entry, error), "a current preset file loads");
    const auto* restoredShape = restored.getApvts().getRawParameterValue(swaraxt::IDs::osc1Shape);
    expect(restoredShape != nullptr && static_cast<int>(std::lround(restoredShape->load())) == before,
           "the loaded preset keeps the saved oscillator shape");

    const auto oversized = directory.getChildFile("oversize.swaraxtpreset");
    {
        juce::FileOutputStream stream(oversized);
        expect(stream.openedOk(), "the oversized fixture can be created");
        juce::MemoryBlock chunk(64 * 1024, true);
        juce::int64 remaining = Processor::maxUserPresetFileBytes + 1;
        while (remaining > 0 && stream.getStatus().wasOk())
        {
            const auto chunkBytes = remaining > static_cast<juce::int64>(chunk.getSize())
                ? chunk.getSize() : static_cast<size_t>(remaining);
            stream.write(chunk.getData(), chunkBytes);
            remaining -= static_cast<juce::int64>(chunkBytes);
        }
    }
    expect(oversized.getSize() == Processor::maxUserPresetFileBytes + 1, "the fixture is one byte over the limit");
    expect(! load(oversized, error), "a file one byte over the limit is rejected");
    expect(error.contains("1 MiB"), "the oversized preset explains the limit");
    expect(static_cast<int>(std::lround(raw->load())) == before, "an oversized preset leaves the current sound");
    directory.deleteRecursively();
}

void pngBudget()
{
    const auto directory = juce::File::getCurrentWorkingDirectory().getChildFile("mission04-png-fixtures");
    directory.createDirectory();
    auto writeHeader = [](const juce::File& file, std::uint32_t width, std::uint32_t height) {
        unsigned char bytes[24] = { 137, 80, 78, 71, 13, 10, 26, 10, 0, 0, 0, 13, 'I', 'H', 'D', 'R' };
        bytes[16] = static_cast<unsigned char>(width >> 24);
        bytes[17] = static_cast<unsigned char>(width >> 16);
        bytes[18] = static_cast<unsigned char>(width >> 8);
        bytes[19] = static_cast<unsigned char>(width);
        bytes[20] = static_cast<unsigned char>(height >> 24);
        bytes[21] = static_cast<unsigned char>(height >> 16);
        bytes[22] = static_cast<unsigned char>(height >> 8);
        bytes[23] = static_cast<unsigned char>(height);
        file.replaceWithData(bytes, sizeof(bytes));
    };
    const auto small = directory.getChildFile("small.png");
    writeHeader(small, 32, 32);
    expect(swaraxt::ui::SkinUserSvg::pngDeclaredSizeAllowed(small), "a small declared PNG passes the budget");
    const auto bomb = directory.getChildFile("bomb.png");
    writeHeader(bomb, 8192, 8192);
    expect(! swaraxt::ui::SkinUserSvg::pngDeclaredSizeAllowed(bomb),
           "an 8192 square declaration is rejected before decoding");
    const auto zero = directory.getChildFile("zero.png");
    writeHeader(zero, 0, 16);
    expect(! swaraxt::ui::SkinUserSvg::pngDeclaredSizeAllowed(zero), "a zero dimension is rejected");
    const auto truncated = directory.getChildFile("truncated.png");
    truncated.replaceWithData("\x89PNG", 4);
    expect(! swaraxt::ui::SkinUserSvg::pngDeclaredSizeAllowed(truncated), "a truncated header is rejected");
    const auto bad = directory.getChildFile("bad.png");
    writeHeader(bad, 8, 8);
    {
        juce::MemoryBlock block;
        bad.loadFileAsData(block);
        auto* bytes = static_cast<unsigned char*>(block.getData());
        bytes[12] = 'I';
        bytes[13] = 'X';
        bad.replaceWithData(bytes, block.getSize());
    }
    expect(! swaraxt::ui::SkinUserSvg::pngDeclaredSizeAllowed(bad), "a malformed IHDR is rejected");

    juce::Image image(juce::Image::ARGB, 8, 8, true);
    const auto real = directory.getChildFile("real.png");
    juce::FileOutputStream stream(real);
    juce::PNGImageFormat format;
    expect(format.writeImageToStream(image, stream), "a real 8x8 PNG can be written");
    stream.flush();
    expect(swaraxt::ui::SkinUserSvg::pngDeclaredSizeAllowed(real), "a real small PNG passes the preflight");
    directory.deleteRecursively();
}

void tuningBounds()
{
    int periodIndex = 0;
    int inside = -1;
    swaraxt::tuning::splitSignedPeriod(std::numeric_limits<int>::min(), 12, periodIndex, inside);
    expect(inside >= 0 && inside < 12, "INT_MIN folds into the period");
    swaraxt::tuning::splitSignedPeriod(-1, 12, periodIndex, inside);
    expect(periodIndex == -1 && inside == 11, "negative floor division keeps a non-negative remainder");
    swaraxt::tuning::splitSignedPeriod(0, 12, periodIndex, inside);
    expect(periodIndex == 0 && inside == 0, "degree 0 stays at the tonic");
    swaraxt::tuning::splitSignedPeriod(std::numeric_limits<int>::max(), 12, periodIndex, inside);
    expect(inside >= 0 && inside < 12, "INT_MAX folds into the period");

    swaraxt::SequenceState::ScaleRandomView view;
    view.formalPeriod = 12;
    view.periodCents = 1200.0;
    view.degreeCount = 1;
    view.degreeCents[0] = 100.0;
    const auto cents = swaraxt::SequenceState::scaleViewCents(view, std::numeric_limits<int>::min());
    expect(std::isfinite(cents), "scale cents stay finite at INT_MIN");

    swaraxt::tuning::TuningSnapshot snapshot;
    snapshot.formalPeriod = 12;
    snapshot.referenceNote = 69;
    snapshot.referenceHz = 440.0;
    snapshot.periodCents = 1200.0;
    snapshot.signedKeyboardMapping = true;
    snapshot.midiDegree[69] = 0;
    snapshot.midiHz[69] = 440.0;
    const auto ordinary = snapshot.hzForDegree(0);
    expect(std::abs(ordinary - 440.0) < 1.0e-6, "degree 0 remains the reference frequency");
    expect(snapshot.hzForDegree(std::numeric_limits<int>::min()) == 0.0,
           "an extreme degree that cannot make a finite frequency is rejected");
    expect(swaraxt::tuning::TuningSnapshot::equalHzFromCvSemitones(69.0, 69.0, 440.0) == 440.0,
           "equal temperament at the reference note is unchanged");
    expect(swaraxt::tuning::TuningSnapshot::equalHzFromCvSemitones(
               std::numeric_limits<double>::infinity(), 69.0, 440.0) == 0.0,
           "a nonfinite CV frequency is rejected");
    expect(swaraxt::tuning::TuningSnapshot::equalHzFromCvSemitones(1.0e9, 69.0, 440.0) == 0.0,
           "an overflowing equal-tempered frequency is rejected");
}

void missionPreservation()
{
    Processor processor;
    processor.prepareToPlay(48000.0, 64);
    if (auto* arp = processor.getApvts().getParameter(swaraxt::IDs::arpPattern))
        arp->setValueNotifyingHost(arp->convertTo0to1(5.0f));
    processor.sequenceState().setFreePitch(1, 60 * swaraxt::FreeCvPitch::kUnitsPerSemitone);
    expect(processor.sequenceState().arpPattern() == 5, "a sequencer edit still leaves host ARP in place");

    juce::MidiBuffer midi;
    midi.addEvent(juce::MidiMessage::controllerEvent(1, 82, 127), 0);
    render(processor, midi);
    expect(processor.engineForTests().shruthiPart().system_settings().octave == 0,
           "CC82 remains outside the persistent parameter authority");
}

}

int main()
{
    setvbuf(stdout, nullptr, _IONBF, 0);
    juce::ScopedJuceInitialiser_GUI initialise;
    juce::File("D:/MontroneDSP_Studio/build-worktrees/mission04-defensive-hardening/hostile-inputs")
        .setAsCurrentWorkingDirectory();
    midiLifecycle();
    busContract();
    presetBounds();
    pngBudget();
    tuningBounds();
    missionPreservation();
    std::printf("Defensive hardening tests: %s (%d failures)\n", failures ? "FAILED" : "PASSED", failures);
    return failures ? 1 : 0;
}
