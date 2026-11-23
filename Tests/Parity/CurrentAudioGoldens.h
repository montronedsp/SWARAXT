// Copyright2026 MontroneDSP. SPDX-License-Identifier: GPL-3.0-or-later
// Qualified current-correctness contract, Phase3 SteamOS GCC15/Clang20.
// Base source a21530355b03d78749f30afc3c1048d7ca36000d; JUCE3fb5cbc0340758596dac39f7c8de47d562b4896e.
// public Sub/Noise>>1; explicit integrated Sub64->32. Exact comparisons retained.
// Full inputs, raw samples, division oracle, repeats, shipping and negative controls:
// Tests/References/CurrentAudioReferenceProvenance.json and private Phase3 evidence.
#pragma once
#include "ExactParityGoldens.h"
namespace swaraxt_current_golden {
using swaraxt_golden::Entry;using swaraxt_golden::find;
inline constexpr Entry kIntegrated[] = {
    {"off_default", 0x9259aab1c591727eull, 4096},
    {"distortion_default", 0x8817d1113ae623f6ull, 4096},
    {"crush_default", 0x8a31616a6fa6aab2ull, 4096},
    {"combPositive_default", 0x267920d638fd9335ull, 4096},
    {"combNegative_default", 0xb4073e2570f056b0ull, 4096},
    {"ringMod_default", 0x28a756a09db1f074ull, 4096},
    {"delay_default", 0x400dc09a72426a7bull, 4096},
    {"delayFeedback_default", 0x400dc09a72426a7bull, 4096},
    {"delayDub_default", 0x400dc09a72426a7bull, 4096},
    {"crushDelayFeedback_default", 0x400dc09a72426a7bull, 4096},
    {"crushDelayDub_default", 0x400dc09a72426a7bull, 4096},
    {"delay16_default", 0x400dc09a72426a7bull, 4096},
    {"delay12_default", 0x400dc09a72426a7bull, 4096},
    {"delay8_default", 0x400dc09a72426a7bull, 4096},
    {"delay3_16_default", 0x400dc09a72426a7bull, 4096},
    {"looper_default", 0x400dc09a72426a7bull, 4096},
    {"pitch_default", 0x5673780a78b12c38ull, 4096},
    {"distortion_cv1min", 0x5fcd5a4fa71958f5ull, 4096},
    {"distortion_cv1max", 0x5af78ba07ed4cba8ull, 4096},
    {"delayFeedback_cv2max", 0x5dcc4a73a0c1ba86ull, 4096},
    {"looper_replay", 0xa538dccc999e97dbull, 2048},
    {"seq_ringmod", 0xbe7fb0911be00383ull, 4096},
};
inline constexpr int kIntegratedCount = 22;
inline constexpr Entry kClassic[] = {
    {"Test A basic saw", 0xe49a2f5d8a7090f9ull, 44100},
    {"Test B basic square", 0x608f87853120af8eull, 44100},
    {"Test C two oscillators", 0x2e5f9d73aeaa1820ull, 44100},
    {"Test F filter low resonance", 0x0099139bef0f1690ull, 44100},
    {"Test G filter high resonance", 0x1ac4a6fabf9a3da5ull, 44100},
    {"Test I normal envelope", 0x8bed59407b8e5da6ull, 44100},
    {"Test D noise only", 0xc473d3385b5767baull, 44100},
    {"Test J silence", 0x81296f5b21ed7843ull, 44100},
};
inline constexpr int kClassicCount = 8;
}
