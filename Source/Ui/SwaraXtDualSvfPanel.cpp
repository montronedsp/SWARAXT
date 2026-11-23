// Copyright 2026 MontroneDSP.
// SPDX-License-Identifier: GPL-3.0-or-later

#include "Ui/SwaraXtDualSvfPanel.h"
#include "Ui/SwaraXtPanels.h"
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
    for (auto* label : { &title_, &requested_, &path_, &filter1Title_, &filter2Title_,
                         &section2Note_, &calibrationTitle_, &paper_, &eagle_, &session_ })
    {
        addAndMakeVisible(*label);
        setTextRole(*label, TextRole::parameterLabel);
        label->setJustificationType(juce::Justification::centred);
    }
    for (auto* label : { &title_, &filter1Title_, &filter2Title_, &calibrationTitle_ })
        setTextRole(*label, TextRole::sectionHeader);
    title_.setText("DUAL SVF", juce::dontSendNotification);
    title_.setJustificationType(juce::Justification::centredLeft);
    filter1Title_.setText("FILTER 1", juce::dontSendNotification);
    filter2Title_.setText("FILTER 2", juce::dontSendNotification);
    calibrationTitle_.setText("CALIBRATION", juce::dontSendNotification);
    paper_.setText(juce::String::fromUTF8(u8"220 k\u03a9 / 4.7 V"), juce::dontSendNotification);
    eagle_.setText(juce::String::fromUTF8(u8"330 k\u03a9 / 3.6 V"), juce::dontSendNotification);
    session_.setText("SESSION ONLY", juce::dontSendNotification);
    setTextRole(session_, TextRole::smallIndex);
    for (auto* segments : { &routing_, &mode1_, &mode2_, &calibration_, &control2_ })
        addAndMakeVisible(*segments);
    cutoff2_ = std::make_unique<SwaraXtKnob>("CUTOFF 2");
    resonance2_ = std::make_unique<SwaraXtKnob>("RESONANCE 2");
    addAndMakeVisible(*cutoff2_);
    addAndMakeVisible(*resonance2_);
    cutoff2_->slider().setRange(0, 127, 1);
    resonance2_->slider().setRange(0, 63, 1);
    cutoff2_->slider().setName("Filter 2 cutoff / relative offset");
    resonance2_->slider().setName("Filter 2 resonance");
    cutoff2_->slider().setTooltip("Independent: 0-127. Relative: 64 = zero, 52 = -12 semitones, 76 = +12. CV1 matrix rows also retain their FX destination.");
    resonance2_->slider().setTooltip("Filter 2 resonance: 0-63. CV2 matrix rows also retain their FX destination.");
    control2_.buttonForTests(0).setTooltip("Legacy Follow: Filter 1 controls both cutoff and resonance");
    control2_.buttonForTests(1).setTooltip("Independent Filter 2 cutoff and resonance");
    control2_.buttonForTests(2).setTooltip("Cutoff offset from Filter 1; independent resonance; 64 = zero offset");
    cutoff2_->slider().onValueChange = [this] {
        processor_.setExperimentalSvfCutoff2(juce::roundToInt(cutoff2_->slider().getValue()));
        published();
    };
    resonance2_->slider().onValueChange = [this] {
        processor_.setExperimentalSvfResonance2(juce::roundToInt(resonance2_->slider().getValue()));
        published();
    };
    control2_.onSelect = [this](int index) {
        processor_.setExperimentalSvfControl2(static_cast<ShruthiSvfSection2Control>(index));
        published();
    };
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

