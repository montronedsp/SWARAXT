// Copyright 2026 MontroneDSP.
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include <JuceHeader.h>

#include <array>
#include <memory>
#include <vector>

#include "Engine/FreeCvPitch.h"
#include "Engine/SequenceState.h"
#include "Ui/SwaraXtPanelWidgets.h"

class SwaraXtAudioProcessor;

namespace swaraxt::ui {

class SeqPanel : public juce::Component,
                 private swaraxt::SequenceState::Listener,
                 private juce::AsyncUpdater,
                 private juce::Timer {
 public:
    SeqPanel();
    void paintOverChildren(juce::Graphics&) override;
    void visibilityChanged() override;
    ~SeqPanel() override;
    void attach(SwaraXtAudioProcessor& processor);
    void reflow();
    void setHostSyncForTests(bool enabled);
    void lookAndFeelChanged() override;
    void selectStepForTests(int step);
    void setArpPatternForTests(int pattern);
    int arpPatternForTests() const noexcept;
    void setSequenceLayoutForTests(int length, int rotation, int groove);
    void setSequenceStepForTests(int step, int note, int event, int velocity, int value);
    juce::ComboBox& eventComboForTests() noexcept { return event_.combo(); }
    juce::ComboBox& arpComboForTests(int index) noexcept
    {
        return selectors_[static_cast<size_t>(juce::jlimit(0, 5, index))]->combo();
    }
    juce::ComboBox& pitchModeBoxForTests() noexcept { return pitchModeBox_; }
    juce::ComboBox& tuningBoxForTests() noexcept { return tuningBox_; }
    juce::TextButton& lockButtonForTests() noexcept { return lockButton_; }
    juce::TextButton& midiOutButtonForTests() noexcept { return midiOutButton_; }
    juce::Label& sequenceHeadingForTests() noexcept { return sequenceHeading_; }
    juce::String noteLabelForTests() const { return note_.labelText(); }
    juce::Slider& noteSliderForTests() noexcept { return note_.slider(); }
    juce::TextButton& stepButtonForTests(int index) noexcept
    {
        return stepButtons_[static_cast<size_t>(
            juce::jlimit(0, swaraxt::SequenceSnapshot::kNumSteps - 1, index))];
    }
    juce::Rectangle<int> startBoundsForTests() const noexcept { return rotation_.getBounds(); }
    juce::Rectangle<int> lengthBoundsForTests() const noexcept { return length_.getBounds(); }
    juce::Rectangle<int> noteBoundsForTests() const noexcept { return note_.getBounds(); }
    juce::Rectangle<int> velocityBoundsForTests() const noexcept { return velocity_.getBounds(); }
    juce::Rectangle<int> eventComboBoundsForTests() const noexcept
    {
        return event_.combo().getBounds().translated(event_.getX(), event_.getY());
    }
    juce::Rectangle<int> arpComboBoundsForTests(int index) const noexcept
    {
        const auto& selector = *selectors_[static_cast<size_t>(juce::jlimit(0, 5, index))];
        return selector.combo().getBounds().translated(selector.getX(), selector.getY());
    }

 private:
    enum KnobIndex {
        kTempo,
        kSwing,
        kGate
    };

    enum SelectorIndex {
        kMode,
        kClockMode,
        kDirection,
        kPattern,
        kOctaves,
        kDivision
    };

    std::vector<std::unique_ptr<SwaraXtKnob>> knobs_;
    std::vector<std::unique_ptr<SwaraXtSelector>> selectors_;
    std::vector<std::unique_ptr<juce::AudioProcessorValueTreeState::SliderAttachment>> sliderAttachments_;
    std::vector<std::unique_ptr<juce::AudioProcessorValueTreeState::ComboBoxAttachment>> selectorAttachments_;
    std::unique_ptr<juce::ParameterAttachment> arpPatternAttachment_;
    juce::Label arpHeading_, sequenceHeading_;
    juce::TextButton lockButton_ { "LOCK" };
    juce::ComboBox pitchModeBox_;
    juce::ComboBox tuningBox_;
    juce::TextButton importTuning_ { "LOAD" };
    std::unique_ptr<juce::FileChooser> tuningChooser_;
    juce::TextButton midiOutButton_ { "MIDI OUT" };
    std::unique_ptr<juce::ButtonParameterAttachment> midiOutAttachment_;
    juce::TextButton randomNotes_ { "RANDOM NOTES" }, randomVelocity_ { "RANDOM VELOCITY" }, randomSequence_ { "RANDOM SEQ" };
    juce::TextButton randomEvent_ { "RANDOM EVENT" }, randomValue_ { "RANDOM VALUE" };
    juce::ComboBox randomType_;
    juce::Slider noteRange_;
    juce::Label rangeLabel_;
    juce::Random editRandom_;
    SwaraXtAudioProcessor* processor_ = nullptr;
    std::array<juce::TextButton, swaraxt::SequenceSnapshot::kNumSteps> stepButtons_;
    SwaraXtKnob length_ { "LENGTH" };
    SwaraXtKnob rotation_ { "START" };
    SwaraXtKnob note_ { "NOTE" };
    SwaraXtSelector event_ { "EVENT", juce::StringArray { "Rest", "Note", "Tie" } };
    SwaraXtKnob velocity_ { "VELOCITY" };
    SwaraXtKnob controller_ { "VALUE" };
    SwaraXtSelector groove_ { "GROOVE", juce::StringArray {
        "Swing", "Shuffle", "Push", "Lag", "Human", "Monkey" } };
    swaraxt::SequenceState* sequenceState_ = nullptr;
    int selectedStep_ = 0;
    int activeStep_ = -1;
    bool refreshingSequenceControls_ = false;
    void updateClockControls();
    void updateModeEmphasis();
    void timerCallback() override;
    void refreshSequenceControls();
    void writeSelectedStep(uint8_t dataA, uint8_t dataB);
    void sequenceStateChanged() override;
    void handleAsyncUpdate() override;
    bool attached_ = false;
};

}  // namespace swaraxt::ui
