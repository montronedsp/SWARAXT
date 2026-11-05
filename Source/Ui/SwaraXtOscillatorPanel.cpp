// Copyright 2026 MontroneDSP.
// SPDX-License-Identifier: GPL-3.0-or-later

#include "Ui/SwaraXtOscillatorPanel.h"
#include "Ui/SwaraXtTypography.h"

namespace swaraxt::ui {

OscillatorPanel::OscillatorPanel(const juce::String& title, bool hasDetune)
    : title_(title),
      model_("MODEL", oscillatorNames(), true),
      pitch_("PITCH"),
      timbre_("TIMBRE"),
      detune_("DETUNE"),
      hasDetune_(hasDetune)
{
    setTextRole(titleLabel_, TextRole::sectionHeader);
    titleLabel_.setText(title_, juce::dontSendNotification);
    titleLabel_.setJustificationType(juce::Justification::centred);
    addAndMakeVisible(titleLabel_);
    pitch_.slider().setRange(-48.0, 48.0, 1.0);
    pitch_.slider().textFromValueFunction = [](double value) {
        const int semitones = static_cast<int>(std::round(value));
        return (semitones > 0 ? "+" : "") + juce::String(semitones) + " st";
    };
    timbre_.slider().setRange(0.0, 127.0, 1.0);
    timbre_.slider().textFromValueFunction = [](double value) {
        return juce::String(static_cast<int>(std::round(value)));
    };
    detune_.slider().setRange(0.0, 127.0, 1.0);
    detune_.slider().textFromValueFunction = [](double value) {
        return juce::String(value / 128.0, 2) + " st";
    };
    addAndMakeVisible(model_);
    addAndMakeVisible(pitch_);
    addAndMakeVisible(timbre_);
    if (hasDetune_)
        addAndMakeVisible(detune_);
}

void OscillatorPanel::attach(juce::AudioProcessorValueTreeState& apvts,
                             const char* modelId,
                             const char* pitchId,
                             const char* timbreId,
                             const char* detuneId)
{
    setSliderDefault(apvts, pitchId, pitch_.slider());
    setSliderDefault(apvts, timbreId, timbre_.slider());
    modelAttachment_ = std::make_unique<juce::AudioProcessorValueTreeState::ComboBoxAttachment>(
        apvts, modelId, model_.combo());
    pitchAttachment_ = std::make_unique<juce::AudioProcessorValueTreeState::SliderAttachment>(
        apvts, pitchId, pitch_.slider());
    timbreAttachment_ = std::make_unique<juce::AudioProcessorValueTreeState::SliderAttachment>(
        apvts, timbreId, timbre_.slider());
    if (hasDetune_ && detuneId != nullptr)
    {
        setSliderDefault(apvts, detuneId, detune_.slider());
        detuneAttachment_ = std::make_unique<juce::AudioProcessorValueTreeState::SliderAttachment>(
            apvts, detuneId, detune_.slider());
    }
}

void OscillatorPanel::paint(juce::Graphics& g)
{
    paintSplitDivider(g, static_cast<float>(getWidth()), titleLabel_.getBounds().toFloat());
}

void OscillatorPanel::resized()
{
    titleLabel_.setBounds((getWidth() - 64) / 2, 1, 64, 20);
    auto area = getLocalBounds().withTrimmedTop(24);
    const int modelWidth = 104;
    model_.setBounds(area.removeFromLeft(modelWidth));
    area.removeFromLeft(4);
    if (hasDetune_)
        layoutRow(area, { &pitch_, &timbre_, &detune_ }, 3);
    else
        layoutRow(area, { &pitch_, &timbre_ }, 4);
}

void OscillatorPanel::lookAndFeelChanged()
{
    titleLabel_.setColour(juce::Label::textColourId, Palette::skin().moduleHeaderText);
}

bool OscillatorPanel::modelIsSelector() const noexcept
{
    return model_.combo().isVisible();
}

bool OscillatorPanel::pitchIsRotary() const noexcept
{
    return pitch_.slider().getSliderStyle() == juce::Slider::RotaryHorizontalVerticalDrag;
}

bool OscillatorPanel::waveVariationIsRotary() const noexcept
{
    return timbre_.slider().getSliderStyle() == juce::Slider::RotaryHorizontalVerticalDrag;
}

}  // namespace swaraxt::ui
