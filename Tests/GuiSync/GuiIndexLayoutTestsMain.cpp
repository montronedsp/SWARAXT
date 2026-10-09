// Copyright 2026 MontroneDSP.
// SPDX-License-Identifier: GPL-3.0-or-later
// Exercises MainPanel ENV/LFO layout indexes after explicit size_t conversions.

#include <JuceHeader.h>

#include "Plugin/PluginEditor.h"
#include "Plugin/PluginProcessor.h"

#include <cstdio>

namespace {

int gFailures = 0;

void expect(bool ok, const char* name)
{
    if (! ok)
    {
        std::printf("FAIL: %s\n", name);
        ++gFailures;
        return;
    }
    std::printf("PASS: %s\n", name);
}

bool hasPositiveArea(const juce::Component& c)
{
    return c.getWidth() > 0 && c.getHeight() > 0;
}

void checkLayout(SwaraXtAudioProcessorEditor& editor, const char* label)
{
    editor.resized();
    char name[128];
    auto named = [&](const char* check) {
        std::snprintf(name, sizeof(name), "%s: %s", label, check);
        return name;
    };

    expect(hasPositiveArea(editor.envelopeKnobForTests(0)), named("env first index"));
    expect(hasPositiveArea(editor.envelopeKnobForTests(7)), named("env last index"));
    bool allEnv = true;
    for (int i = 0; i < 8; ++i)
        allEnv = allEnv && hasPositiveArea(editor.envelopeKnobForTests(i));
    expect(allEnv, named("env ADSR 0..7"));

    for (int bank = 0; bank < 2; ++bank)
    {
        char wave[64], retrig[64], sync[64], division[64], rate[64], attack[64];
        std::snprintf(wave, sizeof(wave), "lfo%d wave", bank + 1);
        std::snprintf(retrig, sizeof(retrig), "lfo%d retrig", bank + 1);
        std::snprintf(sync, sizeof(sync), "lfo%d sync", bank + 1);
        std::snprintf(division, sizeof(division), "lfo%d division", bank + 1);
        std::snprintf(rate, sizeof(rate), "lfo%d rate", bank + 1);
        std::snprintf(attack, sizeof(attack), "lfo%d attack", bank + 1);
        expect(hasPositiveArea(editor.lfoWaveComboForTests(bank)), named(wave));
        expect(hasPositiveArea(editor.lfoRetrigComboForTests(bank)), named(retrig));
        expect(hasPositiveArea(editor.lfoSyncComboForTests(bank)), named(sync));
        expect(hasPositiveArea(editor.lfoDivisionComboForTests(bank)), named(division));
        expect(hasPositiveArea(editor.lfoRateKnobForTests(bank)), named(rate));
        expect(hasPositiveArea(editor.lfoAttackKnobForTests(bank)), named(attack));
    }
    expect(hasPositiveArea(editor.filterKeyTrackForTests()), named("filter key adjacent"));
}

}

int main()
{
    juce::ScopedJuceInitialiser_GUI juce;
    SwaraXtAudioProcessor processor;
    processor.prepareToPlay(48000.0, 64);
    SwaraXtAudioProcessorEditor editor(processor);
    editor.setVisible(true);

    checkLayout(editor, "default");
    editor.setGuiSizeForTests(swaraxt::ui::GuiSize::small);
    checkLayout(editor, "small");
    editor.setGuiSizeForTests(swaraxt::ui::GuiSize::large);
    checkLayout(editor, "large");
    editor.setModuleViewsForTests(false, true);
    checkLayout(editor, "sequencer");
    editor.setModuleViewsForTests(false, false);
    checkLayout(editor, "synth");
    editor.setSkinForTests(swaraxt::ui::SkinId::pastel);
    checkLayout(editor, "skin");

    juce::MemoryBlock state;
    processor.getStateInformation(state);
    processor.setStateInformation(state.getData(), static_cast<int>(state.getSize()));
    checkLayout(editor, "restore");

    std::printf("GUI index layout failures=%d\n", gFailures);
    return gFailures == 0 ? 0 : 1;
}
