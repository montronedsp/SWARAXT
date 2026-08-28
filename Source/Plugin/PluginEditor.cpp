// Copyright 2026 MontroneDSP.
// SPDX-License-Identifier: GPL-3.0-or-later

#include "Plugin/PluginEditor.h"

#include "Ui/SwaraXtUiPalette.h"

namespace {

const juce::Rectangle<float> companyWordmarkBounds { 474.0f, 20.0f, 165.0f, 23.5f };
const juce::Rectangle<float> presetFrameBounds { 414.0f, 58.0f, 284.0f, 52.0f };

constexpr int presetSelectorX = 447;
constexpr int presetSelectorY = 64;
constexpr int presetSelectorWidth = 218;
constexpr int presetControlHeight = 25;
constexpr int presetArrowWidth = 24;
constexpr int presetControlGap = 4;

void paintMaterialBackground(juce::Graphics& g, juce::Rectangle<float> bounds,
                             swaraxt::ui::SkinId skin)
{
    using namespace swaraxt::ui;
    if (skin == SkinId::midnightGold)
    {
        const auto base = Palette::background();
        const auto top = base.brighter(0.055f);
        const auto bottom = base.darker(0.045f);
        g.setGradientFill({ top, bounds.getCentreX(), bounds.getY(),
                            bottom, bounds.getCentreX(), bounds.getBottom(), false });
        g.fillRect(bounds);

        juce::ColourGradient light(
            juce::Colour(0x24251c38), bounds.getCentreX(), bounds.getY() + 34.0f,
            juce::Colours::transparentBlack, bounds.getCentreX(), bounds.getBottom(), true);
        light.addColour(0.55, juce::Colour(0x0f171226));
        g.setGradientFill(light);
        g.fillRect(bounds);
        return;
    }

    if (skin == SkinId::neonCobalt)
        return;

    const auto top = Palette::background().brighter(skin == SkinId::pastel ? 0.16f : 0.05f);
    const auto bottom = skin == SkinId::pastel
        ? Palette::panel().darker(0.08f)
        : Palette::panelDeep().darker(0.16f);
    g.setGradientFill({ top, bounds.getTopLeft(), bottom, bounds.getBottomRight(), false });
    g.fillRect(bounds);

    g.setColour(Palette::panel().withAlpha(skin == SkinId::pastel ? 0.15f : 0.10f));
    for (int i = 0; i < 7; ++i)
    {
        const auto x = -80.0f + static_cast<float>(i) * 205.0f;
        const auto y = 35.0f + static_cast<float>((i * 73) % 260);
        g.fillEllipse({ x, y, 310.0f, 92.0f });
    }
}

class AboutPanel final : public juce::Component,
                         private juce::Button::Listener {
 public:
    explicit AboutPanel(const swaraxt::ui::SwaraXtLookAndFeel& lookAndFeel)
        : lookAndFeel_(lookAndFeel),
          email_("Support: support@montronedsp.com",
                 juce::URL("mailto:support@montronedsp.com"))
    {
        addAndMakeVisible(email_);
        addAndMakeVisible(close_);
        close_.addListener(this);
        setSize(360, 270);
    }

    ~AboutPanel() override { close_.removeListener(this); }

    void setCloseCallback(std::function<void()> callback) { closeCallback_ = std::move(callback); }

    void paint(juce::Graphics& g) override
    {
        using namespace swaraxt::ui;
        const auto bounds = getLocalBounds().toFloat();
        g.setColour(Palette::panelDeep());
        g.fillRoundedRectangle(bounds, 8.0f);
        g.setColour(Palette::line());
        g.drawRoundedRectangle(bounds.reduced(0.75f), 8.0f, 1.5f);

        g.setColour(Palette::cream());
        g.setFont(lookAndFeel_.regularFont(21.0f));
        g.drawText("SWARA XT", 20, 14, 320, 28, juce::Justification::centred);

        g.setColour(Palette::mutedText());
        g.setFont(lookAndFeel_.regularFont(12.5f));
        g.drawFittedText("MontroneDSP\nAuthor: Andrea Montrone\n\u00a9 2026 MontroneDSP\n\nLicensed under the GNU GPL v3 or later.\n\nContains software derived in part from\nMutable Instruments Shruthi firmware.\n\nVersion: " SWARAXT_VERSION_STRING,
                         22, 45, 316, 174, juce::Justification::centred, 10);
    }

    void resized() override
    {
        email_.setBounds(52, 218, 256, 22);
        close_.setBounds(150, 243, 60, 22);
    }

    void lookAndFeelChanged() override
    {
        using namespace swaraxt::ui;
        email_.setColour(juce::HyperlinkButton::textColourId, Palette::accent());
        close_.setColour(juce::TextButton::buttonColourId, Palette::panel());
        close_.setColour(juce::TextButton::textColourOffId, Palette::cream());
        repaint();
    }

 private:
    void buttonClicked(juce::Button*) override
    {
        if (closeCallback_)
            closeCallback_();
    }

    const swaraxt::ui::SwaraXtLookAndFeel& lookAndFeel_;
    juce::HyperlinkButton email_;
    juce::TextButton close_ { "Close" };
    std::function<void()> closeCallback_;
};

}  // namespace

