// Copyright 2026 MontroneDSP.
// SPDX-License-Identifier: GPL-3.0-or-later

#include "Ui/SwaraXtDualSvfPanel.h"
#include "Plugin/PluginProcessor.h"
#include "Ui/SwaraXtTypography.h"
#include "Ui/SwaraXtUiPalette.h"
#include <array>

namespace swaraxt::ui {

namespace {
class SegmentButton final : public juce::TextButton {
 public:
    explicit SegmentButton(const juce::String& caption) : juce::TextButton(caption) {}
    void paintButton(juce::Graphics& g, bool over, bool down) override
    {
        juce::TextButton::paintButton(g, over, down);
        if (hasKeyboardFocus(false))
        {
            g.setColour(Palette::skin().focus);
            g.drawRoundedRectangle(getLocalBounds().toFloat().reduced(1.5f), 4.0f, 1.5f);
        }
    }
};

constexpr std::array<ShruthiSvf2pMode, 3> modes {
    ShruthiSvf2pMode::lowPass, ShruthiSvf2pMode::bandPass, ShruthiSvf2pMode::highPass
};
constexpr std::array<ShruthiSvfDualRouting, 3> routes {
    ShruthiSvfDualRouting::section1, ShruthiSvfDualRouting::serial, ShruthiSvfDualRouting::parallel
};
constexpr std::array<ShruthiSvfCalibration, 2> calibrations {
    ShruthiSvfCalibration::paperApproved, ShruthiSvfCalibration::eagleHardware
};
}

SvfSegments::SvfSegments(const juce::String& name, const juce::StringArray& choices)
{
    setName(name);
    for (int i = 0; i < choices.size(); ++i)
    {
        auto button = std::make_unique<SegmentButton>(choices[i]);
        button->setName(name + ": " + choices[i]);
        button->setTitle(button->getName());
        button->setTooltip(button->getName());
        button->setWantsKeyboardFocus(true);
        button->setClickingTogglesState(true);
        button->setRadioGroupId(1);
        button->onClick = [this, i] {
            // JUCE also notifies the radio button that is being switched off.
            if (buttons_[static_cast<size_t>(i)]->getToggleState() && onSelect) onSelect(i);
        };
        addAndMakeVisible(*button);
        buttons_.push_back(std::move(button));
    }
    lookAndFeelChanged();
}

void SvfSegments::resized()
{
    const int count = static_cast<int>(buttons_.size());
    for (int i = 0; i < count; ++i)
    {
        const int left = getWidth() * i / count;
        const int right = getWidth() * (i + 1) / count;
        buttons_[static_cast<size_t>(i)]->setBounds(left, 0, right - left - (i + 1 < count ? 3 : 0), getHeight());
    }
}

void SvfSegments::lookAndFeelChanged()
{
    for (auto& button : buttons_)
    {
        button->setColour(juce::TextButton::textColourOffId, Palette::skin().primaryText);
        button->setColour(juce::TextButton::textColourOnId, Palette::skin().primaryText);
    }
}

void SvfSegments::setSelectedIndex(int index)
{
    for (size_t i = 0; i < buttons_.size(); ++i)
        buttons_[i]->setToggleState(static_cast<int>(i) == index, juce::dontSendNotification);
}

int SvfSegments::selectedIndex() const noexcept
{
    for (size_t i = 0; i < buttons_.size(); ++i)
        if (buttons_[i]->getToggleState()) return static_cast<int>(i);
    return -1;
}

DualSvfPanel::DualSvfPanel(SwaraXtAudioProcessor& processor) : processor_(processor)
{
    setName("Experimental Dual SVF controls");
    setFocusContainerType(juce::Component::FocusContainerType::keyboardFocusContainer);
    for (auto* label : { &title_, &requested_, &pathTitle_, &path_, &filter1Title_, &filter2Title_,
                         &section2Note_, &calibrationTitle_, &paper_, &eagle_, &session_ })
    {
        addAndMakeVisible(*label);
        setTextRole(*label, TextRole::parameterLabel);
        label->setJustificationType(juce::Justification::centred);
    }
    for (auto* label : { &title_, &pathTitle_, &filter1Title_, &filter2Title_, &calibrationTitle_ })
        setTextRole(*label, TextRole::sectionHeader);
    title_.setText("DUAL SVF", juce::dontSendNotification);
    title_.setJustificationType(juce::Justification::centredLeft);
    pathTitle_.setText("FILTER PATH", juce::dontSendNotification);
    filter1Title_.setText("FILTER 1", juce::dontSendNotification);
    filter2Title_.setText("FILTER 2", juce::dontSendNotification);
    calibrationTitle_.setText("CALIBRATION", juce::dontSendNotification);
    paper_.setText(juce::String::fromUTF8(u8"220 k\u03a9 / 4.7 V"), juce::dontSendNotification);
    eagle_.setText(juce::String::fromUTF8(u8"330 k\u03a9 / 3.6 V"), juce::dontSendNotification);
    session_.setText("EXPERIMENTAL / SESSION ONLY", juce::dontSendNotification);
    setTextRole(session_, TextRole::smallIndex);
    for (auto* segments : { &routing_, &mode1_, &mode2_, &calibration_ })
        addAndMakeVisible(*segments);
    addAndMakeVisible(close_);
    close_.setTooltip("Close Dual SVF controls (Escape)");
    close_.onClick = [this] { if (closeRequested) closeRequested(); };
    routing_.onSelect = [this](int index) {
        processor_.setExperimentalSvfRouting(routes[static_cast<size_t>(index)]);
        published();
    };
    mode1_.onSelect = [this](int index) {
        processor_.setExperimentalSvfMode1(modes[static_cast<size_t>(index)]);
        published();
    };
    mode2_.onSelect = [this](int index) {
        processor_.setExperimentalSvfMode2(modes[static_cast<size_t>(index)]);
        published();
    };
    calibration_.onSelect = [this](int index) {
        processor_.setExperimentalSvfCalibration(calibrations[static_cast<size_t>(index)]);
        published();
    };
    lookAndFeelChanged();
    sync(processor_.experimentalSvfSelection());
}

void DualSvfPanel::published()
{
    if (selectionChanged) selectionChanged();
    else sync(processor_.experimentalSvfSelection());
}

void DualSvfPanel::sync(const ExperimentalSvfSelection& requested)
{
    routing_.setSelectedIndex(static_cast<int>(requested.routing));
    mode1_.setSelectedIndex(static_cast<int>(requested.mode1));
    mode2_.setSelectedIndex(static_cast<int>(requested.mode2));
    calibration_.setSelectedIndex(static_cast<int>(requested.calibration));
    requested_.setText(requested.enabled ? "REQUESTED: DUAL SVF" : "REQUESTED: CLASSIC IR3109",
                       juce::dontSendNotification);
    const bool section2Active = requested.routing != ShruthiSvfDualRouting::section1;
    section2Note_.setText(section2Active ? "Shared cutoff / resonance" : "Filter 2 outside audible path",
                          juce::dontSendNotification);
    const char* paths[] = { u8"MIX \u2192 F1 \u2192 VCA", u8"MIX \u2192 F1 \u2192 F2 \u2192 VCA",
                            u8"MIX \u2192 (F1 + F2) \u2192 VCA" };
    path_.setText(juce::String::fromUTF8(paths[static_cast<size_t>(requested.routing)]), juce::dontSendNotification);
    for (auto* segments : { &routing_, &mode1_, &mode2_, &calibration_ })
    {
        segments->setEnabled(requested.enabled);
        segments->setAlpha(requested.enabled ? 1.0f : 0.45f);
    }
    // Section 2 keeps its requested mode while the reference path bypasses it.
    mode2_.setAlpha(requested.enabled && section2Active ? 1.0f : 0.45f);
}

void DualSvfPanel::resized()
{
    title_.setBounds(16, 10, 200, 24);
    close_.setBounds(getWidth() - 66, 12, 50, 22);
    requested_.setBounds(16, 38, getWidth() - 32, 18);
    pathTitle_.setBounds(16, 66, getWidth() - 32, 17);
    routing_.setBounds(16, 88, getWidth() - 32, 28);
    path_.setBounds(16, 124, getWidth() - 32, 23);
    const int half = (getWidth() - 44) / 2;
    filter1Title_.setBounds(16, 162, half, 18);
    filter2Title_.setBounds(28 + half, 162, half, 18);
    mode1_.setBounds(16, 186, half, 28);
    mode2_.setBounds(28 + half, 186, half, 28);
    section2Note_.setBounds(16, 220, getWidth() - 32, 16);
    calibrationTitle_.setBounds(16, 248, getWidth() - 32, 18);
    calibration_.setBounds(16, 272, getWidth() - 32, 28);
    paper_.setBounds(16, 304, (getWidth() - 32) / 2, 18);
    eagle_.setBounds(getWidth() / 2, 304, (getWidth() - 32) / 2, 18);
    session_.setBounds(16, 329, getWidth() - 32, 16);
}

void DualSvfPanel::paint(juce::Graphics& g)
{
    g.setColour(Palette::panel());
    g.fillRoundedRectangle(getLocalBounds().toFloat(), 6.0f);
    g.setColour(Palette::line());
    g.drawRoundedRectangle(getLocalBounds().toFloat().reduced(0.75f), 6.0f, 1.5f);
    for (float y : { 59.0f, 154.0f, 241.0f })
        g.drawHorizontalLine(juce::roundToInt(y), 16.0f, static_cast<float>(getWidth() - 16));
}

void DualSvfPanel::lookAndFeelChanged()
{
    for (auto* label : { &title_, &pathTitle_, &filter1Title_, &filter2Title_, &calibrationTitle_ })
        label->setColour(juce::Label::textColourId, Palette::skin().primaryText);
    for (auto* label : { &requested_, &path_, &section2Note_, &paper_, &eagle_, &session_ })
        label->setColour(juce::Label::textColourId, Palette::skin().secondaryText);
    close_.setColour(juce::TextButton::textColourOffId, Palette::skin().primaryText);
    repaint();
}

bool DualSvfPanel::keyPressed(const juce::KeyPress& key)
{
    if (key == juce::KeyPress::escapeKey && closeRequested)
    {
        closeRequested();
        return true;
    }
    return false;
}

}  // namespace swaraxt::ui
