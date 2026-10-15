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
            "montronedsp_wordmark_svg",
            "midnight_product_lockup_svg",
            "braid_svg",
            "jam_ornament_svg",
            "mod_matrix_frame_svg",
            "visualizer_frame_svg",
            "vco1_header_svg",
            "vco2_header_svg", "stones_letters_svg"
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
            "montronedsp_wordmark_svg",
            "neon_product_lockup_svg",
            "neon_braid_svg",
            "neon_jam_ornament_svg",
            "neon_mod_matrix_frame_svg",
            "neon_visualizer_frame_svg",
            "vco1_header_svg",
            "vco2_header_svg", "stones_letters_svg"
        }
    };
    return skin;
}

const SwaraXtSkin& SkinRegistry::pastel()
{
    static const SwaraXtSkin skin {
        SkinId::pastel, "pastel", "Pastel", pastelPalette(),
        { "", "montronedsp_wordmark_svg", "midnight_product_lockup_svg", "braid_svg",
          "jam_ornament_svg", "mod_matrix_frame_svg", "visualizer_frame_svg",
          "vco1_header_svg", "vco2_header_svg", "stones_letters_svg" }
    };
    return skin;
}

const SwaraXtSkin& SkinRegistry::jungle()
{
    static const SwaraXtSkin skin {
        SkinId::jungle, "jungle", "Jungle", junglePalette(),
        { "", "montronedsp_wordmark_svg", "midnight_product_lockup_svg", "braid_svg",
          "jam_ornament_svg", "mod_matrix_frame_svg", "visualizer_frame_svg",
          "vco1_header_svg", "vco2_header_svg", "stones_letters_svg" }
    };
    return skin;
}

