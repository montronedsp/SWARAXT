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
inline constexpr std::uint64_t preLatencyDualRelativeGoldens[2][3] = {
    { 0x9134ba4bc6be92ccull, 0xf240c1c4b54da391ull, 0x1fde7e1df3bcaff1ull },
    { 0x9eeaea362a7dabd0ull, 0x9012aed1b41b3e5bull, 0xe8262b15fa76afacull }
};
// Same stimulus, Independent serial LP/BP, cutoff/resonance 40/32,90/32,90/63.
// Unmodulated CV bytes are 80/128,180/128,180/252 respectively.
inline constexpr std::uint64_t preLatencyDualIndependentGoldens[2][3] = {
    { 0x0a7b6b85a54e1f34ull, 0x030dfdf8261a383eull, 0xbcc372faf138f8c2ull },
    { 0x21ae3ef6c0f5dfa2ull, 0x0a51f4e5d30b83fcull, 0x207ffd7c274cf57bull }
};

// F2.1 host-output references: the same twelve stimuli and Q20/FNV1a rule,
// now with 256 native samples after the complete Dual VCA, before shared FX.
// SVF equations, firmware reconstruction and gain laws are unchanged.
// Verified against published 74f25d2bebbffa3618a30ec58ac77068a81e0c83:
// baseline native captures -> indexed delay -> FX/post-DAC -> master -> SRC/DC.
// Both unshifted baseline and corrected candidate host waveforms matched their
// independent replay bit-for-bit. At 48 kHz this is 313.344 host samples;
// the legacy whole-render hashes above therefore cannot remain unchanged.
// Environment: MSVC 19.38.33145 x64 Release/Debug, JUCE 9.0.1, native 20e6/510,
// Normal quality. No host transport; factory preset 0 and MIDI as above.
inline constexpr std::uint64_t dualRelativeGoldens[2][3] = {
    { 0x97a5625193a2326dull, 0xd8b8558a76679c7bull, 0x3af2d505b736708eull },
    { 0x2efe0fbc75874c4bull, 0x7998ba0118727a6eull, 0xd4a8fbe60fde8a40ull }
};
inline constexpr std::uint64_t dualIndependentGoldens[2][3] = {
    { 0xef139144414c5c37ull, 0xcfb4deaeda072793ull, 0xe3ae9c3d53ff973bull },
    { 0x7e874a740b73392eull, 0xf5f0dc043c0a672cull, 0x696ac69cd52ec5f4ull }
};

// Immutable PRE-delay native Q20 fingerprints from the published baseline.
// These anchor synthesis independently of the new host hashes and the delay
// replay. Same twelve stimuli; each stream includes all 20960 rendered native
// samples (including SRC lookahead), before shared FX and master gain.
inline constexpr std::uint64_t dualRelativeNativeGoldens[2][3] = {
    { 0x4ef241d9f8eb9497ull, 0x66aad0d7b284df11ull, 0xe86b5e75817bc511ull },
    { 0xa20e730156d8afd7ull, 0x1676cfa3dff83f88ull, 0x37458b4229218ce9ull }
};
inline constexpr std::uint64_t dualIndependentNativeGoldens[2][3] = {
    { 0x3251d9ffb512f701ull, 0x2521425e569b0947ull, 0x935046a2b103f7ecull },
    { 0x78183c0ca2d3b26full, 0x1d2388dc3784a61bull, 0x8d9a56148a6f62c1ull }
};

// Broader timing invariant: [Paper/Eagle][Section1/Serial/Parallel]
// [Follow/Independent/Relative], same factory/MIDI stimulus, LP/BP.
// F2 cutoff40/resonance32, except Relative cutoff76. Captured separately with
// FX Off and Delay Dub; both give the same pre-FX native fingerprint.
inline constexpr std::uint64_t dualTimingNativeGoldens[2][3][3] = {
    {
        { 0x45141e1aa5e81b09ull, 0x45141e1aa5e81b09ull, 0x45141e1aa5e81b09ull },
        { 0xcbfea6e6ead98e03ull, 0x3251d9ffb512f701ull, 0xf79a06dde02c3cbeull },
        { 0x52f36293f51df8f9ull, 0xd8146a8ac0746797ull, 0x6941778dd4124e32ull }
    },
    {
        { 0xbd31818003032f9eull, 0xbd31818003032f9eull, 0xbd31818003032f9eull },
        { 0x324521cba2df3387ull, 0x78183c0ca2d3b26full, 0xcab40b77f20eb5c5ull },
        { 0x906ddd1e71c3177eull, 0x138691fc335db05aull, 0xac39c2dc286b668dull }
    }
};
