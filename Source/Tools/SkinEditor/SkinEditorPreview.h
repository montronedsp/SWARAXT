// Copyright 2026 MontroneDSP.
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include <JuceHeader.h>

#include "Ui/SwaraXtSkin.h"

namespace swaraxt::skin_editor {

class PreviewPanel final : public juce::Component {
 public:
    void setPalette(const ui::SkinPalette& palette);
    void paint(juce::Graphics& g) override;

 private:
    ui::SkinPalette palette_ {};
};

}  // namespace swaraxt::skin_editor
