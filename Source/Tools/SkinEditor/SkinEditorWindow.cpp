// Copyright 2026 MontroneDSP.
// SPDX-License-Identifier: GPL-3.0-or-later

#include "Tools/SkinEditor/SkinEditorWindow.h"

namespace swaraxt::skin_editor {

using namespace swaraxt::ui;

MainComponent::MainComponent()
{
    addAndMakeVisible(baseSkinCombo_);
    baseSkinCombo_.addItem("Pastel", 1);
    baseSkinCombo_.addItem("Midnight Gold", 2);
    baseSkinCombo_.addItem("Neon Cobalt", 3);
    baseSkinCombo_.addItem("Jungle", 4);
    baseSkinCombo_.addItem("Rossocorsa", 5);
    baseSkinCombo_.setSelectedId(1, juce::dontSendNotification);
    baseSkinCombo_.addListener(this);

    colourList_.setRowHeight(26);
    colourList_.setColour(juce::ListBox::backgroundColourId, juce::Colour(0xff12141a));
    colourList_.setColour(juce::ListBox::outlineColourId, juce::Colour(0xff3a3f4a));
    addAndMakeVisible(colourList_);

    selector_.setCurrentColour(juce::Colours::white, juce::dontSendNotification);
    selector_.addChangeListener(this);
    addAndMakeVisible(selector_);
    addAndMakeVisible(preview_);

    for (auto* button : { &loadButton_, &saveButton_, &installButton_, &resetButton_ })
    {
        addAndMakeVisible(button);
        button->addListener(this);
    }

    status_.setJustificationType(juce::Justification::centredLeft);
    status_.setColour(juce::Label::textColourId, juce::Colours::lightgrey);
    addAndMakeVisible(status_);

    setSize(1180, 720);
    loadBaseSkin(SkinId::pastel);
    colourList_.selectRow(0);
}

MainComponent::~MainComponent()
{
    selector_.removeChangeListener(this);
    baseSkinCombo_.removeListener(this);
    for (auto* button : { &loadButton_, &saveButton_, &installButton_, &resetButton_ })
        button->removeListener(this);
}

void MainComponent::resized()
{
    auto area = getLocalBounds().reduced(10);
    auto top = area.removeFromTop(34);
    baseSkinCombo_.setBounds(top.removeFromLeft(220));
    top.removeFromLeft(8);
    loadButton_.setBounds(top.removeFromLeft(90));
    top.removeFromLeft(6);
    saveButton_.setBounds(top.removeFromLeft(100));
    top.removeFromLeft(6);
    installButton_.setBounds(top.removeFromLeft(140));
    top.removeFromLeft(6);
    resetButton_.setBounds(top.removeFromLeft(120));

    auto bottom = area.removeFromBottom(28);
    status_.setBounds(bottom);

    area.removeFromTop(8);
    auto left = area.removeFromLeft(280);
    colourList_.setBounds(left);
    area.removeFromLeft(10);
    auto right = area.removeFromRight(320);
    selector_.setBounds(right);
    area.removeFromRight(10);
    preview_.setBounds(area);
}

int MainComponent::getNumRows()
{
    return SkinColourCatalog::count;
}

void MainComponent::paintListBoxItem(int row, juce::Graphics& g, int width, int height, bool selected)
{
    if (! juce::isPositiveAndBelow(row, SkinColourCatalog::count))
        return;
    const auto& entry = SkinColourCatalog::entries()[static_cast<size_t>(row)];
    const auto colour = draft_.*(entry.member);

    if (selected)
        g.fillAll(juce::Colour(0xff2a3344));
    else if (row % 2 == 0)
        g.fillAll(juce::Colour(0xff161920));

    g.setColour(colour);
    g.fillRoundedRectangle(6.0f, 5.0f, 28.0f, static_cast<float>(height - 10), 3.0f);
    g.setColour(juce::Colours::white.withAlpha(0.35f));
    g.drawRoundedRectangle(6.0f, 5.0f, 28.0f, static_cast<float>(height - 10), 3.0f, 1.0f);

    g.setColour(selected ? juce::Colours::white : juce::Colour(0xffd7dbe3));
    g.setFont(juce::FontOptions(13.0f));
    g.drawText(entry.label, 42, 0, width - 50, height, juce::Justification::centredLeft);
}

void MainComponent::selectedRowsChanged(int lastRowSelected)
{
    selectedRow_ = lastRowSelected;
    syncSelectorFromPalette();
}

void MainComponent::changeListenerCallback(juce::ChangeBroadcaster* source)
{
    if (source == &selector_)
        applySelectedColour(selector_.getCurrentColour());
}

void MainComponent::comboBoxChanged(juce::ComboBox* combo)
{
    if (combo != &baseSkinCombo_)
        return;
    switch (combo->getSelectedId())
    {
        case 2: loadBaseSkin(SkinId::midnightGold); break;
        case 3: loadBaseSkin(SkinId::neonCobalt); break;
        case 4: loadBaseSkin(SkinId::jungle); break;
        case 5: loadBaseSkin(SkinId::rossocorsa); break;
        default: loadBaseSkin(SkinId::pastel); break;
    }
}

void MainComponent::buttonClicked(juce::Button* button)
{
    if (button == &resetButton_)
    {
        loadBaseSkin(baseSkin_);
        setStatus("Reset to built-in " + juce::String(SkinRegistry::get(baseSkin_).displayName) + ".");
        return;
    }

    if (button == &loadButton_)
    {
        chooser_ = std::make_unique<juce::FileChooser>("Load colour text", juce::File(), "*.txt");
        constexpr auto browserFlags = juce::FileBrowserComponent::openMode
                                    | juce::FileBrowserComponent::canSelectFiles;
        chooser_->launchAsync(browserFlags, [this](const juce::FileChooser& chooser) {
            const auto results = chooser.getResults();
            if (results.isEmpty())
                return;
            auto next = draft_;
            const auto applied = SkinColourCatalog::applyText(results.getReference(0).loadFileAsString(), next);
            if (applied <= 0)
            {
                showError("No valid colour lines in that file.");
                return;
            }
            draft_ = next;
            preview_.setPalette(draft_);
            colourList_.updateContent();
            colourList_.repaint();
            syncSelectorFromPalette();
            setStatus("Loaded " + juce::String(applied) + " colours from "
                      + results.getReference(0).getFileName());
        });
        return;
    }

    if (button == &saveButton_)
    {
        chooser_ = std::make_unique<juce::FileChooser>("Save colour text",
                                                       juce::File().getChildFile("colours.txt"),
                                                       "*.txt");
        constexpr auto browserFlags = juce::FileBrowserComponent::saveMode
                                    | juce::FileBrowserComponent::canSelectFiles
                                    | juce::FileBrowserComponent::warnAboutOverwriting;
        chooser_->launchAsync(browserFlags, [this](const juce::FileChooser& chooser) {
            auto file = chooser.getResult();
            if (file == juce::File{})
                return;
            if (! file.hasFileExtension(".txt"))
                file = file.withFileExtension(".txt");
            juce::String error;
            if (! SkinColourCatalog::saveText(file, draft_, error))
            {
                showError(error);
                return;
            }
            setStatus("Saved " + file.getFullPathName());
        });
        return;
    }

    if (button == &installButton_)
    {
        juce::String error;
        if (! SkinColourCatalog::saveText(SkinColourOverride::file(), draft_, error))
        {
            showError(error);
            return;
        }
        SkinColourOverride::refresh();
        setStatus("Installed for the plugin: " + SkinColourOverride::file().getFullPathName());
    }
}

void MainComponent::loadBaseSkin(SkinId id)
{
    baseSkin_ = id;
    SkinRegistry::setActive(id);
    draft_ = SkinRegistry::get(id).palette;
    preview_.setPalette(draft_);
    colourList_.updateContent();
    colourList_.repaint();
    syncSelectorFromPalette();
    setStatus("Editing on top of " + juce::String(SkinRegistry::get(id).displayName)
              + ". Preview updates live.");
}

void MainComponent::syncSelectorFromPalette()
{
    if (! juce::isPositiveAndBelow(selectedRow_, SkinColourCatalog::count))
        return;
    const auto& entry = SkinColourCatalog::entries()[static_cast<size_t>(selectedRow_)];
    selector_.setCurrentColour(draft_.*(entry.member), juce::dontSendNotification);
}

void MainComponent::applySelectedColour(juce::Colour colour)
{
    if (! juce::isPositiveAndBelow(selectedRow_, SkinColourCatalog::count))
        return;
    const auto& entry = SkinColourCatalog::entries()[static_cast<size_t>(selectedRow_)];
    draft_.*(entry.member) = colour.withAlpha(1.0f);
    preview_.setPalette(draft_);
    colourList_.repaintRow(selectedRow_);
}

void MainComponent::showError(const juce::String& message)
{
    juce::AlertWindow::showMessageBoxAsync(juce::AlertWindow::WarningIcon, "Skin Editor", message);
}

void MainComponent::setStatus(const juce::String& message)
{
    status_.setText(message, juce::dontSendNotification);
}

MainWindow::MainWindow()
    : DocumentWindow("SWARA XT Skin Editor",
                     juce::Desktop::getInstance().getDefaultLookAndFeel()
                         .findColour(juce::ResizableWindow::backgroundColourId),
                     DocumentWindow::allButtons)
{
    setUsingNativeTitleBar(true);
    setContentOwned(new MainComponent(), true);
    setResizable(true, true);
    centreWithSize(getWidth(), getHeight());
    setVisible(true);
}

void MainWindow::closeButtonPressed()
{
    juce::JUCEApplication::getInstance()->systemRequestedQuit();
}

}  // namespace swaraxt::skin_editor