const SwaraXtSkin& SkinRegistry::rossocorsa()
{
    static const SwaraXtSkin skin {
        SkinId::rossocorsa, "rossocorsa", "Rossocorsa", rossocorsaPalette(),
        { "", "montronedsp_wordmark_svg", "midnight_product_lockup_svg", "braid_svg",
          "jam_ornament_svg", "mod_matrix_frame_svg", "visualizer_frame_svg",
          "vco1_header_svg", "vco2_header_svg", "stones_letters_svg" }
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

namespace {

juce::String hexColour(juce::Colour value)
{
    return "#" + value.toDisplayString(false);
}

juce::String recolourManufacturerSvg(const juce::String& svg, const SkinPalette& palette)
{
    // Same placeholder map as the embedded wordmarks so user SVGs that follow
    // the shipping template pick up the active skin colours.
    return juce::String(svg)
        .replace("#ffffff", hexColour(palette.primaryText))
        .replace("#f5e9fb", hexColour(palette.primaryText))
        .replace("#9b4f9e", hexColour(palette.primaryText))
        .replace("#d0d0d0", hexColour(palette.primaryText.interpolatedWith(palette.editorBackground, 0.15f)))
        .replace("#0a0608", hexColour(palette.editorBackground.darker(0.5f)));
}

std::unique_ptr<juce::Drawable> parseSvgBytes(const void* data, size_t size)
{
    if (data == nullptr || size == 0)
        return nullptr;
    auto drawable = juce::Drawable::createFromImageData(data, size);
    if (drawable == nullptr || drawable->getDrawableBounds().isEmpty())
        return nullptr;
    return drawable;
}

std::unique_ptr<juce::Drawable> drawableFromPng(const juce::File& file)
{
    const auto image = juce::ImageFileFormat::loadFrom(file);
    if (! image.isValid() || image.getWidth() < 1 || image.getHeight() < 1)
        return nullptr;
    if (image.getWidth() > SkinUserSvg::maxImageEdge || image.getHeight() > SkinUserSvg::maxImageEdge)
        return nullptr;
    auto drawable = std::make_unique<juce::DrawableImage>();
    drawable->setImage(image);
    if (drawable->getDrawableBounds().isEmpty())
        return nullptr;
    return drawable;
}

bool copyReplacing(const juce::File& source, const juce::File& destination, juce::String& error)
{
    if (! destination.getParentDirectory().createDirectory()
        && ! destination.getParentDirectory().isDirectory())
    {
        error = "Could not create skin_assets folder.";
        return false;
    }
    const auto incoming = destination.getSiblingFile(destination.getFileName() + ".incoming");
    incoming.deleteFile();
    if (! source.copyFileTo(incoming))
    {
        error = "Could not copy the file into preferences.";
        return false;
    }
    if (destination.existsAsFile())
    {
        const auto backup = destination.getSiblingFile(destination.getFileName() + ".bak");
        backup.deleteFile();
        if (! destination.moveFileTo(backup))
        {
            incoming.deleteFile();
            error = "Could not replace the installed file. Built-in art is unchanged.";
            return false;
        }
        if (! incoming.moveFileTo(destination))
        {
            backup.moveFileTo(destination);
            error = "Could not replace the installed file. Built-in art is unchanged.";
            return false;
        }
        backup.deleteFile();
        return true;
    }
    if (! incoming.moveFileTo(destination))
    {
        incoming.deleteFile();
        error = "Could not install the file.";
        return false;
    }
    return true;
}

}  // namespace

juce::File SkinUserSvg::directory()
{
    return UiPreferences::userDataDirectory().getChildFile("skin_assets");
}

juce::File SkinUserSvg::manufacturerMarkFile()
{
    return directory().getChildFile("manufacturer_mark.svg");
}

juce::File SkinUserSvg::decorationFile()
{
    return directory().getChildFile("decoration.svg");
}

juce::File SkinUserSvg::backgroundSvgFile()
{
    return directory().getChildFile("background.svg");
}

juce::File SkinUserSvg::backgroundPngFile()
{
    return directory().getChildFile("background.png");
}

bool SkinUserSvg::hasManufacturerMark()
{
    return manufacturerMarkFile().existsAsFile();
}

bool SkinUserSvg::hasDecoration()
{
    return decorationFile().existsAsFile();
}

bool SkinUserSvg::hasBackground()
{
    return backgroundSvgFile().existsAsFile() || backgroundPngFile().existsAsFile();
}

bool SkinUserSvg::install(const juce::File& source, const juce::File& destination, juce::String& error)
{
    if (! source.existsAsFile())
    {
        error = "File not found. Built-in art is unchanged.";
        return false;
    }
    if (! source.getFileExtension().equalsIgnoreCase(".svg"))
    {
        error = "Choose an .svg file. Built-in art is unchanged.";
        return false;
    }
    const auto bytes = source.getSize();
    if (bytes <= 0 || bytes > maxSvgBytes)
    {
        error = "SVG must be between 1 byte and 256 KiB. Built-in art is unchanged.";
        return false;
    }
    const auto text = source.loadFileAsString();
    if (! text.containsIgnoreCase("<svg")
        || parseSvgBytes(text.toRawUTF8(), static_cast<size_t>(text.getNumBytesAsUTF8())) == nullptr)
    {
        error = "SVG could not be parsed. Built-in art is unchanged.";
        return false;
    }
    return copyReplacing(source, destination, error);
}

bool SkinUserSvg::installBackground(const juce::File& source, juce::String& error)
{
    if (! source.existsAsFile())
    {
        error = "File not found. Built-in background is unchanged.";
        return false;
    }
    const auto png = source.getFileExtension().equalsIgnoreCase(".png");
    const auto svg = source.getFileExtension().equalsIgnoreCase(".svg");
    if (! png && ! svg)
    {
        error = "Choose a .png or .svg file. Built-in background is unchanged.";
        return false;
    }
    const auto bytes = source.getSize();
    if (png)
    {
        if (bytes <= 0 || bytes > maxPngBytes)
        {
            error = "PNG must be between 1 byte and 4 MiB. Built-in background is unchanged.";
            return false;
        }
        if (drawableFromPng(source) == nullptr)
        {
            error = "PNG could not be decoded, or it is larger than 8192 px on a side. Built-in background is unchanged.";
            return false;
        }
        if (! copyReplacing(source, backgroundPngFile(), error))
            return false;
        backgroundSvgFile().deleteFile();
        return true;
    }
    if (bytes <= 0 || bytes > maxSvgBytes)
    {
        error = "SVG must be between 1 byte and 256 KiB. Built-in background is unchanged.";
        return false;
    }
    const auto text = source.loadFileAsString();
    if (! text.containsIgnoreCase("<svg")
        || parseSvgBytes(text.toRawUTF8(), static_cast<size_t>(text.getNumBytesAsUTF8())) == nullptr)
    {
        error = "SVG could not be parsed. Built-in background is unchanged.";
        return false;
    }
    if (! copyReplacing(source, backgroundSvgFile(), error))
        return false;
    backgroundPngFile().deleteFile();
    return true;
}

void SkinUserSvg::clear(const juce::File& file)
{
    file.deleteFile();
}

void SkinUserSvg::clearBackground()
{
    backgroundSvgFile().deleteFile();
    backgroundPngFile().deleteFile();
}

std::unique_ptr<juce::Drawable> SkinUserSvg::loadManufacturerMark(const SkinPalette& palette)
{
    const auto file = manufacturerMarkFile();
    if (! file.existsAsFile() || file.getSize() <= 0 || file.getSize() > maxSvgBytes)
        return nullptr;
    const auto recoloured = recolourManufacturerSvg(file.loadFileAsString(), palette);
    return parseSvgBytes(recoloured.toRawUTF8(), static_cast<size_t>(recoloured.getNumBytesAsUTF8()));
}

std::unique_ptr<juce::Drawable> SkinUserSvg::loadDecoration(const SkinPalette& palette)
{
    const auto file = decorationFile();
    if (! file.existsAsFile() || file.getSize() <= 0 || file.getSize() > maxSvgBytes)
        return nullptr;
    const auto text = file.loadFileAsString();
    auto drawable = parseSvgBytes(text.toRawUTF8(), static_cast<size_t>(text.getNumBytesAsUTF8()));
    if (drawable != nullptr)
        drawable->replaceColour(juce::Colours::white, palette.braidTint);
    return drawable;
}

std::unique_ptr<juce::Drawable> SkinUserSvg::loadBackground()
{
    const auto svgFile = backgroundSvgFile();
    const auto pngFile = backgroundPngFile();
    const bool svgOk = svgFile.existsAsFile() && svgFile.getSize() > 0 && svgFile.getSize() <= maxSvgBytes;
    const bool pngOk = pngFile.existsAsFile() && pngFile.getSize() > 0 && pngFile.getSize() <= maxPngBytes;
    const bool preferPng = pngOk && (! svgOk || pngFile.getLastModificationTime() >= svgFile.getLastModificationTime());

    auto loadSvg = [&svgFile]() -> std::unique_ptr<juce::Drawable>
    {
        const auto text = svgFile.loadFileAsString();
        return parseSvgBytes(text.toRawUTF8(), static_cast<size_t>(text.getNumBytesAsUTF8()));
    };

    if (preferPng)
    {
        if (auto png = drawableFromPng(pngFile))
            return png;
        if (svgOk)
            return loadSvg();
        return nullptr;
    }
    if (svgOk)
    {
        if (auto svg = loadSvg())
            return svg;
        if (pngOk)
            return drawableFromPng(pngFile);
    }
    return nullptr;
}

SkinAssetCache::SkinAssetCache(const SwaraXtSkin& skin)
{
    load(skin);
}

void SkinAssetCache::load(const SwaraXtSkin& skin)
{
    const auto palette = SkinColourOverride::resolved();
    for (size_t i = 0; i < assets_.size(); ++i)
    {
        int size = 0;
        const auto* resourceName = skin.resourceNames[i];
        const auto* data = resourceName != nullptr && resourceName[0] != '\0'
            ? BinaryData::getNamedResource(resourceName, size) : nullptr;
        if ((i == indexOf(AssetRole::companyWordmark) || i == indexOf(AssetRole::stoneWordmark)) && data != nullptr && size > 0)
        {
            // Recolour presentation attributes before parsing to preserve the
            // artwork's independent outline, shadow and highlight opacities.
            const auto svg = juce::String::fromUTF8(data, size)
                .replace("#ffffff", hexColour(palette.primaryText))
                .replace("#f5e9fb", i == indexOf(AssetRole::companyWordmark)
                                        ? hexColour(palette.primaryText) : "#f5e9fb")
                .replace("#9b4f9e", i == indexOf(AssetRole::companyWordmark)
                                        ? hexColour(palette.primaryText) : "#9b4f9e")
                .replace("#d0d0d0", hexColour(palette.primaryText.interpolatedWith(palette.editorBackground, 0.15f)))
                .replace("#0a0608", hexColour(palette.editorBackground.darker(0.5f)));
            assets_[i] = parseSvgBytes(svg.toRawUTF8(), static_cast<size_t>(svg.getNumBytesAsUTF8()));
        }
        else
            assets_[i] = data != nullptr && size > 0
                ? parseSvgBytes(data, static_cast<size_t>(size))
                : nullptr;
    }

    manufacturerOverride_ = SkinUserSvg::loadManufacturerMark(palette);
    backgroundOverride_ = SkinUserSvg::loadBackground();

    const auto recolour = [this](juce::Colour source, juce::Colour target)
    {
        for (size_t i = 0; i < assets_.size(); ++i)
            if (i != indexOf(AssetRole::companyWordmark) && i != indexOf(AssetRole::stoneWordmark) && assets_[i] != nullptr)
                assets_[i]->replaceColour(source, target);
    };

    recolour(juce::Colour(0xffd9dadb), palette.moduleHeaderBackground);
    recolour(juce::Colour(0xff1a171b), palette.moduleHeaderText);
    if (skin.id != SkinId::midnightGold && skin.id != SkinId::neonCobalt)
    {
        recolour(juce::Colour(0xffe7ca83), palette.primaryText);
        recolour(juce::Colour(0xff8e6b29), palette.frame);
        recolour(juce::Colour(0xffe42523), palette.visualizerSecondary);
        recolour(juce::Colour(0xffffffff), palette.outline);
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
    static const DecorationDefinition greekKey1Turn {
        DecorationId::greekKey1Turn, "greek_key_1turn", "Square", "greek_key_1turn_svg"
    };
    static const DecorationDefinition greekKey4Turns {
        DecorationId::greekKey4Turns, "greek_key_4turns", "Eurocrack", "greek_key_4turns_svg"
    };
    static const DecorationDefinition romanPlait {
        DecorationId::romanPlait, "roman_plait", "Plait", "roman_plait_svg"
    };
    static const DecorationDefinition romanIntertwined {
        DecorationId::romanIntertwined, "roman_intertwined", "Intertwined", "roman_intertwined_svg"
    };
    static const DecorationDefinition romanGuilloche {
        DecorationId::romanGuilloche, "roman_guilloche", "Guilloche", "roman_guilloche_svg"
    };
    static const DecorationDefinition romanInterlace {
        DecorationId::romanInterlace, "roman_interlace", "Interlace", "roman_interlace_svg"
    };
    static const DecorationDefinition romanWicker {
        DecorationId::romanWicker, "roman_wicker", "Wicker", "roman_wicker_svg"
    };
    static const DecorationDefinition romanMeander {
        DecorationId::romanMeander, "roman_meander", "Meander", "roman_meander_svg"
    };
    static const DecorationDefinition romanKeyBorder {
        DecorationId::romanKeyBorder, "roman_key_border", "Key Border", "roman_key_border_svg"
    };
    switch (id)
    {
        case DecorationId::pcbTrace: return pcbTrace;
        case DecorationId::greekKey1Turn: return greekKey1Turn;
        case DecorationId::greekKey4Turns: return greekKey4Turns;
        case DecorationId::romanPlait: return romanPlait;
        case DecorationId::romanIntertwined: return romanIntertwined;
        case DecorationId::romanGuilloche: return romanGuilloche;
        case DecorationId::romanInterlace: return romanInterlace;
        case DecorationId::romanWicker: return romanWicker;
        case DecorationId::romanMeander: return romanMeander;
        case DecorationId::romanKeyBorder: return romanKeyBorder;
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
    usingCustom_ = false;
    if (auto custom = SkinUserSvg::loadDecoration(palette))
    {
        asset_ = std::move(custom);
        usingCustom_ = true;
        return;
    }

    const auto& definition = DecorationRegistry::get(decoration);
    int size = 0;
    const auto* data = definition.resourceName != nullptr
        ? BinaryData::getNamedResource(definition.resourceName, size) : nullptr;
    asset_ = data != nullptr && size > 0
        ? parseSvgBytes(data, static_cast<size_t>(size))
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

namespace {

struct ColourOverrideState {
    bool active = false;
    SkinPalette palette {};
};

ColourOverrideState& colourOverrideState()
{
    static ColourOverrideState state;
    return state;
}

bool parseHexColour(juce::String text, juce::Colour& colour)
{
    text = text.trim();
    if (text.startsWithChar('#'))
        text = text.substring(1);
    if (text.length() == 3)
    {
        juce::String expanded;
        for (int i = 0; i < 3; ++i)
        {
            expanded += text[i];
            expanded += text[i];
        }
        text = expanded;
    }
    if (text.length() != 6 && text.length() != 8)
        return false;
    for (int i = 0; i < text.length(); ++i)
        if (juce::CharacterFunctions::getHexDigitValue(text[i]) < 0)
            return false;
    const auto value = text.getHexValue64();
    if (text.length() == 6)
        colour = juce::Colour(static_cast<juce::uint8>((value >> 16) & 0xff),
                              static_cast<juce::uint8>((value >> 8) & 0xff),
                              static_cast<juce::uint8>(value & 0xff));
    else
        colour = juce::Colour(static_cast<juce::uint32>(value));
    return true;
}

}  // namespace

const SkinColourEntry* SkinColourCatalog::entries() noexcept
{
    static const SkinColourEntry table[count] = {
        { "editor_background", "Editor background", &SkinPalette::editorBackground },
        { "panel_background", "Panel background", &SkinPalette::panelBackground },
        { "panel_secondary", "Panel secondary", &SkinPalette::panelSecondary },
        { "screen_background", "Screen background", &SkinPalette::screenBackground },
        { "primary_text", "Primary text", &SkinPalette::primaryText },
        { "secondary_text", "Secondary text", &SkinPalette::secondaryText },
        { "muted_text", "Muted text", &SkinPalette::mutedText },
        { "module_header_background", "Module header fill", &SkinPalette::moduleHeaderBackground },
        { "module_header_text", "Module header text", &SkinPalette::moduleHeaderText },
        { "divider", "Divider", &SkinPalette::divider },
        { "braid_tint", "Decoration tint", &SkinPalette::braidTint },
        { "frame", "Frame", &SkinPalette::frame },
        { "outline", "Outline", &SkinPalette::outline },
        { "knob_body", "Knob body", &SkinPalette::knobBody },
        { "knob_inner", "Knob inner", &SkinPalette::knobInner },
        { "knob_rim", "Knob rim", &SkinPalette::knobRim },
        { "knob_indicator", "Knob indicator", &SkinPalette::knobIndicator },
        { "knob_active", "Knob active", &SkinPalette::knobActive },
        { "combo_background", "Combo background", &SkinPalette::comboBackground },
        { "combo_border", "Combo border", &SkinPalette::comboBorder },
        { "combo_text", "Combo text", &SkinPalette::comboText },
        { "combo_arrow", "Combo arrow", &SkinPalette::comboArrow },
        { "display_background", "Display background", &SkinPalette::displayBackground },
        { "display_text", "Display text", &SkinPalette::displayText },
        { "display_border", "Display border", &SkinPalette::displayBorder },
        { "visualizer_primary", "Visualizer primary", &SkinPalette::visualizerPrimary },
        { "visualizer_secondary", "Visualizer secondary", &SkinPalette::visualizerSecondary },
        { "visualizer_grid", "Visualizer grid", &SkinPalette::visualizerGrid },
        { "matrix_guide", "Matrix guide", &SkinPalette::matrixGuide },
        { "matrix_text", "Matrix text", &SkinPalette::matrixText },
        { "matrix_amount_positive", "Matrix + amount", &SkinPalette::matrixAmountPositive },
        { "matrix_amount_negative", "Matrix - amount", &SkinPalette::matrixAmountNegative },
        { "focus", "Focus", &SkinPalette::focus },
        { "hover", "Hover", &SkinPalette::hover },
        { "disabled", "Disabled", &SkinPalette::disabled },
    };
    return table;
}

juce::String SkinColourCatalog::toText(const SkinPalette& palette)
{
    juce::String text;
    text << "; SWARA XT interface colours\n";
    text << "; name = #RRGGBB\n\n";
    for (int i = 0; i < count; ++i)
    {
        const auto& entry = entries()[static_cast<size_t>(i)];
        const auto colour = palette.*(entry.member);
        text << entry.id << " = #" << colour.toDisplayString(false) << "\n";
    }
    return text;
}

int SkinColourCatalog::applyText(const juce::String& text, SkinPalette& palette)
{
    int applied = 0;
    for (auto line : juce::StringArray::fromLines(text))
    {
        line = line.trim();
        if (line.isEmpty() || line.startsWithChar(';') || line.startsWith("//"))
            continue;
        if (line.startsWithChar('#') && (line.length() == 1 || juce::CharacterFunctions::isWhitespace(line[1])))
            continue;
        const auto split = line.indexOfChar('=');
        if (split <= 0)
            continue;
        juce::Colour colour;
        if (! parseHexColour(line.substring(split + 1), colour))
            continue;
        const auto key = line.substring(0, split).trim().toLowerCase();
        for (int i = 0; i < count; ++i)
        {
            const auto& entry = entries()[static_cast<size_t>(i)];
            if (key == entry.id)
            {
                palette.*(entry.member) = colour;
                ++applied;
                break;
            }
        }
    }
    return applied;
}

bool SkinColourCatalog::saveText(const juce::File& destination, const SkinPalette& palette, juce::String& error)
{
    if (! destination.getParentDirectory().createDirectory()
        && ! destination.getParentDirectory().isDirectory())
    {
        error = "Could not create the destination folder.";
        return false;
    }
    if (! destination.replaceWithText(toText(palette)))
    {
        error = "Could not write the colour file.";
        return false;
    }
    return true;
}

juce::File SkinColourOverride::file()
{
    return SkinUserSvg::directory().getChildFile("colours.txt");
}

bool SkinColourOverride::hasFile()
{
    return file().existsAsFile();
}

bool SkinColourOverride::active()
{
    return colourOverrideState().active;
}

const SkinPalette& SkinColourOverride::resolved()
{
    if (colourOverrideState().active)
        return colourOverrideState().palette;
    return SkinRegistry::active().palette;
}

void SkinColourOverride::refresh()
{
    auto& state = colourOverrideState();
    state.active = false;
    state.palette = SkinRegistry::active().palette;
    const auto installed = file();
    if (! installed.existsAsFile() || installed.getSize() <= 0 || installed.getSize() > maxBytes)
        return;
    const auto applied = SkinColourCatalog::applyText(installed.loadFileAsString(), state.palette);
    state.active = applied > 0;
}

bool SkinColourOverride::install(const juce::File& source, juce::String& error)
{
    if (! source.existsAsFile())
    {
        error = "File not found. Built-in colours are unchanged.";
        return false;
    }
    if (! source.getFileExtension().equalsIgnoreCase(".txt"))
    {
        error = "Choose a .txt file. Built-in colours are unchanged.";
        return false;
    }
    const auto bytes = source.getSize();
    if (bytes <= 0 || bytes > maxBytes)
    {
        error = "Colour file must be between 1 byte and 64 KiB. Built-in colours are unchanged.";
        return false;
    }
    auto trial = SkinRegistry::active().palette;
    if (SkinColourCatalog::applyText(source.loadFileAsString(), trial) <= 0)
    {
        error = "No valid colour lines. Use name = #RRGGBB. Built-in colours are unchanged.";
        return false;
    }
    return copyReplacing(source, file(), error);
}

void SkinColourOverride::clear()
{
    file().deleteFile();
    refresh();
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

juce::File UiPreferences::userDataDirectory()
{
    return open()->getFile().getParentDirectory();
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
    for (const auto id : DecorationRegistry::all)
        if (value == DecorationRegistry::get(id).stableId) return id;
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

ManufacturerMark UiPreferences::loadManufacturerMark()
{
    const auto properties = open();
    return properties->getValue("manufacturer_mark") == "stone"
        ? ManufacturerMark::stone : ManufacturerMarkDefinition::defaultMark;
}

void UiPreferences::saveManufacturerMark(ManufacturerMark mark)
{
    auto properties = open();
    properties->setValue("manufacturer_mark", ManufacturerMarkDefinition::stableId(mark));
    properties->saveIfNeeded();
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
    const auto value = properties->getValue("filter_quality", "normal");
    if (value == "eco") return swaraxt::FilterQuality::eco;
    if (value == "high") return swaraxt::FilterQuality::high;
    return swaraxt::FilterQuality::normal;
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
