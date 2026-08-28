// Copyright 2026 MontroneDSP.
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include <JuceHeader.h>

#include <array>
#include <memory>

#include "Engine/Filter/FilterQuality.h"

namespace swaraxt::ui {

enum class SkinId { midnightGold, neonCobalt, pastel, jungle, rossocorsa };
enum class DecorationId { legacy, pcbTrace };
enum class GuiSize { small, medium, large };

enum class AssetRole : size_t {
    backdrop,
    companyWordmark,
    productLockup,
    braid,
    jamOrnament,
    modulationFrame,
    visualizerFrame,
    vco1Header,
    vco2Header,
    count
};

struct SkinPalette {
    juce::Colour editorBackground;
    juce::Colour panelBackground;
    juce::Colour panelSecondary;
    juce::Colour screenBackground;
    juce::Colour primaryText;
    juce::Colour secondaryText;
    juce::Colour mutedText;
    juce::Colour moduleHeaderBackground;
    juce::Colour moduleHeaderText;
    juce::Colour divider;
    juce::Colour braidTint;
    juce::Colour frame;
    juce::Colour outline;
    juce::Colour knobBody;
    juce::Colour knobInner;
    juce::Colour knobRim;
    juce::Colour knobIndicator;
    juce::Colour knobActive;
    juce::Colour comboBackground;
    juce::Colour comboBorder;
    juce::Colour comboText;
    juce::Colour comboArrow;
    juce::Colour displayBackground;
    juce::Colour displayText;
    juce::Colour displayBorder;
    juce::Colour visualizerPrimary;
    juce::Colour visualizerSecondary;
    juce::Colour visualizerGrid;
    juce::Colour matrixGuide;
    juce::Colour matrixText;
    juce::Colour matrixAmountPositive;
    juce::Colour matrixAmountNegative;
    juce::Colour focus;
    juce::Colour hover;
    juce::Colour disabled;
};

struct SwaraXtSkin {
    SkinId id;
    const char* stableId;
    const char* displayName;
    SkinPalette palette;
    std::array<const char*, static_cast<size_t>(AssetRole::count)> resourceNames;
};

class SkinRegistry {
 public:
    static const SwaraXtSkin& midnightGold();
    static const SwaraXtSkin& neonCobalt();
    static const SwaraXtSkin& pastel();
    static const SwaraXtSkin& jungle();
    static const SwaraXtSkin& rossocorsa();
    static const SwaraXtSkin& get(SkinId id);
    static const SwaraXtSkin& active();
    static void setActive(SkinId id);
};

struct ProductLockupGeometry {
    static constexpr float editorOffsetY = -10.0f;
    static juce::Rectangle<float> instrumentMark() { return { 433.340f, 156.590f, 21.436f, 21.435f }; }
    static juce::Rectangle<float> wordmark() { return { 459.032f, 158.481f, 116.871f, 18.500f }; }
    static juce::Rectangle<float> tagline() { return { 585.600f, 167.690f, 94.414f, 11.094f }; }
    static juce::Rectangle<float> svaraMark() { return { 583.428f, 185.594f, 28.806f, 15.278f }; }
    static juce::Rectangle<float> overall() { return { 433.340f, 156.590f, 246.674f, 44.282f }; }
    static juce::Rectangle<float> editorPlacement()
    {
        return overall().translated(0.0f, editorOffsetY);
    }
};

class SkinAssetCache {
 public:
    explicit SkinAssetCache(const SwaraXtSkin& skin);
    void load(const SwaraXtSkin& skin);
    const juce::Drawable* get(AssetRole role) const noexcept;

 private:
    std::array<std::unique_ptr<juce::Drawable>, static_cast<size_t>(AssetRole::count)> assets_;
};

struct DecorationDefinition {
    DecorationId id;
    const char* stableId;
    const char* displayName;
    const char* resourceName;
};

class DecorationRegistry {
 public:
    static const DecorationDefinition& get(DecorationId id);
};

class DecorationAssetCache {
 public:
    DecorationAssetCache(DecorationId decoration, const SkinPalette& palette);
    void load(DecorationId decoration, const SkinPalette& palette);
    const juce::Drawable* get() const noexcept { return asset_.get(); }

 private:
    std::unique_ptr<juce::Drawable> asset_;
};

struct GuiGeometry {
    static constexpr int designWidth = 1113;
    static constexpr int designHeight = 521;
    static constexpr double designAspect = static_cast<double>(designWidth) / designHeight;

    static juce::Rectangle<int> editorBounds(GuiSize size);
    static float scaleFor(GuiSize size);
    static const char* stableId(GuiSize size);
};

class UiPreferences {
 public:
    static SkinId loadSkin();
    static DecorationId loadDecoration();
    static GuiSize loadGuiSize();
    static swaraxt::FilterQuality loadFilterQuality();
    static void save(SkinId skin, DecorationId decoration, GuiSize size);
    static void saveFilterQuality(swaraxt::FilterQuality quality);

 private:
    static std::unique_ptr<juce::PropertiesFile> open();
};

}  // namespace swaraxt::ui
