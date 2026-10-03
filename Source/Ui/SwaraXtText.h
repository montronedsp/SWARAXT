// Copyright 2026 MontroneDSP.
// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
#include <JuceHeader.h>

namespace swaraxt::ui {
// A single line has a font-metric baseline, independent of glyph ink bounds.
// Width fitting never changes its height or baseline, even at fractional scales.
inline juce::GlyphArrangement singleLineText(const juce::Font& font, const juce::String& text,
                                             juce::Rectangle<float> area,
                                             juce::Justification justification)
{
    juce::GlyphArrangement glyphs;
    const float baseline = area.getCentreY() + (font.getAscent() - font.getDescent()) * 0.5f;
    glyphs.addLineOfText(font, text, 0.0f, baseline);
    const auto bounds = glyphs.getBoundingBox(0, -1, true);
    const float width = bounds.getRight();
    if (width > area.getWidth() && width > 0.0f)
        glyphs.stretchRangeOfGlyphs(0, -1, area.getWidth() / width);
    const float fittedWidth = juce::jmin(width, area.getWidth());
    const float x = justification.testFlags(juce::Justification::horizontallyCentred)
        ? area.getCentreX() - fittedWidth * 0.5f
        : (justification.testFlags(juce::Justification::right)
            ? area.getRight() - fittedWidth : area.getX());
    glyphs.moveRangeOfGlyphs(0, -1, x, 0.0f);
    return glyphs;
}
}
