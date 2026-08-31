// Copyright 2026 MontroneDSP.
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include <JuceHeader.h>

#include <memory>
#include <vector>

#include "Ui/SwaraXtUiPalette.h"
#include "Ui/SwaraXtModulationNames.h"
#include "Engine/SequenceState.h"

class SwaraXtAudioProcessor;

namespace swaraxt::ui {

class ModPanel;
class SeqPanel;

enum class KnobSize { standard, large };

class SwaraXtKnob : public juce::Component {
 public:
    SwaraXtKnob(const juce::String& label, KnobSize size = KnobSize::standard);

    void resized() override;
    void lookAndFeelChanged() override;
    juce::Slider& slider() noexcept { return slider_; }
    const juce::Slider& slider() const noexcept { return slider_; }
    KnobSize knobSize() const noexcept { return size_; }

 private:
    juce::Label label_;
    juce::Slider slider_;
    KnobSize size_ = KnobSize::standard;
};

class SwaraXtSelector : public juce::Component {
 public:
    SwaraXtSelector(const juce::String& label,
                   const juce::StringArray& items,
                   bool primary = false);

    void resized() override;
    void lookAndFeelChanged() override;
    juce::ComboBox& combo() noexcept { return combo_; }
    const juce::ComboBox& combo() const noexcept { return combo_; }
    bool isPrimary() const noexcept { return primary_; }
    void refreshDescription();

 private:
    juce::Label label_;
    juce::ComboBox combo_;
    juce::Label description_;
    bool primary_ = false;
};

class SwaraXtModulePanel : public juce::Component {
 public:
    explicit SwaraXtModulePanel(const juce::String& title,
                                const juce::String& secondaryTitle = {},
                                int secondaryDividerY = 0);

    void paint(juce::Graphics& g) override;
    void resized() override;
    void lookAndFeelChanged() override;
    void setSecondaryActionBounds(juce::Rectangle<int> bounds);
    void setSecondaryHeaderVisible(bool visible);
    bool secondaryHeaderVisibleForTests() const noexcept { return secondaryHeaderVisible_; }
    juce::Component& body() noexcept { return body_; }
    const juce::Component& body() const noexcept { return body_; }

 private:
    juce::Label title_;
    juce::Label secondaryTitle_;
    juce::Component body_;
    juce::Rectangle<int> secondaryActionBounds_;
    int secondaryDividerY_ = 0;
    bool secondaryHeaderVisible_ = true;
};

class SwaraXtDepthSlider : public juce::Component {
 public:
    SwaraXtDepthSlider();
    void resized() override;
    juce::Slider& slider() noexcept { return slider_; }

 private:
    juce::Slider slider_;
};

class OscillatorPanel : public juce::Component {
 public:
    OscillatorPanel(const juce::String& title, bool hasDetune);
    void attach(juce::AudioProcessorValueTreeState& apvts,
                const char* modelId,
                const char* pitchId,
                const char* timbreId,
                const char* detuneId = nullptr);
    void paint(juce::Graphics& g) override;
    void resized() override;
    void lookAndFeelChanged() override;
    bool modelIsSelector() const noexcept;
    bool pitchIsRotary() const noexcept;
    bool waveVariationIsRotary() const noexcept;
    bool detuneIsVisible() const noexcept { return detune_.isVisible(); }
    juce::Slider& pitchSlider() noexcept { return pitch_.slider(); }
    juce::Slider& waveVariationSlider() noexcept { return timbre_.slider(); }
    juce::ComboBox& modelComboForTests() noexcept { return model_.combo(); }
    const juce::ComboBox& modelComboForTests() const noexcept { return model_.combo(); }

