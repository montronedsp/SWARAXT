// Copyright 2009 Emilie Gillet.
//
// Author: Emilie Gillet (emilie.o.gillet@gmail.com)
//
// This program is free software: you can redistribute it and/or modify
// it under the terms of the GNU General Public License as published by
// the Free Software Foundation, either version 3 of the License, or
// (at your option) any later version.
// This program is distributed in the hope that it will be useful,
// but WITHOUT ANY WARRANTY; without even the implied warranty of
// MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
// GNU General Public License for more details.
// You should have received a copy of the GNU General Public License
// along with this program.  If not, see <http://www.gnu.org/licenses/>.
//
// -----------------------------------------------------------------------------
// Portable host variant for SwaraXt (MontroneDSP).
// Derived from pichenettes/avril @ af7266e5b48ae20c0f83d4352c75c1068545cffc.
// AVR inline-assembly paths are omitted; C fallbacks are used so the Shruthi
// DSP can compile on desktop hosts while preserving integer wrapping semantics.
// Parentheses were added to mix/shift expressions so evaluation matches the
// intended AVR multiply-high-byte behavior.

#ifndef AVRLIB_OP_H_
#define AVRLIB_OP_H_

#include <avr/pgmspace.h>

#include "avrlib/base.h"

namespace avrlib {

static inline int16_t Clip(int16_t value, int16_t min, int16_t max)
{
    return value < min ? min : (value > max ? max : value);
}

static inline int16_t S16ClipU14(int16_t value)
{
    uint8_t msb = static_cast<uint16_t>(value) >> 8;
    if (msb & 0x80)
        return 0;
    if (msb & 0x40)
        return 16383;
    return value;
}

static inline uint8_t U8AddClip(uint8_t value, uint8_t increment, uint8_t max)
{
    value += increment;
    if (value > max)
        value = max;
    return value;
}

static inline uint8_t S16ShiftRight8(int16_t value)
{
    return static_cast<uint16_t>(value) >> 8;
}

static inline uint24c_t U24AddC(uint24c_t a, uint24_t b)
{
    uint24c_t result;
    uint32_t av = (static_cast<uint32_t>(a.integral) << 8) + a.fractional;
    uint32_t bv = (static_cast<uint32_t>(b.integral) << 8) + b.fractional;
    uint32_t sum = av + bv;
    result.integral = static_cast<uint16_t>(sum >> 8);
    result.fractional = static_cast<uint8_t>(sum & 0xff);
    result.carry = (sum & 0xff000000u) != 0;
    return result;
}

static inline uint24_t U24Add(uint24_t a, uint24_t b)
{
    uint24_t result;
    uint32_t av = (static_cast<uint32_t>(a.integral) << 8) + a.fractional;
    uint32_t bv = (static_cast<uint32_t>(b.integral) << 8) + b.fractional;
    uint32_t sum = av + bv;
    result.integral = static_cast<uint16_t>(sum >> 8);
    result.fractional = static_cast<uint8_t>(sum & 0xff);
    return result;
}

static inline uint24_t U24Sub(uint24_t a, uint24_t b)
{
    uint24_t result;
    uint32_t av = (static_cast<uint32_t>(a.integral) << 8) + a.fractional;
    uint32_t bv = (static_cast<uint32_t>(b.integral) << 8) + b.fractional;
    uint32_t difference = av - bv;
    result.integral = static_cast<uint16_t>(difference >> 8);
    result.fractional = static_cast<uint8_t>(difference & 0xff);
    return result;
}

static inline uint24_t U24ShiftRight(uint24_t a)
{
    uint24_t result;
    uint32_t av = (static_cast<uint32_t>(a.integral) << 8) + a.fractional;
    av >>= 1;
    result.integral = static_cast<uint16_t>(av >> 8);
    result.fractional = static_cast<uint8_t>(av & 0xff);
    return result;
}

static inline uint24_t U24ShiftLeft(uint24_t a)
{
    uint24_t result;
    uint32_t av = (static_cast<uint32_t>(a.integral) << 8) + a.fractional;
    av <<= 1;
    result.integral = static_cast<uint16_t>(av >> 8);
    result.fractional = static_cast<uint8_t>(av & 0xff);
    return result;
}

static inline uint8_t S16ClipU8(int16_t value)
{
    return value < 0 ? 0 : (value > 255 ? 255 : static_cast<uint8_t>(value));
}

static inline int8_t S16ClipS8(int16_t value)
{
    return value < -128 ? -128 : (value > 127 ? 127 : static_cast<int8_t>(value));
}

static inline uint8_t U8Mix(uint8_t a, uint8_t b, uint8_t balance)
{
    return static_cast<uint8_t>((a * (255 - balance) + b * balance) >> 8);
}

static inline uint8_t U8Mix(uint8_t a, uint8_t b, uint8_t gain_a, uint8_t gain_b)
{
    return static_cast<uint8_t>((a * gain_a + b * gain_b) >> 8);
}

static inline int8_t S8Mix(int8_t a, int8_t b, uint8_t gain_a, uint8_t gain_b)
{
    return static_cast<int8_t>((a * gain_a + b * gain_b) >> 8);
}

static inline uint16_t U8MixU16(uint8_t a, uint8_t b, uint8_t balance)
{
    return static_cast<uint16_t>(a * (255 - balance) + b * balance);
}

static inline uint8_t U8U4MixU8(uint8_t a, uint8_t b, uint8_t balance)
{
    return static_cast<uint8_t>((a * (15 - balance) + b * balance) >> 4);
}

static inline uint16_t U8U4MixU12(uint8_t a, uint8_t b, uint8_t balance)
{
    return static_cast<uint16_t>(a * (15 - balance) + b * balance);
}

static inline uint8_t U8ShiftRight4(uint8_t a) { return a >> 4; }
static inline uint8_t U8ShiftLeft4(uint8_t a) { return static_cast<uint8_t>(a << 4); }
static inline uint8_t U8Swap4(uint8_t a) { return static_cast<uint8_t>((a << 4) | (a >> 4)); }

static inline uint8_t U8U8MulShift8(uint8_t a, uint8_t b)
{
    return static_cast<uint8_t>((a * b) >> 8);
}

static inline int8_t S8U8MulShift8(int8_t a, uint8_t b)
{
    return static_cast<int8_t>((a * b) >> 8);
}

static inline int16_t S8U8Mul(int8_t a, uint8_t b) { return static_cast<int16_t>(a * b); }
static inline int16_t S8S8Mul(int8_t a, int8_t b) { return static_cast<int16_t>(a * b); }
static inline uint16_t U8U8Mul(uint8_t a, uint8_t b) { return static_cast<uint16_t>(a * b); }

static inline int8_t S8S8MulShift8(int8_t a, int8_t b)
{
    return static_cast<int8_t>((a * b) >> 8);
}

static inline uint16_t Mul16Scale8(uint16_t a, uint16_t b)
{
    return static_cast<uint16_t>((static_cast<uint32_t>(a) * b) >> 8);
}

static inline uint8_t U14ShiftRight6(uint16_t value) { return static_cast<uint8_t>(value >> 6); }
static inline uint8_t U15ShiftRight7(uint16_t value) { return static_cast<uint8_t>(value >> 7); }
static inline uint16_t U16ShiftRight4(uint16_t a) { return a >> 4; }

static inline int16_t S16U16MulShift16(int16_t a, uint16_t b)
{
    return static_cast<int16_t>((static_cast<int32_t>(a) * static_cast<uint32_t>(b)) >> 16);
}

static inline int16_t S16U8MulShift8(int16_t a, uint8_t b)
{
    return static_cast<int16_t>((static_cast<int32_t>(a) * static_cast<uint32_t>(b)) >> 8);
}

static inline uint16_t U16U8MulShift8(uint16_t a, uint8_t b)
{
    return static_cast<uint16_t>((static_cast<uint32_t>(a) * static_cast<uint32_t>(b)) >> 8);
}

static inline uint8_t InterpolateSample(const prog_uint8_t* table, uint16_t phase)
{
    return U8Mix(pgm_read_byte(table + (phase >> 8)),
                 pgm_read_byte(1 + table + (phase >> 8)),
                 phase & 0xff);
}

}  // namespace avrlib

#endif  // AVRLIB_OP_H_
