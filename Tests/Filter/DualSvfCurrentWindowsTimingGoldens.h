// Copyright 2026 MontroneDSP. SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
#include <cstdint>

// Current MSVC native pre-delay anchors for the single Analog-SVF-v03 board.
// Factory0 public Sub64 -> firmware32, Noise0 ->0; 48 kHz/256, 100 blocks,
// note48/110 at block0, note60/96 at block40, no transport, 20960 native samples.
// Same signed Q20/word-FNV1a rule as the host references.
// Both rows are required to match: the retired Paper/Eagle selector is ignored.
// FX Off and Dub share pre-FX anchors; Section1 ignores Section2 controls.
// These replace the pre-v03 native anchors. They are not a Paper acceptance target.
inline constexpr std::uint64_t dualRelativeNativeGoldens[2][3] = {
    { 0xfde332b682381658ull, 0xa49c0d199b545d33ull, 0xdd9df110f6e23ff3ull },
    { 0xfde332b682381658ull, 0xa49c0d199b545d33ull, 0xdd9df110f6e23ff3ull },
};
inline constexpr std::uint64_t dualIndependentNativeGoldens[2][3] = {
    { 0x15733d79c5102414ull, 0xaf27fbaf431e921cull, 0x60bc86ea57c03a38ull },
    { 0x15733d79c5102414ull, 0xaf27fbaf431e921cull, 0x60bc86ea57c03a38ull },
};
inline constexpr std::uint64_t dualTimingNativeGoldens[2][3][3] = {
    {
        { 0xeed41075c74b14a4ull, 0xeed41075c74b14a4ull, 0xeed41075c74b14a4ull },
        { 0x2b2212b85ee407a5ull, 0x15733d79c5102414ull, 0x48b750def6d63c6dull },
        { 0xb39d72850a40f0b9ull, 0x041147905828f3b9ull, 0xd17d106e8fe313d9ull },
    },
    {
        { 0xeed41075c74b14a4ull, 0xeed41075c74b14a4ull, 0xeed41075c74b14a4ull },
        { 0x2b2212b85ee407a5ull, 0x15733d79c5102414ull, 0x48b750def6d63c6dull },
        { 0xb39d72850a40f0b9ull, 0x041147905828f3b9ull, 0xd17d106e8fe313d9ull },
    },
};
