// Copyright 2026 MontroneDSP.
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include <JuceHeader.h>
#include <memory>
#include <vector>
#include "Engine/Filter/ShruthiSvf2p.h"

class SwaraXtAudioProcessor;

namespace swaraxt::ui {

class SvfSegments final : public juce::Component {
 public:
    SvfSegments(const juce::String& name, const juce::StringArray& choices);
    void resized() override;
    void lookAndFeelChanged() override;
    void setSelectedIndex(int index);
    int selectedIndex() const noexcept;
    std::function<void(int)> onSelect;
    juce::TextButton& buttonForTests(int index) { return *buttons_[static_cast<size_t>(index)]; }

 private:
    std::vector<std::unique_ptr<juce::TextButton>> buttons_;
};

class DualSvfPanel final : public juce::Component {
 public:
    explicit DualSvfPanel(SwaraXtAudioProcessor& processor);
    void paint(juce::Graphics& g) override;
    void resized() override;
    void lookAndFeelChanged() override;
    bool keyPressed(const juce::KeyPress& key) override;
    void sync(const ExperimentalSvfSelection& requested);
    std::function<void()> closeRequested;
    std::function<void()> selectionChanged;

    SvfSegments& routingForTests() noexcept { return routing_; }
    SvfSegments& mode1ForTests() noexcept { return mode1_; }
    SvfSegments& mode2ForTests() noexcept { return mode2_; }
    SvfSegments& calibrationForTests() noexcept { return calibration_; }
    juce::TextButton& closeForTests() noexcept { return close_; }
    juce::String requestedTextForTests() const { return requested_.getText(); }
    juce::String section2TextForTests() const { return section2Note_.getText(); }

 private:
    void published();
    SwaraXtAudioProcessor& processor_;
    juce::Label title_, requested_, pathTitle_, path_, filter1Title_, filter2Title_;
    juce::Label section2Note_, calibrationTitle_, paper_, eagle_, session_;
    juce::TextButton close_ { "CLOSE" };
    SvfSegments routing_ { "Filter path", { "SECTION 1", "SERIAL", "PARALLEL" } };
    SvfSegments mode1_ { "Filter 1 mode", { "LP", "BP", "HP" } };
    SvfSegments mode2_ { "Filter 2 mode", { "LP", "BP", "HP" } };
    SvfSegments calibration_ { "Calibration", { "PAPER", "EAGLE HW" } };
};

}  // namespace swaraxt::ui
