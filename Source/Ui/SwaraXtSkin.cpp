// Copyright 2026 MontroneDSP.
// SPDX-License-Identifier: GPL-3.0-or-later

#include "Ui/SwaraXtSkin.h"

#include <BinaryData.h>

namespace swaraxt::ui {

namespace {

constexpr size_t indexOf(AssetRole role) { return static_cast<size_t>(role); }

SkinPalette midnightPalette()
{
    const auto midnight = juce::Colour(0xff0b0912);
    const auto panel = juce::Colour(0xff151126);
    const auto gold = juce::Colour(0xff8e6b29);
    const auto ivory = juce::Colour(0xffe7ca83);
    const auto violet = juce::Colour(0xffa64cff);
    return {
        midnight, panel, juce::Colour(0xff1b1630), juce::Colour(0xff100d1b),
        ivory, juce::Colour(0xffc8aa66), juce::Colour(0xff99886b),
        juce::Colour(0xffc7a753), midnight, gold, ivory, gold, gold,
        juce::Colour(0xff15141c), juce::Colour(0xff08070c), gold, ivory, ivory,
        juce::Colour(0xff100d1b), gold, ivory, ivory,
        juce::Colour(0xff100d1b), ivory, gold,
        ivory, violet, juce::Colour(0xff342843),
        juce::Colour(0xff493c4d), ivory, violet, juce::Colour(0xffc48448),
        violet, juce::Colour(0xffd9b862), juce::Colour(0xff5c5665)
    };
}

SkinPalette neonPalette()
{
    const auto cobalt = juce::Colour(0xff060b18);
    const auto panel = juce::Colour(0xff07152f);
    const auto cyan = juce::Colour(0xff00c9ff);
    const auto white = juce::Colour(0xfff2f7ff);
    const auto violet = juce::Colour(0xff8d54ff);
    return {
        cobalt, panel, juce::Colour(0xff0a1030), juce::Colour(0xff050b15),
        white, juce::Colour(0xffd9e8ff), juce::Colour(0xff9db6d6),
        juce::Colour(0xff318fdf), cobalt, juce::Colour(0xff318fdf),
        juce::Colour(0xff00c9ff), juce::Colour(0xff318fdf), juce::Colour(0xff204d83),
        juce::Colour(0xff071322), juce::Colour(0xff03060c), cyan,
        white, juce::Colour(0xff09d7ff), juce::Colour(0xff071322),
        cyan, white, juce::Colour(0xff72e7ff), juce::Colour(0xff050b15),
        white, cyan, cyan, violet, juce::Colour(0xff1c293b),
        juce::Colour(0xff315a89), white, cyan, violet,
        cyan, juce::Colour(0xff43bbff), juce::Colour(0xff50627a)
    };
}

SkinPalette pastelPalette()
{
    SkinPalette p {};
    p.editorBackground = juce::Colour(0xfff7e8bd);
    p.panelBackground = juce::Colour(0xffe7ca83);
    p.panelSecondary = juce::Colour(0xffc89432);
    p.screenBackground = juce::Colour(0xff130d14);
    p.primaryText = juce::Colour(0xff542044);
    p.secondaryText = juce::Colour(0xff7c355f);
    p.mutedText = juce::Colour(0xff7a654f);
    p.moduleHeaderBackground = juce::Colour(0xffc58e2f);
    p.moduleHeaderText = juce::Colour(0xff160d15);
    p.divider = juce::Colour(0xffb9852b);
    p.braidTint = juce::Colour(0xffc58e2f);
    p.frame = juce::Colour(0xffb9852b);
    p.outline = juce::Colour(0xff542044);
    p.knobBody = juce::Colour(0xff181018);
    p.knobInner = juce::Colour(0xff050508);
    p.knobRim = juce::Colour(0xffb9852b);
    p.knobIndicator = juce::Colour(0xfff7e8bd);
    p.knobActive = juce::Colour(0xffd951a5);
    p.comboBackground = juce::Colour(0xff160d15);
    p.comboBorder = juce::Colour(0xffb9852b);
    p.comboText = juce::Colour(0xfff7e8bd);
    p.comboArrow = juce::Colour(0xffd951a5);
    p.displayBackground = juce::Colour(0xff100b11);
    p.displayText = juce::Colour(0xfff7e8bd);
    p.displayBorder = juce::Colour(0xffc58e2f);
    p.visualizerPrimary = juce::Colour(0xffd951a5);
    p.visualizerSecondary = juce::Colour(0xffe7ca83);
    p.visualizerGrid = juce::Colour(0xff5b3f54);
    p.matrixGuide = juce::Colour(0xff8a6b77);
    p.matrixText = juce::Colour(0xff542044);
    p.matrixAmountPositive = juce::Colour(0xffd951a5);
    p.matrixAmountNegative = juce::Colour(0xff8a3d75);
    p.focus = juce::Colour(0xffd951a5);
    p.hover = juce::Colour(0xffe7ca83);
    p.disabled = juce::Colour(0xff9c8b7d);
    return p;
}

SkinPalette junglePalette()
{
    SkinPalette p {};
    p.editorBackground = juce::Colour(0xff080c08);
    p.panelBackground = juce::Colour(0xff101710);
    p.panelSecondary = juce::Colour(0xff354124);
    p.screenBackground = juce::Colour(0xff080c08);
    p.primaryText = juce::Colour(0xffe2d8aa);
    p.secondaryText = juce::Colour(0xffaa9862);
    p.mutedText = juce::Colour(0xff817657);
    p.moduleHeaderBackground = juce::Colour(0xff6f8444);
    p.moduleHeaderText = juce::Colour(0xff080c08);
    p.divider = juce::Colour(0xff647042);
    p.braidTint = juce::Colour(0xffaa9862);
    p.frame = juce::Colour(0xff817657);
    p.outline = juce::Colour(0xff647042);
    p.knobBody = juce::Colour(0xff172017);
    p.knobInner = juce::Colour(0xff080c08);
    p.knobRim = juce::Colour(0xff647042);
    p.knobIndicator = juce::Colour(0xffe2d8aa);
    p.knobActive = juce::Colour(0xff48c7a8);
    p.comboBackground = juce::Colour(0xff0d150f);
    p.comboBorder = juce::Colour(0xff6f8444);
    p.comboText = juce::Colour(0xffe2d8aa);
    p.comboArrow = juce::Colour(0xff48c7a8);
    p.displayBackground = juce::Colour(0xff080c08);
    p.displayText = juce::Colour(0xffe2d8aa);
    p.displayBorder = juce::Colour(0xffaa9862);
    p.visualizerPrimary = juce::Colour(0xff48c7a8);
    p.visualizerSecondary = juce::Colour(0xffaa9862);
    p.visualizerGrid = juce::Colour(0xff354124);
    p.matrixGuide = juce::Colour(0xff647042);
    p.matrixText = juce::Colour(0xffe2d8aa);
    p.matrixAmountPositive = juce::Colour(0xff48c7a8);
    p.matrixAmountNegative = juce::Colour(0xffaa9862);
    p.focus = juce::Colour(0xff48c7a8);
    p.hover = juce::Colour(0xff6f8444);
    p.disabled = juce::Colour(0xff59624d);
    return p;
}

SkinPalette rossocorsaPalette()
{
    SkinPalette p {};
    p.editorBackground = juce::Colour(0xff08090a);
    p.panelBackground = juce::Colour(0xff111214);
    p.panelSecondary = juce::Colour(0xff251516);
    p.screenBackground = juce::Colour(0xff090a0b);
    p.primaryText = juce::Colour(0xfff1eee7);
    p.secondaryText = juce::Colour(0xffb9b2a7);
    p.mutedText = juce::Colour(0xff6f6965);
    p.moduleHeaderBackground = juce::Colour(0xffd31d17);
    p.moduleHeaderText = juce::Colour(0xfff1eee7);
    p.divider = juce::Colour(0xff7e1b18);
    p.braidTint = juce::Colour(0xffd31d17);
    p.frame = juce::Colour(0xff6f6965);
    p.outline = juce::Colour(0xff3d1110);
    p.knobBody = juce::Colour(0xff151617);
    p.knobInner = juce::Colour(0xff08090a);
    p.knobRim = juce::Colour(0xff3d1110);
    p.knobIndicator = juce::Colour(0xfff1eee7);
    p.knobActive = juce::Colour(0xffef2b22);
    p.comboBackground = juce::Colour(0xff0c0d0e);
    p.comboBorder = juce::Colour(0xff7e1b18);
    p.comboText = juce::Colour(0xfff1eee7);
    p.comboArrow = juce::Colour(0xffef2b22);
    p.displayBackground = juce::Colour(0xff090a0b);
    p.displayText = juce::Colour(0xfff1eee7);
    p.displayBorder = juce::Colour(0xffd31d17);
    p.visualizerPrimary = juce::Colour(0xffef2b22);
    p.visualizerSecondary = juce::Colour(0xfff1eee7);
    p.visualizerGrid = juce::Colour(0xff3d1110);
    p.matrixGuide = juce::Colour(0xff6f6965);
    p.matrixText = juce::Colour(0xfff1eee7);
    p.matrixAmountPositive = juce::Colour(0xffef2b22);
    p.matrixAmountNegative = juce::Colour(0xff9c1d19);
    p.focus = juce::Colour(0xffef2b22);
    p.hover = juce::Colour(0xffd31d17);
    p.disabled = juce::Colour(0xff575351);
    return p;
}

SkinId activeSkin = SkinId::pastel;

}  // namespace

const SwaraXtSkin& SkinRegistry::midnightGold()
{
    static const SwaraXtSkin skin {
        SkinId::midnightGold,
        "midnight_gold",
        "Midnight Gold",
        midnightPalette(),
        {
            "midnight_backdrop_svg",
            "company_wordmark_svg",
            "midnight_product_lockup_svg",
            "braid_svg",
            "jam_ornament_svg",
            "mod_matrix_frame_svg",
            "visualizer_frame_svg",
            "vco1_header_svg",
            "vco2_header_svg"
        }
    };
    return skin;
}

const SwaraXtSkin& SkinRegistry::neonCobalt()
{
    static const SwaraXtSkin skin {
        SkinId::neonCobalt,
        "neon_cobalt",
        "Neon Cobalt",
        neonPalette(),
        {
            "neon_backdrop_svg",
            "neon_company_wordmark_svg",
            "neon_product_lockup_svg",
            "neon_braid_svg",
            "neon_jam_ornament_svg",
            "neon_mod_matrix_frame_svg",
            "neon_visualizer_frame_svg",
            "vco1_header_svg",
            "vco2_header_svg"
        }
    };
    return skin;
}

const SwaraXtSkin& SkinRegistry::pastel()
{
    static const SwaraXtSkin skin {
        SkinId::pastel, "pastel", "Pastel", pastelPalette(),
        { "", "company_wordmark_svg", "midnight_product_lockup_svg", "braid_svg",
          "jam_ornament_svg", "mod_matrix_frame_svg", "visualizer_frame_svg",
          "vco1_header_svg", "vco2_header_svg" }
    };
    return skin;
}

const SwaraXtSkin& SkinRegistry::jungle()
{
    static const SwaraXtSkin skin {
        SkinId::jungle, "jungle", "Jungle", junglePalette(),
        { "", "company_wordmark_svg", "midnight_product_lockup_svg", "braid_svg",
          "jam_ornament_svg", "mod_matrix_frame_svg", "visualizer_frame_svg",
          "vco1_header_svg", "vco2_header_svg" }
    };
    return skin;
}

const SwaraXtSkin& SkinRegistry::rossocorsa()
{
    static const SwaraXtSkin skin {
        SkinId::rossocorsa, "rossocorsa", "Rossocorsa", rossocorsaPalette(),
        { "", "company_wordmark_svg", "midnight_product_lockup_svg", "braid_svg",
          "jam_ornament_svg", "mod_matrix_frame_svg", "visualizer_frame_svg",
          "vco1_header_svg", "vco2_header_svg" }
    };
    return skin;
}

const SwaraXtSkin& SkinRegistry::get(SkinId id)
{
    switch (id)
    {
        case SkinId::neonCobalt: return neonCobalt();
        case SkinId::pastel: return pastel();
        case SkinId::jungle: return jungle();
        case SkinId::rossocorsa: return rossocorsa();
        case SkinId::midnightGold: break;
    }
    return midnightGold();
}

const SwaraXtSkin& SkinRegistry::active()
{
    return get(activeSkin);
}

void SkinRegistry::setActive(SkinId id)
{
    activeSkin = id;
}

SkinAssetCache::SkinAssetCache(const SwaraXtSkin& skin)
{
    load(skin);
}

void SkinAssetCache::load(const SwaraXtSkin& skin)
{
    for (size_t i = 0; i < assets_.size(); ++i)
    {
        int size = 0;
        const auto* resourceName = skin.resourceNames[i];
        const auto* data = resourceName != nullptr && resourceName[0] != '\0'
            ? BinaryData::getNamedResource(resourceName, size) : nullptr;
        assets_[i] = data != nullptr && size > 0
            ? juce::Drawable::createFromImageData(data, static_cast<size_t>(size))
            : nullptr;
    }

    const auto recolour = [this](juce::Colour source, juce::Colour target)
    {
        for (auto& asset : assets_)
            if (asset != nullptr)
                asset->replaceColour(source, target);
    };

    recolour(juce::Colour(0xffd9dadb), skin.palette.moduleHeaderBackground);
    recolour(juce::Colour(0xff1a171b), skin.palette.moduleHeaderText);
    if (skin.id != SkinId::midnightGold && skin.id != SkinId::neonCobalt)
    {
        recolour(juce::Colour(0xffe7ca83), skin.palette.primaryText);
        recolour(juce::Colour(0xff8e6b29), skin.palette.frame);
        recolour(juce::Colour(0xffe42523), skin.palette.visualizerSecondary);
        recolour(juce::Colour(0xffffffff), skin.palette.outline);
    }
}

const juce::Drawable* SkinAssetCache::get(AssetRole role) const noexcept
{
    return assets_[indexOf(role)].get();
}

const DecorationDefinition& DecorationRegistry::get(DecorationId id)
{
    static const DecorationDefinition legacy {
        DecorationId::legacy, "legacy", "Legacy", nullptr
    };
    static const DecorationDefinition pcbTrace {
        DecorationId::pcbTrace, "pcb_trace", "PCB Trace", "pcb_trace_separator_svg"
    };
    switch (id)
    {
        case DecorationId::pcbTrace: return pcbTrace;
        case DecorationId::legacy: break;
    }
    return legacy;
}

DecorationAssetCache::DecorationAssetCache(DecorationId decoration,
                                           const SkinPalette& palette)
{
    load(decoration, palette);
}

void DecorationAssetCache::load(DecorationId decoration, const SkinPalette& palette)
{
    const auto& definition = DecorationRegistry::get(decoration);
    int size = 0;
    const auto* data = definition.resourceName != nullptr
        ? BinaryData::getNamedResource(definition.resourceName, size) : nullptr;
    asset_ = data != nullptr && size > 0
        ? juce::Drawable::createFromImageData(data, static_cast<size_t>(size))
        : nullptr;
    if (asset_ != nullptr)
        asset_->replaceColour(juce::Colours::white, palette.braidTint);
}

juce::Rectangle<int> GuiGeometry::editorBounds(GuiSize size)
{
    switch (size)
    {
        case GuiSize::small: return { 0, 0, 891, 417 };
        case GuiSize::large: return { 0, 0, 1391, 651 };
        case GuiSize::medium: break;
    }
    return { 0, 0, designWidth, designHeight };
}

float GuiGeometry::scaleFor(GuiSize size)
{
    const auto bounds = editorBounds(size);
    return juce::jmin(static_cast<float>(bounds.getWidth()) / designWidth,
                      static_cast<float>(bounds.getHeight()) / designHeight);
}

const char* GuiGeometry::stableId(GuiSize size)
{
    switch (size)
    {
        case GuiSize::small: return "small";
        case GuiSize::large: return "large";
        case GuiSize::medium: break;
    }
    return "medium";
}

std::unique_ptr<juce::PropertiesFile> UiPreferences::open()
{
    juce::PropertiesFile::Options options;
    options.applicationName = "Swara XT";
    options.filenameSuffix = ".settings";
    options.folderName = "MontroneDSP/Swara XT";
    options.osxLibrarySubFolder = "Application Support";
    options.storageFormat = juce::PropertiesFile::storeAsXML;
    return std::make_unique<juce::PropertiesFile>(options);
}

SkinId UiPreferences::loadSkin()
{
    const auto properties = open();
    const auto value = properties->getValue("skin", "pastel");
    if (value == "midnight_gold") return SkinId::midnightGold;
    if (value == "neon_cobalt") return SkinId::neonCobalt;
    if (value == "pastel") return SkinId::pastel;
    if (value == "jungle") return SkinId::jungle;
    if (value == "rossocorsa") return SkinId::rossocorsa;
    return SkinId::pastel;
}

DecorationId UiPreferences::loadDecoration()
{
    const auto properties = open();
    const auto value = properties->getValue("decoration", "legacy");
    if (value == "pcb_trace") return DecorationId::pcbTrace;
    return DecorationId::legacy;
}

GuiSize UiPreferences::loadGuiSize()
{
    const auto properties = open();
    const auto value = properties->getValue("gui_size", "medium");
    if (value == "small") return GuiSize::small;
    if (value == "large") return GuiSize::large;
    return GuiSize::medium;
}

void UiPreferences::save(SkinId skin, DecorationId decoration, GuiSize size)
{
    auto properties = open();
    properties->setValue("skin", SkinRegistry::get(skin).stableId);
    properties->setValue("decoration", DecorationRegistry::get(decoration).stableId);
    properties->setValue("gui_size", GuiGeometry::stableId(size));
    properties->saveIfNeeded();
}

swaraxt::FilterQuality UiPreferences::loadFilterQuality()
{
    const auto properties = open();
    const auto value = properties->getValue("filter_quality", "high");
    if (value == "normal") return swaraxt::FilterQuality::normal;
    if (value == "eco") return swaraxt::FilterQuality::eco;
    return swaraxt::FilterQuality::high;
}

void UiPreferences::saveFilterQuality(swaraxt::FilterQuality quality)
{
    auto properties = open();
    const char* value = "high";
    if (quality == swaraxt::FilterQuality::normal) value = "normal";
    else if (quality == swaraxt::FilterQuality::eco) value = "eco";
    properties->setValue("filter_quality", value);
    properties->saveIfNeeded();
}

}  // namespace swaraxt::ui