SwaraXtAudioProcessorEditor::SwaraXtAudioProcessorEditor(SwaraXtAudioProcessor& processor)
    : AudioProcessorEditor(&processor),
      processor_(processor)
{
    setLookAndFeel(&lookAndFeel_);
    setResizable(false, false);
    skinId_ = swaraxt::ui::UiPreferences::loadSkin();
    decorationId_ = swaraxt::ui::UiPreferences::loadDecoration();
    swaraxt::ui::SkinRegistry::setActive(skinId_);
    lookAndFeel_.applySkin();
    guiSize_ = swaraxt::ui::UiPreferences::loadGuiSize();
    filterQuality_ = swaraxt::ui::UiPreferences::loadFilterQuality();
    processor_.setFilterQuality(filterQuality_);
    addAndMakeVisible(designSurface_);
    designSurface_.addMouseListener(this, true);
    presetSelector_.setJustificationType(juce::Justification::centred);
    presetSelector_.setScrollWheelEnabled(false);
    presetSelector_.addListener(this);
    designSurface_.addAndMakeVisible(presetSelector_);

    for (auto* button : { &prevPresetButton_, &nextPresetButton_, &savePresetButton_ })
    {
        designSurface_.addAndMakeVisible(button);
        button->addListener(this);
        button->setClickingTogglesState(false);
    }
    prevPresetButton_.getProperties().set("swaraxtPresetNavigation", true);
    nextPresetButton_.getProperties().set("swaraxtPresetNavigation", true);
    savePresetButton_.getProperties().set("swaraxtSecondaryAction", true);

    designSurface_.addAndMakeVisible(aboutBrandButton_);
    aboutBrandButton_.addListener(this);
    aboutBrandButton_.setTooltip("About Swara XT");

    designSurface_.addAndMakeVisible(mainPanel_);
    mainPanel_.toBack();
    mainPanel_.attach(processor_);
    applySkin(skinId_, false);
    refreshPresetList();
    applyGuiSize(guiSize_, false);
    startTimerHz(4);
}

SwaraXtAudioProcessorEditor::~SwaraXtAudioProcessorEditor()
{
    stopTimer();
    designSurface_.removeMouseListener(this);
    presetSelector_.removeListener(this);
    for (auto* button : { &prevPresetButton_, &nextPresetButton_, &savePresetButton_ })
        button->removeListener(this);
    aboutBrandButton_.removeListener(this);
    setLookAndFeel(nullptr);
}

void SwaraXtAudioProcessorEditor::paint(juce::Graphics& g)
{
    g.fillAll(swaraxt::ui::Palette::background());
}

