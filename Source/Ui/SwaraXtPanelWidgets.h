// Copyright 2026 MontroneDSP.
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include <JuceHeader.h>

#include <array>
#include <memory>
#include <utility>
#include <vector>

#include "Ui/SwaraXtUiPalette.h"
#include "Ui/SwaraXtModulationNames.h"
#include "Ui/SwaraXtLfoVisualizer.h"

namespace swaraxt::ui {

enum class KnobSize { standard, large };

// Shared layout constants and inline helpers previously held in the anonymous
// namespace of SwaraXtPanels.cpp. Kept header-only so every panel TU can use
// them without cross-translation-unit dependencies.
constexpr float dividerTitleGap = 8.0f;
constexpr float dividerActionGap = 7.0f;
constexpr float dividerThickness = 1.5f;

inline void paintSplitDivider(juce::Graphics& g,
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

inline juce::StringArray subShapeNames()
{
    return {
        "Square -1", "Triangle -1", "Pulse -1", "Square -2",
        "Triangle -2", "Pulse -2", "Click", "Glitch",
        "Blow", "Metallic", "Pop"
    };
}

inline juce::StringArray operatorNames()
{
    return {
        "Sum", "Sync", "Ring", "XOR", "Fuzz", "Crush 4", "Crush 8",
        "Fold", "Bits", "Duo", "Ping 2", "Ping 4", "Ping 8", "Ping Seq"
    };
}

inline juce::StringArray lfoModeNames()
{
    return { "Free phase", "Retrigger", "Env master", "One-shot" };
}

inline juce::StringArray timingModeNames()
{
    return { "Free", "Sync" };
}

inline juce::StringArray beatDivisionNames()
{
    return { "1/32", "1/16", "1/8", "1/4", "1/2", "1/1", "2/1", "4/1" };
}

inline juce::StringArray clockModeNames()
{
    return { "Free", "Host sync" };
}

inline juce::StringArray legatoNames()
{
    return { "Retrigger", "Legato" };
}

inline juce::StringArray seqModeNames()
{
    return { "Step", "Arp", "Sequence" };
}

inline juce::StringArray arpDirectionNames()
{
    return { "Up", "Down", "Up/Down", "Random", "Played" };
}

inline juce::StringArray numberedNames(const juce::String& prefix, int first, int last)
{
    juce::StringArray names;
    for (int value = first; value <= last; ++value)
        names.add(prefix + " " + juce::String(value));
    return names;
}

inline float readValue(const juce::AudioProcessorValueTreeState* apvts, const char* id,
                       float fallback = 0.0f)
{
    if (apvts == nullptr || id == nullptr)
        return fallback;
    if (auto* value = apvts->getRawParameterValue(id))
        return value->load();
    return fallback;
}

inline float read01(const juce::AudioProcessorValueTreeState* apvts,
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

inline void setSliderDefault(juce::AudioProcessorValueTreeState& apvts,
                             const char* id,
                             juce::Slider& slider)
{
    if (auto* parameter = apvts.getParameter(id))
        slider.setDoubleClickReturnValue(true,
            parameter->convertFrom0to1(parameter->getDefaultValue()));
}

inline void layoutRow(juce::Rectangle<int> area,
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

inline void paintInset(juce::Graphics& g, juce::Rectangle<float> bounds)
{
    g.setColour(Palette::panelDeep());
    g.fillRoundedRectangle(bounds, 5.0f);
    g.setColour(Palette::line().withMultipliedAlpha(0.65f));
    g.drawRoundedRectangle(bounds.reduced(0.5f), 5.0f, 1.0f);
}

class SwaraXtKnob : public juce::Component {
 public:
    SwaraXtKnob(const juce::String& label, KnobSize size = KnobSize::standard);

    void resized() override;
    void lookAndFeelChanged() override;
    juce::Slider& slider() noexcept { return slider_; }
    const juce::Slider& slider() const noexcept { return slider_; }
    KnobSize knobSize() const noexcept { return size_; }
    void setLabel(const juce::String& text) { label_.setText(text, juce::dontSendNotification); }
    juce::String labelText() const { return label_.getText(); }

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
    void setPrimaryActionBounds(juce::Rectangle<int> bounds);
    void setSecondaryHeaderVisible(bool visible);
    void setTitle(const juce::String& title) { title_.setText(title, juce::dontSendNotification); resized(); repaint(); }
    bool secondaryHeaderVisibleForTests() const noexcept { return secondaryHeaderVisible_; }
    juce::Component& body() noexcept { return body_; }
    const juce::Component& body() const noexcept { return body_; }

 private:
    juce::Label title_;
    juce::Label secondaryTitle_;
    juce::Component body_;
    juce::Rectangle<int> secondaryActionBounds_;
    juce::Rectangle<int> primaryActionBounds_;
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

}  // namespace swaraxt::ui
