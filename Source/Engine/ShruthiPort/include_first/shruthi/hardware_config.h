// Copyright 2026 MontroneDSP.
// SPDX-License-Identifier: GPL-3.0-or-later

#ifndef SWARAXT_SHRUTHI_HARDWARE_CONFIG_H_
#define SWARAXT_SHRUTHI_HARDWARE_CONFIG_H_

#include "avrlib/base.h"

namespace shruthi {

static const uint8_t kPinVcoOut = 12;
static const uint8_t kPinVcaOut = 13;
static const uint8_t kPinVcfCutoffOut = 14;
static const uint8_t kPinVcfResonanceOut = 15;
static const uint8_t kPinCv1Out = 3;
static const uint8_t kPinCv2Out = 4;

static const uint8_t kPinAnalogInput = 0;
static const uint8_t kPinCvInput = 4;

static const uint16_t kInternalEepromSize = 2048;

}  // namespace shruthi

#endif  // SWARAXT_SHRUTHI_HARDWARE_CONFIG_H_
