// Copyright 2026 MontroneDSP.
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include <JuceHeader.h>

#include <memory>
#include <vector>

#include "Ui/SwaraXtPanelWidgets.h"
#include "Ui/SwaraXtModulationNames.h"

class SwaraXtAudioProcessor;

namespace swaraxt::ui {

class ModPanel : public juce::Component {
 public:
    ModPanel();
    void attach(SwaraXtAudioProcessor& processor);
    void reflow();
    void showPage(int page);
    void lookAndFeelChanged() override;
    juce::ComboBox& sourceComboForTests(int row) noexcept
    {
        return rows_[static_cast<size_t>(row)]->source;
    }
    juce::ComboBox& destinationComboForTests(int row) noexcept
    {
        return rows_[static_cast<size_t>(row)]->destination;
    }
    juce::String slot12TooltipForTests() const
    {
        return rows_[11]->indexLabel.getTooltip();
    }

 private:
    struct Row {
        juce::Label indexLabel;
        juce::ComboBox source;
        juce::ComboBox destination;
        std::unique_ptr<SwaraXtDepthSlider> amount;
        std::unique_ptr<EngineIdComboAttachment> sourceAttachment;
        std::unique_ptr<EngineIdComboAttachment> destAttachment;
        std::unique_ptr<juce::AudioProcessorValueTreeState::SliderAttachment> amountAttachment;
    };

    juce::Label pageLabel_;
    juce::TextButton previousPage_ { "<" };
    juce::TextButton nextPage_ { ">" };
    std::vector<std::unique_ptr<Row>> rows_;
    bool attached_ = false;
    int page_ = 0;
};

}  // namespace swaraxt::ui
