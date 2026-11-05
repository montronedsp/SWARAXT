// Copyright 2026 MontroneDSP.
// SPDX-License-Identifier: GPL-3.0-or-later

#include "Ui/SwaraXtPanels.h"
#include "Ui/SwaraXtTypography.h"
#include "Ui/SwaraXtBoardPanel.h"
#include "Ui/SwaraXtModPanel.h"
#include "Ui/SwaraXtSeqPanel.h"

#include "Plugin/SwaraXtParameterLayout.h"
#include "Plugin/PluginProcessor.h"

namespace swaraxt::ui {

MainPanel::MainPanel()
{
    modulationView_ = std::make_unique<ModPanel>();
    sequencerView_ = std::make_unique<SeqPanel>();
    boardView_ = std::make_unique<BoardPanel>();
    addAndMakeVisible(sourceModule_);
    addAndMakeVisible(mixModule_);
    addAndMakeVisible(filterModule_);
    addAndMakeVisible(envModule_);
    addAndMakeVisible(lfoModule_);
    addAndMakeVisible(perfModule_);

    auto addKnob = [this](const char* label, KnobSize size = KnobSize::standard) {
        knobs_.push_back(std::make_unique<SwaraXtKnob>(label, size));
    };
    addKnob("MIX");
    addKnob("SUB");
    addKnob("NOISE");
    addKnob("CUTOFF", KnobSize::large);
    addKnob("RES", KnobSize::large);
    addKnob("ENV");
    addKnob("KEY TRACK");
    addKnob("LFO");
    addKnob("A");
    addKnob("D");
    addKnob("S");
    addKnob("R");
    addKnob("A");
    addKnob("D");
    addKnob("S");
    addKnob("R");
    addKnob("RATE");
    addKnob("FADE");
    addKnob("RATE");
    addKnob("FADE");
    addKnob("GLIDE");
    addKnob("MASTER");

    auto addSelector = [this](const char* label, const juce::StringArray& items, bool primary = false) {
        selectors_.push_back(std::make_unique<SwaraXtSelector>(label, items, primary));
    };
    addSelector("OPERATOR", operatorNames(), true);
    addSelector("SHAPE", subShapeNames());
    addSelector("LFO 1 WAVE", lfoWaveformNames());
    addSelector("RETRIGGER", lfoModeNames());
    addSelector("TIMING", timingModeNames());
    addSelector("DIVISION", beatDivisionNames());
    addSelector("LFO 2 WAVE", lfoWaveformNames());
    addSelector("RETRIGGER", lfoModeNames());
    addSelector("TIMING", timingModeNames());
    addSelector("DIVISION", beatDivisionNames());
    addSelector("VOICE", legatoNames());
    addSelector("INPUT TUNE", juce::StringArray { "OFF", "TUNED", "QUANTIZED" });
    selectors_.back()->combo().setTooltip(
        "OFF: original MIDI keyboard tuning. "
        "TUNED: incoming MIDI notes follow the active SWT1 keyboard mapping. "
        "QUANTIZED: incoming MIDI notes snap to the nearest pitch of the active tuning.");

    sourceModule_.body().addAndMakeVisible(osc1_);
    sourceModule_.body().addAndMakeVisible(osc2_);

    for (int i = kMixBalance; i <= kMixNoise; ++i)
        mixModule_.body().addAndMakeVisible(*knobs_[static_cast<size_t>(i)]);
    mixModule_.body().addAndMakeVisible(*selectors_[static_cast<size_t>(kMixOperator)]);
    mixModule_.body().addAndMakeVisible(*selectors_[static_cast<size_t>(kSubShape)]);
    mixModule_.body().addChildComponent(*modulationView_);

    for (int i = kFilterCutoff; i <= kFilterLfo; ++i)
        filterModule_.body().addAndMakeVisible(*knobs_[static_cast<size_t>(i)]);
    filterModule_.addAndMakeVisible(filterModel_);
    filterModel_.addItem("Classic IR3109", 1);
    filterModel_.addItem("Dual SVF", 2);
    filterModel_.setName("Filter model");
    filterModel_.setTooltip("Classic IR3109 / experimental Dual SVF (session only)");
    filterModel_.setScrollWheelEnabled(false);
    filterModule_.addAndMakeVisible(dualSvfViewButton_);
    dualSvfViewButton_.setTooltip("Open routing, section modes and Paper / Eagle calibration");
    dualSvfViewButton_.onClick = [this] {
        if (dualSvfView_) setDualSvfView(! dualSvfView_->isVisible());
    };
    perfModule_.addAndMakeVisible(boardViewButton_);
    boardViewButton_.getProperties().set("swaraxtSecondaryAction", true);
    boardViewButton_.setTooltip("Effects after the Classic filter and post-mixer level");
    perfModule_.body().addChildComponent(*boardView_);
    boardViewButton_.onClick = [this] { setBoardEditorView(!showingBoard_); };

    envModule_.body().addAndMakeVisible(env1Trace_);
    envModule_.body().addAndMakeVisible(env2Trace_);
    for (int i = kEnv1Attack; i <= kEnv2Release; ++i)
        envModule_.body().addAndMakeVisible(*knobs_[static_cast<size_t>(i)]);
    for (auto* role : { &env1Role_, &env2Role_ })
    {
        role->setJustificationType(juce::Justification::centred);
        envModule_.body().addAndMakeVisible(*role);
    }
    env1Role_.setText("VCF", juce::dontSendNotification);
    env2Role_.setText("VCA", juce::dontSendNotification);

    lfoModule_.body().addAndMakeVisible(lfo1Trace_);
    lfoModule_.body().addAndMakeVisible(lfo2Trace_);
    for (int i = kLfo1Rate; i <= kLfo2Attack; ++i)
        lfoModule_.body().addAndMakeVisible(*knobs_[static_cast<size_t>(i)]);
    for (int i = kLfo1Wave; i <= kLfo2Retrig; ++i)
        lfoModule_.body().addAndMakeVisible(*selectors_[static_cast<size_t>(i)]);
    for (int i : { kLfo2Sync, kLfo2Division })
        lfoModule_.body().addAndMakeVisible(*selectors_[static_cast<size_t>(i)]);
    mixModule_.addAndMakeVisible(seqViewButton_);
    seqViewButton_.getProperties().set("swaraxtSecondaryAction", true);
    mixModule_.body().addChildComponent(*sequencerView_);

    perfModule_.body().addAndMakeVisible(*knobs_[static_cast<size_t>(kGlide)]);
    perfModule_.body().addAndMakeVisible(*knobs_[static_cast<size_t>(kMaster)]);
    perfModule_.body().addAndMakeVisible(*selectors_[static_cast<size_t>(kLegato)]);
    perfModule_.body().addAndMakeVisible(*selectors_[static_cast<size_t>(kInputTune)]);

    seqViewButton_.onClick = [this] { setLocalViews(false, ! showingSequencer_); };
    lookAndFeelChanged();
    setLocalViews(false, false);
}

MainPanel::~MainPanel() = default;

void MainPanel::attach(SwaraXtAudioProcessor& processor)
{
    if (attached_)
        return;

    processor_ = &processor;
    dualSvfView_ = std::make_unique<DualSvfPanel>(processor);
    addChildComponent(*dualSvfView_);
    dualSvfView_->closeRequested = [this] { setDualSvfView(false); };
    dualSvfView_->selectionChanged = [this] { refreshExperimentalSvf(); };
    filterModel_.onChange = [this] {
        processor_->setExperimentalSvfEnabled(filterModel_.getSelectedId() == 2);
        refreshExperimentalSvf();
    };
    refreshExperimentalSvf();
    auto& apvts = processor.getApvts();
    const char* knobIds[] = {
        swaraxt::IDs::mixBalance, swaraxt::IDs::mixSub, swaraxt::IDs::mixNoise,
        swaraxt::IDs::filterCutoff, swaraxt::IDs::filterResonance, swaraxt::IDs::filterEnvDepth,
        swaraxt::IDs::filterKeyTracking, swaraxt::IDs::filterLfoDepth,
        swaraxt::IDs::env1Attack, swaraxt::IDs::env1Decay, swaraxt::IDs::env1Sustain, swaraxt::IDs::env1Release,
        swaraxt::IDs::env2Attack, swaraxt::IDs::env2Decay, swaraxt::IDs::env2Sustain, swaraxt::IDs::env2Release,
        swaraxt::IDs::lfo1Rate, swaraxt::IDs::lfo1Attack,
        swaraxt::IDs::lfo2Rate, swaraxt::IDs::lfo2Attack,
        swaraxt::IDs::perfGlide, swaraxt::IDs::master
    };

    sliderAttachments_.clear();
    sliderAttachments_.reserve(knobs_.size());
    for (size_t i = 0; i < knobs_.size(); ++i)
    {
        if (i == kFilterEnv) knobs_[i]->slider().setTooltip("ENV 1 -> Filter Cutoff");
        if (i == kFilterLfo) knobs_[i]->slider().setTooltip("LFO 2 -> Filter Cutoff");
        setSliderDefault(apvts, knobIds[i], knobs_[i]->slider());
        sliderAttachments_.push_back(std::make_unique<juce::AudioProcessorValueTreeState::SliderAttachment>(
            apvts, knobIds[i], knobs_[i]->slider()));
        if (dynamic_cast<juce::AudioParameterFloat*>(apvts.getParameter(knobIds[i])) != nullptr) {
            auto& slider = knobs_[i]->slider();
            const bool frequency = juce::String(knobIds[i]) == swaraxt::IDs::filterCutoff;
            slider.textFromValueFunction = [frequency](double value) {
                if (!frequency)
                    return juce::String(value, 3);
                if (value < 100.0)
                    return juce::String(value, 1) + " Hz";
                if (value < 1000.0)
                    return juce::String(juce::roundToInt(value)) + " Hz";
                return juce::String(value / 1000.0, value < 10000.0 ? 2 : 1) + " kHz";
            };
            slider.updateText();
        }
    }

    const char* selectorIds[] = {
        swaraxt::IDs::osc1Option,
        swaraxt::IDs::mixSubShape,
        swaraxt::IDs::lfo1Wave, swaraxt::IDs::lfo1Retrig,
        swaraxt::IDs::lfo1Sync, swaraxt::IDs::lfo1Division,
        swaraxt::IDs::lfo2Wave, swaraxt::IDs::lfo2Retrig,
        swaraxt::IDs::lfo2Sync, swaraxt::IDs::lfo2Division,
        swaraxt::IDs::perfLegato,
        swaraxt::IDs::midiInputTuning
    };

    selectorAttachments_.clear();
    selectorAttachments_.reserve(selectors_.size());
    for (size_t i = 0; i < selectors_.size(); ++i)
        selectorAttachments_.push_back(std::make_unique<juce::AudioProcessorValueTreeState::ComboBoxAttachment>(
            apvts, selectorIds[i], selectors_[i]->combo()));

    osc1_.attach(apvts, swaraxt::IDs::osc1Shape, swaraxt::IDs::osc1Range,
                 swaraxt::IDs::osc1Param);
    osc2_.attach(apvts, swaraxt::IDs::osc2Shape, swaraxt::IDs::osc2Range,
                 swaraxt::IDs::osc2Param, swaraxt::IDs::osc2Option);

    env1Trace_.bind(apvts, swaraxt::IDs::env1Attack, swaraxt::IDs::env1Decay,
                    swaraxt::IDs::env1Sustain, swaraxt::IDs::env1Release);
    env2Trace_.bind(apvts, swaraxt::IDs::env2Attack, swaraxt::IDs::env2Decay,
                    swaraxt::IDs::env2Sustain, swaraxt::IDs::env2Release);
    lfo1Trace_.bind(apvts, swaraxt::IDs::lfo1Wave, swaraxt::IDs::lfo1Rate);
    lfo2Trace_.bind(apvts, swaraxt::IDs::lfo2Wave, swaraxt::IDs::lfo2Rate);
    modulationView_->attach(processor);
    sequencerView_->attach(processor);
    boardView_->attach(apvts);
    attached_ = true;
}

void MainPanel::setSkinAssets(const SkinAssetCache& assets)
{
    const auto* frame = assets.get(AssetRole::visualizerFrame);
    lfo1Trace_.setFrame(frame);
    lfo2Trace_.setFrame(frame);
    env1Trace_.setFrame(frame);
    env2Trace_.setFrame(frame);
}

void MainPanel::lookAndFeelChanged()
{
    env1Role_.setColour(juce::Label::textColourId, Palette::skin().secondaryText);
    env2Role_.setColour(juce::Label::textColourId, Palette::skin().secondaryText);
    seqViewButton_.setColour(juce::TextButton::textColourOffId,
                             Palette::skin().secondaryText);
    seqViewButton_.setColour(juce::TextButton::textColourOnId,
                             Palette::skin().primaryText);
    boardViewButton_.setColour(juce::TextButton::textColourOffId, Palette::skin().secondaryText);
    boardViewButton_.setColour(juce::TextButton::textColourOnId, Palette::skin().primaryText);
    dualSvfViewButton_.setColour(juce::TextButton::textColourOffId, Palette::skin().primaryText);
    dualSvfViewButton_.setColour(juce::TextButton::textColourOnId, Palette::skin().primaryText);
}

void MainPanel::refreshExperimentalSvf()
{
    if (processor_ == nullptr) return;
    const auto requested = processor_->experimentalSvfSelection();
    const auto packed = ExperimentalSvfSelection::pack(requested);
    if (displayedSvfSelection_ == packed) return;
    displayedSvfSelection_ = packed;
    filterModel_.setSelectedId(requested.enabled ? 2 : 1, juce::dontSendNotification);
    dualSvfView_->sync(requested);
}

void MainPanel::setDualSvfView(bool visible)
{
    if (! dualSvfView_) return;
    refreshExperimentalSvf();
    dualSvfView_->setVisible(visible);
    // Covered modules must also leave keyboard focus traversal while expanded.
    perfModule_.setVisible(! visible);
    envModule_.setVisible(! visible);
    dualSvfViewButton_.setToggleState(visible, juce::dontSendNotification);
    if (visible) dualSvfView_->toFront(false);
    else if (dualSvfViewButton_.isShowing()) dualSvfViewButton_.grabKeyboardFocus();
}

void MainPanel::setBoardEditorView(bool visible)
{
    showingBoard_ = visible;
    perfModule_.setTitle(visible ? "FX" : "GLOBAL");
    boardViewButton_.setButtonText(visible ? "GLOBAL" : "FX");
    boardViewButton_.setTooltip(visible ? "Return to Global controls" : "Effects after the Classic filter");
    reflow();
}

void MainPanel::setLocalViews(bool modulation, bool sequencer)
{
    showingModulation_ = modulation;
    showingSequencer_ = sequencer;
    modulationView_->setVisible(modulation);
    sequencerView_->setVisible(sequencer);
    seqViewButton_.setToggleState(sequencer, juce::dontSendNotification);
    seqViewButton_.setButtonText(sequencer ? "SYNTH" : "SEQ/ARP");
    mixModule_.setSecondaryHeaderVisible(! sequencer);
    mixModule_.setTitle(sequencer ? "SEQ / ARP" : "MIXER");
    seqViewButton_.toFront(false);
    reflow();
    if (workspaceChanged) workspaceChanged();
}

juce::ComboBox& MainPanel::sequenceEventComboForTests() noexcept
{
    return sequencerView_->eventComboForTests();
}

juce::ComboBox& MainPanel::arpComboForTests(int index) noexcept
{
    return sequencerView_->arpComboForTests(index);
}

juce::Rectangle<int> MainPanel::sequenceStartBoundsForTests() const
{
    return mixModule_.body().getLocalArea(sequencerView_.get(),
                                          sequencerView_->startBoundsForTests());
}

juce::Rectangle<int> MainPanel::sequenceLengthBoundsForTests() const
{
    return mixModule_.body().getLocalArea(sequencerView_.get(),
                                          sequencerView_->lengthBoundsForTests());
}

juce::Rectangle<int> MainPanel::sequenceEventComboBoundsForTests() const
{
    return mixModule_.body().getLocalArea(sequencerView_.get(),
                                          sequencerView_->eventComboBoundsForTests());
}

juce::Rectangle<int> MainPanel::sequenceNoteBoundsForTests() const
{
    return mixModule_.body().getLocalArea(sequencerView_.get(),
                                          sequencerView_->noteBoundsForTests());
}

juce::Rectangle<int> MainPanel::sequenceVelocityBoundsForTests() const
{
    return mixModule_.body().getLocalArea(sequencerView_.get(),
                                          sequencerView_->velocityBoundsForTests());
}

juce::Rectangle<int> MainPanel::sequenceNavigationBoundsForTests() const
{
    return mixModule_.body().getLocalArea(&seqViewButton_, seqViewButton_.getLocalBounds());
}

juce::Rectangle<int> MainPanel::arpComboBoundsForTests(int index) const
{
    return mixModule_.body().getLocalArea(sequencerView_.get(),
                                          sequencerView_->arpComboBoundsForTests(index));
}

void MainPanel::setSequencerHostSyncForTests(bool enabled)
{
    sequencerView_->setHostSyncForTests(enabled);
}


void MainPanel::setSequencerPatternForTests(int pattern)
{
    sequencerView_->setArpPatternForTests(pattern);
}

int MainPanel::sequencerPatternForTests() const
{
    return sequencerView_->arpPatternForTests();
}

void MainPanel::setSequenceLayoutForTests(int length, int rotation, int groove)
{
    sequencerView_->setSequenceLayoutForTests(length, rotation, groove);
}

void MainPanel::setSequenceStepForTests(int step, int note, int event, int velocity, int value)
{
    sequencerView_->setSequenceStepForTests(step, note, event, velocity, value);
}

void MainPanel::reflow()
{
    const int gap = Layout::moduleGap;
    sourceModule_.setBounds(26, 22, 318, 184);
    lfoModule_.setBounds(26, 209, 318, 298);
    mixModule_.setBounds(383, showingSequencer_ ? 105 : 180, 347, showingSequencer_ ? 398 : 323);
    filterModule_.setBounds(769, 22, 318, 132);
    filterModel_.setBounds(0, 1, 122, 20);
    dualSvfViewButton_.setBounds(241, 1, 77, 20);
    filterModule_.setPrimaryActionBounds(dualSvfViewButton_.getBounds());
    if (dualSvfView_) dualSvfView_->setBounds(769, 155, 318, 348);
    perfModule_.setBounds(769, 157, 318, 100);
    envModule_.setBounds(769, 260, 318, 243);

    auto source = sourceModule_.body().getLocalBounds();
    auto osc1 = source.removeFromTop((source.getHeight() - gap) / 2);
    source.removeFromTop(gap);
    osc1_.setBounds(osc1);
    osc2_.setBounds(source);

    auto mix = mixModule_.body().getLocalBounds();
    if (!showingSequencer_) mix.removeFromTop(20);
    constexpr int sequenceActionWidth = 58;
    constexpr int sequenceActionHeight = 18;
    constexpr int modMatrixTitleWidth = 100;
    constexpr int modMatrixDividerY = 180;
    const auto bodyBounds = mixModule_.body().getBounds();
    const int sequenceActionX = (mixModule_.getWidth() + modMatrixTitleWidth) / 2
        + 10 - bodyBounds.getX();
    const int sequenceActionY = modMatrixDividerY - bodyBounds.getY()
        - sequenceActionHeight / 2;
    seqViewButton_.setBounds(sequenceActionX + bodyBounds.getX(), showingSequencer_ ? 2 : sequenceActionY + bodyBounds.getY(),
                             sequenceActionWidth, sequenceActionHeight);
    mixModule_.setSecondaryActionBounds(
        showingSequencer_ ? juce::Rectangle<int>{} : seqViewButton_.getBounds());
    mixModule_.setPrimaryActionBounds(showingSequencer_ ? seqViewButton_.getBounds() : juce::Rectangle<int>{});
    sequencerView_->setBounds(mix);
    auto matrix = mix.removeFromBottom(149);
    matrix.removeFromTop(Layout::moduleHeaderRow);
    modulationView_->setBounds(matrix);
    modulationView_->reflow();
    sequencerView_->reflow();
    layoutRow(mix.removeFromTop(68),
               { knobs_[kMixBalance].get(), knobs_[kMixSub].get(), knobs_[kMixNoise].get() });
    mix.removeFromTop(4);
    layoutRow(mix, { selectors_[kMixOperator].get(), selectors_[kSubShape].get() }, 5);

    auto filter = filterModule_.body().getLocalBounds();
    boardViewButton_.setBounds(247, 1, 60, 18);
    perfModule_.setPrimaryActionBounds(boardViewButton_.getBounds());
    layoutRow(filter, { knobs_[kFilterCutoff].get(), knobs_[kFilterResonance].get(),
                        knobs_[kFilterEnv].get(), knobs_[kFilterKey].get(),
                        knobs_[kFilterLfo].get() }, 3);

    auto env = envModule_.body().getLocalBounds();
    auto layoutEnv = [&](juce::Rectangle<int> bounds,
                         int a,
                         int d,
                         int s,
                         int r) {
        layoutRow(bounds, { knobs_[static_cast<size_t>(a)].get(),
                            knobs_[static_cast<size_t>(d)].get(),
                            knobs_[static_cast<size_t>(s)].get(),
                            knobs_[static_cast<size_t>(r)].get() }, 4);
    };
    env1Role_.setBounds(env.removeFromTop(12).removeFromLeft(46));
    layoutEnv(env.removeFromTop(65), kEnv1Attack, kEnv1Decay, kEnv1Sustain, kEnv1Release);
    env.removeFromTop(Layout::moduleHeaderRow);
    env2Role_.setBounds(env.removeFromTop(12).removeFromLeft(46));
    layoutEnv(env.removeFromTop(65), kEnv2Attack, kEnv2Decay, kEnv2Sustain, kEnv2Release);
    env.removeFromTop(3);
    auto traces = env;
    env1Trace_.setBounds(traces.removeFromLeft((traces.getWidth() - 4) / 2));
    traces.removeFromLeft(4);
    env2Trace_.setBounds(traces);

    auto lfo = lfoModule_.body().getLocalBounds();
    auto layoutLfo = [&](juce::Rectangle<int> bounds,
                         int wave,
                         int mode,
                         int rate,
                         int attack) {
        auto selectorArea = bounds.removeFromLeft(188);
        bounds.removeFromLeft(4);
        auto selectors = selectorArea.removeFromTop(selectorArea.getHeight() / 2);
        layoutRow(selectors, { selectors_[static_cast<size_t>(wave)].get(),
                               selectors_[static_cast<size_t>(mode)].get() }, 4);
        auto timing = selectorArea;
        const int sync = mode == kLfo1Retrig ? kLfo1Sync : kLfo2Sync;
        const int division = mode == kLfo1Retrig ? kLfo1Division : kLfo2Division;
        layoutRow(timing, { selectors_[static_cast<size_t>(sync)].get(),
                            selectors_[static_cast<size_t>(division)].get() }, 4);
        layoutRow(bounds, { knobs_[static_cast<size_t>(rate)].get(),
                            knobs_[static_cast<size_t>(attack)].get() }, 4);
    };
    auto lfoTraces = lfo.removeFromBottom(48);
    auto lfo1 = lfo.removeFromTop((lfo.getHeight() - Layout::moduleHeaderRow) / 2);
    lfo.removeFromTop(Layout::moduleHeaderRow);
    layoutLfo(lfo1, kLfo1Wave, kLfo1Retrig, kLfo1Rate, kLfo1Attack);
    layoutLfo(lfo, kLfo2Wave, kLfo2Retrig, kLfo2Rate, kLfo2Attack);
    lfo1Trace_.setBounds(lfoTraces.removeFromLeft((lfoTraces.getWidth() - 4) / 2));
    lfoTraces.removeFromLeft(4);
    lfo2Trace_.setBounds(lfoTraces);

    const bool showMix = ! showingModulation_ && ! showingSequencer_;
    juce::ignoreUnused(showMix);
    for (int i = kMixBalance; i <= kMixNoise; ++i)
        knobs_[static_cast<size_t>(i)]->setVisible(! showingSequencer_);
    selectors_[kMixOperator]->setVisible(! showingSequencer_);
    selectors_[kSubShape]->setVisible(! showingSequencer_);
    modulationView_->setVisible(! showingSequencer_);
    sequencerView_->setVisible(showingSequencer_);

    lfo1Trace_.setVisible(true);
    lfo2Trace_.setVisible(true);
    for (int i = kLfo1Rate; i <= kLfo2Attack; ++i)
        knobs_[static_cast<size_t>(i)]->setVisible(true);
    for (int i = kLfo1Wave; i <= kLfo2Division; ++i)
        selectors_[static_cast<size_t>(i)]->setVisible(true);

    auto perf = perfModule_.body().getLocalBounds();
    boardView_->setBounds(perf);
    boardView_->setVisible(showingBoard_);
    knobs_[kMaster]->setVisible(!showingBoard_);
    knobs_[kGlide]->setVisible(!showingBoard_);
    selectors_[kLegato]->setVisible(!showingBoard_);
    selectors_[kInputTune]->setVisible(!showingBoard_);
    layoutRow(perf, { knobs_[kMaster].get(), knobs_[kGlide].get(),
                      selectors_[kLegato].get(), selectors_[kInputTune].get() }, 4);
}

}  // namespace swaraxt::ui
