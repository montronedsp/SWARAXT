// Copyright 2026 MontroneDSP. SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
#include <cstdint>

// Current MSVC native pre-delay anchors for DualSvfIntegrationAuditMain.cpp.
// Factory0 public Sub64 -> firmware32, Noise0 ->0; 48 kHz/256, 100 blocks,
// note48/110 at block0, note60/96 at block40, no transport, 20960 native samples.
// Same signed Q20/word-FNV1a rule as the preserved DualSvfSection2Goldens.h.
// Restoring ONLY historical mixer assignments reproduces all 48 old native
// and 12 old host anchors exactly. Current/historical PCM independently rehashed;
// repeats and MSVC Release/Debug agree; indexed delay/FX/SRC/DC replay is exact.
// FX Off and Dub share pre-FX anchors; Section1 ignores Section2 controls.
// See Tests/References/DualSvfCurrentWindowsTimingProvenance.json.
inline constexpr std::uint64_t dualRelativeNativeGoldens[2][3] = {
    { 0x31231bf7e0a69ff0ull, 0xf919e99d55f5b7a7ull, 0x26429e00e764559dull },
    { 0x3ab94afdcac2b2f1ull, 0xb86fe6aef9983edfull, 0x1f64af609bf2a329ull },
};
inline constexpr std::uint64_t dualIndependentNativeGoldens[2][3] = {
    { 0xd76fe2932d46c058ull, 0xa0bbfdce3f8d1554ull, 0xa23cc73126baf381ull },
    { 0x84920f63f7fff5e0ull, 0xfa0850b69949a553ull, 0xb818508f4dfab2fcull },
};
inline constexpr std::uint64_t dualTimingNativeGoldens[2][3][3] = {
    {
        { 0xc2642b9bbe15cab3ull, 0xc2642b9bbe15cab3ull, 0xc2642b9bbe15cab3ull },
        { 0x90e325878624db83ull, 0xd76fe2932d46c058ull, 0x90d9c605cfd0f3fbull },
        { 0x003d95711ed2b3feull, 0x4d6dddb76e9a9cbdull, 0x575a10cc22e6749full },
    },
    {
        { 0x24b380a5039b5120ull, 0x24b380a5039b5120ull, 0x24b380a5039b5120ull },
        { 0xd088c7ec4c2d9093ull, 0x84920f63f7fff5e0ull, 0x7147ebb8f253e4c0ull },
        { 0xa7ab1df7ad656b4eull, 0xcddc64e9d7f14851ull, 0x88cdc0bab3459880ull },
    },
};
