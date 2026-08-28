// Copyright 2026 MontroneDSP.
// SPDX-License-Identifier: GPL-3.0-or-later

#include "Ui/SwaraXtLookAndFeel.h"

#if __has_include("BinaryData.h")
#include "BinaryData.h"
#define SWARAXT_HAS_BINARY_FONTS 1
#else
#define SWARAXT_HAS_BINARY_FONTS 0
#endif

namespace swaraxt::ui {

namespace {

juce::Typeface::Ptr loadEmbeddedFont(const char* resourceName)
{
#if SWARAXT_HAS_BINARY_FONTS
    int size = 0;
    if (const void* data = BinaryData::getNamedResource(resourceName, size))
        if (size > 0)
            return juce::Typeface::createSystemTypefaceFor(data, static_cast<size_t>(size));
#else
    juce::ignoreUnused(resourceName);
#endif
    return nullptr;
}

}  // namespace

SwaraXtLookAndFeel::SwaraXtLookAndFeel()
{
    applySkin();
    regular_ = loadEmbeddedFont("din1451alt_ttf");
    jassert(regular_ != nullptr);
}

juce::Font SwaraXtLookAndFeel::regularFont(float height) const
{
    return juce::Font(regular_).withHeight(height);
}

void SwaraXtLookAndFeel::applySkin()
{
    setColour(juce::ResizableWindow::backgroundColourId, Palette::background());
    setColour(juce::Label::textColourId, Palette::cream());
    setColour(juce::Slider::textBoxTextColourId, Palette::cream());
    setColour(juce::Slider::textBoxBackgroundColourId, Palette::panelDeep());
    setColour(juce::Slider::textBoxOutlineColourId, Palette::line());
    setColour(juce::TextButton::buttonColourId, Palette::panelRaised());
    setColour(juce::TextButton::textColourOffId, Palette::mutedText());
    setColour(juce::TextButton::textColourOnId, Palette::cream());
    setColour(juce::ComboBox::backgroundColourId, Palette::skin().comboBackground);
    setColour(juce::ComboBox::outlineColourId, Palette::skin().comboBorder);
    setColour(juce::ComboBox::textColourId, Palette::skin().comboText);
    setColour(juce::ComboBox::arrowColourId, Palette::skin().comboArrow);
    setColour(juce::PopupMenu::backgroundColourId, Palette::panelRaised());
    setColour(juce::PopupMenu::textColourId, Palette::cream());
    setColour(juce::PopupMenu::highlightedBackgroundColourId, Palette::accent().withAlpha(0.35f));
    setColour(juce::PopupMenu::highlightedTextColourId, Palette::cream());
    setColour(juce::CaretComponent::caretColourId, Palette::orange());
}

void SwaraXtLookAndFeel::drawRotarySlider(juce::Graphics& g,
                                         int x,
                                         int y,
                                         int width,
                                         int height,
                                         float sliderPosProportional,
                                         float rotaryStartAngle,
                                         float rotaryEndAngle,
                                         juce::Slider& slider)
{
    auto bounds = juce::Rectangle<float>(static_cast<float>(x), static_cast<float>(y),
                                         static_cast<float>(width), static_cast<float>(height))
                      .reduced(2.0f);
    const bool large = slider.getProperties().getWithDefault("swaraxtKnobSize", "standard") == "large";
    const float requestedDiameter = static_cast<float>(large ? Layout::knobLarge
                                                              : Layout::knobStandard);
    const float diameter = juce::jmin(requestedDiameter,
                                      juce::jmin(bounds.getWidth(), bounds.getHeight()));
    bounds = bounds.withSizeKeepingCentre(diameter, diameter);

    const float radius = diameter * 0.5f;
    const auto centre = bounds.getCentre();
    const float angle = rotaryStartAngle
        + sliderPosProportional * (rotaryEndAngle - rotaryStartAngle);

    juce::Path track;
    track.addCentredArc(centre.x, centre.y, radius - 2.0f, radius - 2.0f,
                        0.0f, rotaryStartAngle, rotaryEndAngle, true);
    const float stroke = large ? 3.2f : 2.6f;
    g.setColour(Palette::skin().knobRim.withMultipliedAlpha(0.42f));
    g.strokePath(track, juce::PathStrokeType(stroke));

    juce::Path valueArc;
    valueArc.addCentredArc(centre.x, centre.y, radius - 2.0f, radius - 2.0f,
                           0.0f, rotaryStartAngle, angle, true);
    g.setColour(slider.isEnabled() ? Palette::skin().knobActive : Palette::skin().disabled);
    g.strokePath(valueArc, juce::PathStrokeType(stroke, juce::PathStrokeType::curved,
                                                juce::PathStrokeType::rounded));

    const auto body = bounds.reduced(diameter * 0.18f);
    g.setColour(Palette::skin().knobBody);
    g.fillEllipse(body);
    g.setColour(Palette::skin().knobInner);
    g.fillEllipse(body.reduced(diameter * 0.08f));
    g.setColour(Palette::skin().outline.withMultipliedAlpha(0.9f));
    g.drawEllipse(body, 1.1f);

    // JUCE supplies the normalized rotary position. Both knob sizes share this
    // single forward mapping; no secondary inversion is applied here.
    juce::Path tick;
    const float pointerWidth = large ? 2.8f : 2.3f;
    tick.addRoundedRectangle(-pointerWidth * 0.5f, -radius * 0.70f,
                             pointerWidth, radius * 0.36f, pointerWidth * 0.5f);
    g.setColour(Palette::skin().knobIndicator);
    g.fillPath(tick, juce::AffineTransform::rotation(angle).translated(centre.x, centre.y));
}

void SwaraXtLookAndFeel::drawButtonBackground(juce::Graphics& g,
                                             juce::Button& button,
                                             const juce::Colour& backgroundColour,
                                             bool shouldDrawButtonAsHighlighted,
                                             bool shouldDrawButtonAsDown)
{
    juce::ignoreUnused(backgroundColour);
    auto bounds = button.getLocalBounds().toFloat().reduced(0.5f);
    const bool presetNavigation = button.getProperties().getWithDefault(
        "swaraxtPresetNavigation", false);
    const bool secondaryAction = button.getProperties().getWithDefault(
        "swaraxtSecondaryAction", false);
    auto fill = presetNavigation || secondaryAction
        ? Palette::skin().displayBackground
        : (button.getToggleState() ? Palette::accent().withAlpha(0.28f)
                                   : Palette::panelRaised());
    if (shouldDrawButtonAsDown)
        fill = fill.brighter(0.08f);
    else if (shouldDrawButtonAsHighlighted)
        fill = fill.brighter(0.05f);

    const float radius = presetNavigation ? 4.0f : 5.0f;
    g.setColour(fill);
    g.fillRoundedRectangle(bounds, radius);
    g.setColour(button.getToggleState()
                    ? Palette::accent()
                    : ((presetNavigation || secondaryAction)
                           ? Palette::skin().displayBorder.withMultipliedAlpha(
                                 secondaryAction ? 0.72f : 1.0f)
                           : Palette::line()));
    g.drawRoundedRectangle(bounds, radius, 1.0f);
}

void SwaraXtLookAndFeel::drawComboBox(juce::Graphics& g,
                                     int width,
                                     int height,
                                     bool isButtonDown,
                                     int buttonX,
                                     int buttonY,
                                     int buttonW,
                                     int buttonH,
                                     juce::ComboBox& box)
{
    juce::ignoreUnused(buttonX, buttonY, buttonW, buttonH);
    auto bounds = juce::Rectangle<float>(0.5f, 0.5f,
                                         static_cast<float>(width) - 1.0f,
                                         static_cast<float>(height) - 1.0f);
    auto fill = Palette::skin().comboBackground;
    if (isButtonDown)
        fill = fill.brighter(0.10f);
    else if (box.isMouseOver())
        fill = Palette::skin().hover.withAlpha(0.22f).overlaidWith(fill);
    g.setColour(fill);
    g.fillRoundedRectangle(bounds, 4.0f);
    g.setColour(box.hasKeyboardFocus(true) ? Palette::skin().focus
                                            : Palette::skin().comboBorder);
    g.drawRoundedRectangle(bounds, 4.0f, box.hasKeyboardFocus(true) ? 1.5f : 1.0f);

    juce::Path arrow;
    const float cx = bounds.getRight() - 14.0f;
    const float cy = bounds.getCentreY();
    arrow.startNewSubPath(cx - 4.0f, cy - 2.0f);
    arrow.lineTo(cx, cy + 3.0f);
    arrow.lineTo(cx + 4.0f, cy - 2.0f);
    g.setColour(Palette::skin().comboArrow);
    g.strokePath(arrow, juce::PathStrokeType(1.7f, juce::PathStrokeType::curved,
                                             juce::PathStrokeType::rounded));
}

void SwaraXtLookAndFeel::drawLinearSlider(juce::Graphics& g,
                                          int x,
                                          int y,
                                          int width,
                                          int height,
                                          float sliderPos,
                                          float minSliderPos,
                                          float maxSliderPos,
                                          juce::Slider::SliderStyle style,
                                          juce::Slider& slider)
{
    if (style != juce::Slider::LinearHorizontal)
    {
        juce::LookAndFeel_V4::drawLinearSlider(g, x, y, width, height, sliderPos,
                                               minSliderPos, maxSliderPos, style, slider);
        return;
    }

    const float centre = 0.5f * (minSliderPos + maxSliderPos);
    const float trackY = static_cast<float>(y) + static_cast<float>(height) * 0.5f;
    const auto left = juce::jmin(centre, sliderPos);
    const auto right = juce::jmax(centre, sliderPos);

    g.setColour(Palette::skin().matrixGuide);
    g.fillRoundedRectangle(static_cast<float>(x), trackY - 1.5f,
                           static_cast<float>(width), 3.0f, 1.5f);
    g.setColour(sliderPos < centre ? Palette::skin().matrixAmountNegative
                                   : Palette::skin().matrixAmountPositive);
    g.fillRoundedRectangle(left, trackY - 2.0f, right - left, 4.0f, 2.0f);
    g.fillEllipse(sliderPos - 4.0f, trackY - 4.0f, 8.0f, 8.0f);
    g.setColour(Palette::skin().matrixText.withAlpha(0.8f));
    g.drawVerticalLine(static_cast<int>(std::round(centre)), trackY - 6.0f, trackY + 6.0f);
}

void SwaraXtLookAndFeel::positionComboBoxText(juce::ComboBox& box, juce::Label& label)
{
    label.setBounds(box.getLocalBounds().reduced(9, 1).withTrimmedRight(18));
    label.setJustificationType(juce::Justification::centredLeft);
}

juce::Font SwaraXtLookAndFeel::getComboBoxFont(juce::ComboBox&)
{
    return regularFont(12.0f);
}

juce::PopupMenu::Options SwaraXtLookAndFeel::getOptionsForComboBoxPopupMenu(
    juce::ComboBox& box, juce::Label& label)
{
    return juce::PopupMenu::Options().withTargetComponent(&box)
        .withItemThatMustBeVisible(box.getSelectedId())
        .withInitiallySelectedItem(box.getSelectedId())
        .withMinimumWidth(juce::jmax(210, box.getWidth()))
        .withMaximumNumColumns(1)
        .withStandardItemHeight(juce::jmax(24, label.getHeight()));
}

juce::Font SwaraXtLookAndFeel::getPopupMenuFont()
{
    return regularFont(13.0f);
}

juce::Font SwaraXtLookAndFeel::getLabelFont(juce::Label&)
{
    return regularFont(12.0f);
}

juce::Font SwaraXtLookAndFeel::getTextButtonFont(juce::TextButton&, int buttonHeight)
{
    return regularFont(static_cast<float>(buttonHeight) * 0.52f);
}

}  // namespace swaraxt::ui