void SwaraXtAudioProcessorEditor::resized()
{
    const float scale = swaraxt::ui::GuiGeometry::scaleFor(guiSize_);
    designSurface_.setTransform(juce::AffineTransform::scale(scale));
    designSurface_.setBounds(0, 0, swaraxt::ui::GuiGeometry::designWidth,
                             swaraxt::ui::GuiGeometry::designHeight);

    presetSelector_.setBounds(presetSelectorX, presetSelectorY,
                              presetSelectorWidth, presetControlHeight);
    prevPresetButton_.setBounds(presetSelectorX - presetControlGap - presetArrowWidth,
                                presetSelectorY, presetArrowWidth, presetControlHeight);
    nextPresetButton_.setBounds(presetSelectorX + presetSelectorWidth + presetControlGap,
                                presetSelectorY, presetArrowWidth, presetControlHeight);
    savePresetButton_.setBounds(presetSelectorX + presetSelectorWidth - 40,
                                90, 40, 18);
    aboutBrandButton_.setBounds(companyWordmarkBounds.toNearestInt());

    mainPanel_.setBounds(0, 0, swaraxt::ui::GuiGeometry::designWidth,
                         swaraxt::ui::GuiGeometry::designHeight);
    mainPanel_.reflow();
}

void SwaraXtAudioProcessorEditor::paintDesignSurface(juce::Graphics& g)
{
    using namespace swaraxt::ui;
    const auto bounds = designSurface_.getLocalBounds().toFloat();
    g.fillAll(Palette::background());
    paintMaterialBackground(g, bounds, skinId_);
    if (skinId_ != SkinId::midnightGold)
    {
        if (const auto* backdrop = assets_.get(AssetRole::backdrop))
            backdrop->drawWithin(g, bounds, juce::RectanglePlacement::stretchToFit, 1.0f);
    }
    g.setColour(Palette::line());
    g.drawRect(bounds.reduced(0.5f), 1.0f);

    const auto* separator = decorationAssets_.get();
    if (separator == nullptr)
        separator = assets_.get(AssetRole::braid);
    if (separator != nullptr)
    {
        const auto sourceBounds = separator->getDrawableBounds();
        const float width = sourceBounds.getHeight() > 0.0f
            ? 513.0f * sourceBounds.getWidth() / sourceBounds.getHeight() : 9.4f;
        for (const float centreX : { 362.0f, 750.0f })
            separator->drawWithin(g, { centreX - width * 0.5f, 4.0f, width, 513.0f },
                                  juce::RectanglePlacement::centred, 1.0f);
    }
    if (const auto* company = assets_.get(AssetRole::companyWordmark))
        company->drawWithin(g, companyWordmarkBounds,
                            juce::RectanglePlacement::centred, 1.0f);

    g.setColour(Palette::skin().displayBackground);
    g.fillRoundedRectangle(presetFrameBounds, 8.0f);
    g.setColour(Palette::skin().displayBorder);
    g.drawRoundedRectangle(presetFrameBounds, 8.0f, 1.2f);

    if (const auto* lockup = assets_.get(AssetRole::productLockup))
        lockup->drawWithin(g, productLockupBoundsForTests(),
                           juce::RectanglePlacement::stretchToFit, 1.0f);
}

void SwaraXtAudioProcessorEditor::mouseDown(const juce::MouseEvent& event)
{
    if (event.mods.isPopupMenu())
        showContextMenu();
}