 private:
    juce::String title_;
    juce::Label titleLabel_;
    SwaraXtSelector model_;
    SwaraXtKnob pitch_;
    SwaraXtKnob timbre_;
    SwaraXtKnob detune_;
    bool hasDetune_ = false;
    std::unique_ptr<juce::AudioProcessorValueTreeState::ComboBoxAttachment> modelAttachment_;
    std::unique_ptr<juce::AudioProcessorValueTreeState::SliderAttachment> pitchAttachment_;
    std::unique_ptr<juce::AudioProcessorValueTreeState::SliderAttachment> timbreAttachment_;
    std::unique_ptr<juce::AudioProcessorValueTreeState::SliderAttachment> detuneAttachment_;
};

class EnvelopeTrace : public juce::Component,
                      private juce::Timer {
 public:
    ~EnvelopeTrace() override;
    void bind(juce::AudioProcessorValueTreeState& apvts,
              const char* attack,
              const char* decay,
              const char* sustain,
              const char* release);
    void setFrame(const juce::Drawable* frame) noexcept { frame_ = frame; }
    void paint(juce::Graphics& g) override;

 private:
    void timerCallback() override;

    juce::AudioProcessorValueTreeState* apvts_ = nullptr;
    const char* attack_ = nullptr;
    const char* decay_ = nullptr;
    const char* sustain_ = nullptr;
    const char* release_ = nullptr;
    const juce::Drawable* frame_ = nullptr;
};

class LfoTrace : public juce::Component,
                 private juce::Timer {
 public:
    ~LfoTrace() override;
    void bind(juce::AudioProcessorValueTreeState& apvts,
              const char* waveform,
              const char* rate);
    void setFrame(const juce::Drawable* frame) noexcept { frame_ = frame; }
    void paint(juce::Graphics& g) override;

 private:
    void timerCallback() override;

    juce::AudioProcessorValueTreeState* apvts_ = nullptr;
    const char* waveform_ = nullptr;
    const char* rate_ = nullptr;
    float phase_ = 0.0f;
    const juce::Drawable* frame_ = nullptr;
};

class MainPanel : public juce::Component {
 public:
    MainPanel();
    ~MainPanel() override;
    void attach(SwaraXtAudioProcessor& processor);
    void setSkinAssets(const SkinAssetCache& assets);
    void lookAndFeelChanged() override;
    void reflow();
    void setLocalViews(bool modulation, bool sequencer);
    void setSequencerHostSyncForTests(bool enabled);
    void setSequencerEditorViewForTests(bool sequence);
    void setSequencerPatternForTests(int pattern);
    int sequencerPatternForTests() const;
    void setSequenceLayoutForTests(int length, int rotation, int groove);
    void setSequenceStepForTests(int step, int note, int event, int velocity, int value);
    const OscillatorPanel& oscillatorForTests(int index) const noexcept
    {
        return index == 0 ? osc1_ : osc2_;
    }
    OscillatorPanel& oscillatorForTests(int index) noexcept
    {
        return index == 0 ? osc1_ : osc2_;
    }
    SwaraXtKnob& filterKeyTrackForTests() noexcept
    {
        return *knobs_[static_cast<size_t>(kFilterKey)];
    }
    const SwaraXtKnob& filterKeyTrackForTests() const noexcept
    {
        return *knobs_[static_cast<size_t>(kFilterKey)];
    }
    ModPanel& modulationForTests() noexcept { return *modulationView_; }
    const ModPanel& modulationForTests() const noexcept { return *modulationView_; }
    juce::ComboBox& lfoWaveComboForTests(int index) noexcept
    {
        const int selector = index == 0 ? kLfo1Wave : kLfo2Wave;
        return selectors_[static_cast<size_t>(selector)]->combo();
    }
    juce::ComboBox& mixOperatorComboForTests() noexcept
    {
        return selectors_[static_cast<size_t>(kMixOperator)]->combo();
    }
    juce::ComboBox& subShapeComboForTests() noexcept
    {
        return selectors_[static_cast<size_t>(kSubShape)]->combo();
    }
    juce::ComboBox& sequenceEventComboForTests() noexcept;
    juce::ComboBox& arpComboForTests(int index) noexcept;
    bool modMatrixHeaderVisibleForTests() const noexcept
    {
        return mixModule_.secondaryHeaderVisibleForTests();
    }
    juce::Rectangle<int> sequenceStartBoundsForTests() const;
    juce::Rectangle<int> sequenceLengthBoundsForTests() const;
    juce::Rectangle<int> sequenceEventComboBoundsForTests() const;
    juce::Rectangle<int> sequenceNoteBoundsForTests() const;
    juce::Rectangle<int> sequenceVelocityBoundsForTests() const;
    juce::Rectangle<int> sequenceNavigationBoundsForTests() const;
    juce::Rectangle<int> arpComboBoundsForTests(int index) const;

