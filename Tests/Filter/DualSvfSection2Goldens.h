// Copyright 2026 MontroneDSP.
// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
#include <cstdint>

// Base 03b02949dfc45971750123a767157bd48b195143, MSVC 19.38.33145 /O2,
// 48 kHz, 256 samples, 100 blocks, factory preset 0; MIDI note48/110 at
// block0 and note60/96 at block40. Relative cutoff52/64/76, resonance0,
// serial LP/LP, Paper then Eagle. Matrix defaults; final cutoff14 carries
// firmware tracking/env/LFO before offset*128 -8192 and clipping, then /64.
// FNV1a over signed Q20 mono samples, one uint32 xor/multiply per sample.
// Frozen only after HD firmware-reference and production independence checks.
inline constexpr std::uint64_t dualRelativeGoldens[2][3] = {
    { 0x9134ba4bc6be92ccull, 0xf240c1c4b54da391ull, 0x1fde7e1df3bcaff1ull },
    { 0x9eeaea362a7dabd0ull, 0x9012aed1b41b3e5bull, 0xe8262b15fa76afacull }
};
// Same stimulus, Independent serial LP/BP, cutoff/resonance 40/32,90/32,90/63.
// Unmodulated CV bytes are 80/128,180/128,180/252 respectively.
inline constexpr std::uint64_t dualIndependentGoldens[2][3] = {
    { 0x0a7b6b85a54e1f34ull, 0x030dfdf8261a383eull, 0xbcc372faf138f8c2ull },
    { 0x21ae3ef6c0f5dfa2ull, 0x0a51f4e5d30b83fcull, 0x207ffd7c274cf57bull }
};
