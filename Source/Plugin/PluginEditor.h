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

    SwaraXtAudioProcessor& processor_;
    swaraxt::ui::SwaraXtLookAndFeel lookAndFeel_;
    swaraxt::ui::SkinId skinId_ = swaraxt::ui::SkinId::pastel;
    swaraxt::ui::DecorationId decorationId_ = swaraxt::ui::DecorationId::legacy;
    swaraxt::ui::GuiSize guiSize_ = swaraxt::ui::GuiSize::medium;
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
