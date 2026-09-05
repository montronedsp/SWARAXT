// Copyright 2011 Emilie Gillet; desktop adaptation Copyright 2026 MontroneDSP.
// SPDX-License-Identifier: GPL-3.0-or-later
// Algorithms from dsp/fx_engine.cc at Shruthi 56bfe78. Storage is instance-owned.
#include "BoardEffects.h"
#include "BoardResources.h"

namespace swaraxt::board {
using namespace arithmetic;
using namespace resources;
void BoardEffects::reset(bool offsetBinarySilence) noexcept
{
    *this = BoardEffects{};
    if (offsetBinarySilence) line_.fill(-128); // Stored byte 128, not signed audio -128.
}
void BoardEffects::process(Block& b, Effect effect, std::uint8_t x, std::uint8_t y, std::uint8_t tempo) noexcept
{
    switch (effect)
    {
        case Effect::off: break;
        case Effect::distortion: distortion(b, x, y); break;
        case Effect::crush: crush(b, x, y); break;
        case Effect::combPositive: comb(b, true, x, y); break;
        case Effect::combNegative: comb(b, false, x, y); break;
        case Effect::ringMod: ringMod(b, x, y); break;
        case Effect::looper: if (y < 128) loopRecord(b, x); else loopReplay(b, x); break;
        case Effect::pitch: pitch(b, x, y); break;
        default:
            if (effect >= Effect::delay && effect <= Effect::delay3_16) delay(b, effect, x, y, tempo);
            break;
    }
}
void BoardEffects::distortion(Block& b, std::uint8_t foldWet, std::uint8_t fuzzWet) noexcept
{
    for (auto& sample : b)
    {
        const auto fold = s16(static_cast<int>(lut_res_fold[static_cast<std::size_t>(sample + 2048)]) - 2048);
        const auto folded = add(S16U8MulShift8(sample, static_cast<std::uint8_t>(255 - foldWet)), S16U8MulShift8(fold, foldWet));
        const auto fuzz = s16(static_cast<int>(lut_res_distortion[static_cast<std::size_t>(folded + 2048)]) - 2048);
        sample = add(S16U8MulShift8(folded, static_cast<std::uint8_t>(255 - fuzzWet)), S16U8MulShift8(fuzz, fuzzWet));
    }
}
void BoardEffects::crush(Block& b, std::uint8_t rate, std::uint8_t bits) noexcept
{
    const auto decimate = (rate >> 3) + 1;
    auto count = 12 - U8U8MulShift8(bits, 12);
    std::int16_t mask = -2048;
    while (--count) mask = s16(shift(mask, 1));
    for (auto& sample : b)
    {
        if (crushCounter_ >= decimate)
        {
            crushCounter_ = 0;
            held_ = s16(static_cast<std::uint16_t>(sample) & static_cast<std::uint16_t>(mask));
        }
        ++crushCounter_;
        sample = held_;
    }
}
void BoardEffects::comb(Block& b, bool positive, std::uint8_t time, std::uint8_t feedback) noexcept
{
    auto write = static_cast<unsigned>(delayPtr_);
    auto read = (write - lut_res_comb_delays[time]) & 1023u;
    for (auto& sample : b)
    {
        const auto fb = S8U8MulShift8(line_[read], feedback);
        line_[write] = S16ClipS8(s16(shift(sample, 4) + (positive ? fb : -fb)));
        sample = clip12(add(S8U8Mul(line_[read], 16), sample));
        read = (read + 1) & 1023u;
        write = (write + 1) & 1023u;
    }
    delayPtr_ = static_cast<std::uint16_t>((delayPtr_ + blockSize) & 1023u);
}
void BoardEffects::ringMod(Block& b, std::uint8_t frequency, std::uint8_t wet) noexcept
{
    for (auto& sample : b)
    {
        ringPhase_ = static_cast<std::uint16_t>(ringPhase_ + lut_res_phase_increment[frequency]);
        const auto sine = s8(128 + interpolate(waveform_res_sine, ringPhase_));
        const auto modulated = S16S8MulShift8(sample, sine);
        sample = add(S16U8MulShift8(sample, static_cast<std::uint8_t>(255 - wet)), S16U8MulShift8(modulated, wet));
    }
}
void BoardEffects::delay(Block& b, Effect effect, std::uint8_t time, std::uint8_t amount, std::uint8_t bpm) noexcept
{
    std::uint8_t feedback = 0;
    std::uint16_t decimate, filterGain, duration;
    const bool raw = effect == Effect::crushDelayFeedback || effect == Effect::crushDelayDub;
    if (effect <= Effect::crushDelayDub)
    {
        if (effect == Effect::delayFeedback || effect == Effect::crushDelayFeedback) feedback = 85;
        if (effect == Effect::delayDub || effect == Effect::crushDelayDub) feedback = 224;
        decimate = lut_res_delay_decimation[time];
        filterGain = lut_res_delay_filter_gain[time];
        duration = lut_res_delay_duration[time];
    }
    else
    {
        auto tempo = static_cast<unsigned>(std::clamp<int>(bpm, 40, 240));
        if (effect == Effect::delay16) tempo *= 3;
        else if (effect == Effect::delay12) tempo = (tempo * 9) >> 2;
        else if (effect == Effect::delay8) tempo = (tempo * 3) >> 1;
        tempo -= 40;
        decimate = lut_res_tap_delay_decimation[tempo];
        filterGain = lut_res_tap_delay_filter_gain[tempo];
        duration = lut_res_tap_delay_duration[tempo];
        feedback = time;
    }
    auto write = static_cast<unsigned>(delayPtr_);
    auto read = (write + 1280u - duration) % 1280u;
    for (auto& sample : b)
    {
        if (delayCounter_ >= decimate)
        {
            delayCounter_ = 0;
            const auto fb = add(raw ? s16(shift(sample, 4)) : delayInput_, S8U8MulShift8(line_[read], feedback));
            line_[write] = S16ClipS8(fb);
            delayed_ = S8U8Mul(line_[read], 16);
            if (++read >= 1280) read -= 1280;
            if (++write >= 1280) write -= 1280;
            ++delayPtr_;
        }
        if (!raw)
        {
            delayInput_ = add(delayInput_, S16U8MulShift8(sub(s16(shift(sample, 4)), delayInput_), static_cast<std::uint8_t>(filterGain)));
            delayOutput_ = add(delayOutput_, S16U8MulShift8(sub(delayed_, delayOutput_), static_cast<std::uint8_t>(filterGain)));
        }
        ++delayCounter_;
        sample = clip12(add(sample, S16U8MulShift8(raw ? delayed_ : delayOutput_, amount)));
    }
    // Preserve the source's guard-cell behavior at exactly 1280 (strict >).
    while (delayPtr_ > 1280) delayPtr_ = static_cast<std::uint16_t>(delayPtr_ - 1280);
}
void BoardEffects::loopRecord(Block& b, std::uint8_t time) noexcept
{
    loopPitch_ = time;
    const auto decimate = lut_res_delay_decimation[time];
    const auto duration = lut_res_delay_duration[time];
    const auto gain = static_cast<std::uint8_t>(lut_res_delay_filter_gain[time]);
    auto write = static_cast<unsigned>(loopPtr_);
    for (const auto sample : b)
    {
        if (delayCounter_ >= decimate)
        {
            delayCounter_ = 0;
            const auto stored = s8(S16ClipS8(delayInput_) + 128);
            line_[write++] = stored;
            if (write >= 1024) { line_[1024] = stored; write -= 1024; }
            ++loopPtr_;
            recordedLoop_ = true;
        }
        delayInput_ = add(delayInput_, S16U8MulShift8(sub(s16(shift(sample, 4)), delayInput_), gain));
        ++delayCounter_;
    }
    loopPtr_ &= 1023;
    loopStart_ = static_cast<std::uint16_t>((loopPtr_ + 1024u - duration) & 1023u);
    loopPhase_ = 0;
    loopDuration_ = duration;
}
void BoardEffects::loopReplay(Block& b, std::uint8_t pitchCode) noexcept
{
    const auto pitchShift = static_cast<std::uint8_t>(128 - (pitchCode >> 1) + (loopPitch_ >> 1));
    auto increment = lut_res_pitch_ratio[pitchShift];
    const auto scaling = lut_res_delay_phase_scaling[loopPitch_];
    if (scaling != 0) increment = static_cast<std::uint16_t>(S16U16MulShift16(s16(increment), scaling));
    if (increment == 0) increment = 1;
    for (auto& sample : b)
    {
        const auto read = (loopStart_ + (loopPhase_ >> 8)) & 1023u;
        sample = s16((U8MixU16(static_cast<std::uint8_t>(line_[read]), static_cast<std::uint8_t>(line_[read + 1]), static_cast<std::uint8_t>(loopPhase_)) >> 4) - 2048);
        loopPhase_ = phaseAdd(loopPhase_, increment);
        if ((loopPhase_ >> 8) >= loopDuration_) loopPhase_ = (loopPhase_ - (static_cast<std::uint32_t>(loopDuration_) << 8)) & 0xffffffu;
    }
}
void BoardEffects::pitch(Block& b, std::uint8_t pitchCode, std::uint8_t wet) noexcept
{
    const auto increment = lut_res_pitch_ratio[pitchCode];
    auto write = static_cast<unsigned>(pitchWrite_);
    for (auto& sample : b)
    {
        const auto stored = s8(shift(sample, 4) + 128);
        line_[write++] = stored;
        if (write >= 1024) { line_[1024] = stored; write -= 1024; }
        const auto first = pitchPhase_ >> 8;
        const auto second = (first + 512) & 1023u;
        auto distance = (second + 1024 - write) & 1023u;
        if (distance >= 512) distance = 1024 - distance - 1;
        const auto blend = U8Mix(static_cast<std::uint8_t>(line_[first]), static_cast<std::uint8_t>(line_[second]), static_cast<std::uint8_t>(distance >> 1));
        const auto shifted = s16(U8U8Mul(blend, 16) - 2048);
        sample = add(S16U8MulShift8(sample, static_cast<std::uint8_t>(255 - wet)), S16U8MulShift8(shifted, wet));
        pitchPhase_ = phaseAdd(pitchPhase_, increment);
        if ((pitchPhase_ >> 8) >= 1024) pitchPhase_ -= 1024u << 8;
    }
    pitchWrite_ = static_cast<std::uint16_t>((pitchWrite_ + blockSize) & 1023u);
}
}
