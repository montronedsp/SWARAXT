// Copyright 2026 MontroneDSP. SPDX-License-Identifier: GPL-3.0-or-later
#include "Plugin/PluginEditor.h"
#include "Plugin/SequenceParameters.h"
#include "Plugin/ApvtsFactoryPresets.h"
#include "Ui/SwaraXtBoardPanel.h"
#include <cstdio>
#include <filesystem>

namespace {
int failures = 0;
void check(bool ok, const char* message) { if (!ok) { ++failures; std::printf("FAIL: %s\n", message); } }
void set(SwaraXtAudioProcessor& p, const char* id, float value) {
    auto* parameter = p.getApvts().getParameter(id);
    parameter->setValueNotifyingHost(parameter->convertTo0to1(value));
}
float get(SwaraXtAudioProcessor& p, const char* id) { return p.getApvts().getRawParameterValue(id)->load(); }
bool same(const swaraxt::SequenceSnapshot& a, const swaraxt::SequenceSnapshot& b) {
    return a.steps == b.steps && a.length == b.length && a.rotation == b.rotation
        && a.grooveTemplate == b.grooveTemplate && a.arpPattern == b.arpPattern;
}
void engineMatches(SwaraXtAudioProcessor& p) {
    juce::AudioBuffer<float> audio(2, 128); juce::MidiBuffer midi;
    p.processBlock(audio, midi);
    auto state = p.sequenceState().snapshot();
    const auto& engine = p.engineForTests().shruthiPart().sequencer_settings();
    check(engine.pattern_size == state.length && engine.pattern_rotation == state.rotation
        && engine.seq_groove_template == state.grooveTemplate && engine.arp_pattern == state.arpPattern, "engine sequence configuration");
    for (int i = 0; i < 16; ++i) {
        check(engine.steps[i].data_[0] == swaraxt::SequenceSnapshot::dataA(state.steps[static_cast<size_t>(i)])
            && engine.steps[i].data_[1] == swaraxt::SequenceSnapshot::dataB(state.steps[static_cast<size_t>(i)]), "engine pattern bytes");
    }
}
void lockChecks(const juce::File& output) {
    SwaraXtAudioProcessor p;
    p.prepareToPlay(48000, 128);
    p.setUserPresetDirectoryForTests(output.getChildFile("presets"));
    juce::String error;
    check(p.saveUserPreset("Workflow", true, error), "save workflow fixture");
    SwaraXtAudioProcessor::PresetEntry user { "Workflow", output.getChildFile("presets/Workflow.swaraxtpreset"), -1, false };
    const std::array<float, 9> values {2, 173, 39, 1, 71, 3, 6, 4, 9};
    for (size_t i = 0; i < values.size(); ++i) set(p, swaraxt::sequenceParameterIds[i], values[i]);
    auto pattern = swaraxt::SequenceState::defaultSnapshot();
    pattern.length = 11; pattern.rotation = 3; pattern.grooveTemplate = 4; pattern.arpPattern = 15;
    p.sequenceState().store(pattern);
    juce::Random rng(1234); p.sequenceState().randomize(swaraxt::SequenceState::Randomize::sequence, rng);
    pattern = p.sequenceState().snapshot();
    p.setSequenceLocked(true);
    auto preserved = [&] {
        check(p.sequenceLocked(), "preset does not disable lock");
        check(same(pattern, p.sequenceState().snapshot()), "locked pattern/extended arp preserved");
        for (size_t i = 0; i < values.size(); ++i) check(get(p, swaraxt::sequenceParameterIds[i]) == values[i], "locked playback parameter");
        engineMatches(p);
    };
    int imported = -1, overridden = -1;
    for (size_t i = 0; i < swaraxt::kShruthiFactoryPresetCount; ++i) {
        if (swaraxt::ApvtsFactoryPresets::findMutableOverride(swaraxt::kShruthiFactoryPresets[i].displayName)) overridden = swaraxt::kShruthiFactoryPresetStart + static_cast<int>(i);
        else imported = swaraxt::kShruthiFactoryPresetStart + static_cast<int>(i);
    }
    check(imported >= 0 && overridden >= 0, "cover imported and APVTS override factory paths");
    for (int program : {1, imported, overridden, swaraxt::kUserFactoryPresetStart}) {
        set(p, swaraxt::IDs::osc1Param, 113);
        p.setCurrentProgram(program); preserved();
        check(get(p, swaraxt::IDs::osc1Param) != 113, "unrelated timbre changes while locked");
    }
    check(p.loadPresetEntry(user, error), "locked user preset loads"); preserved();
    auto entries = p.getPresetEntries();
    check(p.loadPresetEntry(entries[2], error), "GUI preset path loads"); preserved();
    juce::MemoryBlock session; p.getStateInformation(session);
    p.setSequenceLocked(false); p.sequenceState().resetToDefault();
    p.setStateInformation(session.getData(), static_cast<int>(session.getSize())); preserved();
    check(p.saveUserPreset("Locked", true, error), "save musical preset while locked");
    juce::MemoryBlock saved; output.getChildFile("presets/Locked.swaraxtpreset").loadFileAsData(saved);
    auto xml = juce::AudioProcessor::getXmlFromBinary(saved.getData(), static_cast<int>(saved.getSize()));
    check(xml && !xml->hasAttribute("sequenceLocked"), "musical preset excludes workflow lock");
    p.setSequenceLocked(false);
    check(p.loadPresetEntry(user, error), "unlocked user preset loads");
    check(!p.sequenceLocked() && !same(pattern, p.sequenceState().snapshot()), "lock off loads preset sequence normally");
    // Older sessions have no workflow property and must reset the preference.
    if (xml) {
        juce::MemoryBlock old; juce::AudioProcessor::copyXmlToBinary(*xml, old);
        p.setSequenceLocked(true); p.setStateInformation(old.getData(), static_cast<int>(old.getSize()));
        check(!p.sequenceLocked(), "old host session defaults lock off");
    }
    std::puts("Lock: native/imported/override/APVTS factory, user, GUI load, host state and engine publication checked");
}
juce::Button* button(juce::Component& c, const juce::String& name) {
    if (auto* b = dynamic_cast<juce::Button*>(&c); b && b->getButtonText() == name) return b;
    for (auto* child : c.getChildren()) if (auto* b = button(*child, name)) return b;
    return nullptr;
}
void randomChecks() {
    swaraxt::SequenceState state; juce::Random rng(7254);
    for (auto kind : {swaraxt::SequenceState::Randomize::notes, swaraxt::SequenceState::Randomize::velocity,
                      swaraxt::SequenceState::Randomize::sequence}) {
        const auto before = state.snapshot(); state.randomize(kind, rng); const auto after = state.snapshot();
        check(before.length == after.length && before.rotation == after.rotation
            && before.arpPattern == after.arpPattern && before.grooveTemplate == after.grooveTemplate, "random preserves playback settings");
        check(before.steps != after.steps, "random changes musical content");
        for (size_t i = 0; i < after.steps.size(); ++i) {
            if (kind == swaraxt::SequenceState::Randomize::notes)
                check((before.steps[i] & 0xff80) == (after.steps[i] & 0xff80), "random notes preserves events/velocity/controller");
            if (kind == swaraxt::SequenceState::Randomize::velocity)
                check((before.steps[i] & 0x8fff) == (after.steps[i] & 0x8fff), "random velocity preserves pitches/events/controller");
            if (kind != swaraxt::SequenceState::Randomize::notes)
                check((after.steps[i] & 0x7000) != 0, "random velocity is in audible 1..7 domain");
            check((after.steps[i] & 0x7f) <= 105, "random notes valid musical range");
        }
    }
    std::puts("Random Notes / Velocity / Seq masks and domains checked");
}
void writeImage(SwaraXtAudioProcessorEditor& e, const juce::File& file, float scale = 1.0f) {
    juce::FileOutputStream stream(file); juce::PNGImageFormat png;
    check(stream.openedOk() && png.writeImageToStream(e.createComponentSnapshot(e.getLocalBounds(), true, scale), stream), "render GUI evidence");
}
void uiChecks(const juce::File& output) {
    SwaraXtAudioProcessor p; p.prepareToPlay(48000, 128);
    SwaraXtAudioProcessorEditor e(p); e.setVisible(true);
    e.setModuleViewsForTests(false, true);
    for (const char* name : {"RANDOM NOTES", "RANDOM VELOCITY", "RANDOM SEQ"}) {
        auto* b = button(e, name); check(b && b->isVisible(), "random button visible");
        if (b) { auto before = p.sequenceState().snapshot(); b->onClick(); check(!same(before, p.sequenceState().snapshot()), "random UI publishes immediately"); engineMatches(p); }
    }
    auto* lock = button(e, "LOCK"); check(lock != nullptr, "lock visible");
    if (lock) { lock->setToggleState(true, juce::dontSendNotification); lock->onClick(); check(p.sequenceLocked(), "lock button binding"); }
    for (auto skin : {swaraxt::ui::SkinId::pastel, swaraxt::ui::SkinId::midnightGold, swaraxt::ui::SkinId::neonCobalt,
                      swaraxt::ui::SkinId::jungle, swaraxt::ui::SkinId::rossocorsa}) {
        e.setSkinForTests(skin);
        for (auto size : {swaraxt::ui::GuiSize::small, swaraxt::ui::GuiSize::medium, swaraxt::ui::GuiSize::large}) {
            e.setGuiSizeForTests(size);
            for (int mode : {1, 2}) {
                e.arpComboForTests(0).setSelectedItemIndex(mode, juce::sendNotificationSync);
                auto* random = button(e, "RANDOM SEQ");
                check(random && random->getParentComponent()->getLocalBounds().contains(random->getBounds()), "sequence actions inside page");
                check(e.arpComboForTests(2).isVisible() && e.arpComboForTests(2).getParentComponent()->isVisible()
                    && e.sequenceEventComboForTests().isVisible() && e.sequenceEventComboForTests().getParentComponent()->isVisible(), "arp and sequence visible together");
                check(std::abs(e.arpComboForTests(2).getParentComponent()->getAlpha() - (mode == 1 ? 1.0f : 0.55f)) < 0.005f, "arp mode emphasis");
                const auto name = juce::String(swaraxt::ui::SkinRegistry::get(skin).stableId) + "-" + swaraxt::ui::GuiGeometry::stableId(size) + "-" + juce::String(mode) + ".png";
                writeImage(e, output.getChildFile(name));
            }
        }
    }
    e.setSkinForTests(swaraxt::ui::SkinId::pastel); e.setGuiSizeForTests(swaraxt::ui::GuiSize::medium);
    for (float scale : {1.5f, 1.75f, 2.0f}) writeImage(e, output.getChildFile("hidpi-" + juce::String(scale) + ".png"), scale);
    e.setModuleViewsForTests(false, false); writeImage(e, output.getChildFile("synth.png"));
    e.setBoardEditorViewForTests(true);
    auto& fx = e.boardPanelForTests(); check(fx.fxCombo().getNumItems() == 17, "16 effects plus Off preserved");
    for (int i = 0; i <= 16; ++i) {
        fx.fxCombo().setSelectedItemIndex(i, juce::sendNotificationSync);
        check(get(p, swaraxt::IDs::dspFxProgram) == i, "FX selection reaches APVTS");
        check(fx.replayButton().isVisible() == (i == 15), "looper replay context");
    }
    writeImage(e, output.getChildFile("fx.png")); e.setBoardEditorViewForTests(false);
    std::puts("UI: unified controls, lock, random actions, 17 FX choices; rendered 5 skins x 3 sizes x 2 modes and HiDPI");
}
}
int main(int argc, char** argv) {
    juce::ScopedJuceInitialiser_GUI init;
    const juce::File output(argc > 1 ? juce::String(argv[1]) : juce::File::getCurrentWorkingDirectory().getChildFile("workflow-smoke").getFullPathName());
    check(output.createDirectory().wasOk(), "create evidence directory");
    lockChecks(output); randomChecks(); uiChecks(output);
    std::printf("Workflow smoke: %s (%d failures)\n", failures ? "FAIL" : "PASS", failures);
    return failures ? 1 : 0;
}