 private:
    enum KnobIndex {
        kMixBalance,
        kMixSub,
        kMixNoise,
        kFilterCutoff,
        kFilterResonance,
        kFilterEnv,
        kFilterKey,
        kFilterMod,
        kEnv1Attack,
        kEnv1Decay,
        kEnv1Sustain,
        kEnv1Release,
        kEnv2Attack,
        kEnv2Decay,
        kEnv2Sustain,
        kEnv2Release,
        kLfo1Rate,
        kLfo1Attack,
        kLfo2Rate,
        kLfo2Attack,
        kGlide,
        kMaster
    };

    enum SelectorIndex {
        kMixOperator,
        kSubShape,
        kLfo1Wave,
        kLfo1Retrig,
        kLfo1Sync,
        kLfo1Division,
        kLfo2Wave,
        kLfo2Retrig,
        kLfo2Sync,
        kLfo2Division,
        kLegato,
        kMidiChannel
    };

    SwaraXtModulePanel sourceModule_ { "" };
    SwaraXtModulePanel mixModule_ { "MIXER", "MOD MATRIX", 180 };
    SwaraXtModulePanel filterModule_ { "FILTER" };
    SwaraXtModulePanel envModule_ { "ENV 1", "ENV 2", 117 };
    SwaraXtModulePanel lfoModule_ { "LFO 1", "LFO 2", 136 };
    SwaraXtModulePanel perfModule_ { "GLOBAL" };

    OscillatorPanel osc1_ { "OSC 1", false };
    OscillatorPanel osc2_ { "OSC 2", true };

    EnvelopeTrace env1Trace_;
    EnvelopeTrace env2Trace_;
    LfoTrace lfo1Trace_;
    LfoTrace lfo2Trace_;
    juce::TextButton seqViewButton_ { "SEQ/ARP" };
    juce::Label env1Role_;
    juce::Label env2Role_;
    std::unique_ptr<ModPanel> modulationView_;
    std::unique_ptr<SeqPanel> sequencerView_;

    std::vector<std::unique_ptr<SwaraXtKnob>> knobs_;
    std::vector<std::unique_ptr<SwaraXtSelector>> selectors_;
    std::vector<std::unique_ptr<juce::AudioProcessorValueTreeState::SliderAttachment>> sliderAttachments_;
    std::vector<std::unique_ptr<juce::AudioProcessorValueTreeState::ComboBoxAttachment>> selectorAttachments_;
    bool attached_ = false;
    bool showingModulation_ = false;
    bool showingSequencer_ = false;
};

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

class SeqPanel : public juce::Component,
                 private swaraxt::SequenceState::Listener,
                 private juce::AsyncUpdater {
 public:
    SeqPanel();
    ~SeqPanel() override;
    void attach(SwaraXtAudioProcessor& processor);
    void reflow();
    void setHostSyncForTests(bool enabled);
    void setEditorViewForTests(bool sequence);
    bool showingSequenceEditorForTests() const noexcept { return showingSequenceEditor_; }
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
    juce::TextButton arpViewButton_ { "ARP" };
    juce::TextButton sequenceViewButton_ { "SEQ" };
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
    bool showingSequenceEditor_ = false;
    bool refreshingSequenceControls_ = false;
    void updateClockControls();
    void setEditorView(bool sequence);
    void refreshSequenceControls();
    void writeSelectedStep(uint8_t dataA, uint8_t dataB);
    void sequenceStateChanged() override;
    void handleAsyncUpdate() override;
    bool attached_ = false;
};

}  // namespace swaraxt::ui
