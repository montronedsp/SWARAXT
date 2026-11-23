// Copyright 2026 MontroneDSP.
// SPDX-License-Identifier: GPL-3.0-or-later

#ifndef SWARAXT_AVRLIB_GPIO_H_
#define SWARAXT_AVRLIB_GPIO_H_

#include "avrlib/base.h"

namespace avrlib {

template<typename Port, uint8_t pin>
struct Gpio {
    static void set_mode(uint8_t) {}
    static void set_value(uint8_t) {}
    static uint8_t value() { return 0; }
};

struct PortB {};
struct PortC {};
struct PortD {};

template<typename Port, uint8_t mode>
struct SerialPort0 {
    enum { data_size = 1 };
    static void Init() {}
};

template<typename Port, uint8_t mode>
struct SerialPort1 {
    enum { data_size = 1 };
    static void Init() {}
};

template<uint8_t pin>
struct PwmOutput {
    enum { data_size = 1 };
    static void Init() {}
    static void Write(uint8_t) {}
};

}  // namespace avrlib

#endif  // SWARAXT_AVRLIB_GPIO_H_
