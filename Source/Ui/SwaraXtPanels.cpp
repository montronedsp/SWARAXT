// Copyright 2026 MontroneDSP.
// SPDX-License-Identifier: GPL-3.0-or-later

#include "Ui/SwaraXtPanels.h"
#include "Ui/SwaraXtTypography.h"
#include "Ui/SwaraXtBoardPanel.h"

#include <cmath>

#include "Plugin/SwaraXtParameterLayout.h"
#include "Plugin/PluginProcessor.h"
#include "Ui/SwaraXtLfoVisualizer.h"
#include "Ui/SwaraXtModulationNames.h"

namespace swaraxt::ui {

namespace {

constexpr float dividerTitleGap = 7.0f;
constexpr float dividerActionGap = 5.0f;
constexpr float dividerThickness = 2.5f;

void paintSplitDivider(juce::Graphics& g,
                       float availableWidth,
                       juce::Rectangle<float> titleBounds,
                       juce::Rectangle<float> actionBounds = {})
{
    const float y = titleBounds.getCentreY();
    g.setColour(Palette::skin().moduleHeaderBackground);
    g.fillRoundedRectangle(titleBounds, 5.0f);

    std::array<juce::Range<float>, 2> exclusions {
        juce::Range<float> { titleBounds.getX() - dividerTitleGap,
                             titleBounds.getRight() + dividerTitleGap },
        actionBounds.isEmpty()
            ? juce::Range<float> { availableWidth, availableWidth }
            : juce::Range<float> { actionBounds.getX() - dividerActionGap,
                                   actionBounds.getRight() + dividerActionGap }
    };
    if (exclusions[1].getStart() < exclusions[0].getStart())
        std::swap(exclusions[0], exclusions[1]);

    g.setColour(Palette::line());
    float cursor = 0.0f;
    for (const auto exclusion : exclusions)
    {
        const float start = juce::jlimit(0.0f, availableWidth, exclusion.getStart());
        const float end = juce::jlimit(0.0f, availableWidth, exclusion.getEnd());
        if (start > cursor)
            g.drawLine(cursor, y, start, y, dividerThickness);
        cursor = juce::jmax(cursor, end);
    }
    if (cursor < availableWidth)
        g.drawLine(cursor, y, availableWidth, y, dividerThickness);
}

juce::StringArray subShapeNames()
{
    return {
        "Square -1", "Triangle -1", "Pulse -1", "Square -2",
        "Triangle -2", "Pulse -2", "Click", "Glitch",
        "Blow", "Metallic", "Pop"
    };
}

juce::StringArray operatorNames()
{
    return {
        "Sum", "Sync", "Ring", "XOR", "Fuzz", "Crush 4", "Crush 8",
        "Fold", "Bits", "Duo", "Ping 2", "Ping 4", "Ping 8", "Ping Seq"
    };
}

juce::StringArray lfoModeNames()
{
    return { "Free phase", "Retrigger", "Env master", "One-shot" };
}

juce::StringArray timingModeNames()
{
    return { "Free", "Sync" };
}

juce::StringArray beatDivisionNames()
{
    return { "1/32", "1/16", "1/8", "1/4", "1/2", "1/1", "2/1", "4/1" };
}

juce::StringArray clockModeNames()
{
    return { "Free", "Host sync" };
}

juce::StringArray legatoNames()
{
    return { "Retrigger", "Legato" };
}

juce::StringArray seqModeNames()
{
    return { "Step", "Arp", "Sequence" };
}

juce::StringArray arpDirectionNames()
{
    return { "Up", "Down", "Up/Down", "Random", "Played" };
}

juce::StringArray numberedNames(const juce::String& prefix, int first, int last)
{
    juce::StringArray names;
    for (int value = first; value <= last; ++value)
        names.add(prefix + " " + juce::String(value));
    return names;
}

float readValue(const juce::AudioProcessorValueTreeState* apvts, const char* id, float fallback = 0.0f)
{
    if (apvts == nullptr || id == nullptr)
        return fallback;
    if (auto* value = apvts->getRawParameterValue(id))
        return value->load();
    return fallback;
}

float read01(const juce::AudioProcessorValueTreeState* apvts,
             const char* id,
             float minValue,
             float maxValue,
             float fallback = 0.0f)
{
    const float value = readValue(apvts, id, fallback);
    if (maxValue <= minValue)
        return 0.0f;
    return juce::jlimit(0.0f, 1.0f, (value - minValue) / (maxValue - minValue));
}

void setSliderDefault(juce::AudioProcessorValueTreeState& apvts,
                      const char* id,
                      juce::Slider& slider)
{
    if (auto* parameter = apvts.getParameter(id))
        slider.setDoubleClickReturnValue(true,
            parameter->convertFrom0to1(parameter->getDefaultValue()));
}

void layoutRow(juce::Rectangle<int> area,
               std::initializer_list<juce::Component*> components,
               int gap = Layout::controlGap)
{
    const auto count = static_cast<int>(components.size());
    if (count <= 0)
        return;

    const int cellW = juce::jmax(1, (area.getWidth() - gap * (count - 1)) / count);
    for (auto* component : components)
    {
        component->setBounds(area.removeFromLeft(cellW));
        area.removeFromLeft(gap);
    }
}

void paintInset(juce::Graphics& g, juce::Rectangle<float> bounds)
{
    g.setColour(Palette::panelDeep());
    g.fillRoundedRectangle(bounds, 5.0f);
    g.setColour(Palette::line().withMultipliedAlpha(0.65f));
    g.drawRoundedRectangle(bounds.reduced(0.5f), 5.0f, 1.0f);
}

}  // namespace

SwaraXtKnob::SwaraXtKnob(const juce::String& labelText, KnobSize size)
    : size_(size)
{
    const bool large = size_ == KnobSize::large;
    addAndMakeVisible(label_);
    setTextRole(label_, TextRole::parameterLabel);
    label_.setText(labelText, juce::dontSendNotification);
    label_.setJustificationType(juce::Justification::centred);
    label_.setColour(juce::Label::textColourId, large ? Palette::cream() : Palette::mutedText());

    addAndMakeVisible(slider_);
    slider_.setSliderStyle(juce::Slider::RotaryHorizontalVerticalDrag);
    slider_.setTextBoxStyle(juce::Slider::TextBoxBelow, false,
                            large ? Layout::valueBoxWide : Layout::valueBoxWidth,
                            Layout::valueBoxHeight);
    slider_.setRotaryParameters(juce::MathConstants<float>::pi * 1.25f,
                                juce::MathConstants<float>::pi * 2.75f,
                                true);
    slider_.getProperties().set("swaraxtKnobSize", large ? "large" : "standard");
    const int diameter = large ? Layout::knobLarge : Layout::knobStandard;
    setSize(diameter + 12, diameter + 48);
}

void SwaraXtKnob::resized()
{
    auto area = getLocalBounds();
    label_.setBounds(area.removeFromTop(Layout::knobLabelHeight));
    slider_.setBounds(area);
}

void SwaraXtKnob::lookAndFeelChanged()
{
    label_.setColour(juce::Label::textColourId,
                     size_ == KnobSize::large ? Palette::cream() : Palette::mutedText());
}

SwaraXtSelector::SwaraXtSelector(const juce::String& labelText,
                               const juce::StringArray& items,
                               bool primary)
    : primary_(primary)
{
    addAndMakeVisible(label_);
    setTextRole(label_, TextRole::parameterLabel);
    label_.setText(labelText, juce::dontSendNotification);
    label_.setJustificationType(juce::Justification::centredLeft);
    label_.setColour(juce::Label::textColourId, primary ? Palette::cream() : Palette::mutedText());

    combo_.addItemList(items, 1);
    combo_.setTooltip(labelText);
    combo_.setJustificationType(juce::Justification::centredLeft);
    combo_.onChange = [this] { refreshDescription(); };
    addAndMakeVisible(combo_);

    description_.setJustificationType(juce::Justification::centredLeft);
    setTextRole(description_, TextRole::comboValue);
    description_.setColour(juce::Label::textColourId, Palette::mutedText());
    addAndMakeVisible(description_);

    refreshDescription();
}

void SwaraXtSelector::resized()
{
    auto area = getLocalBounds();
    label_.setBounds(area.removeFromTop(Layout::selectorLabelHeight));
    combo_.setBounds(area.removeFromTop(Layout::selectorHeight));
    if (area.getHeight() >= Layout::selectorDescriptionHeight)
        description_.setBounds(area.removeFromTop(Layout::selectorDescriptionHeight));
}

void SwaraXtSelector::lookAndFeelChanged()
{
    label_.setColour(juce::Label::textColourId,
                     primary_ ? Palette::cream() : Palette::mutedText());
    description_.setColour(juce::Label::textColourId, Palette::mutedText());
}

void SwaraXtSelector::refreshDescription()
{
    const auto text = combo_.getText();
    description_.setText(text.isEmpty() ? "Select" : text, juce::dontSendNotification);
}

SwaraXtModulePanel::SwaraXtModulePanel(const juce::String& titleText,
                                       const juce::String& secondaryTitleText,
                                       int secondaryDividerY)
    : secondaryDividerY_(secondaryDividerY)
{
    addAndMakeVisible(title_);
    setTextRole(title_, TextRole::sectionHeader);
    title_.setText(titleText, juce::dontSendNotification);
    title_.setJustificationType(juce::Justification::centred);
    addAndMakeVisible(secondaryTitle_);
    setTextRole(secondaryTitle_, TextRole::sectionHeader);
    secondaryTitle_.setText(secondaryTitleText, juce::dontSendNotification);
    secondaryTitle_.setJustificationType(juce::Justification::centred);
    addAndMakeVisible(body_);
    lookAndFeelChanged();
}

void SwaraXtModulePanel::paint(juce::Graphics& g)
{
    const auto paintDivider = [&g, this](const juce::Label& label,
                                         juce::Rectangle<int> actionBounds = {})
    {
        if (label.getText().isEmpty())
            return;
        paintSplitDivider(g, static_cast<float>(getWidth()), label.getBounds().toFloat(),
                          actionBounds.toFloat());
    };
    paintDivider(title_, primaryActionBounds_);
    if (secondaryHeaderVisible_)
        paintDivider(secondaryTitle_, secondaryActionBounds_);
}

void SwaraXtModulePanel::setSecondaryActionBounds(juce::Rectangle<int> bounds)
{
    secondaryActionBounds_ = bounds;
    repaint();
}

void SwaraXtModulePanel::setPrimaryActionBounds(juce::Rectangle<int> bounds)
{
    primaryActionBounds_ = bounds;
    repaint();
}

void SwaraXtModulePanel::setSecondaryHeaderVisible(bool visible)
{
    if (secondaryHeaderVisible_ == visible)
        return;

    secondaryHeaderVisible_ = visible;
    secondaryTitle_.setVisible(visible);
    resized();
    repaint();
}

SwaraXtDepthSlider::SwaraXtDepthSlider()
{
    slider_.setSliderStyle(juce::Slider::LinearHorizontal);
    slider_.setRange(-63.0, 63.0, 1.0);
    slider_.setTextBoxStyle(juce::Slider::TextBoxRight, false, 31, 18);
    slider_.setTextValueSuffix("");
    slider_.textFromValueFunction = [](double value) {
        const int depth = static_cast<int>(std::round(value));
        return (depth > 0 ? "+" : "") + juce::String(depth);
    };
    slider_.setTooltip("Bipolar modulation depth");
    addAndMakeVisible(slider_);
}

void SwaraXtDepthSlider::resized()
{
    slider_.setBounds(getLocalBounds());
}

void SwaraXtModulePanel::resized()
{
    auto area = getLocalBounds();
    if (title_.getText().isNotEmpty())
    {
        const int width = juce::jmax(64, title_.getText().length() * 8 + 20);
        title_.setBounds((getWidth() - width) / 2, 1, width, 20);
        area.removeFromTop(Layout::moduleHeaderRow);
    }
    else
        title_.setBounds({});
    if (secondaryHeaderVisible_ && secondaryTitle_.getText().isNotEmpty()
        && secondaryDividerY_ > 0)
    {
        const int width = juce::jmax(64, secondaryTitle_.getText().length() * 8 + 20);
        secondaryTitle_.setBounds((getWidth() - width) / 2,
                                  secondaryDividerY_ - 10, width, 20);
    }
    else
        secondaryTitle_.setBounds({});
    body_.setBounds(area.reduced(Layout::moduleInnerPad));
}

void SwaraXtModulePanel::lookAndFeelChanged()
{
    title_.setColour(juce::Label::textColourId, Palette::skin().moduleHeaderText);
    secondaryTitle_.setColour(juce::Label::textColourId, Palette::skin().moduleHeaderText);
}

OscillatorPanel::OscillatorPanel(const juce::String& title, bool hasDetune)
    : title_(title),
      model_("MODEL", oscillatorNames(), true),
      pitch_("PITCH"),
      timbre_("TIMBRE"),
      detune_("DETUNE"),
      hasDetune_(hasDetune)
{
    setTextRole(titleLabel_, TextRole::sectionHeader);
    titleLabel_.setText(title_, juce::dontSendNotification);
    titleLabel_.setJustificationType(juce::Justification::centred);
    addAndMakeVisible(titleLabel_);
    pitch_.slider().setRange(-48.0, 48.0, 1.0);
    pitch_.slider().textFromValueFunction = [](double value) {
        const int semitones = static_cast<int>(std::round(value));
        return (semitones > 0 ? "+" : "") + juce::String(semitones) + " st";
    };
    timbre_.slider().setRange(0.0, 127.0, 1.0);
    timbre_.slider().textFromValueFunction = [](double value) {
        return juce::String(static_cast<int>(std::round(value)));
    };
    detune_.slider().setRange(0.0, 127.0, 1.0);
    detune_.slider().textFromValueFunction = [](double value) {
        return juce::String(value / 128.0, 2) + " st";
    };
    addAndMakeVisible(model_);
    addAndMakeVisible(pitch_);
    addAndMakeVisible(timbre_);
    if (hasDetune_)
        addAndMakeVisible(detune_);
}

void OscillatorPanel::attach(juce::AudioProcessorValueTreeState& apvts,
                             const char* modelId,
                             const char* pitchId,
                             const char* timbreId,
                             const char* detuneId)
{
    setSliderDefault(apvts, pitchId, pitch_.slider());
    setSliderDefault(apvts, timbreId, timbre_.slider());
    modelAttachment_ = std::make_unique<juce::AudioProcessorValueTreeState::ComboBoxAttachment>(
        apvts, modelId, model_.combo());
    pitchAttachment_ = std::make_unique<juce::AudioProcessorValueTreeState::SliderAttachment>(
        apvts, pitchId, pitch_.slider());
    timbreAttachment_ = std::make_unique<juce::AudioProcessorValueTreeState::SliderAttachment>(
        apvts, timbreId, timbre_.slider());
    if (hasDetune_ && detuneId != nullptr)
    {
        setSliderDefault(apvts, detuneId, detune_.slider());
        detuneAttachment_ = std::make_unique<juce::AudioProcessorValueTreeState::SliderAttachment>(
            apvts, detuneId, detune_.slider());
    }
}

void OscillatorPanel::paint(juce::Graphics& g)
{
    paintSplitDivider(g, static_cast<float>(getWidth()), titleLabel_.getBounds().toFloat());
}

void OscillatorPanel::resized()
{
    titleLabel_.setBounds((getWidth() - 64) / 2, 1, 64, 20);
    auto area = getLocalBounds().withTrimmedTop(24);
    const int modelWidth = 104;
    model_.setBounds(area.removeFromLeft(modelWidth));
    area.removeFromLeft(4);
    if (hasDetune_)
        layoutRow(area, { &pitch_, &timbre_, &detune_ }, 3);
    else
        layoutRow(area, { &pitch_, &timbre_ }, 4);
}

void OscillatorPanel::lookAndFeelChanged()
{
    titleLabel_.setColour(juce::Label::textColourId, Palette::skin().moduleHeaderText);
}

bool OscillatorPanel::modelIsSelector() const noexcept
{
    return model_.combo().isVisible();
}

bool OscillatorPanel::pitchIsRotary() const noexcept
{
    return pitch_.slider().getSliderStyle() == juce::Slider::RotaryHorizontalVerticalDrag;
}

bool OscillatorPanel::waveVariationIsRotary() const noexcept
{
    return timbre_.slider().getSliderStyle() == juce::Slider::RotaryHorizontalVerticalDrag;
}

EnvelopeTrace::~EnvelopeTrace()
{
    stopTimer();
}

void EnvelopeTrace::bind(juce::AudioProcessorValueTreeState& apvts,
                         const char* attack,
                         const char* decay,
                         const char* sustain,
                         const char* release)
{
    apvts_ = &apvts;
    attack_ = attack;
    decay_ = decay;
    sustain_ = sustain;
    release_ = release;
    startTimerHz(12);
}

void EnvelopeTrace::paint(juce::Graphics& g)
{
    const auto frameBounds = getLocalBounds().toFloat();
    auto area = frameBounds.reduced(1.0f);
    paintInset(g, area);
    area = area.reduced(8.0f, 7.0f);

    const float a = read01(apvts_, attack_, 0.0f, 127.0f);
    const float d = read01(apvts_, decay_, 0.0f, 127.0f);
    const float s = read01(apvts_, sustain_, 0.0f, 127.0f);
    const float r = read01(apvts_, release_, 0.0f, 127.0f);

    const float startX = area.getX();
    const float bottomY = area.getBottom();
    const float topY = area.getY();
    const float sustainY = juce::jmap(s, bottomY, topY);
    const float attackX = startX + area.getWidth() * (0.12f + 0.18f * a);
    const float decayX = attackX + area.getWidth() * (0.12f + 0.18f * d);
    const float holdX = area.getRight() - area.getWidth() * (0.12f + 0.20f * r);

    juce::Path envelope;
    envelope.startNewSubPath(startX, bottomY);
    envelope.lineTo(attackX, topY);
    envelope.lineTo(decayX, sustainY);
    envelope.lineTo(juce::jmax(decayX + 4.0f, holdX), sustainY);
    envelope.lineTo(area.getRight(), bottomY);

    g.setColour(Palette::grid());
    for (int i = 1; i < 4; ++i)
    {
        const float y = area.getY() + area.getHeight() * static_cast<float>(i) / 4.0f;
        g.drawHorizontalLine(static_cast<int>(std::round(y)), area.getX(), area.getRight());
    }

    g.setColour(Palette::accent());
    g.strokePath(envelope, juce::PathStrokeType(2.2f, juce::PathStrokeType::curved,
                                                juce::PathStrokeType::rounded));
    if (frame_ != nullptr)
        frame_->drawWithin(g, frameBounds, juce::RectanglePlacement::stretchToFit, 1.0f);
}

void EnvelopeTrace::timerCallback()
{
    repaint();
}

LfoTrace::~LfoTrace()
{
    stopTimer();
}

void LfoTrace::bind(juce::AudioProcessorValueTreeState& apvts,
                    const char* waveform,
                    const char* rate)
{
    apvts_ = &apvts;
    waveform_ = waveform;
    rate_ = rate;
    startTimerHz(15);
}

void LfoTrace::paint(juce::Graphics& g)
{
    const auto frameBounds = getLocalBounds().toFloat();
    auto area = frameBounds.reduced(1.0f);
    paintInset(g, area);
    area = area.reduced(8.0f, 7.0f);

    const int waveform = static_cast<int>(std::round(readValue(apvts_, waveform_, 0.0f)));
    const float rate = read01(apvts_, rate_, 0.0f, 127.0f);

    juce::Path path;
    const int points = 64;
    for (int i = 0; i < points; ++i)
    {
        const float x01 = static_cast<float>(i) / static_cast<float>(points - 1);
        const float phase = std::fmod(x01 + phase_, 1.0f);
        const float y01 = lfoVisualizerY01(waveform, phase);

        const float x = area.getX() + area.getWidth() * x01;
        const float y = juce::jmap(y01, area.getBottom(), area.getY());
        if (i == 0)
            path.startNewSubPath(x, y);
        else
            path.lineTo(x, y);
    }

    g.setColour(Palette::grid());
    g.drawHorizontalLine(static_cast<int>(std::round(area.getCentreY())),
                         area.getX(),
                         area.getRight());
    g.setColour(Palette::secondaryAccent());
    g.strokePath(path, juce::PathStrokeType(2.0f, juce::PathStrokeType::curved,
                                            juce::PathStrokeType::rounded));
    if (frame_ != nullptr)
        frame_->drawWithin(g, frameBounds, juce::RectanglePlacement::stretchToFit, 1.0f);

    phase_ = std::fmod(phase_ + 0.004f + rate * 0.015f, 1.0f);
}

void LfoTrace::timerCallback()
{
    repaint();
}

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

