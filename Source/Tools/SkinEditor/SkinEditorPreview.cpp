// Copyright 2026 MontroneDSP.
// SPDX-License-Identifier: GPL-3.0-or-later

#include "Tools/SkinEditor/SkinEditorPreview.h"

#include <cmath>

namespace swaraxt::skin_editor {

void PreviewPanel::setPalette(const ui::SkinPalette& palette)
{
    palette_ = palette;
    repaint();
}

void PreviewPanel::paint(juce::Graphics& g)
{
    const auto bounds = getLocalBounds().toFloat();
    g.fillAll(palette_.editorBackground);

    auto module = bounds.reduced(18.0f).withTrimmedBottom(bounds.getHeight() * 0.34f);
    g.setColour(palette_.panelBackground);
    g.fillRoundedRectangle(module, 8.0f);
    g.setColour(palette_.outline.withAlpha(0.55f));
    g.drawRoundedRectangle(module, 8.0f, 1.2f);

    auto header = module.removeFromTop(28.0f);
    g.setColour(palette_.moduleHeaderBackground);
    g.fillRoundedRectangle(header.getX(), header.getY(), header.getWidth(), header.getHeight() + 4.0f, 8.0f);
    g.setColour(palette_.moduleHeaderText);
    g.setFont(juce::FontOptions(14.0f, juce::Font::bold));
    g.drawText("FILTER", header.reduced(10.0f, 0.0f), juce::Justification::centredLeft);

    g.setColour(palette_.divider);
    g.fillRect(module.getX() + 10.0f, module.getY() + 2.0f, module.getWidth() - 20.0f, 1.0f);

    auto body = module.reduced(14.0f, 12.0f);
    g.setColour(palette_.primaryText);
    g.setFont(juce::FontOptions(15.0f, juce::Font::bold));
    g.drawText("Cutoff", body.removeFromTop(20.0f), juce::Justification::centredLeft);
    g.setColour(palette_.secondaryText);
    g.setFont(juce::FontOptions(12.0f));
    g.drawText("Resonance", body.removeFromTop(18.0f), juce::Justification::centredLeft);
    g.setColour(palette_.mutedText);
    g.drawText("Key track", body.removeFromTop(18.0f), juce::Justification::centredLeft);

    auto knobArea = body.removeFromLeft(72.0f).withSizeKeepingCentre(56.0f, 56.0f);
    g.setColour(palette_.knobBody);
    g.fillEllipse(knobArea);
    g.setColour(palette_.knobRim);
    g.drawEllipse(knobArea, 2.0f);
    g.setColour(palette_.knobInner);
    g.fillEllipse(knobArea.reduced(12.0f));
    g.setColour(palette_.knobIndicator);
    g.drawLine(knobArea.getCentreX(), knobArea.getY() + 8.0f,
               knobArea.getCentreX(), knobArea.getCentreY(), 2.0f);
    g.setColour(palette_.knobActive);
    g.fillEllipse(knobArea.getCentreX() - 3.0f, knobArea.getY() + 6.0f, 6.0f, 6.0f);

    auto combo = body.removeFromTop(28.0f).reduced(8.0f, 0.0f);
    g.setColour(palette_.comboBackground);
    g.fillRoundedRectangle(combo, 4.0f);
    g.setColour(palette_.comboBorder);
    g.drawRoundedRectangle(combo, 4.0f, 1.0f);
    g.setColour(palette_.comboText);
    g.setFont(juce::FontOptions(13.0f));
    g.drawText("Low Pass", combo.reduced(8.0f, 0.0f), juce::Justification::centredLeft);
    g.setColour(palette_.comboArrow);
    juce::Path arrow;
    arrow.addTriangle(combo.getRight() - 18.0f, combo.getCentreY() - 3.0f,
                      combo.getRight() - 8.0f, combo.getCentreY() - 3.0f,
                      combo.getRight() - 13.0f, combo.getCentreY() + 4.0f);
    g.fillPath(arrow);

    auto display = body.removeFromTop(34.0f).reduced(8.0f, 4.0f);
    g.setColour(palette_.displayBackground);
    g.fillRoundedRectangle(display, 5.0f);
    g.setColour(palette_.displayBorder);
    g.drawRoundedRectangle(display, 5.0f, 1.2f);
    g.setColour(palette_.displayText);
    g.drawText("Preset 01", display, juce::Justification::centred);

    auto lower = bounds.reduced(18.0f).withTrimmedTop(bounds.getHeight() * 0.62f);
    auto viz = lower.removeFromLeft(lower.getWidth() * 0.58f);
    g.setColour(palette_.screenBackground);
    g.fillRoundedRectangle(viz, 6.0f);
    g.setColour(palette_.visualizerGrid);
    for (int i = 1; i < 4; ++i)
        g.drawHorizontalLine(static_cast<int>(viz.getY() + viz.getHeight() * 0.25f * static_cast<float>(i)),
                             viz.getX() + 6.0f, viz.getRight() - 6.0f);
    juce::Path wave;
    const float mid = viz.getCentreY();
    wave.startNewSubPath(viz.getX() + 8.0f, mid);
    for (int x = 0; x < 48; ++x)
    {
        const float t = static_cast<float>(x) / 47.0f;
        const float y = mid + std::sin(t * juce::MathConstants<float>::twoPi * 2.0f) * (viz.getHeight() * 0.28f);
        wave.lineTo(viz.getX() + 8.0f + t * (viz.getWidth() - 16.0f), y);
    }
    g.setColour(palette_.visualizerPrimary);
    g.strokePath(wave, juce::PathStrokeType(2.0f));
    g.setColour(palette_.visualizerSecondary.withAlpha(0.7f));
    g.strokePath(wave, juce::PathStrokeType(1.0f));

    auto matrix = lower.reduced(10.0f, 0.0f);
    g.setColour(palette_.panelSecondary);
    g.fillRoundedRectangle(matrix, 6.0f);
    g.setColour(palette_.matrixGuide);
    g.drawRoundedRectangle(matrix, 6.0f, 1.0f);
    g.setColour(palette_.matrixText);
    g.setFont(juce::FontOptions(12.0f, juce::Font::bold));
    g.drawText("MOD", matrix.removeFromTop(22.0f).reduced(8.0f, 0.0f), juce::Justification::centredLeft);
    g.setColour(palette_.matrixAmountPositive);
    g.drawText("+64", matrix.removeFromTop(18.0f).reduced(8.0f, 0.0f), juce::Justification::centredLeft);
    g.setColour(palette_.matrixAmountNegative);
    g.drawText("-32", matrix.removeFromTop(18.0f).reduced(8.0f, 0.0f), juce::Justification::centredLeft);
    g.setColour(palette_.focus);
    g.drawRoundedRectangle(matrix.reduced(8.0f).removeFromTop(18.0f), 3.0f, 1.5f);
    g.setColour(palette_.hover);
    g.fillRect(matrix.reduced(8.0f).removeFromTop(10.0f));
    g.setColour(palette_.disabled);
    g.setFont(juce::FontOptions(11.0f));
    g.drawText("disabled", matrix.reduced(8.0f), juce::Justification::centredLeft);

    g.setColour(palette_.braidTint);
    g.fillRect(bounds.getX() + 6.0f, bounds.getY() + 24.0f, 4.0f, bounds.getHeight() - 48.0f);
    g.setColour(palette_.frame);
    g.drawRect(bounds.reduced(1.0f), 1.0f);
}

}  // namespace swaraxt::skin_editor
