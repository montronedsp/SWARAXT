// Copyright 2026 MontroneDSP.
// SPDX-License-Identifier: GPL-3.0-or-later

#include "Ui/SwaraXtSeqPanel.h"
#include "Ui/SwaraXtTypography.h"

#include "Engine/SequencerMidiRoute.h"
#include "Plugin/PluginProcessor.h"
#include "Tuning/ScalePitch.h"

namespace swaraxt::ui {

SeqPanel::SeqPanel()
{
    knobs_.push_back(std::make_unique<SwaraXtKnob>("TEMPO"));
    knobs_.push_back(std::make_unique<SwaraXtKnob>("SWING"));
    knobs_.push_back(std::make_unique<SwaraXtKnob>("GATE"));
    for (auto& knob : knobs_)
        addAndMakeVisible(*knob);

    selectors_.push_back(std::make_unique<SwaraXtSelector>("MODE", seqModeNames()));
    selectors_.push_back(std::make_unique<SwaraXtSelector>("CLOCK", clockModeNames()));
    selectors_.push_back(std::make_unique<SwaraXtSelector>("DIRECTION", arpDirectionNames()));
    auto patterns = numberedNames("Pattern", 1, 15);
    patterns.add("Sequence");
    selectors_.push_back(std::make_unique<SwaraXtSelector>("PATTERN", patterns));
    selectors_.push_back(std::make_unique<SwaraXtSelector>("OCTAVES", numberedNames("Octave", 1, 4)));
    selectors_.push_back(std::make_unique<SwaraXtSelector>("DIVISION", juce::StringArray {
        "1/1", "1/2", "1/2T", "1/4", "1/4T", "1/8", "1/8T", "1/16",
        "1/16T", "1/32", "1/32T", "1/64" }));
    for (auto& selector : selectors_)
        addAndMakeVisible(*selector);

    selectors_[kClockMode]->combo().onChange = [this] {
        selectors_[kClockMode]->refreshDescription();
        updateClockControls();
    };
    selectors_[kPattern]->combo().onChange = [this] {
        selectors_[kPattern]->refreshDescription();
        if (refreshingSequenceControls_ || sequenceState_ == nullptr)
            return;
        const int pattern = selectors_[kPattern]->combo().getSelectedItemIndex();
        sequenceState_->setArpPattern(pattern);
        if (pattern <= 15 && arpPatternAttachment_ != nullptr)
            arpPatternAttachment_->setValueAsCompleteGesture(static_cast<float>(pattern));
    };

    for (auto* heading : { &arpHeading_, &sequenceHeading_ }) {
        addAndMakeVisible(*heading);
        setTextRole(*heading, TextRole::sectionHeader);
        heading->setJustificationType(juce::Justification::centredLeft);
    }
    arpHeading_.setText("ARPEGGIATOR", juce::dontSendNotification);
    sequenceHeading_.setText("SEQUENCER", juce::dontSendNotification);
    for (auto* button : { &midiOutButton_, &importTuning_, &lockButton_, &randomNotes_, &randomVelocity_, &randomEvent_, &randomValue_, &randomSequence_ }) {
        addAndMakeVisible(*button);
        button->getProperties().set("swaraxtSecondaryAction", true);
    }
    midiOutButton_.setClickingTogglesState(true);
    midiOutButton_.setTooltip(
        "Send sequencer notes to the host MIDI output. LEGACY and QUANTIZED CV emit notes. LINEAR and SCALE stay silent.");
    addAndMakeVisible(pitchModeBox_);
    pitchModeBox_.addItem("LEGACY", 1);
    pitchModeBox_.addItem("LINEAR", 2);
    pitchModeBox_.addItem("SCALE", 3);
    pitchModeBox_.addItem("QUANTIZED CV", 4);
    pitchModeBox_.setSelectedId(1, juce::dontSendNotification);
    pitchModeBox_.setTooltip(
        "Sequencer pitch. LEGACY uses integer notes. LINEAR uses free CV. SCALE uses tuning degrees. QUANTIZED CV snaps free CV to the active tuning.");
    pitchModeBox_.onChange = [this] {
        if (refreshingSequenceControls_ || sequenceState_ == nullptr)
            return;
        int16_t midiDegree[128];
        bool midiMapped[128] {};
        for (int note = 0; note < 128; ++note)
            midiDegree[note] = -1;
        if (processor_ != nullptr)
            processor_->copyTuningDegrees(midiDegree, 128, midiMapped);
        sequenceState_->setPitchMode(static_cast<uint8_t>(juce::jlimit(0, 3, pitchModeBox_.getSelectedId() - 1)),
                                     midiDegree, midiMapped);
    };
    addAndMakeVisible(tuningBox_);
    tuningBox_.setTooltip("Built-in tuning table shared by SCALE, QUANTIZED CV and INPUT TUNE.");
    importTuning_.setTooltip("Load .swt or .scl. Select the .scl and optional .kbm together to use a keyboard mapping.");
    importTuning_.onClick = [this] {
        if (processor_ == nullptr)
            return;
        tuningChooser_ = std::make_unique<juce::FileChooser>(
            "Select one SWT or SCL (optionally select its KBM too)", juce::File(), "*.swt;*.scl;*.kbm");
        tuningChooser_->launchAsync(juce::FileBrowserComponent::openMode | juce::FileBrowserComponent::canSelectFiles
                                        | juce::FileBrowserComponent::canSelectMultipleItems,
                                    [this](const juce::FileChooser& chooser) {
            const auto files = chooser.getResults();
            if (files.isEmpty() || processor_ == nullptr)
                return;
            juce::File scale, keyboard;
            bool valid = true;
            for (const auto& file : files)
            {
                if (file.hasFileExtension("kbm") && keyboard == juce::File())
                    keyboard = file;
                else if (file.hasFileExtension("scl;swt") && scale == juce::File())
                    scale = file;
                else
                    valid = false;
            }
            juce::String error;
            if (! valid || scale == juce::File())
                error = "Select exactly one .scl or .swt file and, for Scala, at most one .kbm";
            else if (processor_->importTuningFile(scale, keyboard, error))
                refreshSequenceControls();
            if (error.isNotEmpty())
                juce::AlertWindow::showMessageBoxAsync(juce::MessageBoxIconType::WarningIcon,
                                                       "Tuning import failed", error);
        });
    };
    lockButton_.setClickingTogglesState(true);
    lockButton_.setTooltip("Preserve Seq/Arp settings and pattern across preset changes");
    lockButton_.onClick = [this] { if (processor_) processor_->setSequenceLocked(lockButton_.getToggleState()); };
    const auto randomize = [this](SequenceState::Randomize kind) {
        if (sequenceState_) {
            const auto range = processor_ ? processor_->randomNoteRange() : SequenceState::NoteRange {};
            const auto scaleView = processor_ != nullptr && sequenceState_->snapshot().pitchMode == 2
                ? processor_->scaleRandomView() : swaraxt::SequenceState::ScaleRandomView {};
            sequenceState_->randomize(kind, editRandom_, range, swaraxt::SequenceState::RandomizationType::type1,
                                      sequenceState_->snapshot().pitchMode == 2 ? &scaleView : nullptr);
            refreshSequenceControls();
        }
    };
    randomNotes_.onClick = [randomize] { randomize(SequenceState::Randomize::notes); };
    randomVelocity_.onClick = [randomize] { randomize(SequenceState::Randomize::velocity); };
    randomSequence_.onClick = [randomize] { randomize(SequenceState::Randomize::sequence); };
    randomEvent_.onClick = [randomize] { randomize(SequenceState::Randomize::events); };
    randomValue_.onClick = [randomize] { randomize(SequenceState::Randomize::value); };
    randomEvent_.setTooltip("Change Note/Rest/Tie only; preserve pitches, velocities and values");
    randomValue_.setTooltip("Change controller values only (0--15)");
    addAndMakeVisible(randomType_);
    randomType_.addItem("Type 1", 1);
    randomType_.setSelectedId(1, juce::dontSendNotification);
    randomType_.setTooltip("Type 1: pentatonic notes, velocities, rests/ties and controller values");
    addAndMakeVisible(rangeLabel_);
    rangeLabel_.setJustificationType(juce::Justification::centredLeft);
    addAndMakeVisible(noteRange_);
    noteRange_.setSliderStyle(juce::Slider::TwoValueHorizontal);
    noteRange_.setTextBoxStyle(juce::Slider::NoTextBox, false, 0, 0);
    noteRange_.setRange(-SequenceState::NoteRange::extent, SequenceState::NoteRange::extent, 1);
    noteRange_.setMinAndMaxValues(-12, 12, juce::dontSendNotification);
    noteRange_.setTooltip("Note offsets from step 1: lower -48..0, upper 0..+48 semitones; pentatonic Type 1");
    noteRange_.onValueChange = [this] {
        const SequenceState::NoteRange range {
            juce::jmin(0, static_cast<int>(noteRange_.getMinValue())),
            juce::jmax(0, static_cast<int>(noteRange_.getMaxValue())) };
        noteRange_.setMinAndMaxValues(range.lower, range.upper, juce::dontSendNotification);
        rangeLabel_.setText("RANGE " + juce::String(range.lower) + " / +" + juce::String(range.upper),
                             juce::dontSendNotification);
        if (processor_) processor_->setRandomNoteRange(range);
    };
    noteRange_.onValueChange();
    randomNotes_.setTooltip("Change pitches only; preserve events, velocities and controller values");
    randomVelocity_.setTooltip("Change velocities only; preserve pitches, events and controller values");
    randomSequence_.setTooltip("New notes, velocities, rests/ties and controller values; preserve playback settings");
    selectors_[kMode]->combo().onChange = [this] {
        selectors_[kMode]->refreshDescription(); updateModeEmphasis();
    };
    controller_.slider().setTooltip("Sequence controller value: feeds Seq modulation sources in every mode");
    length_.slider().setTooltip("Shared sequence/arp cycle length");
    rotation_.slider().setTooltip("Step start offset; also used by the arp's Sequence pattern");

    for (int i = 0; i < static_cast<int>(stepButtons_.size()); ++i)
    {
        auto& button = stepButtons_[static_cast<size_t>(i)];
        addAndMakeVisible(button);
        button.getProperties().set("swaraxtSecondaryAction", true);
        button.onClick = [this, i] {
            selectedStep_ = i;
            refreshSequenceControls();
        };
    }

    const std::array<juce::Component*, 7> sequenceControls {
        &length_, &rotation_, &note_, &event_, &velocity_, &controller_, &groove_
    };
    for (auto* control : sequenceControls)
        addAndMakeVisible(control);

    auto configureInteger = [](SwaraXtKnob& knob, int minimum, int maximum, int initial) {
        knob.slider().setRange(minimum, maximum, 1.0);
        knob.slider().setValue(initial, juce::dontSendNotification);
    };
    configureInteger(length_, 1, 16, 16);
    configureInteger(rotation_, 0, 15, 0);
    configureInteger(note_, 0, 127, 48);
    configureInteger(velocity_, 0, 7, 7);
    configureInteger(controller_, 0, 15, 0);

    length_.slider().onValueChange = [this] {
        if (! refreshingSequenceControls_ && sequenceState_ != nullptr)
            sequenceState_->setLength(static_cast<int>(length_.slider().getValue()));
    };
    rotation_.slider().onValueChange = [this] {
        if (! refreshingSequenceControls_ && sequenceState_ != nullptr)
            sequenceState_->setRotation(static_cast<int>(rotation_.slider().getValue()));
    };
    groove_.combo().onChange = [this] {
        groove_.refreshDescription();
        if (! refreshingSequenceControls_ && sequenceState_ != nullptr)
            sequenceState_->setGrooveTemplate(groove_.combo().getSelectedItemIndex());
    };
    note_.slider().onValueChange = [this] {
        if (refreshingSequenceControls_ || sequenceState_ == nullptr)
            return;
        const auto snapshot = sequenceState_->snapshot();
        if (snapshot.pitchMode == 1 || snapshot.pitchMode == 3)
        {
            const double semis = note_.slider().getValue();
            const auto units = static_cast<int32_t>(std::llround(semis * swaraxt::FreeCvPitch::kUnitsPerSemitone));
            sequenceState_->setFreePitch(selectedStep_, units);
            return;
        }
        if (snapshot.pitchMode == 2)
        {
            sequenceState_->setScaleDegree(selectedStep_, static_cast<int>(std::lround(note_.slider().getValue())));
            return;
        }
        const auto packed = snapshot.steps[static_cast<size_t>(selectedStep_)];
        writeSelectedStep(static_cast<uint8_t>((SequenceSnapshot::dataA(packed) & 0x80)
                              | (static_cast<int>(note_.slider().getValue()) & 0x7f)),
                          SequenceSnapshot::dataB(packed));
    };
    event_.combo().onChange = [this] {
        event_.refreshDescription();
        if (refreshingSequenceControls_ || sequenceState_ == nullptr)
            return;
        const auto snapshot = sequenceState_->snapshot();
        const auto packed = snapshot.steps[static_cast<size_t>(selectedStep_)];
        uint8_t dataA = SequenceSnapshot::dataA(packed);
        uint8_t dataB = SequenceSnapshot::dataB(packed);
        const int event = event_.combo().getSelectedItemIndex();
        dataA = event == 0 ? static_cast<uint8_t>(dataA & 0x7f)
                           : static_cast<uint8_t>(dataA | 0x80);
        dataB = event == 2 ? static_cast<uint8_t>(dataB | 0x80)
                           : static_cast<uint8_t>(dataB & 0x7f);
        writeSelectedStep(dataA, dataB);
    };
    velocity_.slider().onValueChange = [this] {
        if (refreshingSequenceControls_ || sequenceState_ == nullptr)
            return;
        const auto snapshot = sequenceState_->snapshot();
        const auto packed = snapshot.steps[static_cast<size_t>(selectedStep_)];
        const auto velocity = static_cast<uint8_t>(static_cast<int>(velocity_.slider().getValue()) & 0x07);
        writeSelectedStep(SequenceSnapshot::dataA(packed),
                          static_cast<uint8_t>((SequenceSnapshot::dataB(packed) & 0x8f)
                                               | (velocity << 4)));
    };
    controller_.slider().onValueChange = [this] {
        if (refreshingSequenceControls_ || sequenceState_ == nullptr)
            return;
        const auto snapshot = sequenceState_->snapshot();
        const auto packed = snapshot.steps[static_cast<size_t>(selectedStep_)];
        const auto value = static_cast<uint8_t>(static_cast<int>(controller_.slider().getValue()) & 0x0f);
        writeSelectedStep(SequenceSnapshot::dataA(packed),
                          static_cast<uint8_t>((SequenceSnapshot::dataB(packed) & 0xf0) | value));
    };

    lookAndFeelChanged();
    updateModeEmphasis();
}

SeqPanel::~SeqPanel()
{
    if (sequenceState_ != nullptr)
        sequenceState_->removeListener(this);
}

void SeqPanel::attach(SwaraXtAudioProcessor& processor)
{
    if (attached_)
        return;

    auto& apvts = processor.getApvts();
    processor_ = &processor;
    sequenceState_ = &processor.sequenceState();
    timerCallback();
    lockButton_.setToggleState(processor.sequenceLocked(), juce::dontSendNotification);
    if (auto* midiOut = apvts.getParameter(swaraxt::IDs::seqMidiOut))
        midiOutAttachment_ = std::make_unique<juce::ButtonParameterAttachment>(
            *midiOut, midiOutButton_, nullptr);
    tuningBox_.clear(juce::dontSendNotification);
    for (int index = 0; index < processor.builtinTuningCount(); ++index)
        tuningBox_.addItem(processor.builtinTuningName(index), index + 1);
    tuningBox_.addItem("Imported", 100);
    const int active = processor.activeBuiltinTuning();
    tuningBox_.setSelectedId(active >= 0 ? active + 1 : 100, juce::dontSendNotification);
    tuningBox_.onChange = [this] {
        if (refreshingSequenceControls_ || processor_ == nullptr)
            return;
        const int id = tuningBox_.getSelectedId();
        if (id >= 1 && id <= processor_->builtinTuningCount())
            processor_->loadBuiltinTuning(id - 1);
        refreshSequenceControls();
    };
    visibilityChanged();
    sequenceState_->addListener(this);
    const char* knobIds[] = { swaraxt::IDs::seqTempo, swaraxt::IDs::seqSwing,
                              swaraxt::IDs::seqGate };
    sliderAttachments_.clear();
    for (size_t i = 0; i < knobs_.size(); ++i)
    {
        setSliderDefault(apvts, knobIds[i], knobs_[i]->slider());
        sliderAttachments_.push_back(std::make_unique<juce::AudioProcessorValueTreeState::SliderAttachment>(
            apvts, knobIds[i], knobs_[i]->slider()));
    }

    const char* selectorIds[] = {
        swaraxt::IDs::seqMode,
        swaraxt::IDs::seqClockMode,
        swaraxt::IDs::arpDirection,
        swaraxt::IDs::arpOctaves,
        swaraxt::IDs::arpGate
    };
    selectorAttachments_.clear();
    const int selectorIndices[] = { kMode, kClockMode, kDirection, kOctaves, kDivision };
    for (size_t i = 0; i < std::size(selectorIds); ++i)
        selectorAttachments_.push_back(std::make_unique<juce::AudioProcessorValueTreeState::ComboBoxAttachment>(
            apvts, selectorIds[i], selectors_[static_cast<size_t>(selectorIndices[i])]->combo()));

    if (auto* pattern = apvts.getParameter(swaraxt::IDs::arpPattern))
    {
        arpPatternAttachment_ = std::make_unique<juce::ParameterAttachment>(
            *pattern,
            [this](float value) {
                refreshingSequenceControls_ = true;
                selectors_[kPattern]->combo().setSelectedItemIndex(
                    juce::jlimit(0, 15, static_cast<int>(std::lround(value))),
                    juce::dontSendNotification);
                selectors_[kPattern]->refreshDescription();
                refreshingSequenceControls_ = false;
                updateModeEmphasis();
            },
            nullptr);
        arpPatternAttachment_->sendInitialUpdate();
    }

    updateClockControls();
    refreshSequenceControls();
    attached_ = true;
}

void SeqPanel::updateClockControls()
{
    const bool hostClock = selectors_[kClockMode]->combo().getSelectedItemIndex() == 1;
    knobs_[kTempo]->setEnabled(! hostClock);
    knobs_[kTempo]->setAlpha(hostClock ? 0.38f : 1.0f);
}

void SeqPanel::setHostSyncForTests(bool enabled)
{
    selectors_[kClockMode]->combo().setSelectedItemIndex(enabled ? 1 : 0,
                                                          juce::sendNotificationSync);
}

void SeqPanel::selectStepForTests(int step)
{
    selectedStep_ = juce::jlimit(0, SequenceSnapshot::kNumSteps - 1, step);
    refreshSequenceControls();
}

void SeqPanel::setArpPatternForTests(int pattern)
{
    selectors_[kPattern]->combo().setSelectedItemIndex(juce::jlimit(0, 15, pattern),
                                                        juce::sendNotificationSync);
}

int SeqPanel::arpPatternForTests() const noexcept
{
    return selectors_[kPattern]->combo().getSelectedItemIndex();
}

void SeqPanel::setSequenceLayoutForTests(int length, int rotation, int groove)
{
    length_.slider().setValue(juce::jlimit(1, 16, length), juce::sendNotificationSync);
    rotation_.slider().setValue(juce::jlimit(0, 15, rotation), juce::sendNotificationSync);
    groove_.combo().setSelectedItemIndex(juce::jlimit(0, 5, groove),
                                         juce::sendNotificationSync);
}

void SeqPanel::setSequenceStepForTests(int step, int note, int event, int velocity, int value)
{
    selectStepForTests(step);
    note_.slider().setValue(juce::jlimit(0, 127, note), juce::sendNotificationSync);
    event_.combo().setSelectedItemIndex(juce::jlimit(0, 2, event),
                                        juce::sendNotificationSync);
    velocity_.slider().setValue(juce::jlimit(0, 7, velocity), juce::sendNotificationSync);
    controller_.slider().setValue(juce::jlimit(0, 15, value), juce::sendNotificationSync);
}

void SeqPanel::lookAndFeelChanged()
{
    for (auto* heading : { &arpHeading_, &sequenceHeading_ })
        heading->setColour(juce::Label::textColourId, Palette::skin().primaryText);
}

void SeqPanel::visibilityChanged()
{
    timerCallback();
    startTimerHz(isShowing() ? 30 : 4);
}

void SeqPanel::paintOverChildren(juce::Graphics& g)
{
    if (activeStep_ < 0 || activeStep_ >= static_cast<int>(stepButtons_.size()))
        return;
    const auto bounds = stepButtons_[static_cast<size_t>(activeStep_)].getBounds().toFloat();
    // A separate lower lamp leaves the edit-selection border and number intact.
    g.setColour(Palette::skin().displayText);
    g.fillRoundedRectangle(bounds.getX() + 5.0f, bounds.getBottom() - 4.0f,
                           bounds.getWidth() - 10.0f, 2.0f, 1.0f);
}

void SeqPanel::timerCallback()
{
    // A cheap visibility probe also handles host peer/ancestor visibility
    // changes, which do not necessarily notify the child component.
    const bool showing = isShowing();
    const int interval = showing ? 1000 / 30 : 250;
    if (getTimerInterval() != interval)
        startTimer(interval);
    if (! showing)
        return;

    if (processor_) {
        const int nextStep = processor_->sequencePlaybackStep();
        if (nextStep != activeStep_) {
            for (const int step : { activeStep_, nextStep })
                if (step >= 0 && step < static_cast<int>(stepButtons_.size()))
                    repaint(stepButtons_[static_cast<size_t>(step)].getBounds());
            activeStep_ = nextStep;
        }
        lockButton_.setToggleState(processor_->sequenceLocked(), juce::dontSendNotification);
        const auto range = processor_->randomNoteRange();
        noteRange_.setMinAndMaxValues(range.lower, range.upper, juce::dontSendNotification);
        rangeLabel_.setText("RANGE " + juce::String(range.lower) + " / +" + juce::String(range.upper),
                             juce::dontSendNotification);
    }
}

void SeqPanel::updateModeEmphasis()
{
    const int mode = selectors_[kMode]->combo().getSelectedItemIndex();
    const float arpAlpha = mode == 1 ? 1.0f : 0.55f;
    const bool arpUsesSteps = mode == 1 && selectors_[kPattern]->combo().getSelectedItemIndex() == 15;
    const float seqAlpha = mode == 2 || arpUsesSteps ? 1.0f : 0.55f;
    arpHeading_.setAlpha(arpAlpha);
    for (int index : { kDirection, kPattern, kOctaves }) selectors_[static_cast<size_t>(index)]->setAlpha(arpAlpha);
    sequenceHeading_.setAlpha(seqAlpha);
    pitchModeBox_.setAlpha(seqAlpha);
    tuningBox_.setAlpha(seqAlpha);
    importTuning_.setAlpha(seqAlpha);
    for (auto* control : std::array<juce::Component*, 3>{ &note_, &event_, &velocity_ }) control->setAlpha(seqAlpha);
    for (auto& button : stepButtons_) button.setAlpha(seqAlpha);
    // Controller values and cycle timing also drive modulation in Step/Arp modes.
}

void SeqPanel::refreshSequenceControls()
{
    if (sequenceState_ == nullptr)
        return;

    const auto snapshot = sequenceState_->snapshot();
    refreshingSequenceControls_ = true;
    length_.slider().setValue(snapshot.length, juce::dontSendNotification);
    rotation_.slider().setValue(snapshot.rotation, juce::dontSendNotification);
    groove_.combo().setSelectedItemIndex(snapshot.grooveTemplate, juce::dontSendNotification);
    groove_.refreshDescription();
    selectors_[kPattern]->combo().setSelectedItemIndex(snapshot.arpPattern,
                                                        juce::dontSendNotification);
    selectors_[kPattern]->refreshDescription();

    refreshStepButtons(snapshot);

    const auto selected = snapshot.steps[static_cast<size_t>(selectedStep_)];
    const uint8_t dataA = SequenceSnapshot::dataA(selected);
    const uint8_t dataB = SequenceSnapshot::dataB(selected);
    pitchModeBox_.setSelectedId(snapshot.pitchMode + 1, juce::dontSendNotification);
    if (processor_ != nullptr)
    {
        const int active = processor_->activeBuiltinTuning();
        tuningBox_.setSelectedId(active >= 0 ? active + 1 : 100, juce::dontSendNotification);
        tuningBox_.setTooltip("Active tuning: " + processor_->activeTuningName());
    }
    refreshPitchModeControls(snapshot, dataA);
    velocity_.slider().setValue((dataB & 0x70) >> 4, juce::dontSendNotification);
    controller_.slider().setValue(dataB & 0x0f, juce::dontSendNotification);
    event_.combo().setSelectedItemIndex((dataA & 0x80) == 0 ? 0 : ((dataB & 0x80) != 0 ? 2 : 1),
                                        juce::dontSendNotification);
    event_.refreshDescription();
    refreshingSequenceControls_ = false;
    updateModeEmphasis();
}

void SeqPanel::refreshStepButtons(const SequenceSnapshot& snapshot)
{
    for (int i = 0; i < SequenceSnapshot::kNumSteps; ++i)
    {
        const auto packed = snapshot.steps[static_cast<size_t>(i)];
        const auto a = SequenceSnapshot::dataA(packed);
        const auto b = SequenceSnapshot::dataB(packed);
        juce::String pitch = juce::MidiMessage::getMidiNoteName(a & 0x7f, true, true, 3);
        if (snapshot.pitchMode == 1 || snapshot.pitchMode == 3)
            pitch = juce::String(snapshot.freePitch[static_cast<size_t>(i)]
                                 / static_cast<double>(swaraxt::FreeCvPitch::kUnitsPerSemitone), 2);
        else if (snapshot.pitchMode == 2)
            pitch = juce::String::formatted("%+03d", static_cast<int>(snapshot.scaleDegree[static_cast<size_t>(i)]));
        const auto event = (a & 0x80) == 0 ? "Rest" : ((b & 0x80) != 0 ? "Tie" : "Note");
        auto& button = stepButtons_[static_cast<size_t>(i)];
        button.setButtonText(juce::String(i + 1) + ":" + ((a & 0x80) == 0 ? "-" : pitch));
        button.setTooltip("Step " + juce::String(i + 1) + ": " + event + " " + pitch
            + ", velocity " + juce::String((b >> 4) & 7) + ", value " + juce::String(b & 15));
        button.setToggleState(i == selectedStep_, juce::dontSendNotification);
    }
}

void SeqPanel::refreshPitchModeControls(const SequenceSnapshot& snapshot, uint8_t dataA)
{
    const auto route = swaraxt::routeFromStoredPitchMode(snapshot.pitchMode);

    if (route == swaraxt::SequencerPitchRoute::linear)
    {
        // Linear CV.
        note_.setLabel("CV");
        note_.slider().setRange(0.0, 127.0, 0.01);
        note_.slider().textFromValueFunction = [](double value) {
            return juce::String(value, 2);
        };
        note_.slider().setValue(snapshot.freePitch[static_cast<size_t>(selectedStep_)]
                                    / static_cast<double>(swaraxt::FreeCvPitch::kUnitsPerSemitone),
                                juce::dontSendNotification);
        note_.slider().setTooltip("Free CV pitch in semitones. 60.00 is C4. Not snapped to a scale.");
        randomType_.setTooltip(
            "Type 1: unrestricted continuous CV pitches, velocities, rests/ties and controller values");
        noteRange_.setTooltip(
            "CV offsets from step 1: lower -48..0, upper 0..+48 continuous semitones; LINEAR");
        randomNotes_.setTooltip(
            "Change continuous CV pitches only; preserve events, velocities, values and packed notes");
        randomSequence_.setTooltip(
            "New continuous CV, velocities, rests/ties and controller values; packed notes stay intact");
        midiOutButton_.setTooltip(
            "MIDI OUT is on, but LINEAR does not emit notes. External notes from the previous mode are released.");
    }
    else if (route == swaraxt::SequencerPitchRoute::quantizedCv)
    {
        // Quantized CV: shares CV slider setup with Linear CV.
        note_.setLabel("CV");
        note_.slider().setRange(0.0, 127.0, 0.01);
        note_.slider().textFromValueFunction = [](double value) {
            return juce::String(value, 2);
        };
        note_.slider().setValue(snapshot.freePitch[static_cast<size_t>(selectedStep_)]
                                    / static_cast<double>(swaraxt::FreeCvPitch::kUnitsPerSemitone),
                                juce::dontSendNotification);
        const auto units = snapshot.freePitch[static_cast<size_t>(selectedStep_)];
        juce::String quantized = "no scale pitch";
        if (processor_ != nullptr)
        {
            const auto chosen = processor_->quantizeActiveHz(swaraxt::FreeCvPitch::hzFromUnits(units));
            if (chosen.valid)
                quantized = juce::String(chosen.hz, 2) + " Hz, degree "
                    + juce::String(chosen.degree) + ", MIDI "
                    + juce::String(static_cast<int>(swaraxt::ScalePitch::nearestMidiNote(chosen.hz)));
        }
        note_.slider().setTooltip(
            "Continuous CV source. Playback quantizes it to the active tuning: " + quantized
            + ". Stored CV is not rewritten.");
        randomType_.setTooltip(
            "Type 1: unrestricted continuous CV, quantized to the active tuning on playback");
        noteRange_.setTooltip(
            "CV offsets from step 1: lower -48..0, upper 0..+48 continuous semitones; quantized on playback");
        randomNotes_.setTooltip(
            "Change continuous CV pitches only; quantization happens at playback and does not rewrite CV");
        randomSequence_.setTooltip(
            "New continuous CV, velocities, rests/ties and controller values; packed notes stay intact");
        midiOutButton_.setTooltip(
            "QUANTIZED CV emits the nearest 12-TET MIDI note of the tuned frequency. Exact Scala pitch is internal only.");
    }
    else if (route == swaraxt::SequencerPitchRoute::scale)
    {
        // Scale.
        note_.setLabel("DEGREE");
        note_.slider().setRange(-swaraxt::SequenceState::kScaleDegreeLimit,
                                swaraxt::SequenceState::kScaleDegreeLimit, 1.0);
        note_.slider().textFromValueFunction = [](double value) {
            return juce::String::formatted("%+03d", static_cast<int>(std::lround(value)));
        };
        note_.slider().setValue(snapshot.scaleDegree[static_cast<size_t>(selectedStep_)],
                                juce::dontSendNotification);
        const double hz = processor_ != nullptr
            ? processor_->hzForActiveDegree(snapshot.scaleDegree[static_cast<size_t>(selectedStep_)])
            : 0.0;
        note_.slider().setTooltip("Scale degree. " + juce::String(hz, 2) + " Hz in the active tuning. Not a MIDI note.");
        randomType_.setTooltip("Type 1: scale-degree randomization, velocities, rests/ties and controller values");
        noteRange_.setTooltip("Degree range around step 1, measured in semitone-sized cents, then sampled as whole degrees.");
        randomNotes_.setTooltip("Change scale degrees only; preserve events, velocities, values, packed notes and CV");
        randomSequence_.setTooltip("New scale degrees, velocities, rests/ties and controller values; packed notes and CV stay intact");
        midiOutButton_.setTooltip("MIDI OUT does not emit notes in SCALE. Notes already sent are released.");
    }
    else
    {
        // Legacy-style presentation. Invalid stored modes (route == invalid) fall back here
        // visually but remain MIDI-silent per the routing policy; they never become
        // Legacy note emitters.
        note_.setLabel("NOTE");
        note_.slider().setRange(0.0, 127.0, 1.0);
        note_.slider().textFromValueFunction = nullptr;
        note_.slider().setValue(dataA & 0x7f, juce::dontSendNotification);
        note_.slider().setTooltip("Integer sequence note");
        randomType_.setTooltip(
            "Type 1: pentatonic notes, velocities, rests/ties and controller values");
        noteRange_.setTooltip(
            "Note offsets from step 1: lower -48..0, upper 0..+48 semitones; pentatonic Type 1");
        randomNotes_.setTooltip(
            "Change pitches only; preserve events, velocities and controller values");
        randomSequence_.setTooltip(
            "New notes, velocities, rests/ties and controller values; preserve playback settings");
        midiOutButton_.setTooltip(
            "Send LEGACY sequencer notes to the host MIDI output. LINEAR and SCALE stay silent.");
    }
}

void SeqPanel::writeSelectedStep(uint8_t dataA, uint8_t dataB)
{
    if (sequenceState_ != nullptr)
        sequenceState_->setStep(selectedStep_, dataA, dataB);
}

void SeqPanel::sequenceStateChanged()
{
    triggerAsyncUpdate();
}

void SeqPanel::handleAsyncUpdate()
{
    refreshSequenceControls();
}

void SeqPanel::reflow()
{
    auto area = getLocalBounds().reduced(2);
    constexpr int gap = 4;
    constexpr int choiceRow = Layout::selectorLabelHeight + Layout::selectorHeight;
    constexpr int actionHeight = 22;
    layoutRow(area.removeFromTop(choiceRow), { selectors_[kMode].get(), selectors_[kClockMode].get(),
                                              selectors_[kDivision].get(), &groove_ }, gap);
    area.removeFromTop(gap);
    layoutRow(area.removeFromTop(58), { knobs_[kTempo].get(), knobs_[kSwing].get(), knobs_[kGate].get(),
                                       &length_, &rotation_ }, gap);
    area.removeFromTop(gap);
    arpHeading_.setBounds(area.removeFromTop(16));
    layoutRow(area.removeFromTop(choiceRow), { selectors_[kDirection].get(), selectors_[kPattern].get(),
                                              selectors_[kOctaves].get() }, gap);
    area.removeFromTop(gap);
    auto heading = area.removeFromTop(20);
    lockButton_.setBounds(heading.removeFromRight(58));
    heading.removeFromRight(4);
    pitchModeBox_.setBounds(heading.removeFromRight(118).reduced(0, 1));
    heading.removeFromRight(4);
    midiOutButton_.setBounds(heading.removeFromRight(72));
    sequenceHeading_.setBounds(heading);
    area.removeFromTop(gap);
    for (int row = 0; row < 2; ++row) {
        auto steps = area.removeFromTop(20);
        const int width = (steps.getWidth() - 7 * 3) / 8;
        for (int col = 0; col < 8; ++col) {
            stepButtons_[static_cast<size_t>(row * 8 + col)].setBounds(steps.removeFromLeft(width));
            steps.removeFromLeft(3);
        }
        area.removeFromTop(2);
    }
    layoutRow(area.removeFromTop(58), { &note_, &event_, &velocity_, &controller_ }, gap);
    area.removeFromTop(gap);
    auto range = area.removeFromTop(25);
    tuningBox_.setBounds(range.removeFromLeft(78).withHeight(Layout::selectorHeight));
    range.removeFromLeft(2);
    importTuning_.setBounds(range.removeFromLeft(36).withHeight(Layout::selectorHeight));
    range.removeFromLeft(gap);
    randomType_.setBounds(range.removeFromLeft(64).withHeight(Layout::selectorHeight));
    range.removeFromLeft(gap);
    rangeLabel_.setBounds(range.removeFromLeft(72));
    noteRange_.setBounds(range);
    area.removeFromTop(gap);
    layoutRow(area.removeFromTop(actionHeight), { &randomNotes_, &randomVelocity_, &randomEvent_ }, gap);
    area.removeFromTop(2);
    layoutRow(area.removeFromTop(actionHeight), { &randomValue_, &randomSequence_ }, gap);
}

}  // namespace swaraxt::ui
