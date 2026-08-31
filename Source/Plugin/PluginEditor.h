// Copyright 2026 MontroneDSP.
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include <JuceHeader.h>

#include <memory>
#include <vector>

#include "Plugin/PluginProcessor.h"
#include "Ui/SwaraXtLookAndFeel.h"
#include "Ui/SwaraXtPanels.h"
#include "Ui/SwaraXtSkin.h"

class SwaraXtAudioProcessorEditor : public juce::AudioProcessorEditor,
                                   private juce::Button::Listener,
                                   private juce::ComboBox::Listener,
                                   private juce::Timer {
 public:
    explicit SwaraXtAudioProcessorEditor(SwaraXtAudioProcessor&);
    ~SwaraXtAudioProcessorEditor() override;

    void paint(juce::Graphics& g) override;
    void resized() override;
    void mouseDown(const juce::MouseEvent& event) override;

    void setModuleViewsForTests(bool modulation, bool sequencer);
    void setSequencerHostSyncForTests(bool enabled);
    void setSequencerEditorViewForTests(bool sequence);
    void setSequencerPatternForTests(int pattern);
    int sequencerPatternForTests() const;
    void setSequenceLayoutForTests(int length, int rotation, int groove);
    void setSequenceStepForTests(int step, int note, int event, int velocity, int value);
    juce::Rectangle<float> productLockupBoundsForTests() const;
    juce::Rectangle<int> presetBoundsForTests() const { return presetSelector_.getBounds(); }
    void setGuiSizeForTests(swaraxt::ui::GuiSize size);
    void setSkinForTests(swaraxt::ui::SkinId skin);
    void setDecorationForTests(swaraxt::ui::DecorationId decoration);
    swaraxt::ui::GuiSize guiSizeForTests() const noexcept { return guiSize_; }
    const swaraxt::ui::OscillatorPanel& oscillatorForTests(int index) const noexcept
    {
        return mainPanel_.oscillatorForTests(index);
    }
    swaraxt::ui::OscillatorPanel& oscillatorForTests(int index) noexcept
    {
        return mainPanel_.oscillatorForTests(index);
    }
    swaraxt::ui::SwaraXtKnob& filterKeyTrackForTests() noexcept
    {
        return mainPanel_.filterKeyTrackForTests();
    }
    swaraxt::ui::ModPanel& modulationForTests() noexcept
    {
        return mainPanel_.modulationForTests();
    }
    juce::ComboBox& lfoWaveComboForTests(int index) noexcept
    {
        return mainPanel_.lfoWaveComboForTests(index);
    }
    juce::ComboBox& mixOperatorComboForTests() noexcept
    {
        return mainPanel_.mixOperatorComboForTests();
    }
    juce::ComboBox& subShapeComboForTests() noexcept
    {
        return mainPanel_.subShapeComboForTests();
    }
    juce::ComboBox& sequenceEventComboForTests() noexcept
    {
        return mainPanel_.sequenceEventComboForTests();
    }
    juce::ComboBox& arpComboForTests(int index) noexcept
    {
        return mainPanel_.arpComboForTests(index);
    }
    bool modMatrixHeaderVisibleForTests() const noexcept
    {
        return mainPanel_.modMatrixHeaderVisibleForTests();
    }
    juce::Rectangle<int> sequenceStartBoundsForTests() const
    {
        return mainPanel_.sequenceStartBoundsForTests();
    }
    juce::Rectangle<int> sequenceLengthBoundsForTests() const
    {
        return mainPanel_.sequenceLengthBoundsForTests();
    }
    juce::Rectangle<int> sequenceEventComboBoundsForTests() const
    {
        return mainPanel_.sequenceEventComboBoundsForTests();
    }
    juce::Rectangle<int> sequenceNoteBoundsForTests() const
    {
        return mainPanel_.sequenceNoteBoundsForTests();
    }
    juce::Rectangle<int> sequenceVelocityBoundsForTests() const
    {
        return mainPanel_.sequenceVelocityBoundsForTests();
    }
    juce::Rectangle<int> sequenceNavigationBoundsForTests() const
    {
        return mainPanel_.sequenceNavigationBoundsForTests();
    }
    juce::Rectangle<int> arpComboBoundsForTests(int index) const
    {
        return mainPanel_.arpComboBoundsForTests(index);
    }

 private:
    class BrandButton : public juce::Button {
     public:
        BrandButton() : juce::Button("MontroneDSP About") {}
        void paintButton(juce::Graphics&, bool, bool) override {}
    };

    class DesignSurface : public juce::Component {
     public:
        explicit DesignSurface(SwaraXtAudioProcessorEditor& owner) : owner_(owner) {}
        void paint(juce::Graphics& g) override { owner_.paintDesignSurface(g); }
     private:
        SwaraXtAudioProcessorEditor& owner_;
    };

    void buttonClicked(juce::Button* button) override;
    void comboBoxChanged(juce::ComboBox* combo) override;
    void timerCallback() override;
    void refreshPresetList();
    void promptForPresetName();
    void savePresetWithName(const juce::String& name, bool overwrite);
    void showPresetError(const juce::String& message);
    void paintDesignSurface(juce::Graphics& g);
    void showContextMenu();
    void showAboutPopup();
    void applySkin(swaraxt::ui::SkinId skin, bool persist);
    void applyDecoration(swaraxt::ui::DecorationId decoration, bool persist);
    void applyGuiSize(swaraxt::ui::GuiSize size, bool persist);
    void applyFilterQuality(swaraxt::FilterQuality quality, bool persist);

    SwaraXtAudioProcessor& processor_;
    swaraxt::ui::SwaraXtLookAndFeel lookAndFeel_;
    swaraxt::ui::SkinId skinId_ = swaraxt::ui::SkinId::pastel;
    swaraxt::ui::DecorationId decorationId_ = swaraxt::ui::DecorationId::legacy;
    swaraxt::ui::GuiSize guiSize_ = swaraxt::ui::GuiSize::medium;
    swaraxt::FilterQuality filterQuality_ = swaraxt::FilterQuality::normal;
    swaraxt::ui::SkinAssetCache assets_ { swaraxt::ui::SkinRegistry::pastel() };
    swaraxt::ui::DecorationAssetCache decorationAssets_ {
        swaraxt::ui::DecorationId::legacy, swaraxt::ui::SkinRegistry::pastel().palette
    };
    DesignSurface designSurface_ { *this };

    juce::ComboBox presetSelector_;
    juce::TextButton prevPresetButton_ { "<" };
    juce::TextButton nextPresetButton_ { ">" };
    juce::TextButton savePresetButton_ { "SAVE" };
    BrandButton aboutBrandButton_;
    swaraxt::ui::MainPanel mainPanel_;
    std::vector<SwaraXtAudioProcessor::PresetEntry> presetEntries_;
    bool refreshingPresets_ = false;
};
