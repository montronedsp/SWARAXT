// Copyright 2026 MontroneDSP.
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include <JuceHeader.h>

#include <memory>
#include <vector>

#include "Ui/SwaraXtUiPalette.h"
#include "Ui/SwaraXtModulationNames.h"

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
    juce::Component& body() noexcept { return body_; }

 private:
    juce::Label title_;
    juce::Label secondaryTitle_;
    juce::Component body_;
    juce::Rectangle<int> secondaryActionBounds_;
    int secondaryDividerY_ = 0;
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

class SeqPanel : public juce::Component {
 public:
    SeqPanel();
    void attach(SwaraXtAudioProcessor& processor);
    void reflow();
    void setHostSyncForTests(bool enabled);

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
    void updateClockControls();
    bool attached_ = false;
};

}  // namespace swaraxt::ui
