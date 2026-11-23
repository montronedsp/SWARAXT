// Independent legacy host reference, not the new BoardArithmetic implementation.
#pragma once
#include "Engine/ShruthiPort/include_first/avrlib/op.h"
namespace avrlib {
static inline int16_t S16S8MulShift8(int16_t a, int8_t b)
{
    return static_cast<int16_t>((static_cast<int32_t>(a) * b) >> 8);
}
}
