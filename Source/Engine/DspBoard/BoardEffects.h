// Copyright 2011 Emilie Gillet; desktop adaptation Copyright 2026 MontroneDSP.
// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
#include "BoardArithmetic.h"

namespace swaraxt::board {
enum class Effect : std::int8_t {
    off = -1, distortion = 0, crush, combPositive, combNegative, ringMod,
    delay, delayFeedback, delayDub, crushDelayFeedback, crushDelayDub,
    delay16, delay12, delay8, delay3_16, looper, pitch
};
constexpr Effect effectFromChoice(int choice) noexcept
{
    return choice >= 1 && choice <= 16 ? static_cast<Effect>(choice - 1) : Effect::off;
}
constexpr int choiceFromEffect(Effect effect) noexcept
{
    const auto code = static_cast<int>(effect);
    return code >= 0 && code < 16 ? code + 1 : 0;
}

// Pure algorithm: no automatic program resets, fades or invalid-loop muting.
// BoardProcessor owns those host policies; reference tests use zeroed storage.
class BoardEffects {
public:
    void reset(bool offsetBinarySilence = false) noexcept;
    void process(Block&, Effect, std::uint8_t cv1, std::uint8_t cv2, std::uint8_t tempo) noexcept;
    bool hasRecordedLoop() const noexcept { return recordedLoop_; }
private:
    void distortion(Block&, std::uint8_t, std::uint8_t) noexcept;
    void crush(Block&, std::uint8_t, std::uint8_t) noexcept;
    void comb(Block&, bool, std::uint8_t, std::uint8_t) noexcept;
    void ringMod(Block&, std::uint8_t, std::uint8_t) noexcept;
    void delay(Block&, Effect, std::uint8_t, std::uint8_t, std::uint8_t) noexcept;
    void loopRecord(Block&, std::uint8_t) noexcept;
    void loopReplay(Block&, std::uint8_t) noexcept;
    void pitch(Block&, std::uint8_t, std::uint8_t) noexcept;
    std::array<std::int8_t, 1281> line_{};
    std::uint16_t delayPtr_ = 0, ringPhase_ = 0;
    std::uint8_t crushCounter_ = 0, delayCounter_ = 0;
    std::int16_t held_ = 0, delayed_ = 0, delayOutput_ = 0, delayInput_ = 0;
    std::uint16_t loopPtr_ = 0, loopStart_ = 0, loopDuration_ = 0, pitchWrite_ = 0;
    std::uint8_t loopPitch_ = 0;
    std::uint32_t loopPhase_ = 0, pitchPhase_ = 0;
    bool recordedLoop_ = false;
};
}
