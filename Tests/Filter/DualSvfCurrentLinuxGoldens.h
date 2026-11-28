// Copyright 2026 MontroneDSP.
// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
#include <cstdint>

// Linux CURRENT factory0 contract: public Sub64 -> firmware32; Noise0 ->0.
// Historical DualSvfSection2Goldens.h remains unchanged for provenance.
// See Tests/References/DualSvfCurrentLinuxProvenance.json for causal controls,
// independent quantization, exact inputs and compiler/repeat verification.
// 48 kHz stereo host output; hash left channel, 100 x256 samples, note48/110
// at block0 and note60/96 at block40. Serial LP/BP (Independent) or LP/LP
// (Relative). Both rows are the one v03 board; the retired selector is ignored.
// Clamp [-32,32], llround(x*2^20), uint32 xor then
// multiply1099511628211, initial1469598103934665603, modulo2^64.
// Both rows are the single Analog-SVF-v03 model. The retired selector must
// not change the host fingerprint.
inline constexpr std::uint64_t dualRelativeGoldens[2][3] = {
    { 0x0178d158c89b5c99ull, 0x97498d3b3f551baaull, 0x7fa18cfa65c525c4ull },
    { 0x0178d158c89b5c99ull, 0x97498d3b3f551baaull, 0x7fa18cfa65c525c4ull },
};
inline constexpr std::uint64_t dualIndependentGoldens[2][3] = {
    { 0xb8079899ee2ab2ccull, 0x4fe0fab8ba3e9b3dull, 0x2cd394e9c91812c1ull },
    { 0xb8079899ee2ab2ccull, 0x4fe0fab8ba3e9b3dull, 0x2cd394e9c91812c1ull },
};
