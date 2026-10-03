// Copyright 2026 MontroneDSP. SPDX-License-Identifier: GPL-3.0-or-later
#include "Plugin/PluginEditor.h"
#include "Plugin/SequenceParameters.h"
#include "Plugin/ApvtsFactoryPresets.h"
#include "Ui/SwaraXtBoardPanel.h"
#include "Ui/SwaraXtText.h"
#include "Ui/SwaraXtLookAndFeel.h"
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
    p.setRandomNoteRange({ -24, 0 });
    for (int program : {1, swaraxt::kShruthiFactoryPresetStart,
                         swaraxt::kShruthiFactoryPresetStart + 28, swaraxt::kUserFactoryPresetStart}) {
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
    check(xml && !xml->hasAttribute("sequenceLocked") && !xml->hasAttribute("randomNoteLower")
              && !xml->hasAttribute("randomNoteUpper"), "musical preset excludes workflow preferences");
    p.setSequenceLocked(false);
    check(p.randomNoteRange().lower == -24 && p.randomNoteRange().upper == 0, "random range persists across presets/session");
    check(p.loadPresetEntry(user, error), "unlocked user preset loads");
    check(!p.sequenceLocked() && !same(pattern, p.sequenceState().snapshot()), "lock off loads preset sequence normally");
    // Older sessions have no workflow property and must reset the preference.
    if (xml) {
        juce::MemoryBlock old; juce::AudioProcessor::copyXmlToBinary(*xml, old);
        p.setSequenceLocked(true); p.setStateInformation(old.getData(), static_cast<int>(old.getSize()));
        check(!p.sequenceLocked(), "old host session defaults lock off");
        check(p.randomNoteRange().lower == -12 && p.randomNoteRange().upper == 12, "old session defaults symmetric range");
    }
    std::puts("Lock: native/imported/APVTS factory, user, GUI load, host state and engine publication checked");
}
juce::Slider* rangeControl(juce::Component& c) {
    if (auto* slider = dynamic_cast<juce::Slider*>(&c); slider && slider->getSliderStyle() == juce::Slider::TwoValueHorizontal) return slider;
    for (auto* child : c.getChildren()) if (auto* slider = rangeControl(*child)) return slider;
    return nullptr;
}
juce::ComboBox* typeControl(juce::Component& c) {
    if (auto* combo = dynamic_cast<juce::ComboBox*>(&c); combo && combo->getNumItems() == 1 && combo->getItemText(0) == "Type 1") return combo;
    for (auto* child : c.getChildren()) if (auto* combo = typeControl(*child)) return combo;
    return nullptr;
}
juce::Button* button(juce::Component& c, const juce::String& name) {
    if (auto* b = dynamic_cast<juce::Button*>(&c); b && b->getButtonText() == name) return b;
    for (auto* child : c.getChildren()) if (auto* b = button(*child, name)) return b;
    return nullptr;
}
void randomChecks() {
    using State = swaraxt::SequenceState;
    using Kind = State::Randomize;
    State state; juce::Random rng(7254);
    for (auto kind : {Kind::notes, Kind::velocity, Kind::events, Kind::value, Kind::sequence}) {
        const auto before = state.snapshot(); state.randomize(kind, rng); const auto after = state.snapshot();
        check(before.length == after.length && before.rotation == after.rotation
            && before.arpPattern == after.arpPattern && before.grooveTemplate == after.grooveTemplate, "random preserves playback settings");
        check(before.steps != after.steps, "random changes musical content");
        for (size_t i = 0; i < after.steps.size(); ++i) {
            uint16_t mask = 0;
            if (kind == Kind::notes) mask = 0xff80;
            if (kind == Kind::velocity) mask = 0x8fff;
            if (kind == Kind::events) mask = 0x7f7f;
            if (kind == Kind::value) mask = 0xf0ff;
            check((before.steps[i] & mask) == (after.steps[i] & mask), "dimension-only randomizer preserves other bytes");
            if (kind == Kind::velocity || kind == Kind::sequence)
                check((after.steps[i] & 0x7000) != 0, "generated velocity domain 1..7");
        }
    }
    for (auto kind : {Kind::notes, Kind::sequence})
        for (auto range : {State::NoteRange{-48,0}, State::NoteRange{0,48}, State::NoteRange{-12,12}, State::NoteRange{0,0}})
            for (int root : {0, 1, 60, 126, 127}) {
                auto pattern = State::defaultSnapshot();
                for (auto& step : pattern.steps) step = static_cast<uint16_t>((step & 0xff80) | root);
                pattern.length = 5; pattern.rotation = 3;
                state.store(pattern);
                state.randomize(kind, rng, range);
                auto after = state.snapshot();
                for (auto step : after.steps) {
                    const int note = step & 127;
                    check(note >= juce::jmax(0, root + range.lower) && note <= juce::jmin(127, root + range.upper), "bipolar range and endpoint clamp");
                }
                if (kind == Kind::sequence) {
                    bool previous = false;
                    for (int logical = 0; logical < 5; ++logical) {
                        const auto step = after.steps[static_cast<size_t>((logical + 3) & 15)];
                        const bool gate = (step & 128) != 0, tie = (step & 32768) != 0;
                        check(!tie || (logical > 0 && gate && previous), "rotated short-pattern tie legality");
                        previous = gate;
                    }
                }
            }
    std::puts("All five randomizers: masks, bipolar ranges, boundaries and ties checked");
}
void textChecks() {
    swaraxt::ui::SwaraXtLookAndFeel lnf;
    for (float height : {12.0f, 15.0f, 18.0f}) {
        const auto font = lnf.regularFont(height);
        for (float width : {24.0f, 44.0f, 120.0f}) {
            const juce::Rectangle<float> area(0.0f, 0.0f, width, 20.0f);
            const float baseline = area.getCentreY() + (font.getAscent() - font.getDescent()) * 0.5f;
            for (const char* text : {"0", "-12", "gyp", "OSC 1", "RANDOM VELOCITY", "0.350000"}) {
                auto glyphs = swaraxt::ui::singleLineText(font, text, area, juce::Justification::centred);
                for (int i = 0; i < glyphs.getNumGlyphs(); ++i)
                    check(std::abs(glyphs.getGlyph(i).getBaselineY() - baseline) < 0.0001f, "font metric baseline independent of text/width");
            }
        }
    }
}
void writeImage(SwaraXtAudioProcessorEditor& e, const juce::File& file, float scale = 1.0f) {
    juce::FileOutputStream stream(file); stream.setPosition(0); stream.truncate(); juce::PNGImageFormat png;
    check(stream.openedOk() && png.writeImageToStream(e.createComponentSnapshot(e.getLocalBounds(), true, scale), stream), "render GUI evidence");
}
void uiChecks(const juce::File& output) {
    SwaraXtAudioProcessor p; p.prepareToPlay(48000, 128);
    SwaraXtAudioProcessorEditor e(p); e.setVisible(true);
    e.setModuleViewsForTests(false, true);
    for (const char* name : {"RANDOM NOTES", "RANDOM VELOCITY", "RANDOM EVENT", "RANDOM VALUE", "RANDOM SEQ"}) {
        auto* b = button(e, name); check(b && b->isVisible(), "random button visible");
        if (b) { auto before = p.sequenceState().snapshot(); b->onClick(); check(!same(before, p.sequenceState().snapshot()), "random UI publishes immediately"); engineMatches(p); }
    }
    auto* type = typeControl(e);
    check(type && type->getSelectedId() == 1, "single selected Type 1 workflow choice");
    auto* range = rangeControl(e);
    check(range != nullptr, "bipolar two-thumb control exists");
    if (range) {
        for (auto limits : {swaraxt::SequenceState::NoteRange{-24,0}, swaraxt::SequenceState::NoteRange{0,24}}) {
            range->setMinAndMaxValues(limits.lower, limits.upper, juce::dontSendNotification); range->onValueChange();
            check(p.randomNoteRange().lower == limits.lower && p.randomNoteRange().upper == limits.upper, "range GUI workflow state");
            const auto before = p.sequenceState().snapshot(); const int root = before.steps[0] & 127;
            button(e, "RANDOM NOTES")->onClick();
            for (auto step : p.sequenceState().snapshot().steps)
                check((step & 127) >= juce::jmax(0, root + limits.lower) && (step & 127) <= juce::jmin(127, root + limits.upper), "range UI controls note generation");
        }
        range->setMinAndMaxValues(-12, 12, juce::dontSendNotification); range->onValueChange();
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
                if (random) for (auto* child : random->getParentComponent()->getChildren())
                    if (child->isVisible()) check(random->getParentComponent()->getLocalBounds().contains(child->getBounds()), "all Seq/Arp controls inside page");
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
    check(fx.getParentComponent() != nullptr && fx.getParentComponent()->getLocalBounds().contains(fx.getBounds()), "FX inside existing Global module");
    const auto fxBounds = fx.getBounds();
    const float program = get(p, swaraxt::IDs::dspFxProgram);
    writeImage(e, output.getChildFile("fx.png")); e.setBoardEditorViewForTests(false);
    check(!fx.isVisible() && fx.getBounds() == fxBounds, "FX swap preserves geometry");
    check(get(p, swaraxt::IDs::dspFxProgram) == program, "FX state unchanged by view swap");
    e.setBoardEditorViewForTests(true);
    check(fx.isVisible() && get(p, swaraxt::IDs::dspFxProgram) == program, "FX returns without state loss");
    for (auto skin : {swaraxt::ui::SkinId::pastel, swaraxt::ui::SkinId::midnightGold, swaraxt::ui::SkinId::neonCobalt,
                      swaraxt::ui::SkinId::jungle, swaraxt::ui::SkinId::rossocorsa}) {
        e.setSkinForTests(skin);
        for (auto size : {swaraxt::ui::GuiSize::small, swaraxt::ui::GuiSize::medium, swaraxt::ui::GuiSize::large}) {
            e.setGuiSizeForTests(size);
            writeImage(e, output.getChildFile("fx-" + juce::String(swaraxt::ui::SkinRegistry::get(skin).stableId)
                                            + "-" + swaraxt::ui::GuiGeometry::stableId(size) + ".png"));
        }
    }
    e.setBoardEditorViewForTests(false);
    std::puts("UI: unified controls, lock, random actions, 17 FX choices; rendered 5 skins x 3 sizes x 2 modes and HiDPI");
}
}
int main(int argc, char** argv) {
    juce::ScopedJuceInitialiser_GUI init;
    const juce::File output(argc > 1 ? juce::String(argv[1]) : juce::File::getCurrentWorkingDirectory().getChildFile("workflow-smoke").getFullPathName());
    check(output.createDirectory().wasOk(), "create evidence directory");
    lockChecks(output); randomChecks(); textChecks(); uiChecks(output);
    std::printf("Workflow smoke: %s (%d failures)\n", failures ? "FAIL" : "PASS", failures);
    return failures ? 1 : 0;
}
