// Copyright 2011 Emilie Gillet; desktop adaptation Copyright 2026 MontroneDSP.
// SPDX-License-Identifier: GPL-3.0-or-later
// Defined-width equivalents of pinned avrlib/op.h AVR register operations.
#pragma once
#include <algorithm>
#include <array>
#include <cstdint>

namespace swaraxt::board {
inline constexpr double sampleRate = 20000000.0 / 510.0;
inline constexpr std::size_t blockSize = 40;
using Block = std::array<std::int16_t, blockSize>;

namespace arithmetic {
constexpr std::int8_t s8(std::int32_t x) noexcept
{
    const auto bits = static_cast<std::uint8_t>(x);
    return static_cast<std::int8_t>(bits < 128 ? bits : static_cast<int>(bits) - 256);
}
constexpr std::int16_t s16(std::int32_t x) noexcept
{
    const auto bits = static_cast<std::uint16_t>(x);
    return static_cast<std::int16_t>(bits < 32768 ? bits : static_cast<int>(bits) - 65536);
}
// Floor division reproduces ASR for negative operands without signed shifts.
constexpr std::int32_t shift(std::int32_t x, unsigned n) noexcept
{
    if (n >= 31) return x < 0 ? -1 : 0;
    const auto divisor = std::int64_t{1} << n;
    return static_cast<std::int32_t>(x >= 0 ? x / divisor : -((-std::int64_t{x} + divisor - 1) / divisor));
}
constexpr std::int16_t add(std::int16_t a, std::int16_t b) noexcept { return s16(static_cast<std::int32_t>(a) + b); }
constexpr std::int16_t sub(std::int16_t a, std::int16_t b) noexcept { return s16(static_cast<std::int32_t>(a) - b); }
constexpr std::uint32_t phaseAdd(std::uint32_t a, std::uint16_t increment) noexcept { return (a + increment) & 0xffffffu; }
constexpr std::uint8_t U8U8MulShift8(std::uint8_t a, std::uint8_t b) noexcept { return static_cast<std::uint8_t>((a * b) / 256); }
constexpr std::uint16_t U8U8Mul(std::uint8_t a, std::uint8_t b) noexcept { return static_cast<std::uint16_t>(a * b); }
constexpr std::int16_t S8U8Mul(std::int8_t a, std::uint8_t b) noexcept { return static_cast<std::int16_t>(a * b); }
constexpr std::int8_t S8U8MulShift8(std::int8_t a, std::uint8_t b) noexcept { return s8(shift(a * b, 8)); }
constexpr std::int16_t S16U8MulShift8(std::int16_t a, std::uint8_t b) noexcept { return s16(shift(static_cast<std::int32_t>(a) * b, 8)); }
constexpr std::int16_t S16S8MulShift8(std::int16_t a, std::int8_t b) noexcept { return s16(shift(static_cast<std::int32_t>(a) * b, 8)); }
constexpr std::int16_t S16U16MulShift16(std::int16_t a, std::uint16_t b) noexcept { return s16(shift(static_cast<std::int32_t>(a) * b, 16)); }
constexpr std::int8_t S16ClipS8(std::int16_t x) noexcept { return static_cast<std::int8_t>(std::clamp<int>(x, -128, 127)); }
constexpr std::int16_t clip12(std::int16_t x) noexcept { return std::clamp<std::int16_t>(x, -2047, 2047); }
constexpr std::uint16_t U8MixU16(std::uint8_t a, std::uint8_t b, std::uint8_t balance) noexcept
{
    return static_cast<std::uint16_t>(a * (255 - balance) + b * balance);
}
constexpr std::uint8_t U8Mix(std::uint8_t a, std::uint8_t b, std::uint8_t balance) noexcept
{
    return static_cast<std::uint8_t>(U8MixU16(a, b, balance) >> 8);
}
constexpr std::uint8_t interpolate(const std::uint8_t* table, std::uint16_t phase) noexcept
{
    return U8Mix(table[phase >> 8], table[(phase >> 8) + 1], static_cast<std::uint8_t>(phase));
}
constexpr std::int16_t fromAdc(std::uint8_t code) noexcept { return static_cast<std::int16_t>((static_cast<int>(code) - 128) * 16); }
} // namespace arithmetic
} // namespace swara::board