    sourceModule_.body().addAndMakeVisible(osc1_);
    sourceModule_.body().addAndMakeVisible(osc2_);

    for (int i = kMixBalance; i <= kMixNoise; ++i)
        mixModule_.body().addAndMakeVisible(*knobs_[static_cast<size_t>(i)]);
    mixModule_.body().addAndMakeVisible(*selectors_[static_cast<size_t>(kMixOperator)]);
    mixModule_.body().addAndMakeVisible(*selectors_[static_cast<size_t>(kSubShape)]);
    mixModule_.body().addChildComponent(*modulationView_);

    for (int i = kFilterCutoff; i <= kFilterLfo; ++i)
        filterModule_.body().addAndMakeVisible(*knobs_[static_cast<size_t>(i)]);
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

    seqViewButton_.onClick = [this] { setLocalViews(false, ! showingSequencer_); };
    lookAndFeelChanged();
    setLocalViews(false, false);
}

MainPanel::~MainPanel() = default;

void MainPanel::attach(SwaraXtAudioProcessor& processor)
{
    if (attached_)
        return;

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
        swaraxt::IDs::perfLegato
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
    layoutRow(perf, { knobs_[kMaster].get(), knobs_[kGlide].get(),
                      selectors_[kLegato].get() }, 4);
}