void SwaraXtAudioProcessorEditor::showContextMenu()
{
    using namespace swaraxt::ui;
    juce::PopupMenu skinMenu;
    skinMenu.addItem(102, "Pastel", true, skinId_ == SkinId::pastel);
    skinMenu.addItem(100, "Midnight Gold", true, skinId_ == SkinId::midnightGold);
    skinMenu.addItem(101, "Neon Cobalt", true, skinId_ == SkinId::neonCobalt);
    skinMenu.addItem(103, "Jungle", true, skinId_ == SkinId::jungle);
    skinMenu.addItem(104, "Rossocorsa", true, skinId_ == SkinId::rossocorsa);

    juce::PopupMenu decorationMenu;
    decorationMenu.addItem(150, "Legacy", true, decorationId_ == DecorationId::legacy);
    decorationMenu.addItem(151, "PCB Trace", true, decorationId_ == DecorationId::pcbTrace);

    juce::PopupMenu sizeMenu;
    sizeMenu.addItem(201, "Small", true, guiSize_ == GuiSize::small);
    sizeMenu.addItem(202, "Medium", true, guiSize_ == GuiSize::medium);
    sizeMenu.addItem(203, "Large", true, guiSize_ == GuiSize::large);

    juce::PopupMenu qualityMenu;
    qualityMenu.addItem(301, "High", true, filterQuality_ == swaraxt::FilterQuality::high);
    qualityMenu.addItem(302, "Normal", true, filterQuality_ == swaraxt::FilterQuality::normal);
    qualityMenu.addItem(303, "Eco", true, filterQuality_ == swaraxt::FilterQuality::eco);

    juce::PopupMenu menu;
    menu.addSubMenu("Skin", skinMenu);
    menu.addSubMenu("Decoration", decorationMenu);
    menu.addSubMenu("GUI Size", sizeMenu);
    menu.addSubMenu("Filter Quality", qualityMenu);
    juce::Component::SafePointer<SwaraXtAudioProcessorEditor> safeThis(this);
    menu.showMenuAsync(juce::PopupMenu::Options().withTargetComponent(this),
                       [safeThis](int result) {
        if (safeThis == nullptr) return;
        if (result == 100) safeThis->applySkin(SkinId::midnightGold, true);
        if (result == 101) safeThis->applySkin(SkinId::neonCobalt, true);
        if (result == 102) safeThis->applySkin(SkinId::pastel, true);
        if (result == 103) safeThis->applySkin(SkinId::jungle, true);
        if (result == 104) safeThis->applySkin(SkinId::rossocorsa, true);
        if (result == 150) safeThis->applyDecoration(DecorationId::legacy, true);
        if (result == 151) safeThis->applyDecoration(DecorationId::pcbTrace, true);
        if (result == 201) safeThis->applyGuiSize(GuiSize::small, true);
        if (result == 202) safeThis->applyGuiSize(GuiSize::medium, true);
        if (result == 203) safeThis->applyGuiSize(GuiSize::large, true);
        if (result == 301) safeThis->applyFilterQuality(swaraxt::FilterQuality::high, true);
        if (result == 302) safeThis->applyFilterQuality(swaraxt::FilterQuality::normal, true);
        if (result == 303) safeThis->applyFilterQuality(swaraxt::FilterQuality::eco, true);
    });
}

void SwaraXtAudioProcessorEditor::applySkin(swaraxt::ui::SkinId skin, bool persist)
{
    skinId_ = skin;
    swaraxt::ui::SkinRegistry::setActive(skin);
    assets_.load(swaraxt::ui::SkinRegistry::get(skin));
    decorationAssets_.load(decorationId_, swaraxt::ui::SkinRegistry::get(skin).palette);
    lookAndFeel_.applySkin();
    prevPresetButton_.setColour(juce::TextButton::textColourOffId,
                                swaraxt::ui::Palette::skin().displayText);
    nextPresetButton_.setColour(juce::TextButton::textColourOffId,
                                swaraxt::ui::Palette::skin().displayText);
    savePresetButton_.setColour(juce::TextButton::textColourOffId,
                                swaraxt::ui::Palette::skin().displayText.withAlpha(0.78f));
    mainPanel_.setSkinAssets(assets_);
    sendLookAndFeelChange();
    repaint();
    if (persist)
        swaraxt::ui::UiPreferences::save(skinId_, decorationId_, guiSize_);
}

