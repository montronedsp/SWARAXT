// Copyright 2026 MontroneDSP.
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include <JuceHeader.h>

#include "Ui/SwaraXtUiPalette.h"
#include "Ui/SwaraXtTypography.h"

namespace swaraxt::ui {

class SwaraXtLookAndFeel : public juce::LookAndFeel_V4 {
 public:
    SwaraXtLookAndFeel();
    void applySkin();
    void drawCallOutBoxBackground(juce::CallOutBox&, juce::Graphics&,
                                  const juce::Path&, juce::Image&) override;
    juce::Font regularFont(float height) const;
    juce::Font font(TextRole role) const { return typography_.font(role); }

    void drawRotarySlider(juce::Graphics& g,
                          int x,
                          int y,
                          int width,
                          int height,
                          float sliderPosProportional,
                          float rotaryStartAngle,
                          float rotaryEndAngle,
                          juce::Slider& slider) override;

    void drawButtonBackground(juce::Graphics& g,
                              juce::Button& button,
                              const juce::Colour& backgroundColour,
                              bool shouldDrawButtonAsHighlighted,
                              bool shouldDrawButtonAsDown) override;
    void drawComboBox(juce::Graphics& g,
                      int width,
                      int height,
                      bool isButtonDown,
                      int buttonX,
                      int buttonY,
                      int buttonW,
                      int buttonH,
                      juce::ComboBox& box) override;
    void positionComboBoxText(juce::ComboBox& box, juce::Label& label) override;
    juce::Font getComboBoxFont(juce::ComboBox&) override;
    juce::PopupMenu::Options getOptionsForComboBoxPopupMenu(juce::ComboBox& box,
                                                             juce::Label& label) override;
    juce::Font getPopupMenuFont() override;
    void drawPopupMenuSectionHeader(juce::Graphics&, const juce::Rectangle<int>&,
                                    const juce::String&) override;
    void drawToggleButton(juce::Graphics&, juce::ToggleButton&, bool, bool) override;
    juce::Font getAlertWindowTitleFont() override { return font(TextRole::sectionHeader); }
    juce::Font getAlertWindowMessageFont() override { return font(TextRole::aboutBody); }
    juce::Font getAlertWindowFont() override { return font(TextRole::aboutBody); }
    juce::Rectangle<int> getTooltipBounds(const juce::String&, juce::Point<int>,
                                           juce::Rectangle<int>) override;
    void drawTooltip(juce::Graphics&, const juce::String&, int, int) override;
    void drawLinearSlider(juce::Graphics& g,
                          int x,
                          int y,
                          int width,
                          int height,
                          float sliderPos,
                          float minSliderPos,
                          float maxSliderPos,
                          juce::Slider::SliderStyle style,
                          juce::Slider& slider) override;
    void drawLabel(juce::Graphics&, juce::Label&) override;
    void drawButtonText(juce::Graphics&, juce::TextButton&, bool, bool) override;
    juce::Font getLabelFont(juce::Label&) override;
    juce::Font getTextButtonFont(juce::TextButton&, int buttonHeight) override;

 private:
    Typography typography_;
};

}  // namespace swaraxt::ui