ModPanel::ModPanel()
{
    setTextRole(pageLabel_, TextRole::smallIndex);
    pageLabel_.setJustificationType(juce::Justification::centred);
    pageLabel_.setColour(juce::Label::textColourId, Palette::mutedText());
    addAndMakeVisible(pageLabel_);
    addAndMakeVisible(previousPage_);
    addAndMakeVisible(nextPage_);
    previousPage_.onClick = [this] { showPage((page_ + 3) % 4); };
    nextPage_.onClick = [this] { showPage((page_ + 1) % 4); };

    rows_.reserve(12);
    for (int row = 0; row < 12; ++row)
    {
        auto r = std::make_unique<Row>();
        setTextRole(r->indexLabel, TextRole::smallIndex);
        r->indexLabel.setText(juce::String(row + 1).paddedLeft('0', 2), juce::dontSendNotification);
        r->indexLabel.setJustificationType(juce::Justification::centred);
        r->indexLabel.setColour(juce::Label::textColourId, Palette::mutedText());
        addAndMakeVisible(r->indexLabel);

        populatePluginModulationSourceCombo(r->source);
        r->source.setTooltip("Modulation source");
        populatePluginModulationDestinationCombo(r->destination);
        r->destination.setTooltip("Modulation destination");
        addAndMakeVisible(r->source);
        addAndMakeVisible(r->destination);

        r->amount = std::make_unique<SwaraXtDepthSlider>();
        if (row == 11)
        {
            constexpr auto kSlot12WheelTip = "Slot 12 depth is scaled by Mod Wheel";
            r->indexLabel.setTooltip(kSlot12WheelTip);
            r->source.setTooltip(kSlot12WheelTip);
            r->destination.setTooltip(kSlot12WheelTip);
            r->amount->slider().setTooltip(kSlot12WheelTip);
        }
        addAndMakeVisible(*r->amount);
        rows_.push_back(std::move(r));
    }
    showPage(0);
}