void SwaraXtAudioProcessorEditor::applyDecoration(swaraxt::ui::DecorationId decoration,
                                                  bool persist)
{
    decorationId_ = decoration;
    decorationAssets_.load(decorationId_, swaraxt::ui::SkinRegistry::active().palette);
    repaint();
    if (persist)
        swaraxt::ui::UiPreferences::save(skinId_, decorationId_, guiSize_);
}

void SwaraXtAudioProcessorEditor::applyGuiSize(swaraxt::ui::GuiSize size, bool persist)
{
    guiSize_ = size;
    const auto bounds = swaraxt::ui::GuiGeometry::editorBounds(size);
    setSize(bounds.getWidth(), bounds.getHeight());
    if (persist)
        swaraxt::ui::UiPreferences::save(skinId_, decorationId_, guiSize_);
}

void SwaraXtAudioProcessorEditor::applyFilterQuality(swaraxt::FilterQuality quality, bool persist)
{
    filterQuality_ = quality;
    processor_.setFilterQuality(quality);
    if (persist)
        swaraxt::ui::UiPreferences::saveFilterQuality(quality);
}

void SwaraXtAudioProcessorEditor::showAboutPopup()
{
    auto panel = std::make_unique<AboutPanel>(lookAndFeel_);
    auto* panelPointer = panel.get();
    auto& callout = juce::CallOutBox::launchAsynchronously(
        std::move(panel), aboutBrandButton_.getBounds(), &designSurface_);
    juce::Component::SafePointer<juce::CallOutBox> safeCallout(&callout);
    panelPointer->setCloseCallback([safeCallout] {
        if (safeCallout != nullptr)
            safeCallout->dismiss();
    });
}

void SwaraXtAudioProcessorEditor::setGuiSizeForTests(swaraxt::ui::GuiSize size)
{
    applyGuiSize(size, false);
}

void SwaraXtAudioProcessorEditor::setSkinForTests(swaraxt::ui::SkinId skin)
{
    applySkin(skin, false);
}

void SwaraXtAudioProcessorEditor::setDecorationForTests(
    swaraxt::ui::DecorationId decoration)
{
    applyDecoration(decoration, false);
}

void SwaraXtAudioProcessorEditor::refreshPresetList()
{
    const auto currentName = processor_.currentPresetName();
    const bool currentIsUser = processor_.currentPresetIsUser();
    presetEntries_ = processor_.getPresetEntries();

    // Detach while rebuilding so population cannot load a preset.
    const juce::ScopedValueSetter<bool> guard(refreshingPresets_, true);
    presetSelector_.removeListener(this);
    presetSelector_.clear(juce::dontSendNotification);
    presetSelector_.addSectionHeading("FACTORY");
    int firstUser = -1;
    for (size_t index = 0; index < presetEntries_.size(); ++index)
    {
        if (! presetEntries_[index].isFactory && firstUser < 0)
        {
            firstUser = static_cast<int>(index);
            presetSelector_.addSeparator();
            presetSelector_.addSectionHeading("USER");
        }
        presetSelector_.addItem(presetEntries_[index].name, static_cast<int>(index) + 1);
    }

    int selected = 0;
    for (size_t index = 0; index < presetEntries_.size(); ++index)
        if (presetEntries_[index].name == currentName
            && presetEntries_[index].isFactory != currentIsUser)
            selected = static_cast<int>(index) + 1;

    if (selected > 0)
        presetSelector_.setSelectedId(selected, juce::dontSendNotification);
    else
        // Keep the authoritative processor name visible without selecting another entry.
        presetSelector_.setText(currentName, juce::dontSendNotification);

    presetSelector_.addListener(this);
}

