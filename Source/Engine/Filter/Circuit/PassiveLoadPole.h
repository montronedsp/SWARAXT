// Copyright 2026 MontroneDSP.
// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
#include "Engine/Filter/Circuit/Ir3109Pole.h"
namespace swaraxt {
using PassiveLoadPole = Ir3109Pole;
inline Ir3109Pole makePassiveStage1() { return Ir3109Pole(kPassiveStage1Traits); }
inline Ir3109Pole makePassiveStage3() { return Ir3109Pole(kPassiveStage3Traits); }
}  // namespace swaraxt
