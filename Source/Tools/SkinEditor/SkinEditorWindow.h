// Copyright 2026 MontroneDSP.
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include <JuceHeader.h>

#include "Tools/SkinEditor/SkinEditorPreview.h"
#include "Ui/SwaraXtSkin.h"

namespace swaraxt::skin_editor {

class MainComponent final : public juce::Component,
                            private juce::ListBoxModel,
                            private juce::ChangeListener,
                            private juce::ComboBox::Listener,
                            private juce::Button::Listener {
 public:
    MainComponent();
    ~MainComponent() override;

    void resized() override;

 private:
    int getNumRows() override;
    void paintListBoxItem(int row, juce::Graphics& g, int width, int height, bool selected) override;
    void selectedRowsChanged(int lastRowSelected) override;
    void changeListenerCallback(juce::ChangeBroadcaster* source) override;
    void comboBoxChanged(juce::ComboBox* combo) override;
    void buttonClicked(juce::Button* button) override;

    void loadBaseSkin(ui::SkinId id);
    void syncSelectorFromPalette();
    void applySelectedColour(juce::Colour colour);
    void showError(const juce::String& message);
    void setStatus(const juce::String& message);

    ui::SkinPalette draft_ {};
    ui::SkinId baseSkin_ = ui::SkinId::pastel;
    int selectedRow_ = 0;

    juce::ComboBox baseSkinCombo_;
    juce::ListBox colourList_ { "colours", this };
    juce::ColourSelector selector_ {
        juce::ColourSelector::showColourspace
            | juce::ColourSelector::showSliders
            | juce::ColourSelector::showColourAtTop
            | juce::ColourSelector::editableColour
    };
    PreviewPanel preview_;
    juce::TextButton loadButton_ { "Load..." };
    juce::TextButton saveButton_ { "Save As..." };
    juce::TextButton installButton_ { "Install for Plugin" };
    juce::TextButton resetButton_ { "Reset Base Skin" };
    juce::Label status_;
    std::unique_ptr<juce::FileChooser> chooser_;
};

class MainWindow final : public juce::DocumentWindow {
 public:
    MainWindow();
    void closeButtonPressed() override;
};

}  // namespace swaraxt::skin_editor
