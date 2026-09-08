// Copyright 2026 MontroneDSP. SPDX-License-Identifier: GPL-3.0-or-later
#include "Ui/SwaraXtBoardPanel.h"
#include "Plugin/SwaraXtParameterLayout.h"
#include <array>

namespace swaraxt::ui {
BoardPanel::BoardPanel()
{
    for (auto* selector : { &model_, &fx_, &route_ }) addAndMakeVisible(*selector);
    for (auto* control : { &control1_, &control2_ }) addAndMakeVisible(*control);
    addChildComponent(replay_);
    replay_.setTooltip("Unchecked: record. Checked: replay. Recorded audio is transient and is cleared on preset/state load.");
    model_.combo().addListener(this);
    fx_.combo().addListener(this);
    lookAndFeelChanged();
    refreshContext();
}

BoardPanel::~BoardPanel()
{
    model_.combo().removeListener(this);
    fx_.combo().removeListener(this);
}

void BoardPanel::attach(juce::AudioProcessorValueTreeState& apvts)
{
    if (modelAttachment_) return;
    // The parameter's choice list is the single UI/host source of ordering.
    const auto attachChoice = [&apvts](juce::ComboBox& combo, const char* id) {
        const auto* parameter = dynamic_cast<juce::AudioParameterChoice*>(apvts.getParameter(id));
        jassert(parameter != nullptr);
        combo.addItemList(parameter->choices, 1);
        return std::make_unique<juce::AudioProcessorValueTreeState::ComboBoxAttachment>(apvts, id, combo);
    };
    modelAttachment_ = attachChoice(model_.combo(), IDs::filterModel);
    fxAttachment_ = attachChoice(fx_.combo(), IDs::dspFxProgram);
    routeAttachment_ = attachChoice(route_.combo(), IDs::dspBoardRouting);
    control1Attachment_ = std::make_unique<juce::AudioProcessorValueTreeState::SliderAttachment>(apvts, IDs::dspFxParam1, control1_.slider());
    control2Attachment_ = std::make_unique<juce::AudioProcessorValueTreeState::SliderAttachment>(apvts, IDs::dspFxParam2, control2_.slider());
    // ParameterAttachment uses RAW values. The existing 0..63 domain maps to
    // native CV2 in steps of four; replay begins at CV2=128, not raw 0.5.
    replayAttachment_ = std::make_unique<juce::ParameterAttachment>(
        *apvts.getParameter(IDs::dspFxParam2), [this](float value) {
            replay_.setToggleState(value >= 32, juce::dontSendNotification);
        });
    replay_.onClick = [this] {
        replayAttachment_->setValueAsCompleteGesture(replay_.getToggleState() ? 63.0f : 0.0f);
    };
    replayAttachment_->sendInitialUpdate();
    control1_.slider().setDoubleClickReturnValue(true, 64);
    control2_.slider().setDoubleClickReturnValue(true, 0);
    refreshContext();
}

void BoardPanel::resized()
{
    auto top = getLocalBounds().removeFromTop(35);
    model_.setBounds(top.removeFromLeft(105));
    top.removeFromLeft(5);
    fx_.setBounds(top);
    auto bottom = getLocalBounds().withTrimmedTop(40);
    route_.setBounds(bottom.removeFromLeft(105));
    bottom.removeFromLeft(5);
    control1_.setBounds(bottom.removeFromLeft((bottom.getWidth() - 4) / 2));
    bottom.removeFromLeft(4);
    control2_.setBounds(bottom);
    replay_.setBounds(bottom.withSizeKeepingCentre(bottom.getWidth(), 24));
}

void BoardPanel::comboBoxChanged(juce::ComboBox*) { refreshContext(); }

void BoardPanel::lookAndFeelChanged()
{
    replay_.setColour(juce::ToggleButton::textColourId, Palette::cream());
    replay_.setColour(juce::ToggleButton::tickColourId, Palette::accent());
    replay_.setColour(juce::ToggleButton::tickDisabledColourId, Palette::muted());
}

void BoardPanel::refreshContext()
{
    // Public Off precedes the 16 source programs. Tempo delays use CV1 as
    // feedback, whereas free delays use it as time; CV2 is additive amount.
    static constexpr std::array<std::array<const char*, 2>, 17> names {{
        {{ "", "" }}, {{ "FOLD", "FUZZ" }}, {{ "RATE", "BITS" }},
        {{ "TIME", "FEEDBACK" }}, {{ "TIME", "FEEDBACK" }}, {{ "FREQUENCY", "MIX" }},
        {{ "TIME", "AMOUNT" }}, {{ "TIME", "AMOUNT" }}, {{ "TIME", "AMOUNT" }},
        {{ "TIME", "AMOUNT" }}, {{ "TIME", "AMOUNT" }},
        {{ "FEEDBACK", "AMOUNT" }}, {{ "FEEDBACK", "AMOUNT" }},
        {{ "FEEDBACK", "AMOUNT" }}, {{ "FEEDBACK", "AMOUNT" }},
        {{ "TIME / PITCH", "" }}, {{ "PITCH", "MIX" }}
    }};
    const auto choice = juce::jlimit(0, 16, fx_.combo().getSelectedItemIndex());
    const auto& context = names[static_cast<std::size_t>(choice)];
    control1_.setLabel(context[0]);
    control2_.setLabel(context[1]);
    control1_.slider().setTooltip(context[0]);
    control2_.slider().setTooltip(context[1]);
    route_.setVisible(model_.combo().getSelectedItemIndex() == 1);
    control1_.setVisible(choice != 0);
    control2_.setVisible(choice != 0 && choice != 15);
    replay_.setVisible(choice == 15);
}
}
