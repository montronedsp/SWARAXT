// Copyright 2026 MontroneDSP.
// SPDX-License-Identifier: GPL-3.0-or-later

#ifndef SWARAXT_SHRUTHI_DISPLAY_H_
#define SWARAXT_SHRUTHI_DISPLAY_H_

#include "shruthi/shruthi.h"

namespace shruthi {

class Lcd {
 public:
    void clear() {}
    void write(uint8_t) {}
};

class BufferedDisplayStub {
 public:
    void Init() {}
    void set_status(char) {}
    void Refresh() {}
};

}  // namespace shruthi

#endif  // SWARAXT_SHRUTHI_DISPLAY_H_
