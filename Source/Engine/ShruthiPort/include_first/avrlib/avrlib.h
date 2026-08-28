// Copyright 2026 MontroneDSP.
// SPDX-License-Identifier: GPL-3.0-or-later
// Host stub for avrlib/avrlib.h (no AVR hardware registers).

#ifndef SWARAXT_AVRLIB_AVRLIB_H_
#define SWARAXT_AVRLIB_AVRLIB_H_

#include "avrlib/base.h"
#include "avrlib/size_to_type.h"

namespace avrlib {

enum DataOrder {
    MSB_FIRST = 0,
    LSB_FIRST = 1
};

enum DigitalValue {
    LOW = 0,
    HIGH = 1
};

struct Input {
    enum {
        buffer_size = 0,
        data_size = 0,
    };
    typedef uint8_t Value;

    static inline Value Read() { return 0; }
    static inline uint8_t readable() { return 1; }
    static inline int16_t NonBlockingRead() { return 0; }
    static inline Value ImmediateRead() { return 0; }
    static inline void Received() {}
};

struct Output {
    enum {
        buffer_size = 0,
        data_size = 0
    };
    typedef uint8_t Value;

    static inline void Write(Value) {}
    static inline uint8_t writable() { return 1; }
    static inline uint8_t NonBlockingWrite(Value) { return 1; }
    static inline void Overwrite(Value) {}
    static inline Value Requested() { return 0; }
};

template<typename I, typename O>
struct InputOutput {
    typedef I Input;
    typedef O Output;
};

typedef Input DisabledInput;
typedef Output DisabledOutput;
typedef InputOutput<DisabledInput, DisabledOutput> DisabledInputOutput;

enum PortMode {
    DISABLED = 0,
    POLLED = 1,
    BUFFERED = 2
};

template<typename T>
class scoped_resource {
 public:
    scoped_resource() { T::Begin(); }
    ~scoped_resource() { T::End(); }
};

}  // namespace avrlib

#endif  // SWARAXT_AVRLIB_AVRLIB_H_
