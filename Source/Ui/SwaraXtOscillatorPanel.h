// Copyright 2026 MontroneDSP.
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include <JuceHeader.h>

#include "Ui/SwaraXtPanelWidgets.h"

namespace swaraxt::ui {

class OscillatorPanel : public juce::Component {
 public:
    OscillatorPanel(const juce::String& title, bool hasDetune);
    void attach(juce::AudioProcessorValueTreeState& apvts,
                const char* modelId,
                const char* pitchId,
                const char* timbreId,
                const char* detuneId = nullptr);
    void paint(juce::Graphics& g) override;
    void resized() override;
    void lookAndFeelChanged() override;
    bool modelIsSelector() const noexcept;
    bool pitchIsRotary() const noexcept;
    bool waveVariationIsRotary() const noexcept;
    bool detuneIsVisible() const noexcept { return detune_.isVisible(); }
    juce::Slider& pitchSlider() noexcept { return pitch_.slider(); }
    juce::Slider& waveVariationSlider() noexcept { return timbre_.slider(); }
    juce::ComboBox& modelComboForTests() noexcept { return model_.combo(); }
    const juce::ComboBox& modelComboForTests() const noexcept { return model_.combo(); }

 private:
    juce::String title_;
    juce::Label titleLabel_;
    SwaraXtSelector model_;
    SwaraXtKnob pitch_;
    SwaraXtKnob timbre_;
    SwaraXtKnob detune_;
    bool hasDetune_ = false;
    std::unique_ptr<juce::AudioProcessorValueTreeState::ComboBoxAttachment> modelAttachment_;
    std::unique_ptr<juce::AudioProcessorValueTreeState::SliderAttachment> pitchAttachment_;
    std::unique_ptr<juce::AudioProcessorValueTreeState::SliderAttachment> timbreAttachment_;
    std::unique_ptr<juce::AudioProcessorValueTreeState::SliderAttachment> detuneAttachment_;
};

}  // namespace swaraxt::ui
