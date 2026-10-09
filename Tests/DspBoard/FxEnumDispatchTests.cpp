// Copyright 2026 MontroneDSP. SPDX-License-Identifier: GPL-3.0-or-later
// Exhaustive legal Effect dispatch and frozen tailSeconds table.
#include "Engine/DspBoard/BoardProcessor.h"

#include <cmath>
#include <cstdio>
#include <limits>

namespace {
using namespace swaraxt::board;

int gFailures = 0;

void expect(bool ok, const char* name)
{
    if (! ok)
    {
        std::printf("FAIL: %s\n", name);
        ++gFailures;
        return;
    }
    std::printf("PASS: %s\n", name);
}

constexpr Effect kLegal[] = {
    Effect::off, Effect::distortion, Effect::crush, Effect::combPositive, Effect::combNegative,
    Effect::ringMod, Effect::delay, Effect::delayFeedback, Effect::delayDub,
    Effect::crushDelayFeedback, Effect::crushDelayDub, Effect::delay16, Effect::delay12,
    Effect::delay8, Effect::delay3_16, Effect::looper, Effect::pitch
};

double expectedTail(Effect effect, std::uint8_t cv1, std::uint8_t cv2) noexcept
{
    BoardControl c;
    c.model = Model::classic;
    c.effect = effect;
    c.cv1 = cv1;
    c.cv2 = cv2;
    switch (effect)
    {
        case Effect::off: return 0;
        case Effect::looper: return cv2 >= 128 ? std::numeric_limits<double>::infinity() : .25;
        case Effect::pitch: return .3;
        case Effect::combPositive: return cv2 != 0 ? std::numeric_limits<double>::infinity() : .3;
        case Effect::combNegative: return 100.;
        case Effect::delay: return 1.5;
        case Effect::delayFeedback: case Effect::delayDub:
        case Effect::crushDelayFeedback: case Effect::crushDelayDub:
            return std::numeric_limits<double>::infinity();
        case Effect::delay16: case Effect::delay12: case Effect::delay8: case Effect::delay3_16:
            return cv1 != 0 ? std::numeric_limits<double>::infinity() : 2.;
        case Effect::distortion: case Effect::crush: case Effect::ringMod:
            return .25;
    }
    return .25;
}

bool sameDouble(double a, double b) noexcept
{
    if (std::isinf(a) && std::isinf(b))
        return true;
    return a == b;
}

void testTailTable()
{
    for (Effect effect : kLegal)
    {
        for (const std::uint8_t cv1 : { std::uint8_t { 0 }, std::uint8_t { 128 }, std::uint8_t { 255 } })
        {
            for (const std::uint8_t cv2 : { std::uint8_t { 0 }, std::uint8_t { 127 }, std::uint8_t { 128 }, std::uint8_t { 255 } })
            {
                BoardControl c;
                c.model = Model::classic;
                c.effect = effect;
                c.cv1 = cv1;
                c.cv2 = cv2;
                const double got = BoardProcessor::tailSeconds(c);
                const double want = expectedTail(effect, cv1, cv2);
                expect(sameDouble(got, want), "tailSeconds legal effect table");
                if (gFailures != 0)
                    return;
            }
        }
    }
    BoardControl invalid;
    invalid.model = Model::classic;
    invalid.effect = static_cast<Effect>(127);
    expect(BoardProcessor::tailSeconds(invalid) == .25, "invalid enum tail fallback is 0.25");
}

void testProcessFinite()
{
    for (Effect effect : kLegal)
    {
        BoardProcessor p;
        BoardControl c;
        c.model = Model::classic;
        c.effect = effect;
        c.cv1 = 128;
        c.cv2 = 64;
        p.reset(effect);
        FloatBlock x {};
        x[0] = 0.25f;
        p.processClassicFx(x, c);
        int invalid = 0;
        for (float s : x)
            if (! std::isfinite(s))
                ++invalid;
        expect(invalid == 0, "legal effect process finite");
        if (gFailures != 0)
            return;
    }
}

void testInvalidIsNoOp()
{
    BoardEffects fx;
    Block samples {};
    samples[0] = 100;
    const auto before = samples;
    fx.process(samples, static_cast<Effect>(127), 128, 64, 120);
    expect(samples == before, "invalid enum does not become a delay or other program");
}

void testOffIdentity()
{
    BoardProcessor p;
    BoardControl c;
    c.model = Model::classic;
    c.effect = Effect::off;
    FloatBlock x {};
    x.fill(0.1234567f);
    const auto identical = x;
    p.processClassicFx(x, c);
    expect(x == identical, "Classic Off bypass identity");
}
}

int main()
{
    testTailTable();
    testProcessFinite();
    testInvalidIsNoOp();
    testOffIdentity();
    std::printf("FX enum exhaustive dispatch failures=%d\n", gFailures);
    return gFailures == 0 ? 0 : 1;
}