void ModPanel::lookAndFeelChanged()
{
    pageLabel_.setColour(juce::Label::textColourId, Palette::mutedText());
    for (auto& row : rows_)
        row->indexLabel.setColour(juce::Label::textColourId, Palette::mutedText());
}

void ModPanel::attach(SwaraXtAudioProcessor& processor)
{
    if (attached_)
        return;

    auto& apvts = processor.getApvts();
    for (int row = 0; row < 12; ++row)
    {
        auto& r = *rows_[static_cast<size_t>(row)];
        const auto n = juce::String(row + 1);
        r.sourceAttachment = std::make_unique<EngineIdComboAttachment>(
            apvts, "mod.row" + n + ".source", r.source, modulationSourceName);
        r.destAttachment = std::make_unique<EngineIdComboAttachment>(
            apvts, "mod.row" + n + ".destination", r.destination, modulationDestinationName);
        setSliderDefault(apvts, ("mod.row" + n + ".amount").toRawUTF8(), r.amount->slider());
        r.amountAttachment = std::make_unique<juce::AudioProcessorValueTreeState::SliderAttachment>(
            apvts, "mod.row" + n + ".amount", r.amount->slider());
    }
    attached_ = true;
}

void ModPanel::reflow()
{
    auto area = getLocalBounds().reduced(2);
    auto paging = area.removeFromTop(24);
    previousPage_.setBounds(paging.removeFromLeft(28));
    nextPage_.setBounds(paging.removeFromRight(28));
    pageLabel_.setBounds(paging);
    const int rowH = juce::jmax(30, area.getHeight() / 3);

    for (int row = 0; row < 12; ++row)
    {
        auto& r = *rows_[static_cast<size_t>(row)];
        if (row / 3 != page_)
            continue;
        auto rowArea = area.removeFromTop(rowH).reduced(2, 2);
        r.indexLabel.setBounds(rowArea.removeFromLeft(20));
        rowArea.removeFromLeft(4);
        const int amountW = 88;
        const int sourceW = 82;
        r.source.setBounds(rowArea.removeFromLeft(sourceW));
        rowArea.removeFromLeft(3);
        r.amount->setBounds(rowArea.removeFromLeft(amountW));
        rowArea.removeFromLeft(3);
        r.destination.setBounds(rowArea);
    }
}

