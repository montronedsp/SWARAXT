// Copyright 2026 MontroneDSP. SPDX-License-Identifier: GPL-3.0-or-later
// Windows uses the unchanged headless renderer with an explicit current-data
// selection. The historical header remains available for historical tests.
#include "DualSvfSection2Goldens.h"
namespace swaraxt_windows_current {
#include "DualSvfCurrentLinuxGoldens.h"
}
#define dualRelativeGoldens swaraxt_windows_current::dualRelativeGoldens
#define dualIndependentGoldens swaraxt_windows_current::dualIndependentGoldens
#include "DualSvfHeadlessTestsMain.cpp"
