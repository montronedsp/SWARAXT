// Copyright 2026 MontroneDSP.
// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
#include "Engine/Filter/Circuit/Ir3109Pole.h"
namespace swaraxt {
using ActiveLoadPole = Ir3109Pole;
inline Ir3109Pole makeActiveStage2() { return Ir3109Pole(kActiveStage2Traits); }
inline Ir3109Pole makeActiveStage4() { return Ir3109Pole(kActiveStage4Traits); }
}  // namespace swaraxt
