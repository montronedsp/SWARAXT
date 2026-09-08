// Copyright 2026 MontroneDSP. SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
#include "Ui/SwaraXtPanels.h"

namespace swaraxt::ui {
class BoardPanel final : public juce::Component, private juce::ComboBox::Listener {
public:
    BoardPanel();
    ~BoardPanel() override;
    void attach(juce::AudioProcessorValueTreeState&);
    void resized() override;
    void lookAndFeelChanged() override;
    juce::ComboBox& modelCombo() noexcept { return model_.combo(); }
    juce::ComboBox& fxCombo() noexcept { return fx_.combo(); }
    juce::ComboBox& routeCombo() noexcept { return route_.combo(); }
    juce::ComboBox& conditioningCombo() noexcept { return conditioning_.combo(); }
    SwaraXtKnob& postMixerKnob() noexcept { return postMixer_; }
    SwaraXtKnob& control1() noexcept { return control1_; }
    SwaraXtKnob& control2() noexcept { return control2_; }
    juce::ToggleButton& replayButton() noexcept { return replay_; }
private:
    void comboBoxChanged(juce::ComboBox*) override;
    void refreshContext();
    SwaraXtSelector model_ { "FILTER MODEL", {} };
    SwaraXtSelector conditioning_ { "INPUT", {} };
    SwaraXtSelector fx_ { "FX", {} };
    SwaraXtSelector route_ { "BOARD ROUTE", {} };
    SwaraXtKnob postMixer_ { "POST MIXER" };
    SwaraXtKnob control1_ { "" }, control2_ { "" };
    juce::ToggleButton replay_ { "Replay" };
    std::unique_ptr<juce::AudioProcessorValueTreeState::ComboBoxAttachment> modelAttachment_, fxAttachment_, routeAttachment_, conditioningAttachment_;
    std::unique_ptr<juce::AudioProcessorValueTreeState::SliderAttachment> control1Attachment_, control2Attachment_, postMixerAttachment_;
    std::unique_ptr<juce::ParameterAttachment> replayAttachment_;
};
}
