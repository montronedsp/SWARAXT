// Copyright 2026 MontroneDSP.
// SPDX-License-Identifier: GPL-3.0-or-later

#include "Ui/SwaraXtPanelWidgets.h"
#include "Ui/SwaraXtTypography.h"

namespace swaraxt::ui {

SwaraXtKnob::SwaraXtKnob(const juce::String& labelText, KnobSize size)
    : size_(size)
{
    const bool large = size_ == KnobSize::large;
    addAndMakeVisible(label_);
    setTextRole(label_, TextRole::parameterLabel);
    label_.setText(labelText, juce::dontSendNotification);
    label_.setJustificationType(juce::Justification::centred);
    label_.setColour(juce::Label::textColourId, large ? Palette::cream() : Palette::mutedText());

    addAndMakeVisible(slider_);
    slider_.setSliderStyle(juce::Slider::RotaryHorizontalVerticalDrag);
    slider_.setTextBoxStyle(juce::Slider::TextBoxBelow, false,
                            large ? Layout::valueBoxWide : Layout::valueBoxWidth,
                            Layout::valueBoxHeight);
    slider_.setRotaryParameters(juce::MathConstants<float>::pi * 1.25f,
                                juce::MathConstants<float>::pi * 2.75f,
                                true);
    slider_.getProperties().set("swaraxtKnobSize", large ? "large" : "standard");
    const int diameter = large ? Layout::knobLarge : Layout::knobStandard;
    setSize(diameter + 12, diameter + 48);
}

void SwaraXtKnob::resized()
{
    auto area = getLocalBounds();
    label_.setBounds(area.removeFromTop(Layout::knobLabelHeight));
    slider_.setBounds(area);
}

void SwaraXtKnob::lookAndFeelChanged()
{
    label_.setColour(juce::Label::textColourId,
                     size_ == KnobSize::large ? Palette::cream() : Palette::mutedText());
}

SwaraXtSelector::SwaraXtSelector(const juce::String& labelText,
                                 const juce::StringArray& items,
                                 bool primary)
    : primary_(primary)
{
    addAndMakeVisible(label_);
    setTextRole(label_, TextRole::parameterLabel);
    label_.setText(labelText, juce::dontSendNotification);
    label_.setJustificationType(juce::Justification::centredLeft);
    label_.setColour(juce::Label::textColourId, primary ? Palette::cream() : Palette::mutedText());

    combo_.addItemList(items, 1);
    combo_.setTooltip(labelText);
    combo_.setJustificationType(juce::Justification::centredLeft);
    combo_.onChange = [this] { refreshDescription(); };
    addAndMakeVisible(combo_);

    description_.setJustificationType(juce::Justification::centredLeft);
    setTextRole(description_, TextRole::comboValue);
    description_.setColour(juce::Label::textColourId, Palette::mutedText());
    addAndMakeVisible(description_);

    refreshDescription();
}

void SwaraXtSelector::resized()
{
    auto area = getLocalBounds();
    label_.setBounds(area.removeFromTop(Layout::selectorLabelHeight));
    combo_.setBounds(area.removeFromTop(Layout::selectorHeight));
    if (area.getHeight() >= Layout::selectorDescriptionHeight)
        description_.setBounds(area.removeFromTop(Layout::selectorDescriptionHeight));
}

void SwaraXtSelector::lookAndFeelChanged()
{
    label_.setColour(juce::Label::textColourId,
                     primary_ ? Palette::cream() : Palette::mutedText());
    description_.setColour(juce::Label::textColourId, Palette::mutedText());
}

void SwaraXtSelector::refreshDescription()
{
    const auto text = combo_.getText();
    description_.setText(text.isEmpty() ? "Select" : text, juce::dontSendNotification);
}

SwaraXtModulePanel::SwaraXtModulePanel(const juce::String& titleText,
                                       const juce::String& secondaryTitleText,
                                       int secondaryDividerY)
    : secondaryDividerY_(secondaryDividerY)
{
    addAndMakeVisible(title_);
    setTextRole(title_, TextRole::sectionHeader);
    title_.setText(titleText, juce::dontSendNotification);
    title_.setJustificationType(juce::Justification::centred);
    addAndMakeVisible(secondaryTitle_);
    setTextRole(secondaryTitle_, TextRole::sectionHeader);
    secondaryTitle_.setText(secondaryTitleText, juce::dontSendNotification);
    secondaryTitle_.setJustificationType(juce::Justification::centred);
    addAndMakeVisible(body_);
    lookAndFeelChanged();
}

void SwaraXtModulePanel::paint(juce::Graphics& g)
{
    const auto paintDivider = [&g, this](const juce::Label& label,
                                         juce::Rectangle<int> actionBounds = {})
    {
        if (label.getText().isEmpty())
            return;
        paintSplitDivider(g, static_cast<float>(getWidth()), label.getBounds().toFloat(),
                          actionBounds.toFloat());
    };
    paintDivider(title_, primaryActionBounds_);
    if (secondaryHeaderVisible_)
        paintDivider(secondaryTitle_, secondaryActionBounds_);
}

void SwaraXtModulePanel::setSecondaryActionBounds(juce::Rectangle<int> bounds)
{
    secondaryActionBounds_ = bounds;
    repaint();
}

void SwaraXtModulePanel::setPrimaryActionBounds(juce::Rectangle<int> bounds)
{
    primaryActionBounds_ = bounds;
    repaint();
}

void SwaraXtModulePanel::setSecondaryHeaderVisible(bool visible)
{
    if (secondaryHeaderVisible_ == visible)
        return;

    secondaryHeaderVisible_ = visible;
    secondaryTitle_.setVisible(visible);
    resized();
    repaint();
}

SwaraXtDepthSlider::SwaraXtDepthSlider()
{
    slider_.setSliderStyle(juce::Slider::LinearHorizontal);
    slider_.setRange(-63.0, 63.0, 1.0);
    slider_.setTextBoxStyle(juce::Slider::TextBoxRight, false, 31, 18);
    slider_.setTextValueSuffix("");
    slider_.textFromValueFunction = [](double value) {
        const int depth = static_cast<int>(std::round(value));
        return (depth > 0 ? "+" : "") + juce::String(depth);
    };
    slider_.setTooltip("Bipolar modulation depth");
    addAndMakeVisible(slider_);
}

void SwaraXtDepthSlider::resized()
{
    slider_.setBounds(getLocalBounds());
}

void SwaraXtModulePanel::resized()
{
    auto area = getLocalBounds();
    if (title_.getText().isNotEmpty())
    {
        const int width = juce::jmax(64, title_.getText().length() * 8 + 20);
        title_.setBounds((getWidth() - width) / 2, 1, width, 20);
        area.removeFromTop(Layout::moduleHeaderRow);
    }
    else
        title_.setBounds({});
    if (secondaryHeaderVisible_ && secondaryTitle_.getText().isNotEmpty()
        && secondaryDividerY_ > 0)
    {
        const int width = juce::jmax(64, secondaryTitle_.getText().length() * 8 + 20);
        secondaryTitle_.setBounds((getWidth() - width) / 2,
                                  secondaryDividerY_ - 10, width, 20);
    }
    else
        secondaryTitle_.setBounds({});
    body_.setBounds(area.reduced(Layout::moduleInnerPad));
}

void SwaraXtModulePanel::lookAndFeelChanged()
{
    title_.setColour(juce::Label::textColourId, Palette::skin().moduleHeaderText);
    secondaryTitle_.setColour(juce::Label::textColourId, Palette::skin().moduleHeaderText);
}

EnvelopeTrace::~EnvelopeTrace()
{
    stopTimer();
}

void EnvelopeTrace::bind(juce::AudioProcessorValueTreeState& apvts,
                         const char* attack,
                         const char* decay,
                         const char* sustain,
                         const char* release)
{
    apvts_ = &apvts;
    attack_ = attack;
    decay_ = decay;
    sustain_ = sustain;
    release_ = release;
    startTimerHz(12);
}

void EnvelopeTrace::paint(juce::Graphics& g)
{
    const auto frameBounds = getLocalBounds().toFloat();
    auto area = frameBounds.reduced(1.0f);
    paintInset(g, area);
    area = area.reduced(8.0f, 7.0f);

    const float a = read01(apvts_, attack_, 0.0f, 127.0f);
    const float d = read01(apvts_, decay_, 0.0f, 127.0f);
    const float s = read01(apvts_, sustain_, 0.0f, 127.0f);
    const float r = read01(apvts_, release_, 0.0f, 127.0f);

    const float startX = area.getX();
    const float bottomY = area.getBottom();
    const float topY = area.getY();
    const float sustainY = juce::jmap(s, bottomY, topY);
    const float attackX = startX + area.getWidth() * (0.12f + 0.18f * a);
    const float decayX = attackX + area.getWidth() * (0.12f + 0.18f * d);
    const float holdX = area.getRight() - area.getWidth() * (0.12f + 0.20f * r);

    juce::Path envelope;
    envelope.startNewSubPath(startX, bottomY);
    envelope.lineTo(attackX, topY);
    envelope.lineTo(decayX, sustainY);
    envelope.lineTo(juce::jmax(decayX + 4.0f, holdX), sustainY);
    envelope.lineTo(area.getRight(), bottomY);

    g.setColour(Palette::grid());
    for (int i = 1; i < 4; ++i)
    {
        const float y = area.getY() + area.getHeight() * static_cast<float>(i) / 4.0f;
        g.drawHorizontalLine(static_cast<int>(std::round(y)), area.getX(), area.getRight());
    }

    g.setColour(Palette::accent());
    g.strokePath(envelope, juce::PathStrokeType(2.2f, juce::PathStrokeType::curved,
                                                juce::PathStrokeType::rounded));
    if (frame_ != nullptr)
        frame_->drawWithin(g, frameBounds, juce::RectanglePlacement::stretchToFit, 1.0f);
}

void EnvelopeTrace::timerCallback()
{
    repaint();
}

LfoTrace::~LfoTrace()
{
    stopTimer();
}

void LfoTrace::bind(juce::AudioProcessorValueTreeState& apvts,
                    const char* waveform,
                    const char* rate)
{
    apvts_ = &apvts;
    waveform_ = waveform;
    rate_ = rate;
    startTimerHz(15);
}

void LfoTrace::paint(juce::Graphics& g)
{
    const auto frameBounds = getLocalBounds().toFloat();
    auto area = frameBounds.reduced(1.0f);
    paintInset(g, area);
    area = area.reduced(8.0f, 7.0f);

    const int waveform = static_cast<int>(std::round(readValue(apvts_, waveform_, 0.0f)));
    const float rate = read01(apvts_, rate_, 0.0f, 127.0f);

    juce::Path path;
    const int points = 64;
    for (int i = 0; i < points; ++i)
    {
        const float x01 = static_cast<float>(i) / static_cast<float>(points - 1);
        const float phase = std::fmod(x01 + phase_, 1.0f);
        const float y01 = lfoVisualizerY01(waveform, phase);

        const float x = area.getX() + area.getWidth() * x01;
        const float y = juce::jmap(y01, area.getBottom(), area.getY());
        if (i == 0)
            path.startNewSubPath(x, y);
        else
            path.lineTo(x, y);
    }

    g.setColour(Palette::grid());
    g.drawHorizontalLine(static_cast<int>(std::round(area.getCentreY())),
                         area.getX(),
                         area.getRight());
    g.setColour(Palette::secondaryAccent());
    g.strokePath(path, juce::PathStrokeType(2.0f, juce::PathStrokeType::curved,
                                            juce::PathStrokeType::rounded));
    if (frame_ != nullptr)
        frame_->drawWithin(g, frameBounds, juce::RectanglePlacement::stretchToFit, 1.0f);

    phase_ = std::fmod(phase_ + 0.004f + rate * 0.015f, 1.0f);
}

void LfoTrace::timerCallback()
{
    repaint();
}

}  // namespace swaraxt::ui
