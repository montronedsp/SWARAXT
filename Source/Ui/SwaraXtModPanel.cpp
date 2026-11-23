// Copyright 2026 MontroneDSP.
// SPDX-License-Identifier: GPL-3.0-or-later

#include "Ui/SwaraXtModPanel.h"
#include "Ui/SwaraXtTypography.h"

#include "Plugin/PluginProcessor.h"

namespace swaraxt::ui {

ModPanel::ModPanel()
{
    setTextRole(pageLabel_, TextRole::numericValue);
    pageLabel_.setJustificationType(juce::Justification::centred);
    pageLabel_.setColour(juce::Label::textColourId, Palette::skin().matrixText);
    addAndMakeVisible(pageLabel_);
    addAndMakeVisible(previousPage_);
    addAndMakeVisible(nextPage_);
    previousPage_.onClick = [this] { showPage((page_ + 3) % 4); };
    nextPage_.onClick = [this] { showPage((page_ + 1) % 4); };

    rows_.reserve(12);
    for (int row = 0; row < 12; ++row)
    {
        auto r = std::make_unique<Row>();
        setTextRole(r->indexLabel, TextRole::smallIndex);
        r->indexLabel.setText(juce::String(row + 1).paddedLeft('0', 2), juce::dontSendNotification);
        r->indexLabel.setJustificationType(juce::Justification::centred);
        r->indexLabel.setColour(juce::Label::textColourId, Palette::mutedText());
        addAndMakeVisible(r->indexLabel);

        populatePluginModulationSourceCombo(r->source);
        r->source.setTooltip("Modulation source");
        populatePluginModulationDestinationCombo(r->destination);
        r->destination.setTooltip("Modulation destination");
        addAndMakeVisible(r->source);
        addAndMakeVisible(r->destination);

        r->amount = std::make_unique<SwaraXtDepthSlider>();
        if (row == 11)
        {
            constexpr auto kSlot12WheelTip = "Slot 12 depth is scaled by Mod Wheel";
            r->indexLabel.setTooltip(kSlot12WheelTip);
            r->source.setTooltip(kSlot12WheelTip);
            r->destination.setTooltip(kSlot12WheelTip);
            r->amount->slider().setTooltip(kSlot12WheelTip);
        }
        addAndMakeVisible(*r->amount);
        rows_.push_back(std::move(r));
    }
    showPage(0);
}

void ModPanel::lookAndFeelChanged()
{
    pageLabel_.setColour(juce::Label::textColourId, Palette::skin().matrixText);
    for (auto& row : rows_)
        row->indexLabel.setColour(juce::Label::textColourId, Palette::mutedText());
}

void ModPanel::attach(SwaraXtAudioProcessor& processor)
{
    if (attached_)
        return;

    auto& apvts = processor.getApvts();
    for (int row = 0; row < 12; ++row)
    {
        auto& r = *rows_[static_cast<size_t>(row)];
        const auto n = juce::String(row + 1);
        r.sourceAttachment = std::make_unique<EngineIdComboAttachment>(
            apvts, "mod.row" + n + ".source", r.source, modulationSourceName);
        r.destAttachment = std::make_unique<EngineIdComboAttachment>(
            apvts, "mod.row" + n + ".destination", r.destination, modulationDestinationName);
        setSliderDefault(apvts, ("mod.row" + n + ".amount").toRawUTF8(), r.amount->slider());
        r.amountAttachment = std::make_unique<juce::AudioProcessorValueTreeState::SliderAttachment>(
            apvts, "mod.row" + n + ".amount", r.amount->slider());
    }
    attached_ = true;
}

void ModPanel::reflow()
{
    auto area = getLocalBounds().reduced(2);
    auto paging = area.removeFromTop(24).reduced(2, 0);
    previousPage_.setBounds(paging.removeFromLeft(28));
    nextPage_.setBounds(paging.removeFromRight(28));
    pageLabel_.setBounds(paging);
    const int rowH = juce::jmax(30, area.getHeight() / 3);

    for (int row = 0; row < 12; ++row)
    {
        auto& r = *rows_[static_cast<size_t>(row)];
        if (row / 3 != page_)
            continue;
        auto rowArea = area.removeFromTop(rowH).reduced(2, 2);
        r.indexLabel.setBounds(rowArea.removeFromLeft(18));
        rowArea.removeFromLeft(4);
        const int amountW = 102;
        const int sourceW = 90;
        r.source.setBounds(rowArea.removeFromLeft(sourceW));
        rowArea.removeFromLeft(4);
        r.amount->setBounds(rowArea.removeFromLeft(amountW));
        rowArea.removeFromLeft(4);
        r.destination.setBounds(rowArea);
    }
}

void ModPanel::showPage(int page)
{
    page_ = juce::jlimit(0, 3, page);
    pageLabel_.setText(juce::String(page_ * 3 + 1) + "-" + juce::String(page_ * 3 + 3),
                       juce::dontSendNotification);
    for (int row = 0; row < 12; ++row)
    {
        const bool visible = row / 3 == page_;
        auto& r = *rows_[static_cast<size_t>(row)];
        r.indexLabel.setVisible(visible);
        r.source.setVisible(visible);
        r.destination.setVisible(visible);
        r.amount->setVisible(visible);
    }
    reflow();
}

}  // namespace swaraxt::ui
