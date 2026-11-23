// Copyright 2026 MontroneDSP.
// SPDX-License-Identifier: GPL-3.0-or-later

#ifndef SWARAXT_AVRLIB_PARALLEL_IO_H_
#define SWARAXT_AVRLIB_PARALLEL_IO_H_

#include "avrlib/base.h"
#include "avrlib/gpio.h"

namespace avrlib {

enum ParallelNibbleMode {
    PARALLEL_NIBBLE_HIGH = 0,
    PARALLEL_NIBBLE_LOW = 1
};

template<typename Port, uint8_t mode>
struct ParallelPort {
    static void Init() {}
    static void Write(uint8_t) {}
};

}  // namespace avrlib

#endif  // SWARAXT_AVRLIB_PARALLEL_IO_H_
