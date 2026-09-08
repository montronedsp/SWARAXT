// Copyright 2026 MontroneDSP. SPDX-License-Identifier: GPL-3.0-or-later
#include "BoardProcessor.h"
#include "BoardResources.h"

namespace swaraxt::board {
namespace {
bool autonomousFilter(const BoardControl& c) noexcept
{
    return c.postDcaFilter() && BoardFilter::hasFeedback(c.cutoff, c.resonance, c.route == Route::highPassLast);
}
}
void BoardProcessor::reset(Effect effect) noexcept
{
    input_.reset(); output_.reset(); filter_.reset();
    // Looper/pitch store unsigned offset-binary samples in an int8_t array.
    // Unwritten samples must represent silence at the host boundary.
    effects_.reset(effect == Effect::looper || effect == Effect::pitch);
    sinceInput_ = quietTime_ = 0;
    lastOutput_ = 0;
    tailActive_ = false;
}
void BoardProcessor::processEffects(Block& samples, const BoardControl& c) noexcept
{
    if (c.effect == Effect::looper && c.cv2 >= 128 && !effects_.hasRecordedLoop())
    {
        samples.fill(0);
        return;
    }
    effects_.process(samples, c.effect, c.cv1, c.cv2, c.tempo);
}
void BoardProcessor::processBoard(FloatBlock& samples, const BoardControl& c) noexcept
{
    Block integer{};
    for (std::size_t i = 0; i < blockSize; ++i) integer[i] = arithmetic::fromAdc(input_.process(samples[i]));
    const bool first = c.route == Route::lowPassFirst || c.route == Route::highPassFirst;
    const bool hp = c.route == Route::highPassFirst || c.route == Route::highPassLast;
    if (first) filter_.process(integer, c.cutoff, c.resonance, hp);
    BoardFilter::dca(integer, c.dca);
    processEffects(integer, c);
    if (!first && c.route != Route::fxOnly) filter_.process(integer, c.cutoff, c.resonance, hp);
    for (std::size_t i = 0; i < blockSize; ++i) samples[i] = output_.process(integer[i]);
    observeTail(samples, c, c.dca != 0);
}
void BoardProcessor::processClassicFx(FloatBlock& samples, const BoardControl& c) noexcept
{
    if (c.effect == Effect::off) return;
    Block integer{};
    bool driven = false;
    for (std::size_t i = 0; i < blockSize; ++i)
    {
        const double x = std::isfinite(samples[i]) ? samples[i] : 0.0;
        driven = driven || x != 0;
        integer[i] = static_cast<std::int16_t>(std::clamp(std::round(x * 2048), -2048.0, 2047.0));
    }
    processEffects(integer, c);
    for (std::size_t i = 0; i < blockSize; ++i) samples[i] = static_cast<float>(integer[i]) / 2048;
    observeTail(samples, c, driven);
}
double BoardProcessor::tailSeconds(const BoardControl& c) noexcept
{
    if (autonomousFilter(c)) return std::numeric_limits<double>::infinity();
    switch (c.effect)
    {
        case Effect::off: return c.model == Model::classic ? 0 : .25;
        case Effect::looper: return c.cv2 >= 128 ? std::numeric_limits<double>::infinity() : .25;
        case Effect::pitch: return .3;
        case Effect::combPositive:
            // AVR floor multiplication can sustain negative one-code patterns
            // for ANY positive feedback, even after a geometric decay estimate.
            return c.cv2 != 0 ? std::numeric_limits<double>::infinity() : .3;
        case Effect::combNegative: return 100.;
        case Effect::delay: return 1.5;
        case Effect::delayFeedback: case Effect::delayDub:
        case Effect::crushDelayFeedback: case Effect::crushDelayDub:
            return std::numeric_limits<double>::infinity();
        case Effect::delay16: case Effect::delay12: case Effect::delay8: case Effect::delay3_16:
            return c.cv1 != 0 ? std::numeric_limits<double>::infinity() : 2.;
        default: return .25;
    }
}
void BoardProcessor::observeTail(const FloatBlock& samples, const BoardControl& c, bool driven) noexcept
{
    constexpr double seconds = static_cast<double>(blockSize) / sampleRate;
    sinceInput_ = driven ? 0 : sinceInput_ + seconds;
    bool varying = false;
    for (const float sample : samples)
    {
        varying = varying || std::abs(sample - lastOutput_) > 1.e-8f;
        lastOutput_ = sample;
    }
    quietTime_ = varying ? 0 : quietTime_ + seconds;
    // More than the longest delay, so a quiet gap cannot truncate a later echo.
    // Constant quantization bias is left to the existing host DC drain.
    tailActive_ = sinceInput_ < 2. || quietTime_ < 2.;
    if (c.effect == Effect::off && c.model == Model::classic) tailActive_ = false;
}
bool BoardProcessor::needsAudio(const BoardControl& c) const noexcept
{
    // Recording advances even through silence; only empty replay may sleep.
    return tailActive_ || (c.effect == Effect::looper && (c.cv2 < 128 || hasValidLoop()))
        || autonomousFilter(c);
}
}
