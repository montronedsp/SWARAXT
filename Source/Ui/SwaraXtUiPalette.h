// Copyright 2026 MontroneDSP.
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include <JuceHeader.h>

#include "Ui/SwaraXtSkin.h"

namespace swaraxt::ui {

/** Compatibility facade over the active semantic skin palette. */
struct Palette {
    static const SkinPalette& skin() { return SkinRegistry::active().palette; }
    static juce::Colour background() { return skin().editorBackground; }
    static juce::Colour panelDeep() { return skin().screenBackground; }
    static juce::Colour panel() { return skin().panelBackground; }
    static juce::Colour panelRaised() { return skin().panelSecondary; }
    static juce::Colour edge() { return skin().outline; }
    static juce::Colour line() { return skin().divider; }
    static juce::Colour grid() { return skin().visualizerGrid; }
    static juce::Colour accent() { return skin().knobActive; }
    static juce::Colour secondaryAccent() { return skin().visualizerSecondary; }
    static juce::Colour amber() { return skin().primaryText; }
    static juce::Colour orange() { return accent(); }
    static juce::Colour bronze() { return skin().knobRim; }
    static juce::Colour cream() { return skin().primaryText; }
    static juce::Colour muted() { return skin().disabled; }
    static juce::Colour mutedText() { return skin().mutedText; }
    static juce::Colour focus() { return skin().focus; }
};

/** Central geometry tokens for the native JUCE editor. */
struct Layout {
    static constexpr int editorWidth = GuiGeometry::designWidth;
    static constexpr int editorHeight = GuiGeometry::designHeight;

    static constexpr int outerMargin = 8;
    static constexpr int headerHeight = 0;
    static constexpr int footerHeight = 28;
    static constexpr int moduleGap = 7;
    static constexpr int controlGap = 5;
    static constexpr int moduleRadius = 6;
    static constexpr int moduleHeaderRow = 22;
    static constexpr int moduleInnerPad = 5;
    static constexpr int moduleTitlePad = 8;

    static constexpr int knobStandard = 44;
    static constexpr int knobLarge = 54;
    static constexpr int knobLabelHeight = 12;
    static constexpr int valueBoxHeight = 16;
    static constexpr int valueBoxWidth = 44;
    static constexpr int valueBoxWide = 54;
    static constexpr int selectorLabelHeight = 12;
    static constexpr int selectorHeight = 23;
    static constexpr int selectorPrimaryHeight = 25;
    static constexpr int selectorDescriptionHeight = 0;
    static constexpr int navButtonWidth = 82;

    static constexpr int synthTopRowPct = 58;
};

}  // namespace swaraxt::ui
