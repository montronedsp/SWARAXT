// Copyright 2026 MontroneDSP.
// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
#include <cstdint>

// One Analog-SVF-v03 row per stimulus. Measured on Windows x64,
// MSVC 19.38.33145, Release /O2, JUCE 9.0.1, native rate 20000000/510.
// These are not Linux or macOS qualifications. Historical Paper/Eagle
// rows remain in DualSvfCurrentLinuxGoldens.h and
// DualSvfCurrentWindowsTimingGoldens.h.
//
// Host: 48 kHz, 256 samples, 100 blocks, factory preset 0, note 48/110 at
// block 0 and note 60/96 at block 40. Q20 of the left channel, FNV-1a.
// Native: the same stimuli, 20960 pre-delay samples, same Q20/FNV-1a rule.
// The V2164 audio cell is applied before the C14/R12 output low-pass, so
// these fingerprints differ from the earlier low-pass-then-gain captures.
// Timing columns are Follow, Independent, Relative. Section 1 ignores
// Filter 2. FX Off and Delay Dub share the pre-FX native hash.
// Serial/Relative stayed at the previous fingerprint for this stimulus.
inline constexpr std::uint64_t dualRelativeGoldens[3] = {
    0xbd4e06d48bdcc7a7ull, 0xd8fc5d027f7545baull, 0xfaef65e0848a57f9ull
};
inline constexpr std::uint64_t dualIndependentGoldens[3] = {
    0xf05998c615ece4b8ull, 0x845385b69781e2e1ull, 0x7855e3b751013230ull
};
inline constexpr std::uint64_t dualRelativeNativeGoldens[3] = {
    0xd78ac26743b95df6ull, 0xd3c2f4024d87f122ull, 0x32944d553d77c469ull
};
inline constexpr std::uint64_t dualIndependentNativeGoldens[3] = {
    0x6894ea74cd2a9129ull, 0x0110a97c2c023905ull, 0x114b6a490489fee5ull
};
inline constexpr std::uint64_t dualTimingNativeGoldens[3][3] = {
    { 0xe5ce3185d0af17efull, 0xe5ce3185d0af17efull, 0xe5ce3185d0af17efull },
    { 0xb48e894963b1934aull, 0x6894ea74cd2a9129ull, 0x48b750def6d63c6dull },
    { 0x69e8d374827ada2bull, 0x03100f6567ab5a8full, 0x41779449086f77d9ull }
};