void SwaraXtAudioProcessorEditor::comboBoxChanged(juce::ComboBox* combo)
{
    if (refreshingPresets_ || combo != &presetSelector_)
        return;
    const int index = presetSelector_.getSelectedId() - 1;
    if (index < 0 || index >= static_cast<int>(presetEntries_.size()))
        return;

    const auto& entry = presetEntries_[static_cast<size_t>(index)];
    if (entry.name == processor_.currentPresetName()
        && entry.isFactory == ! processor_.currentPresetIsUser())
        return;

    juce::String error;
    if (! processor_.loadPresetEntry(entry, error))
        showPresetError(error);
}

void SwaraXtAudioProcessorEditor::buttonClicked(juce::Button* button)
{
    if (button == &aboutBrandButton_)
    {
        showAboutPopup();
        return;
    }

    if (button == &savePresetButton_)
    {
        promptForPresetName();
        return;
    }

    if (presetEntries_.empty())
        return;
    int index = presetSelector_.getSelectedId() - 1;
    if (button == &prevPresetButton_)
        index = (index + static_cast<int>(presetEntries_.size()) - 1)
            % static_cast<int>(presetEntries_.size());
    else if (button == &nextPresetButton_)
        index = (index + 1) % static_cast<int>(presetEntries_.size());
    else
        return;
    presetSelector_.setSelectedId(index + 1, juce::sendNotificationSync);
}

void SwaraXtAudioProcessorEditor::promptForPresetName()
{
    auto* window = new juce::AlertWindow("Save user preset",
                                          "Enter a name for the complete Swara XT state.",
                                          juce::MessageBoxIconType::NoIcon);
    window->addTextEditor("name", processor_.currentPresetName(), "Preset name");
    window->addButton("Save", 1, juce::KeyPress(juce::KeyPress::returnKey));
    window->addButton("Cancel", 0, juce::KeyPress(juce::KeyPress::escapeKey));
    juce::Component::SafePointer<SwaraXtAudioProcessorEditor> safeThis(this);
    window->enterModalState(true,
        juce::ModalCallbackFunction::create([safeThis, window](int result) {
            if (result == 1 && safeThis != nullptr)
                safeThis->savePresetWithName(window->getTextEditorContents("name"), false);
        }), true);
}

void SwaraXtAudioProcessorEditor::savePresetWithName(const juce::String& name, bool overwrite)
{
    juce::String error;
    if (processor_.saveUserPreset(name, overwrite, error))
    {
        refreshPresetList();
        return;
    }

    if (! overwrite && error.containsIgnoreCase("already exists"))
    {
        juce::Component::SafePointer<SwaraXtAudioProcessorEditor> safeThis(this);
        juce::AlertWindow::showOkCancelBox(juce::MessageBoxIconType::QuestionIcon,
            "Replace user preset?", error + " Replace it?", "Replace", "Cancel", this,
            juce::ModalCallbackFunction::create([safeThis, name](int result) {
                if (result != 0 && safeThis != nullptr)
                    safeThis->savePresetWithName(name, true);
            }));
        return;
    }
    showPresetError(error);
}

void SwaraXtAudioProcessorEditor::showPresetError(const juce::String& message)
{
    juce::AlertWindow::showMessageBoxAsync(juce::MessageBoxIconType::WarningIcon,
                                           "Preset error", message);
}

void SwaraXtAudioProcessorEditor::timerCallback()
{
    if (presetSelector_.getText() != processor_.currentPresetName())
        refreshPresetList();
}

void SwaraXtAudioProcessorEditor::setModuleViewsForTests(bool modulation, bool sequencer)
{
    mainPanel_.setLocalViews(modulation, sequencer);
}

void SwaraXtAudioProcessorEditor::setSequencerHostSyncForTests(bool enabled)
{
    mainPanel_.setSequencerHostSyncForTests(enabled);
}

juce::Rectangle<float> SwaraXtAudioProcessorEditor::productLockupBoundsForTests() const
{
    return swaraxt::ui::ProductLockupGeometry::editorPlacement();
}