void ModPanel::showPage(int page)
{
    page_ = juce::jlimit(0, 3, page);
    pageLabel_.setText(juce::String(page_ * 3 + 1) + "-" + juce::String(page_ * 3 + 3),
                       juce::dontSendNotification);
    for (int row = 0; row < 12; ++row)
    {
        const bool visible = row / 3 == page_;
        auto& r = *rows_[static_cast<size_t>(row)];
        r.indexLabel.setVisible(visible);
        r.source.setVisible(visible);
        r.destination.setVisible(visible);
        r.amount->setVisible(visible);
    }
    reflow();
}

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
        if (pattern <= 7 && arpPatternAttachment_ != nullptr)
            arpPatternAttachment_->setValueAsCompleteGesture(static_cast<float>(pattern));
    };

    for (auto* heading : { &arpHeading_, &sequenceHeading_ }) {
        addAndMakeVisible(*heading);
        setTextRole(*heading, TextRole::sectionHeader);
        heading->setJustificationType(juce::Justification::centredLeft);
    }
    arpHeading_.setText("ARPEGGIATOR", juce::dontSendNotification);
    sequenceHeading_.setText("SEQUENCER", juce::dontSendNotification);
    for (auto* button : { &lockButton_, &randomNotes_, &randomVelocity_, &randomEvent_, &randomValue_, &randomSequence_ }) {
        addAndMakeVisible(*button);
        button->getProperties().set("swaraxtSecondaryAction", true);
    }
    lockButton_.setClickingTogglesState(true);
    lockButton_.setTooltip("Preserve Seq/Arp settings and pattern across preset changes");
    lockButton_.onClick = [this] { if (processor_) processor_->setSequenceLocked(lockButton_.getToggleState()); };
    const auto randomize = [this](SequenceState::Randomize kind) {
        if (sequenceState_) {
            const auto range = processor_ ? processor_->randomNoteRange() : SequenceState::NoteRange {};
            sequenceState_->randomize(kind, editRandom_, range);
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
    startTimerHz(4);
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
                    juce::jlimit(0, 7, static_cast<int>(std::lround(value))),
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

void SeqPanel::timerCallback()
{
    if (processor_) {
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

    for (int i = 0; i < SequenceSnapshot::kNumSteps; ++i)
    {
        const auto packed = snapshot.steps[static_cast<size_t>(i)];
        const auto a = SequenceSnapshot::dataA(packed);
        const auto b = SequenceSnapshot::dataB(packed);
        const auto pitch = juce::MidiMessage::getMidiNoteName(a & 0x7f, true, true, 3);
        const auto event = (a & 0x80) == 0 ? "Rest" : ((b & 0x80) != 0 ? "Tie" : "Note");
        auto& button = stepButtons_[static_cast<size_t>(i)];
        button.setButtonText(juce::String(i + 1) + ":" + ((a & 0x80) == 0 ? "-" : pitch));
        button.setTooltip("Step " + juce::String(i + 1) + ": " + event + " " + pitch
            + ", velocity " + juce::String((b >> 4) & 7) + ", value " + juce::String(b & 15));
        button.setToggleState(i == selectedStep_, juce::dontSendNotification);
    }

    const auto selected = snapshot.steps[static_cast<size_t>(selectedStep_)];
    const uint8_t dataA = SequenceSnapshot::dataA(selected);
    const uint8_t dataB = SequenceSnapshot::dataB(selected);
    note_.slider().setValue(dataA & 0x7f, juce::dontSendNotification);
    velocity_.slider().setValue((dataB & 0x70) >> 4, juce::dontSendNotification);
    controller_.slider().setValue(dataB & 0x0f, juce::dontSendNotification);
    event_.combo().setSelectedItemIndex((dataA & 0x80) == 0 ? 0 : ((dataB & 0x80) != 0 ? 2 : 1),
                                        juce::dontSendNotification);
    event_.refreshDescription();
    refreshingSequenceControls_ = false;
    updateModeEmphasis();
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
    randomType_.setBounds(range.removeFromLeft(76).withHeight(Layout::selectorHeight));
    range.removeFromLeft(gap);
    rangeLabel_.setBounds(range.removeFromLeft(108));
    noteRange_.setBounds(range);
    area.removeFromTop(gap);
    layoutRow(area.removeFromTop(actionHeight), { &randomNotes_, &randomVelocity_, &randomEvent_ }, gap);
    area.removeFromTop(2);
    layoutRow(area.removeFromTop(actionHeight), { &randomValue_, &randomSequence_ }, gap);
}

}  // namespace swaraxt::ui
