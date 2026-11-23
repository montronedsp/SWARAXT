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
// (Relative), Paper/Eagle. Clamp [-32,32], llround(x*2^20), uint32 xor then
// multiply1099511628211, initial1469598103934665603, modulo2^64.
inline constexpr std::uint64_t dualRelativeGoldens[2][3] = {
    { 0xa5418f72030ca56cull, 0x567ef6fb95cdb472ull, 0x66ed4f4c07a50d83ull },
    { 0x0e9265e82289808aull, 0xd9dbe8c95525e5b8ull, 0xddc5f8a84b6c52a3ull },
};
inline constexpr std::uint64_t dualIndependentGoldens[2][3] = {
    { 0xf666bfd056d076fcull, 0x3acfa698d58aa88eull, 0x3c42ee56a924fb6cull },
    { 0x811d9feadbc5c7d3ull, 0x47b06b64de6c9b05ull, 0x7e672b793567f751ull },
};
