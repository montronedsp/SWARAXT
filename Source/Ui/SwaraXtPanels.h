// Copyright 2026 MontroneDSP.
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include <JuceHeader.h>

#include <functional>
#include <memory>
#include <vector>

#include "Ui/SwaraXtUiPalette.h"
#include "Ui/SwaraXtModulationNames.h"
#include "Ui/SwaraXtDualSvfPanel.h"
#include "Ui/SwaraXtPanelWidgets.h"
#include "Ui/SwaraXtOscillatorPanel.h"
#include "Ui/SwaraXtModPanel.h"
#include "Ui/SwaraXtSeqPanel.h"
#include "Engine/SequenceState.h"

class SwaraXtAudioProcessor;

namespace swaraxt::ui {

class BoardPanel;

class MainPanel : public juce::Component {
 public:
    MainPanel();
    ~MainPanel() override;
    void attach(SwaraXtAudioProcessor& processor);
    void setSkinAssets(const SkinAssetCache& assets);
    void lookAndFeelChanged() override;
    void reflow();
    void setLocalViews(bool modulation, bool sequencer);
    void setBoardEditorView(bool visible);
    void refreshExperimentalSvf();
    void setDualSvfView(bool visible);
    DualSvfPanel& dualSvfPanelForTests() noexcept { return *dualSvfView_; }
    SeqPanel& sequencerForTests() noexcept { return *sequencerView_; }
    juce::ComboBox& filterModelForTests() noexcept { return filterModel_; }
    juce::ComboBox& legatoComboForTests() noexcept { return selectors_[static_cast<size_t>(kLegato)]->combo(); }
    juce::ComboBox& inputTuneBoxForTests() noexcept { return selectors_[static_cast<size_t>(kInputTune)]->combo(); }
    juce::TextButton& dualSvfViewButtonForTests() noexcept { return dualSvfViewButton_; }
    bool showingSequencer() const noexcept { return showingSequencer_; }
    std::function<void()> workspaceChanged;
    BoardPanel& boardPanelForTests() noexcept { return *boardView_; }
    void setSequencerHostSyncForTests(bool enabled);
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
    // Envelope knobs in constructor order: Env1 A/D/S/R then Env2 A/D/S/R (0..7).
    SwaraXtKnob& envelopeKnobForTests(int index) noexcept
    {
        return *knobs_[static_cast<size_t>(kEnv1Attack + index)];
    }
    juce::ComboBox& lfoRetrigComboForTests(int index) noexcept
    {
        const int selector = index == 0 ? kLfo1Retrig : kLfo2Retrig;
        return selectors_[static_cast<size_t>(selector)]->combo();
    }
    juce::ComboBox& lfoSyncComboForTests(int index) noexcept
    {
        const int selector = index == 0 ? kLfo1Sync : kLfo2Sync;
        return selectors_[static_cast<size_t>(selector)]->combo();
    }
    juce::ComboBox& lfoDivisionComboForTests(int index) noexcept
    {
        const int selector = index == 0 ? kLfo1Division : kLfo2Division;
        return selectors_[static_cast<size_t>(selector)]->combo();
    }
    SwaraXtKnob& lfoRateKnobForTests(int index) noexcept
    {
        const int knob = index == 0 ? kLfo1Rate : kLfo2Rate;
        return *knobs_[static_cast<size_t>(knob)];
    }
    SwaraXtKnob& lfoAttackKnobForTests(int index) noexcept
    {
        const int knob = index == 0 ? kLfo1Attack : kLfo2Attack;
        return *knobs_[static_cast<size_t>(knob)];
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
        kFilterLfo,
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
        kInputTune
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
    juce::TextButton boardViewButton_ { "FX" };
    juce::ComboBox filterModel_;
    juce::TextButton dualSvfViewButton_ { "DUAL..." };
    std::unique_ptr<DualSvfPanel> dualSvfView_;
    SwaraXtAudioProcessor* processor_ = nullptr;
    // Rendering invalidation only; requests always come from the processor.
    std::uint32_t displayedSvfSelection_ = ~std::uint32_t { 0 };
    juce::Label env1Role_;
    juce::Label env2Role_;
    std::unique_ptr<ModPanel> modulationView_;
    std::unique_ptr<SeqPanel> sequencerView_;
    std::unique_ptr<BoardPanel> boardView_;

    std::vector<std::unique_ptr<SwaraXtKnob>> knobs_;
    std::vector<std::unique_ptr<SwaraXtSelector>> selectors_;
    std::vector<std::unique_ptr<juce::AudioProcessorValueTreeState::SliderAttachment>> sliderAttachments_;
    std::vector<std::unique_ptr<juce::AudioProcessorValueTreeState::ComboBoxAttachment>> selectorAttachments_;
    bool attached_ = false;
    bool showingModulation_ = false;
    bool showingSequencer_ = false;
    bool showingBoard_ = false;
};

}  // namespace swaraxt::ui