DualSvfPanel::~DualSvfPanel() = default;
juce::Slider& DualSvfPanel::cutoff2ForTests() noexcept { return cutoff2_->slider(); }
juce::Slider& DualSvfPanel::resonance2ForTests() noexcept { return resonance2_->slider(); }

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
    control2_.setSelectedIndex(static_cast<int>(requested.control2));
    const bool relative = requested.control2 == ShruthiSvfSection2Control::relative;
    cutoff2_->setLabel(relative ? "OFFSET (64 = 0)" : "CUTOFF 2");
    cutoff2_->slider().textFromValueFunction = [relative](double value) {
        const int v = juce::roundToInt(value) - (relative ? 64 : 0);
        return (relative && v > 0 ? "+" : "") + juce::String(v) + (relative ? " st" : "");
    };
    cutoff2_->slider().valueFromTextFunction = [relative](const juce::String& value) {
        return static_cast<double>(value.getIntValue() + (relative ? 64 : 0));
    };
    cutoff2_->slider().setValue(requested.cutoff2, juce::dontSendNotification);
    cutoff2_->slider().updateText();
    resonance2_->slider().setValue(requested.resonance2, juce::dontSendNotification);
    const bool independent = requested.enabled && requested.control2 != ShruthiSvfSection2Control::legacyFollow;
    cutoff2_->setEnabled(independent);
    resonance2_->setEnabled(independent);
    cutoff2_->setAlpha(independent ? 1.0f : 0.45f);
    resonance2_->setAlpha(independent ? 1.0f : 0.45f);
    requested_.setText(requested.enabled ? "REQUESTED: DUAL SVF" : "REQUESTED: CLASSIC IR3109",
                       juce::dontSendNotification);
    const bool section2Active = requested.routing != ShruthiSvfDualRouting::section1;
    section2Note_.setText(! section2Active ? "Filter 2 outside audible path"
        : (requested.control2 == ShruthiSvfSection2Control::legacyFollow ? "Shared cutoff / resonance"
            : (relative ? "Relative cutoff / independent resonance" : "Independent cutoff / resonance")),
                          juce::dontSendNotification);
    const char* paths[] = { u8"MIX \u2192 F1 \u2192 VCA", u8"MIX \u2192 F1 \u2192 F2 \u2192 VCA",
                            u8"MIX \u2192 (F1 + F2) \u2192 VCA" };
    path_.setText(juce::String::fromUTF8(paths[static_cast<size_t>(requested.routing)]), juce::dontSendNotification);
    for (auto* segments : { &routing_, &mode1_, &mode2_, &calibration_, &control2_ })
    {
        segments->setEnabled(requested.enabled);
        segments->setAlpha(requested.enabled ? 1.0f : 0.45f);
    }
    // Section 2 keeps its requested mode while the reference path bypasses it.
    mode2_.setAlpha(requested.enabled && section2Active ? 1.0f : 0.45f);
}

void DualSvfPanel::resized()
{
    title_.setBounds(16, 8, 110, 24);
    session_.setBounds(126, 10, getWidth() - 204, 20);
    close_.setBounds(getWidth() - 66, 12, 50, 22);
    requested_.setBounds(16, 34, getWidth() - 32, 18);
    routing_.setBounds(16, 58, getWidth() - 32, 24);
    path_.setBounds(16, 87, getWidth() - 32, 18);
    const int half = (getWidth() - 44) / 2;
    filter1Title_.setBounds(16, 110, half, 18);
    filter2Title_.setBounds(28 + half, 110, half, 18);
    mode1_.setBounds(16, 132, half, 24);
    mode2_.setBounds(28 + half, 132, half, 24);
    control2_.setBounds(16, 166, getWidth() - 32, 24);
    cutoff2_->setBounds(16, 198, half, 70);
    resonance2_->setBounds(28 + half, 198, half, 70);
    section2Note_.setBounds(16, 271, getWidth() - 32, 16);
    calibrationTitle_.setBounds(16, 291, getWidth() - 32, 16);
    calibration_.setBounds(16, 310, getWidth() - 32, 23);
    paper_.setBounds(16, 334, (getWidth() - 32) / 2, 14);
    eagle_.setBounds(getWidth() / 2, 334, (getWidth() - 32) / 2, 14);
}

void DualSvfPanel::paint(juce::Graphics& g)
{
    g.setColour(Palette::panel());
    g.fillRoundedRectangle(getLocalBounds().toFloat(), 6.0f);
    g.setColour(Palette::line());
    g.drawRoundedRectangle(getLocalBounds().toFloat().reduced(0.75f), 6.0f, 1.5f);
    for (float y : { 54.0f, 160.0f, 288.0f })
        g.drawHorizontalLine(juce::roundToInt(y), 16.0f, static_cast<float>(getWidth() - 16));
}

void DualSvfPanel::lookAndFeelChanged()
{
    for (auto* label : { &title_, &filter1Title_, &filter2Title_, &calibrationTitle_ })
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
