// Copyright 2026 MontroneDSP.
// SPDX-License-Identifier: GPL-3.0-or-later

#include <JuceHeader.h>

#include <cmath>
#include <cstdio>
#include <filesystem>

#include "Plugin/PluginEditor.h"
#include "Plugin/PluginProcessor.h"
#include "Plugin/SwaraXtParameterLayout.h"
#include "Ui/SwaraXtModulationNames.h"
#include "shruthi/patch.h"

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
    auto* parameter = processor.getApvts().getParameter(id);
    expect(parameter != nullptr, id);
    if (parameter != nullptr)
        parameter->setValueNotifyingHost(parameter->convertTo0to1(static_cast<float>(value)));
}

class TestPlayHead : public juce::AudioPlayHead {
 public:
    Optional<PositionInfo> getPosition() const override
    {
        PositionInfo position;
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

void processSamples(SwaraXtAudioProcessor& processor,
                    TestPlayHead& playHead,
                    int totalSamples,
                    const std::vector<int>& blockSizes,
                    bool sendNote)
{
    int processed = 0;
    int blockIndex = 0;
    while (processed < totalSamples)
    {
        const int block = juce::jmin(blockSizes[static_cast<size_t>(blockIndex % blockSizes.size())],
                                     totalSamples - processed);
        juce::AudioBuffer<float> buffer(2, block);
        juce::MidiBuffer midi;
        if (sendNote && processed == 0)
            midi.addEvent(juce::MidiMessage::noteOn(1, 60, static_cast<juce::uint8>(100)), 0);
        processor.processBlock(buffer, midi);
        playHead.ppq += static_cast<double>(block) * playHead.bpm / (60.0 * 48000.0);
        processed += block;
        ++blockIndex;
    }
}

void testHostClockAndLfoSync()
{
    SwaraXtAudioProcessor processor;
    TestPlayHead playHead;
    processor.setPlayHead(&playHead);
    processor.prepareToPlay(48000.0, 257);
    setInt(processor, swaraxt::IDs::seqMode, shruthi::SEQUENCER_MODE_ARP);
    setInt(processor, swaraxt::IDs::seqClockMode, 1);
    setInt(processor, swaraxt::IDs::arpGate, 7);
    setInt(processor, swaraxt::IDs::seqGate, 50);
    setInt(processor, swaraxt::IDs::lfo1Sync, 1);
    setInt(processor, swaraxt::IDs::lfo1Division, 3);

    processSamples(processor, playHead, 24000, { 63, 64, 65, 127, 128, 129, 257 }, true);
    expect(processor.engineForTests().hostClockEventCountForTests() == 24,
           "host clock emits exactly 24 ticks per quarter note");
    expect(processor.engineForTests().shruthiPart().step() == 3,
           "six-clock division advances four steps per quarter note");
    expect(processor.engineForTests().shruthiPart().lfo_phase_increment_for_tests(0) == 134,
           "synchronized quarter-note LFO uses native control-rate increment");

    const auto beforeTempoChange = processor.engineForTests().hostClockEventCountForTests();
    playHead.bpm = 174.0;
    processSamples(processor, playHead, 24000, { 65, 129, 63 }, false);
    expect(processor.engineForTests().hostClockEventCountForTests() > beforeTempoChange + 33,
           "host clock follows a running BPM change without block-size quantization");
    expect(processor.engineForTests().shruthiPart().lfo_phase_increment_for_tests(0) == 194,
           "synchronized LFO follows a running BPM change");

    setInt(processor, swaraxt::IDs::seqSwing, 100);
    const auto beforeSwing = processor.engineForTests().hostClockEventCountForTests();
    processSamples(processor, playHead, 48000, { 63, 257, 64, 129, 65 }, false);
    const auto swingTicks = processor.engineForTests().hostClockEventCountForTests() - beforeSwing;
    expect(swingTicks >= 68 && swingTicks <= 71,
           "swing preserves average host-clock rate");

    playHead.playing = false;
    processSamples(processor, playHead, 128, { 128 }, false);
    expect(! processor.engineForTests().shruthiPart().running(),
           "host stop releases synchronized sequencer transport");

    playHead.playing = true;
    playHead.ppq = 9.25;
    processSamples(processor, playHead, 128, { 63, 65 }, false);
    expect(processor.engineForTests().shruthiPart().running(),
           "host restart aligns at a non-zero PPQ position");
    setInt(processor, swaraxt::IDs::seqClockMode, 0);
    processSamples(processor, playHead, 128, { 63, 65 }, false);
    expect(processor.engineForTests().shruthiPart().running(),
           "switching from host sync to free clock preserves the running sequencer");
    processor.releaseResources();
}

void testHostTempoCoverage()
{
    for (const double bpm : { 60.0, 120.0, 128.0, 174.0 })
    {
        SwaraXtAudioProcessor processor;
        TestPlayHead playHead;
        playHead.bpm = bpm;
        processor.setPlayHead(&playHead);
        processor.prepareToPlay(48000.0, 257);
        setInt(processor, swaraxt::IDs::seqMode, shruthi::SEQUENCER_MODE_ARP);
        setInt(processor, swaraxt::IDs::seqClockMode, 1);
        processSamples(processor, playHead, 48000, { 63, 64, 65, 127, 128, 129, 257 }, true);
        const auto expected = static_cast<uint64_t>(std::ceil(bpm * 24.0 / 60.0));
        expect(processor.engineForTests().hostClockEventCountForTests() == expected,
               "host clock tick count follows 60, 120, 128, and 174 BPM");
    }

    SwaraXtAudioProcessor fallback;
    TestPlayHead noPpq;
    noPpq.bpm = 128.0;
    noPpq.providePpq = false;
    fallback.setPlayHead(&noPpq);
    fallback.prepareToPlay(48000.0, 129);
    setInt(fallback, swaraxt::IDs::seqMode, shruthi::SEQUENCER_MODE_ARP);
    setInt(fallback, swaraxt::IDs::seqClockMode, 1);
    processSamples(fallback, noPpq, 48000, { 63, 65, 129 }, true);
    expect(fallback.engineForTests().hostClockEventCountForTests() == 52,
           "host clock remains deterministic when the host omits PPQ position");
}

void testFreeClockAndGate()
{
    SwaraXtAudioProcessor freeClock;
    TestPlayHead playHead;
    freeClock.setPlayHead(&playHead);
    freeClock.prepareToPlay(48000.0, 65);
    setInt(freeClock, swaraxt::IDs::seqMode, shruthi::SEQUENCER_MODE_ARP);
    setInt(freeClock, swaraxt::IDs::seqClockMode, 0);
    setInt(freeClock, swaraxt::IDs::seqTempo, 128);
    processSamples(freeClock, playHead, 48000, { 63, 64, 65 }, true);
    expect(freeClock.engineForTests().hostClockEventCountForTests() == 0,
           "free-clock mode does not consume host clock events");
    expect(freeClock.engineForTests().shruthiPart().running(),
           "free-clock mode retains the native Shruthi sequencer clock");

    SwaraXtAudioProcessor gated;
    TestPlayHead gatedPlayHead;
    gated.setPlayHead(&gatedPlayHead);
    gated.prepareToPlay(48000.0, 64);
    setInt(gated, swaraxt::IDs::seqMode, shruthi::SEQUENCER_MODE_ARP);
    setInt(gated, swaraxt::IDs::seqClockMode, 1);
    setInt(gated, swaraxt::IDs::arpGate, 7);
    setInt(gated, swaraxt::IDs::seqGate, 25);
    processSamples(gated, gatedPlayHead, 4500, { 63, 64, 65 }, true);
    expect(gated.engineForTests().shruthiPart().gate_counter_for_tests() == 0
               && gated.engineForTests().shruthiPart().generated_note_count_for_tests() == 0,
           "host-synchronized sequencer gate closes before the next step");
}

void testLfoRateIndependence()
{
    for (double sampleRate : { 44100.0, 48000.0, 96000.0 })
    {
        for (int block : { 63, 64, 65 })
        {
            SwaraXtAudioProcessor processor;
            TestPlayHead playHead;
            processor.setPlayHead(&playHead);
            processor.prepareToPlay(sampleRate, block);
            setInt(processor, swaraxt::IDs::lfo1Sync, 1);
            setInt(processor, swaraxt::IDs::lfo1Division, 2);
            juce::AudioBuffer<float> buffer(2, block);
            juce::MidiBuffer midi;
            processor.processBlock(buffer, midi);
            expect(processor.engineForTests().shruthiPart().lfo_phase_increment_for_tests(0) == 267,
                   "synchronized eighth-note LFO is host-rate and block-size independent");
        }
    }

    SwaraXtAudioProcessor divisions;
    TestPlayHead playHead;
    divisions.setPlayHead(&playHead);
    divisions.prepareToPlay(48000.0, 64);
    setInt(divisions, swaraxt::IDs::lfo1Sync, 1);
    juce::AudioBuffer<float> buffer(2, 64);
    juce::MidiBuffer midi;
    setInt(divisions, swaraxt::IDs::lfo1Division, 1);
    divisions.processBlock(buffer, midi);
    expect(divisions.engineForTests().shruthiPart().lfo_phase_increment_for_tests(0) == 535,
           "synchronized sixteenth-note LFO uses the requested beat division");
    setInt(divisions, swaraxt::IDs::lfo1Division, 6);
    divisions.processBlock(buffer, midi);
    expect(divisions.engineForTests().shruthiPart().lfo_phase_increment_for_tests(0) == 17,
           "synchronized two-bar LFO uses the requested slow beat division");

    setInt(divisions, swaraxt::IDs::lfo1Sync, 0);
    setInt(divisions, swaraxt::IDs::lfo1Rate, 80);
    playHead.bpm = 60.0;
    divisions.processBlock(buffer, midi);
    const auto freeIncrement = divisions.engineForTests().shruthiPart().lfo_phase_increment_for_tests(0);
    playHead.bpm = 174.0;
    divisions.processBlock(buffer, midi);
    expect(divisions.engineForTests().shruthiPart().lfo_phase_increment_for_tests(0) == freeIncrement,
           "free-running LFO remains independent of host BPM");
}

void testLfoRetrigger()
{
    SwaraXtAudioProcessor processor;
    TestPlayHead playHead;
    processor.setPlayHead(&playHead);
    processor.prepareToPlay(48000.0, 65);
    setInt(processor, swaraxt::IDs::lfo1Sync, 1);
    setInt(processor, swaraxt::IDs::lfo1Division, 3);
    setInt(processor, swaraxt::IDs::lfo1Retrig, 1);
    processSamples(processor, playHead, 4096, { 63, 64, 65 }, false);
    const auto phaseBeforeNote = processor.engineForTests().shruthiPart().lfo_phase_for_tests(0);
    processSamples(processor, playHead, 65, { 65 }, true);
    const auto phaseAfterNote = processor.engineForTests().shruthiPart().lfo_phase_for_tests(0);
    expect(phaseBeforeNote > 1000 && phaseAfterNote < 1000,
           "note retrigger resets a host-synchronized LFO without changing its rate source");
}

void testUserPresets()
{
    const auto root = juce::File::getSpecialLocation(juce::File::tempDirectory)
        .getNonexistentChildFile("swaraxt-preset-test", {}, false);
    expect(root.createDirectory().wasOk(), "create isolated preset test directory");

    SwaraXtAudioProcessor processor;
    processor.setUserPresetDirectoryForTests(root);
    if (auto* cutoff = processor.getApvts().getParameter(swaraxt::IDs::filterCutoff))
        cutoff->setValueNotifyingHost(cutoff->convertTo0to1(432.0f));

    juce::String error;
    expect(processor.saveUserPreset("Focused Bass", false, error), "save complete user preset");
    expect(! processor.saveUserPreset("Focused Bass", false, error), "duplicate requires overwrite");
    expect(processor.saveUserPreset("Focused Bass", true, error), "explicit overwrite succeeds");
    expect(! processor.saveUserPreset("../escape", false, error), "path traversal is rejected");
    expect(processor.saveUserPreset("Saw Bass", false, error),
           "a factory-named save creates a separate user preset");
    expect(root.getChildFile("Saw Bass.swaraxtpreset").existsAsFile(),
           "factory presets are never overwritten in place");

    auto entries = processor.getPresetEntries();
    auto user = std::find_if(entries.begin(), entries.end(), [](const auto& entry) {
        return ! entry.isFactory && entry.name == "Focused Bass";
    });
    expect(user != entries.end(), "saved user preset is rediscovered immediately");

    processor.setCurrentProgram(0);
    if (user != entries.end())
        expect(processor.loadPresetEntry(*user, error), "reload user preset");
    expect(std::abs(swaraxt::getParameterValue(processor.getApvts(), swaraxt::IDs::filterCutoff)
                    - 432.0f) < 0.1f,
           "saved preset recalls authoritative APVTS state");

    if (auto* cutoff = processor.getApvts().getParameter(swaraxt::IDs::filterCutoff))
        cutoff->setValueNotifyingHost(cutoff->convertTo0to1(987.0f));
    expect(processor.saveUserPreset("Focused Lead", false, error), "save independent second preset");
    processor.loadPresetEntry(*user, error);
    auto refreshed = processor.getPresetEntries();
    const auto lead = std::find_if(refreshed.begin(), refreshed.end(), [](const auto& entry) {
        return ! entry.isFactory && entry.name == "Focused Lead";
    });
    expect(lead != refreshed.end() && processor.loadPresetEntry(*lead, error),
           "second preset reloads after a different preset");
    expect(std::abs(swaraxt::getParameterValue(processor.getApvts(), swaraxt::IDs::filterCutoff)
                    - 987.0f) < 0.1f,
           "preset B recall does not depend on preset A state");

    const auto beforeMalformed = processor.getApvts().copyState().createCopy();
    const auto malformed = root.getChildFile("Malformed.swaraxtpreset");
    expect(malformed.replaceWithText("not a Swara XT state"), "write malformed test fixture");
    SwaraXtAudioProcessor::PresetEntry bad { "Malformed", malformed, -1, false };
    expect(! processor.loadPresetEntry(bad, error), "malformed preset fails safely");
    expect(processor.getApvts().copyState().isEquivalentTo(beforeMalformed),
           "malformed preset does not partially apply state");

    juce::MemoryBlock versionOneData;
    processor.getStateInformation(versionOneData);
    if (auto xml = juce::AudioProcessor::getXmlFromBinary(versionOneData.getData(),
                                                           static_cast<int>(versionOneData.getSize())))
    {
        xml->setAttribute("stateVersion", 1);
        juce::MemoryBlock compatible;
        juce::AudioProcessor::copyXmlToBinary(*xml, compatible);
        processor.setStateInformation(compatible.getData(), static_cast<int>(compatible.getSize()));
        expect(processor.getApvts().state.isValid(), "version-one state remains loadable");
    }
    else
    {
        expect(false, "decode version-one compatibility fixture");
    }

    expect(root.deleteRecursively(), "remove isolated preset test directory");
}

void writeScreenshot(SwaraXtAudioProcessorEditor& editor,
                     const std::filesystem::path& output,
                     bool modulation,
                     bool sequencer)
{
    editor.setModuleViewsForTests(modulation, sequencer);
    editor.resized();
    auto image = editor.createComponentSnapshot(editor.getLocalBounds());
    juce::PNGImageFormat png;
    const juce::File outputFile(output.string());
    outputFile.deleteFile();
    juce::FileOutputStream stream(outputFile);
    expect(stream.openedOk() && png.writeImageToStream(image, stream), "write GUI screenshot");
}

void testEditorAndScreenshots(const std::filesystem::path& outputRoot)
{
    SwaraXtAudioProcessor processor;
    SwaraXtAudioProcessorEditor editor(processor);
    editor.setGuiSizeForTests(swaraxt::ui::GuiSize::medium);
    expect(! editor.isResizable(), "editor remains fixed-size");
    expect(editor.getWidth() == 1113 && editor.getHeight() == 521,
           "medium editor uses the canonical SVG dimensions");
    const auto lockup = editor.productLockupBoundsForTests();
    expect(std::abs(lockup.getX() - 433.340f) < 0.001f
               && std::abs(lockup.getY() - 146.590f) < 0.001f
               && std::abs(lockup.getWidth() - 246.674f) < 0.001f
               && std::abs(lockup.getHeight() - 44.282f) < 0.001f,
           "product lockup preserves canonical geometry with the refined group offset");
    expect(editor.presetBoundsForTests().getCentreX() == editor.getWidth() / 2,
           "preset name is centered on the complete editor");

    for (int oscillator = 0; oscillator < 2; ++oscillator)
    {
        auto& panel = editor.oscillatorForTests(oscillator);
        expect(panel.modelIsSelector(), "oscillator model remains a discrete selector");
        expect(panel.pitchIsRotary(), "oscillator pitch is a rotary control");
        expect(panel.waveVariationIsRotary(), "oscillator wave variation is a rotary control");
        expect(panel.pitchSlider().getInterval() == 1.0
                   && panel.pitchSlider().getMinimum() == -48.0
                   && panel.pitchSlider().getMaximum() == 48.0,
               "pitch rotary reaches only authoritative integer semitone states");
        expect(panel.waveVariationSlider().getInterval() == 1.0
                   && panel.waveVariationSlider().getMinimum() == 0.0
                   && panel.waveVariationSlider().getMaximum() == 127.0,
               "wave variation rotary reaches only authoritative integer states");

        const auto originalPitch = panel.pitchSlider().getValue();
        const auto originalVariation = panel.waveVariationSlider().getValue();
        const auto* pitchId = oscillator == 0 ? swaraxt::IDs::osc1Range : swaraxt::IDs::osc2Range;
        const auto* variationId = oscillator == 0 ? swaraxt::IDs::osc1Param : swaraxt::IDs::osc2Param;

        for (int pitch = -48; pitch <= 48; ++pitch)
        {
            panel.pitchSlider().setValue(static_cast<double>(pitch), juce::sendNotificationSync);
            expect(panel.pitchSlider().getValue() == static_cast<double>(pitch),
                   "pitch rotary never lands between valid states");
            expect(std::lround(processor.getApvts().getRawParameterValue(pitchId)->load()) == pitch,
                   "pitch rotary remains attached to authoritative automation state");
        }
        for (int variation = 0; variation <= 127; ++variation)
        {
            panel.waveVariationSlider().setValue(static_cast<double>(variation),
                                                  juce::sendNotificationSync);
            expect(panel.waveVariationSlider().getValue() == static_cast<double>(variation),
                   "wave variation rotary never lands between valid states");
            expect(std::lround(processor.getApvts().getRawParameterValue(variationId)->load()) == variation,
                   "wave variation remains attached to authoritative automation state");
        }
        panel.pitchSlider().setValue(originalPitch, juce::sendNotificationSync);
        panel.waveVariationSlider().setValue(originalVariation, juce::sendNotificationSync);
    }

    auto& keyTrack = editor.filterKeyTrackForTests();
    expect(keyTrack.isVisible(), "Filter Key Track control is visible");
    expect(keyTrack.knobSize() == swaraxt::ui::KnobSize::standard,
           "Filter Key Track uses the standard knob size");
    expect(! keyTrack.getBounds().isEmpty(), "Filter Key Track has usable layout bounds");
    expect(processor.getApvts().getParameter(swaraxt::IDs::filterKeyTracking) != nullptr,
           "Filter Key Track uses the existing APVTS parameter");
    const auto originalKeyTrack = keyTrack.slider().getValue();
    for (const double value : { 0.0, 0.5, 1.0 })
    {
        keyTrack.slider().setValue(value, juce::sendNotificationSync);
        expect(std::abs(processor.getApvts()
                            .getRawParameterValue(swaraxt::IDs::filterKeyTracking)->load()
                        - static_cast<float>(value)) < 1.0e-6f,
               "Filter Key Track GUI remains attached to authoritative automation state");
    }
    keyTrack.slider().setValue(originalKeyTrack, juce::sendNotificationSync);

    auto& osc1 = editor.oscillatorForTests(0);
    expect(osc1.modelComboForTests().getNumItems() == swaraxt::ui::oscillatorNames().size(),
           "OSC MODEL combo lists every valid waveform");
    expect(osc1.modelComboForTests().getNumItems() == static_cast<int>(shruthi::WAVEFORM_LAST),
           "OSC MODEL combo does not include WAVEFORM_LAST");
    for (int i = 0; i < osc1.modelComboForTests().getNumItems(); ++i)
        expect(osc1.modelComboForTests().getItemText(i) != "Reserved",
               "OSC MODEL does not contain Reserved");

    editor.setModuleViewsForTests(true, false);
    auto& mod = editor.modulationForTests();
    expect(mod.destinationComboForTests(0).getNumItems()
               == swaraxt::ui::pluginVisibleModulationDestinationCount(),
           "Mod destination combo lists plugin-valid destinations only");
    expect(mod.sourceComboForTests(0).getNumItems()
               == swaraxt::ui::pluginVisibleModulationSourceCount(),
           "Mod source combo lists plugin-valid sources only");
    bool sawTimbre1 = false;
    bool sawTimbre2 = false;
    for (int i = 0; i < mod.destinationComboForTests(0).getNumItems(); ++i)
    {
        const auto text = mod.destinationComboForTests(0).getItemText(i);
        expect(! text.startsWith("Dest "), "Mod destination combo has no Dest NN placeholders");
        expect(text != "PWM 1" && text != "PWM 2",
               "Mod destination combo does not use PWM labels");
        expect(text != "CV 1" && text != "CV 2",
               "Mod destination combo hides hardware-only CV outputs");
        if (text == "Timbre 1")
        {
            sawTimbre1 = true;
            expect(mod.destinationComboForTests(0).getItemId(i) == shruthi::MOD_DST_PWM_1 + 1,
                   "Timbre 1 maps to the original oscillator-parameter-1 destination ID");
        }
        if (text == "Timbre 2")
        {
            sawTimbre2 = true;
            expect(mod.destinationComboForTests(0).getItemId(i) == shruthi::MOD_DST_PWM_2 + 1,
                   "Timbre 2 maps to the original oscillator-parameter-2 destination ID");
        }
    }
    expect(sawTimbre1 && sawTimbre2, "Timbre 1 and Timbre 2 are visible destinations");
    for (int i = 0; i < mod.sourceComboForTests(0).getNumItems(); ++i)
    {
        const auto text = mod.sourceComboForTests(0).getItemText(i);
        expect(! text.startsWith("CV "), "Mod source combo hides hardware-only CV inputs");
    }
    expect(mod.slot12TooltipForTests().containsIgnoreCase("Mod Wheel"),
           "Slot 12 discloses Mod Wheel scaling");


    std::filesystem::create_directories(outputRoot);
    editor.setGuiSizeForTests(swaraxt::ui::GuiSize::small);
    expect(editor.getWidth() == 891 && editor.getHeight() == 417,
           "small editor is the approved proportional size");
    editor.setGuiSizeForTests(swaraxt::ui::GuiSize::large);
    expect(editor.getWidth() == 1391 && editor.getHeight() == 651,
           "large editor is the approved proportional size");
    editor.setDecorationForTests(swaraxt::ui::DecorationId::legacy);
    editor.setSkinForTests(swaraxt::ui::SkinId::pastel);
    writeScreenshot(editor, outputRoot / "pastel-legacy-large.png", false, false);
    editor.setSkinForTests(swaraxt::ui::SkinId::midnightGold);
    writeScreenshot(editor, outputRoot / "midnight-gold-legacy-large.png", false, false);
    editor.setSkinForTests(swaraxt::ui::SkinId::neonCobalt);
    writeScreenshot(editor, outputRoot / "neon-cobalt-legacy-large.png", false, false);
    editor.setSkinForTests(swaraxt::ui::SkinId::jungle);
    writeScreenshot(editor, outputRoot / "jungle-legacy-large.png", false, false);
    editor.setSkinForTests(swaraxt::ui::SkinId::rossocorsa);
    writeScreenshot(editor, outputRoot / "rossocorsa-legacy-large.png", false, false);
    editor.setSkinForTests(swaraxt::ui::SkinId::pastel);
    editor.setDecorationForTests(swaraxt::ui::DecorationId::pcbTrace);
    writeScreenshot(editor, outputRoot / "pastel-pcb-trace-large.png", false, false);
}

}  // namespace

int main(int argc, char** argv)
{
    juce::ScopedJuceInitialiser_GUI juce;
    std::filesystem::path outputRoot = std::filesystem::current_path() / "artifacts" / "gui";
    if (argc > 1)
        outputRoot = argv[1];

    testHostClockAndLfoSync();
    testHostTempoCoverage();
    testFreeClockAndGate();
    testLfoRateIndependence();
    testLfoRetrigger();
    testUserPresets();
    testEditorAndScreenshots(outputRoot);
    std::printf(failures == 0 ? "Swara XT GUI/sync tests: PASSED\n"
                              : "Swara XT GUI/sync tests: FAILED (%d)\n",
                failures);
    return failures == 0 ? 0 : 1;
}
