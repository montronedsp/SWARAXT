// Copyright 2026 MontroneDSP.
// SPDX-License-Identifier: GPL-3.0-or-later

#include <JuceHeader.h>

#include "Tools/SkinEditor/SkinEditorWindow.h"

namespace {

class SkinEditorApplication final : public juce::JUCEApplication {
 public:
    const juce::String getApplicationName() override { return "SWARA XT Skin Editor"; }
    const juce::String getApplicationVersion() override { return SWARAXT_VERSION_STRING; }
    bool moreThanOneInstanceAllowed() override { return true; }

    void initialise(const juce::String&) override
    {
        mainWindow_ = std::make_unique<swaraxt::skin_editor::MainWindow>();
    }

    void shutdown() override { mainWindow_.reset(); }

    void systemRequestedQuit() override { quit(); }

 private:
    std::unique_ptr<swaraxt::skin_editor::MainWindow> mainWindow_;
};

}  // namespace

START_JUCE_APPLICATION(SkinEditorApplication)
