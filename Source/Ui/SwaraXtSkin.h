// Copyright 2026 MontroneDSP.
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include <JuceHeader.h>

#include <array>
#include <memory>

#include "Engine/Filter/FilterQuality.h"

namespace swaraxt::ui {

enum class SkinId { midnightGold, neonCobalt, pastel, jungle, rossocorsa };
enum class DecorationId { legacy, pcbTrace, greekKey1Turn, greekKey4Turns, romanPlait, romanIntertwined, romanGuilloche, romanInterlace, romanWicker, romanMeander, romanKeyBorder };
enum class ManufacturerMark { montroneDspWordmark, stone };
struct ManufacturerMarkDefinition {
    static constexpr ManufacturerMark defaultMark = ManufacturerMark::montroneDspWordmark;
    static const char* displayName(ManufacturerMark mark) {
        return mark == ManufacturerMark::stone ? "Stones" : "MontroneDSP Wordmark";
    }
    static const char* stableId(ManufacturerMark mark) {
        return mark == ManufacturerMark::stone ? "stone" : "montronedsp_wordmark";
    }
};
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
    stoneWordmark,
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
    static constexpr float editorOffsetY = -26.0f;
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

// User artwork lives under the preferences folder (not the plugin bundle).
// Message-thread only: install/clear/load must not run on the audio thread.
// A file that fails the size or parse rules is rejected on install. A file that
// later fails to parse is ignored and the built-in art is used.
class SkinUserSvg {
 public:
    static constexpr int maxSvgBytes = 256 * 1024;
    static constexpr int maxPngBytes = 4 * 1024 * 1024;
    static constexpr int maxImageEdge = 8192;

    static juce::File directory();
    static juce::File manufacturerMarkFile();
    static juce::File decorationFile();
    static juce::File backgroundSvgFile();
    static juce::File backgroundPngFile();
    static bool hasManufacturerMark();
    static bool hasDecoration();
    static bool hasBackground();
    static bool install(const juce::File& source, const juce::File& destination, juce::String& error);
    static bool installBackground(const juce::File& source, juce::String& error);
    static void clear(const juce::File& file);
    static void clearBackground();
    static std::unique_ptr<juce::Drawable> loadManufacturerMark(const SkinPalette& palette);
    static std::unique_ptr<juce::Drawable> loadDecoration(const SkinPalette& palette);
    static std::unique_ptr<juce::Drawable> loadBackground();
};

class SkinAssetCache {
 public:
    explicit SkinAssetCache(const SwaraXtSkin& skin);
    void load(const SwaraXtSkin& skin);
    const juce::Drawable* get(AssetRole role) const noexcept;
    const juce::Drawable* getManufacturerOverride() const noexcept { return manufacturerOverride_.get(); }
    const juce::Drawable* getBackgroundOverride() const noexcept { return backgroundOverride_.get(); }

 private:
    std::array<std::unique_ptr<juce::Drawable>, static_cast<size_t>(AssetRole::count)> assets_;
    std::unique_ptr<juce::Drawable> manufacturerOverride_;
    std::unique_ptr<juce::Drawable> backgroundOverride_;
};

struct DecorationDefinition {
    DecorationId id;
    const char* stableId;
    const char* displayName;
    const char* resourceName;
};

class DecorationRegistry {
 public:
    static constexpr std::array<DecorationId, 11> all {
        DecorationId::legacy, DecorationId::pcbTrace,
        DecorationId::greekKey1Turn,
        DecorationId::greekKey4Turns,
        DecorationId::romanPlait,
        DecorationId::romanIntertwined,
        DecorationId::romanGuilloche,
        DecorationId::romanInterlace,
        DecorationId::romanWicker,
        DecorationId::romanMeander,
        DecorationId::romanKeyBorder
    };
    static const DecorationDefinition& get(DecorationId id);
};

class DecorationAssetCache {
 public:
    DecorationAssetCache(DecorationId decoration, const SkinPalette& palette);
    void load(DecorationId decoration, const SkinPalette& palette);
    const juce::Drawable* get() const noexcept { return asset_.get(); }
    bool usingCustom() const noexcept { return usingCustom_; }

 private:
    std::unique_ptr<juce::Drawable> asset_;
    bool usingCustom_ = false;
};

struct SkinColourEntry {
    const char* id;
    const char* label;
    juce::Colour SkinPalette::* member;
};

// Shared colour key table for the plugin override file and the standalone editor.
class SkinColourCatalog {
 public:
    static constexpr int count = 35;
    static const SkinColourEntry* entries() noexcept;
    static juce::String toText(const SkinPalette& palette);
    static int applyText(const juce::String& text, SkinPalette& palette);
    static bool saveText(const juce::File& destination, const SkinPalette& palette, juce::String& error);
};

// Partial colour override. Keys that parse replace the active skin; missing or
// invalid keys keep that skin's colour. A file with no valid keys is ignored.
class SkinColourOverride {
 public:
    static constexpr int maxBytes = 64 * 1024;

    static juce::File file();
    static bool hasFile();
    static bool active();
    static const SkinPalette& resolved();
    static void refresh();
    static bool install(const juce::File& source, juce::String& error);
    static void clear();
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
    static ManufacturerMark loadManufacturerMark();
    static swaraxt::FilterQuality loadFilterQuality();
    static void save(SkinId skin, DecorationId decoration, GuiSize size);
    static void saveFilterQuality(swaraxt::FilterQuality quality);
    static void saveManufacturerMark(ManufacturerMark mark);
    static juce::File userDataDirectory();

 private:
    static std::unique_ptr<juce::PropertiesFile> open();
};

}  // namespace swaraxt::ui
