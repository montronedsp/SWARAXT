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
// Timing columns are Follow, Independent, Relative. Section 1 ignores
// Filter 2. FX Off and Delay Dub share the pre-FX native hash.
inline constexpr std::uint64_t dualRelativeGoldens[3] = {
    0x0178d158c89b5c99ull, 0x97498d3b3f551baaull, 0x7fa18cfa65c525c4ull
};
inline constexpr std::uint64_t dualIndependentGoldens[3] = {
    0xb8079899ee2ab2ccull, 0x4fe0fab8ba3e9b3dull, 0x2cd394e9c91812c1ull
};
inline constexpr std::uint64_t dualRelativeNativeGoldens[3] = {
    0xfde332b682381658ull, 0xa49c0d199b545d33ull, 0xdd9df110f6e23ff3ull
};
inline constexpr std::uint64_t dualIndependentNativeGoldens[3] = {
    0x15733d79c5102414ull, 0xaf27fbaf431e921cull, 0x60bc86ea57c03a38ull
};
inline constexpr std::uint64_t dualTimingNativeGoldens[3][3] = {
    { 0xeed41075c74b14a4ull, 0xeed41075c74b14a4ull, 0xeed41075c74b14a4ull },
    { 0x2b2212b85ee407a5ull, 0x15733d79c5102414ull, 0x48b750def6d63c6dull },
    { 0xb39d72850a40f0b9ull, 0x041147905828f3b9ull, 0xd17d106e8fe313d9ull }
};
