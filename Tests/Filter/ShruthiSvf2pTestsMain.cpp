// Copyright 2026 MontroneDSP.
// SPDX-License-Identifier: GPL-3.0-or-later
// Experimental Shruthi Dual-SVF section: control laws, modes, stability.

#include "Engine/Filter/ShruthiSvf2p.h"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <limits>
#include <vector>

namespace {

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

std::uint8_t panelCutoffByte(int panel0to127)
{
    const int code = panel0to127 <= 0 ? 0 : (panel0to127 >= 127 ? 254 : panel0to127 * 2);
    return static_cast<std::uint8_t>(code);
}

std::uint8_t panelResonanceByte(int panel0to63)
{
    const int code = panel0to63 <= 0 ? 0 : (panel0to63 >= 63 ? 252 : panel0to63 * 4);
    return static_cast<std::uint8_t>(code);
}

double goertzelPower(const std::vector<float>& x, double hz, double sampleRate)
{
    const double w = 2.0 * swaraxt::ShruthiSvf2pControl::kPi * hz / sampleRate;
    const double coeff = 2.0 * std::cos(w);
    double s0 = 0.0, s1 = 0.0, s2 = 0.0;
    for (float v : x)
    {
        s0 = static_cast<double>(v) + coeff * s1 - s2;
        s2 = s1;
        s1 = s0;
    }
    return s1 * s1 + s2 * s2 - coeff * s1 * s2;
}

// Standalone section renders own one core and one board. Production Dual does not.
struct ShruthiSvfSectionFixture
{
    swaraxt::ShruthiSvf2p core;
    swaraxt::DualSvfAnalogBoard board;
    double vcaTarget = 0.0;

    void prepare(double rate)
    {
        core.prepare(rate);
        board.prepare(rate);
    }

    void reset()
    {
        core.reset();
        board.reset();
    }

    void setFirmwareCutoff(std::uint8_t value) { core.setFirmwareCutoff(value); }
    void setFirmwareResonance(std::uint8_t value) { core.setFirmwareResonance(value); }
    void setMode(swaraxt::ShruthiSvf2pMode mode) { core.setMode(mode); }

    void setVcaTarget(float vca01)
    {
        const double v = static_cast<double>(vca01);
        vcaTarget = v < 0.0 ? 0.0 : (v > 1.0 ? 1.0 : v);
    }

    void snapControlsForTests()
    {
        core.snapControlsForTests();
        board.snapControl(vcaTarget * swaraxt::ShruthiSvf2pControl::kPwmFullScaleVolts);
    }

    double cutoffHz() const { return core.cutoffHz(); }
    double q() const { return core.q(); }
    double lastBpVolts() const { return core.lastBpVolts(); }

    float process(float mixerUnit, float vcaIn)
    {
        setVcaTarget(vcaIn);
        core.advanceControls();
        const double source = static_cast<double>(mixerUnit) * swaraxt::ShruthiSvf2pControl::kSourceVoltsPerUnit;
        const double selected = core.processSelectedVolts(board.conditionInput(source));
        const double pwm = vcaTarget * swaraxt::ShruthiSvf2pControl::kPwmFullScaleVolts;
        const double outUnit = board.processOutput(selected, 0.0, pwm)
            / swaraxt::ShruthiSvf2pControl::kSourceVoltsPerUnit;
        if (! std::isfinite(outUnit))
        {
            reset();
            return 0.0f;
        }
        return static_cast<float>(outUnit);
    }
};

void testControlLaws()
{
    using C = swaraxt::ShruthiSvf2pControl;
    const double expectedF0 = 1.0 / (2.0 * C::kPi * 33000.0 * 220.0e-12);
    expect(std::abs(C::analogF0Hz() - expectedF0) < 1.0e-9, "f0 = 1/(2 pi R C) from Python R=33k C=220pF");
    expect(std::abs(C::passbandGain() - 1.0) < 1.0e-12, "G = Rg/Ri = 1");
    expect(std::abs(C::kRho - 5.0 / 27.0) < 1.0e-15, "rho = 5/27");
    expect(std::abs(C::kRoOhms - 330000.0) < 1.0e-9, "v03 R_O = 330k");
    expect(std::abs(C::rqOverRo() - 15000.0 / 330000.0) < 1.0e-15, "R_Q/R_O");
    expect(std::abs(C::kClampVolts - 4.3) < 1.0e-12, "anti-series clamp is 3.6 V + 0.7 V");
    expect(std::abs(C::kCutoffCvTauSeconds - 10000.0 * 33.0e-9) < 1.0e-15, "cutoff pole is R10*C11");

    const double fMin = C::cutoffHzFromFirmwareByte(0);
    const double fMax = C::cutoffHzFromFirmwareByte(255);
    const double analogRatio = std::pow(10.0, 1.5 * 2.141);
    expect(std::abs(fMax / fMin - analogRatio) < 1.0e-6, "PWM 0..5 V follows paper 2.141 V 2164 swing");
    expect(std::abs(analogRatio / std::pow(2.0, 128.0 / 12.0) - 1.0) < 0.002,
           "2.141 V is the paper's 128-note approximation");
    expect(fMin > 10.0 && fMin < 20.0, "byte 0 is the analog floor near 13 Hz");
    expect(fMax > 20000.0 && fMax < 24000.0, "byte 255 is analog f0 near 21.9 kHz");

    const int cutoffs[] = { 0, 16, 32, 48, 64, 80, 96, 112, 127 };
    double prev = 0.0;
    for (int panel : cutoffs)
    {
        const double hz = C::cutoffHzFromFirmwareByte(panelCutoffByte(panel));
        expect(std::isfinite(hz) && hz > prev, "cutoff increases with panel code");
        prev = hz;
        const double pwm = C::pwmVoltsFromByte(panelCutoffByte(panel));
        const double vCv = C::kCutoffCvGain * (5.0 - pwm);
        const double expected = C::analogF0Hz() * std::pow(10.0, -1.5 * vCv);
        expect(std::abs(hz - expected) / expected < 1.0e-12, "cutoff matches inverted 2164 law");
    }

    const double vCrit = C::selfOscillationThresholdPwmVolts();
    expect(vCrit > 4.8 && vCrit < 4.9, "330 kΩ singularity near 4.83 V PWM");
    expect(C::selfOscillationThresholdByte() == 246, "singularity ~ PWM byte 246");

    const int resos[] = { 0, 8, 16, 24, 32, 40, 48, 56, 63 };
    double prevQ = 0.0;
    bool sawLossless = false;
    for (int panel : resos)
    {
        const auto byte = panelResonanceByte(panel);
        const double q = C::qFromFirmwareByte(byte);
        const bool osc = C::isSelfOscillatingByte(byte);
        if (osc)
        {
            expect(q == 0.0, "lossless sentinel at/after singularity");
            sawLossless = true;
        }
        else
        {
            expect(q >= prevQ && q >= 0.5, "Q rises through the loaded+R_O law");
            prevQ = q;
            const double vq = C::pwmVoltsFromByte(byte);
            const double expected = 0.5 / (std::pow(10.0, -1.5 * C::kRho * vq) - C::rqOverRo());
            expect(std::abs(q - expected) / expected < 1.0e-10, "Q matches rho/R_O equation");
        }
    }
    expect(sawLossless, "panel 63 crosses self-oscillation");
    expect(! C::isSelfOscillatingByte(panelResonanceByte(61)), "panel 61 is still below the 330 kΩ threshold");
    expect(C::isSelfOscillatingByte(panelResonanceByte(62)), "panel 62 crosses self-oscillation");
}

void testZener()
{
    using C = swaraxt::ShruthiSvf2pControl;
    expect(C::zenerLimit(0.0) == 0.0, "zener odd: zero");
    expect(std::abs(C::zenerLimit(1.0) - 1.0) < 1.0e-15, "zener linear at 1 V");
    expect(std::abs(C::zenerLimit(3.6) - 3.6) < 1.0e-12, "linear through the 3.6 V zener rating");
    expect(C::zenerLimit(4.0) > 3.6 && C::zenerLimit(4.0) < 4.3, "knee between Vz and Vz+Vf");
    expect(C::zenerLimit(20.0) < 4.3 && C::zenerLimit(20.0) > 4.2, "asymptote is 4.3 V");
    expect(std::abs(C::zenerLimit(-4.0) + C::zenerLimit(4.0)) < 1.0e-15, "zener odd symmetry");
}

void testModesAndSlope()
{
    ShruthiSvfSectionFixture svf;
    svf.prepare(swaraxt::ShruthiSvf2pControl::kNativeRate);
    const auto cutoff = panelCutoffByte(64);
    svf.setFirmwareCutoff(cutoff);
    svf.setFirmwareResonance(panelResonanceByte(0));
    svf.setMode(swaraxt::ShruthiSvf2pMode::lowPass);
    svf.snapControlsForTests();
    const double fc = svf.cutoffHz();
    const double sr = swaraxt::ShruthiSvf2pControl::kNativeRate;
    const double lo = fc * 0.25;
    const double hi = std::min(fc * 4.0, sr * 0.2);

    auto measure = [&](swaraxt::ShruthiSvf2pMode mode, double hz) {
        svf.reset();
        svf.setMode(mode);
        svf.setFirmwareCutoff(cutoff);
        svf.setFirmwareResonance(panelResonanceByte(0));
        svf.setVcaTarget(1.0f);
        svf.snapControlsForTests();
        std::vector<float> y;
        y.reserve(8192);
        int invalid = 0;
        for (int i = 0; i < 16384; ++i)
        {
            const float x = static_cast<float>(0.1 * std::sin(2.0 * swaraxt::ShruthiSvf2pControl::kPi * hz * i / sr));
            const float out = svf.process(x, 1.0f);
            if (! std::isfinite(out))
                ++invalid;
            if (i >= 8192)
                y.push_back(out);
        }
        expect(invalid == 0, "mode output finite");
        return goertzelPower(y, hz, sr);
    };

    const double lpLo = measure(swaraxt::ShruthiSvf2pMode::lowPass, lo);
    const double lpHi = measure(swaraxt::ShruthiSvf2pMode::lowPass, hi);
    const double lpDropDb = 10.0 * std::log10(lpLo / std::max(lpHi, 1.0e-30));
    expect(lpDropDb > 18.0, "LP ~12 dB/oct (two octaves => ~24 dB, allow TPT warp)");

    const double hpLo = measure(swaraxt::ShruthiSvf2pMode::highPass, lo);
    const double hpHi = measure(swaraxt::ShruthiSvf2pMode::highPass, hi);
    std::printf("HP power lo=%.6g hi=%.6g  LP drop=%.2f dB\n", hpLo, hpHi, lpDropDb);
    expect(hpHi > hpLo, "HP rises toward the stop of LP");

    const double bpFc = measure(swaraxt::ShruthiSvf2pMode::bandPass, fc);
    const double bpLo = measure(swaraxt::ShruthiSvf2pMode::bandPass, lo);
    expect(bpFc > bpLo, "BP peaks nearer cutoff than a far-below probe");
}

void testSelfOscillation()
{
    ShruthiSvfSectionFixture svf;
    svf.prepare(swaraxt::ShruthiSvf2pControl::kNativeRate);
    svf.setFirmwareCutoff(panelCutoffByte(80));
    svf.setFirmwareResonance(panelResonanceByte(63));
    svf.setMode(swaraxt::ShruthiSvf2pMode::bandPass);
    svf.snapControlsForTests();
    expect(swaraxt::ShruthiSvf2pControl::isSelfOscillatingByte(panelResonanceByte(63)),
           "panel 63 is lossless");

    double peak = 0.0;
    int invalid = 0;
    const double sr = swaraxt::ShruthiSvf2pControl::kNativeRate;
    for (int i = 0; i < static_cast<int>(sr * 2.0); ++i)
    {
        const float excite = i == 0 ? 0.2f : 0.0f;
        const float y = svf.process(excite, 1.0f);
        if (! std::isfinite(y))
            ++invalid;
        peak = std::max(peak, std::abs(static_cast<double>(y)));
    }
    expect(invalid == 0, "self-osc 2 s: no NaN/Inf");
    expect(peak > 0.001 && peak < 8.0, "self-osc bounded and alive after impulse");
    expect(std::abs(svf.lastBpVolts()) <= swaraxt::ShruthiSvf2pControl::kClampVolts + 1.0e-6,
           "BP capacitor within the anti-series clamp");
}

std::uint64_t hashSamples(const std::vector<float>& samples)
{
    std::uint64_t h = 1469598103934665603ull;
    for (float sample : samples)
    {
        const float finite = std::isfinite(sample) ? sample : 0.0f;
        const auto q = static_cast<std::int32_t>(std::lround(
            std::clamp(finite, -8.0f, 8.0f) * 8388608.0f));
        std::uint8_t bytes[sizeof(q)];
        std::memcpy(bytes, &q, sizeof(q));
        for (std::uint8_t b : bytes)
        {
            h ^= b;
            h *= 1099511628211ull;
        }
    }
    return h;
}

std::vector<float> renderSection1(swaraxt::ShruthiSvf2pMode mode, int cutoffPanel, int resPanel,
                                  bool impulseThenSilence, bool automateCutoff, bool automateRes)
{
    ShruthiSvfSectionFixture svf;
    svf.prepare(swaraxt::ShruthiSvf2pControl::kNativeRate);
    svf.setMode(mode);
    svf.setFirmwareCutoff(panelCutoffByte(cutoffPanel));
    svf.setFirmwareResonance(panelResonanceByte(resPanel));
    svf.setVcaTarget(1.0f);
    svf.snapControlsForTests();
    const double sr = swaraxt::ShruthiSvf2pControl::kNativeRate;
    std::vector<float> y;
    y.reserve(4096);
    for (int i = 0; i < 4096; ++i)
    {
        if (automateCutoff)
            svf.setFirmwareCutoff(static_cast<std::uint8_t>((64 + (i / 8)) & 255));
        if (automateRes)
            svf.setFirmwareResonance(static_cast<std::uint8_t>((i / 16) & 255));
        float x = 0.0f;
        if (impulseThenSilence)
            x = i == 0 ? 0.2f : 0.0f;
        else
            x = static_cast<float>(0.1 * std::sin(2.0 * swaraxt::ShruthiSvf2pControl::kPi * 220.0 * i / sr));
        y.push_back(svf.process(x, 1.0f));
    }
    return y;
}

// SVF SECTION 1 SONIC REFERENCE — freeze hashes of the approved Prototype 1 core.
void testSection1SonicReference()
{
    struct Case { const char* name; std::uint64_t expected; std::vector<float> audio; };
    const Case cases[] = {
        { "S1 LP low resonance", 0x0beac7d2e4f8d5faull, renderSection1(swaraxt::ShruthiSvf2pMode::lowPass, 64, 8, false, false, false) },
        { "S1 LP high resonance", 0x73c446eb5d9176f9ull, renderSection1(swaraxt::ShruthiSvf2pMode::lowPass, 64, 48, false, false, false) },
        { "S1 LP self-oscillation", 0x4db9926e35e68d3full, renderSection1(swaraxt::ShruthiSvf2pMode::lowPass, 80, 63, true, false, false) },
        { "S1 BP", 0x567c7e9c2efa316bull, renderSection1(swaraxt::ShruthiSvf2pMode::bandPass, 64, 24, false, false, false) },
        { "S1 HP", 0x4639d29e43353d48ull, renderSection1(swaraxt::ShruthiSvf2pMode::highPass, 64, 16, false, false, false) },
        { "S1 cutoff automation", 0x70e64b90521e3a25ull, renderSection1(swaraxt::ShruthiSvf2pMode::lowPass, 64, 16, false, true, false) },
        { "S1 resonance automation", 0x8134986deb2fe090ull, renderSection1(swaraxt::ShruthiSvf2pMode::lowPass, 64, 16, false, false, true) },
    };
    for (const auto& c : cases)
    {
        const auto h = hashSamples(c.audio);
        std::printf("SVF SECTION 1 SONIC REFERENCE %s hash=%016llx\n", c.name,
                    static_cast<unsigned long long>(h));
        expect(h == c.expected, c.name);
        int invalid = 0;
        for (float s : c.audio)
            if (! std::isfinite(s))
                ++invalid;
        expect(invalid == 0, "section 1 reference finite");
    }
}

std::vector<float> renderDual(swaraxt::ShruthiSvfDualRouting routing,
                              swaraxt::ShruthiSvf2pMode mode1,
                              swaraxt::ShruthiSvf2pMode mode2,
                              int cutoffPanel, int resPanel,
                              bool impulseThenSilence)
{
    swaraxt::ShruthiSvfDual dual;
    dual.prepare(swaraxt::ShruthiSvf2pControl::kNativeRate);
    dual.setRouting(routing);
    dual.setMode1(mode1);
    dual.setMode2(mode2);
    dual.setFirmwareCutoff1(panelCutoffByte(cutoffPanel));
    dual.setFirmwareResonance1(panelResonanceByte(resPanel));
    dual.setVcaTarget(1.0f);
    dual.snapControlsForTests();
    const double sr = swaraxt::ShruthiSvf2pControl::kNativeRate;
    std::vector<float> y;
    y.reserve(4096);
    for (int i = 0; i < 4096; ++i)
    {
        float x = 0.0f;
        if (impulseThenSilence)
            x = i == 0 ? 0.2f : 0.0f;
        else
            x = static_cast<float>(0.1 * std::sin(2.0 * swaraxt::ShruthiSvf2pControl::kPi * 220.0 * i / sr));
        y.push_back(dual.process(x, 1.0f));
    }
    return y;
}

void testDualSection1MatchesFrozenReference()
{
    struct Case { const char* name; std::uint64_t expected; std::vector<float> audio; };
    const Case cases[] = {
        { "Dual S1 LP low resonance", 0x0beac7d2e4f8d5faull,
          renderDual(swaraxt::ShruthiSvfDualRouting::section1,
                     swaraxt::ShruthiSvf2pMode::lowPass, swaraxt::ShruthiSvf2pMode::lowPass,
                     64, 8, false) },
        { "Dual S1 LP high resonance", 0x73c446eb5d9176f9ull,
          renderDual(swaraxt::ShruthiSvfDualRouting::section1,
                     swaraxt::ShruthiSvf2pMode::lowPass, swaraxt::ShruthiSvf2pMode::lowPass,
                     64, 48, false) },
        { "Dual S1 LP self-oscillation", 0x4db9926e35e68d3full,
          renderDual(swaraxt::ShruthiSvfDualRouting::section1,
                     swaraxt::ShruthiSvf2pMode::lowPass, swaraxt::ShruthiSvf2pMode::lowPass,
                     80, 63, true) },
        { "Dual S1 BP", 0x567c7e9c2efa316bull,
          renderDual(swaraxt::ShruthiSvfDualRouting::section1,
                     swaraxt::ShruthiSvf2pMode::bandPass, swaraxt::ShruthiSvf2pMode::lowPass,
                     64, 24, false) },
        { "Dual S1 HP", 0x4639d29e43353d48ull,
          renderDual(swaraxt::ShruthiSvfDualRouting::section1,
                     swaraxt::ShruthiSvf2pMode::highPass, swaraxt::ShruthiSvf2pMode::lowPass,
                     64, 16, false) },
    };
    for (const auto& c : cases)
    {
        const auto h = hashSamples(c.audio);
        std::printf("DUAL SECTION 1 vs SVF SECTION 1 SONIC REFERENCE %s hash=%016llx\n",
                    c.name, static_cast<unsigned long long>(h));
        expect(h == c.expected, c.name);
    }
}

void testPrototype2DualSonicReference()
{
    struct Case { const char* name; std::uint64_t expected; std::vector<float> audio; };
    const Case cases[] = {
        { "P2 serial LP>LP", 0x3bce33d3b4f92cfeull,
          renderDual(swaraxt::ShruthiSvfDualRouting::serial,
                     swaraxt::ShruthiSvf2pMode::lowPass, swaraxt::ShruthiSvf2pMode::lowPass,
                     48, 16, false) },
        { "P2 serial LP>BP", 0x5a45f49ac67bc1b7ull,
          renderDual(swaraxt::ShruthiSvfDualRouting::serial,
                     swaraxt::ShruthiSvf2pMode::lowPass, swaraxt::ShruthiSvf2pMode::bandPass,
                     48, 16, false) },
        { "P2 serial BP>LP", 0x5a45f49ac67bc1b7ull,
          renderDual(swaraxt::ShruthiSvfDualRouting::serial,
                     swaraxt::ShruthiSvf2pMode::bandPass, swaraxt::ShruthiSvf2pMode::lowPass,
                     48, 16, false) },
        { "P2 serial HP>LP", 0x76ec2525c5b0dc86ull,
          renderDual(swaraxt::ShruthiSvfDualRouting::serial,
                     swaraxt::ShruthiSvf2pMode::highPass, swaraxt::ShruthiSvf2pMode::lowPass,
                     48, 16, false) },
        { "P2 parallel LP+HP", 0x32bfb838a7c6d107ull,
          renderDual(swaraxt::ShruthiSvfDualRouting::parallel,
                     swaraxt::ShruthiSvf2pMode::lowPass, swaraxt::ShruthiSvf2pMode::highPass,
                     48, 16, false) },
        { "P2 parallel LP+BP", 0xb13e3f67aa045d03ull,
          renderDual(swaraxt::ShruthiSvfDualRouting::parallel,
                     swaraxt::ShruthiSvf2pMode::lowPass, swaraxt::ShruthiSvf2pMode::bandPass,
                     48, 16, false) },
        { "P2 parallel BP+HP", 0x084195624b690131ull,
          renderDual(swaraxt::ShruthiSvfDualRouting::parallel,
                     swaraxt::ShruthiSvf2pMode::bandPass, swaraxt::ShruthiSvf2pMode::highPass,
                     48, 16, false) },
    };
    for (const auto& c : cases)
    {
        const auto h = hashSamples(c.audio);
        std::printf("PROTOTYPE 2 DUAL SONIC REFERENCE %s hash=%016llx\n", c.name,
                    static_cast<unsigned long long>(h));
        expect(h == c.expected, c.name);
        int invalid = 0;
        for (float s : c.audio)
            if (! std::isfinite(s))
                ++invalid;
        expect(invalid == 0, "prototype 2 dual reference finite");
    }
}

void testSection2IndependentAndIdentical()
{
    const auto s1 = renderSection1(swaraxt::ShruthiSvf2pMode::lowPass, 64, 16, false, false, false);
    ShruthiSvfSectionFixture s2;
    s2.prepare(swaraxt::ShruthiSvf2pControl::kNativeRate);
    s2.setMode(swaraxt::ShruthiSvf2pMode::lowPass);
    s2.setFirmwareCutoff(panelCutoffByte(64));
    s2.setFirmwareResonance(panelResonanceByte(16));
    s2.setVcaTarget(1.0f);
    s2.snapControlsForTests();
    const double sr = swaraxt::ShruthiSvf2pControl::kNativeRate;
    std::vector<float> y;
    y.reserve(4096);
    for (int i = 0; i < 4096; ++i)
    {
        const float x = static_cast<float>(0.1 * std::sin(2.0 * swaraxt::ShruthiSvf2pControl::kPi * 220.0 * i / sr));
        y.push_back(s2.process(x, 1.0f));
    }
    expect(hashSamples(s1) == hashSamples(y), "independent S2 instance matches S1 core");
}

void testSerialAndParallelTopology()
{
    const auto s1Lp = renderDual(swaraxt::ShruthiSvfDualRouting::section1,
                                 swaraxt::ShruthiSvf2pMode::lowPass, swaraxt::ShruthiSvf2pMode::lowPass,
                                 48, 8, false);
    const auto serialLpLp = renderDual(swaraxt::ShruthiSvfDualRouting::serial,
                                       swaraxt::ShruthiSvf2pMode::lowPass, swaraxt::ShruthiSvf2pMode::lowPass,
                                       48, 8, false);
    const auto parallelLpHp = renderDual(swaraxt::ShruthiSvfDualRouting::parallel,
                                         swaraxt::ShruthiSvf2pMode::lowPass, swaraxt::ShruthiSvf2pMode::highPass,
                                         32, 8, false);
    const auto s1LpLowCut = renderDual(swaraxt::ShruthiSvfDualRouting::section1,
                                       swaraxt::ShruthiSvf2pMode::lowPass, swaraxt::ShruthiSvf2pMode::lowPass,
                                       32, 8, false);
    const auto serialLpBp = renderDual(swaraxt::ShruthiSvfDualRouting::serial,
                                       swaraxt::ShruthiSvf2pMode::lowPass, swaraxt::ShruthiSvf2pMode::bandPass,
                                       48, 16, false);
    const auto parallelLpBp = renderDual(swaraxt::ShruthiSvfDualRouting::parallel,
                                         swaraxt::ShruthiSvf2pMode::lowPass, swaraxt::ShruthiSvf2pMode::bandPass,
                                         48, 16, false);
    const auto parallelBpHp = renderDual(swaraxt::ShruthiSvfDualRouting::parallel,
                                         swaraxt::ShruthiSvf2pMode::bandPass, swaraxt::ShruthiSvf2pMode::highPass,
                                         48, 16, false);

    auto rms = [](const std::vector<float>& x) {
        double s = 0.0;
        for (float v : x)
            s += static_cast<double>(v) * static_cast<double>(v);
        return std::sqrt(s / static_cast<double>(x.size()));
    };
    auto highEnergy = [](const std::vector<float>& x) {
        double s = 0.0;
        for (size_t i = 1; i < x.size(); ++i)
        {
            const double d = static_cast<double>(x[i] - x[i - 1]);
            s += d * d;
        }
        return s;
    };

    expect(rms(serialLpLp) < rms(s1Lp) * 0.95, "serial LP>LP is darker/quieter than S1 LP");
    expect(highEnergy(serialLpLp) < highEnergy(s1Lp), "serial LP>LP reduces HF energy vs S1");
    expect(rms(parallelLpHp) > rms(s1LpLowCut), "parallel LP+HP keeps highs that S1 LP cuts");
    expect(hashSamples(serialLpLp) != hashSamples(s1Lp), "serial differs from section1");
    expect(hashSamples(parallelLpHp) != hashSamples(serialLpLp), "parallel differs from serial");
    expect(hashSamples(serialLpBp) != 0, "serial LP>BP produced audio");
    expect(hashSamples(parallelLpBp) != hashSamples(parallelBpHp), "parallel combinations differ");

    int invalid = 0;
    for (const auto* buf : { &s1Lp, &serialLpLp, &parallelLpHp, &serialLpBp, &parallelLpBp, &parallelBpHp })
        for (float s : *buf)
            if (! std::isfinite(s) || std::abs(s) > 16.0f)
                ++invalid;
    expect(invalid == 0, "serial/parallel outputs finite and bounded");
}

void testRoutingSwitchSafety()
{
    swaraxt::ShruthiSvfDual dual;
    dual.prepare(swaraxt::ShruthiSvf2pControl::kNativeRate);
    dual.setFirmwareCutoff1(panelCutoffByte(80));
    dual.setFirmwareResonance1(panelResonanceByte(63));
    dual.snapControlsForTests();
    const swaraxt::ShruthiSvfDualRouting routes[] = {
        swaraxt::ShruthiSvfDualRouting::section1,
        swaraxt::ShruthiSvfDualRouting::serial,
        swaraxt::ShruthiSvfDualRouting::parallel
    };
    const swaraxt::ShruthiSvf2pMode modes[] = {
        swaraxt::ShruthiSvf2pMode::lowPass,
        swaraxt::ShruthiSvf2pMode::bandPass,
        swaraxt::ShruthiSvf2pMode::highPass
    };
    int invalid = 0;
    const double sr = swaraxt::ShruthiSvf2pControl::kNativeRate;
    for (int i = 0; i < 2048; ++i)
    {
        dual.setRouting(routes[i % 3]);
        dual.setMode1(modes[i % 3]);
        dual.setMode2(modes[(i / 2) % 3]);
        dual.setFirmwareCutoff1(static_cast<std::uint8_t>(i & 255));
        dual.setFirmwareResonance1(static_cast<std::uint8_t>((i * 5) & 255));
        const float x = static_cast<float>(0.3 * std::sin(2.0 * swaraxt::ShruthiSvf2pControl::kPi * 330.0 * i / sr));
        const float y = dual.process(x, (i % 3) == 0 ? 1.0f : 0.4f);
        if (! std::isfinite(y) || std::abs(y) > 16.0f)
            ++invalid;
    }
    expect(invalid == 0, "routing/mode switch torture stays finite");
}

void testV03SelfOscillation()
{
    ShruthiSvfSectionFixture svf;
    svf.prepare(swaraxt::ShruthiSvf2pControl::kNativeRate);
    svf.setFirmwareCutoff(panelCutoffByte(80));
    svf.setFirmwareResonance(panelResonanceByte(63));
    svf.setMode(swaraxt::ShruthiSvf2pMode::bandPass);
    svf.snapControlsForTests();
    double peak = 0.0, maxBp = 0.0;
    int invalid = 0;
    const double sr = swaraxt::ShruthiSvf2pControl::kNativeRate;
    for (int i = 0; i < static_cast<int>(sr); ++i)
    {
        const float y = svf.process(i == 0 ? 0.2f : 0.0f, 1.0f);
        if (! std::isfinite(y))
            ++invalid;
        peak = std::max(peak, std::abs(static_cast<double>(y)));
        maxBp = std::max(maxBp, std::abs(svf.lastBpVolts()));
    }
    expect(invalid == 0, "v03 self-osc 1 s finite");
    expect(peak > 0.001 && peak < 8.0, "v03 self-osc bounded and alive");
    expect(maxBp <= swaraxt::ShruthiSvf2pControl::kClampVolts + 1.0e-6,
           "BP stays inside the anti-series clamp");

    swaraxt::ShruthiSvfDual dual;
    dual.prepare(sr);
    int switchInvalid = 0;
    for (int i = 0; i < 2048; ++i)
    {
        dual.setFirmwareCutoff1(static_cast<std::uint8_t>(i & 255));
        dual.setFirmwareResonance1(static_cast<std::uint8_t>((i * 7) & 255));
        const float x = static_cast<float>(0.25 * std::sin(2.0 * swaraxt::ShruthiSvf2pControl::kPi * 220.0 * i / sr));
        const float y = dual.process(x, 1.0f);
        if (! std::isfinite(y) || std::abs(y) > 16.0f)
            ++switchInvalid;
    }
    expect(switchInvalid == 0, "cutoff and resonance automation stays finite");
}

void reportStats(const char* name, const std::vector<float>& audio, double maxBp, double sampleRate)
{
    double peak = 0.0, energy = 0.0;
    int invalid = 0, crossings = 0;
    float prev = 0.0f;
    const int start = static_cast<int>(audio.size() / 2);
    for (int i = 0; i < static_cast<int>(audio.size()); ++i)
    {
        const float s = audio[static_cast<size_t>(i)];
        if (! std::isfinite(s))
            ++invalid;
        peak = std::max(peak, std::abs(static_cast<double>(s)));
        energy += static_cast<double>(s) * static_cast<double>(s);
        if (i >= start && ((prev < 0.0f && s >= 0.0f) || (prev > 0.0f && s <= 0.0f)))
            ++crossings;
        prev = s;
    }
    const double rms = std::sqrt(energy / static_cast<double>(audio.size()));
    const double hz = 0.5 * static_cast<double>(crossings)
        * sampleRate / static_cast<double>(audio.size() - start);
    std::printf("MEAS %s peak=%.4f rms=%.4f maxBp=%.3f zcHz=%.1f invalid=%d\n",
                name, peak, rms, maxBp, hz, invalid);
    expect(invalid == 0, name);
}

void testApprovedVsHardwareMeasurements()
{
    const double sr = swaraxt::ShruthiSvf2pControl::kNativeRate;
    struct Job
    {
        const char* name;
        swaraxt::ShruthiSvfDualRouting routing;
        swaraxt::ShruthiSvf2pMode m1;
        swaraxt::ShruthiSvf2pMode m2;
        int cut, res;
        bool impulse;
    };
    const Job jobs[] = {
        { "s1_lp_low", swaraxt::ShruthiSvfDualRouting::section1,
          swaraxt::ShruthiSvf2pMode::lowPass, swaraxt::ShruthiSvf2pMode::lowPass, 64, 8, false },
        { "s1_lp_med", swaraxt::ShruthiSvfDualRouting::section1,
          swaraxt::ShruthiSvf2pMode::lowPass, swaraxt::ShruthiSvf2pMode::lowPass, 64, 32, false },
        { "s1_lp_high", swaraxt::ShruthiSvfDualRouting::section1,
          swaraxt::ShruthiSvf2pMode::lowPass, swaraxt::ShruthiSvf2pMode::lowPass, 64, 48, false },
        { "s1_lp_near_thr", swaraxt::ShruthiSvfDualRouting::section1,
          swaraxt::ShruthiSvf2pMode::lowPass, swaraxt::ShruthiSvf2pMode::lowPass, 64, 53, false },
        { "s1_lp_selfosc", swaraxt::ShruthiSvfDualRouting::section1,
          swaraxt::ShruthiSvf2pMode::lowPass, swaraxt::ShruthiSvf2pMode::lowPass, 80, 63, true },
        { "s1_bp", swaraxt::ShruthiSvfDualRouting::section1,
          swaraxt::ShruthiSvf2pMode::bandPass, swaraxt::ShruthiSvf2pMode::lowPass, 64, 24, false },
        { "s1_hp", swaraxt::ShruthiSvfDualRouting::section1,
          swaraxt::ShruthiSvf2pMode::highPass, swaraxt::ShruthiSvf2pMode::lowPass, 64, 16, false },
        { "serial_lp_lp", swaraxt::ShruthiSvfDualRouting::serial,
          swaraxt::ShruthiSvf2pMode::lowPass, swaraxt::ShruthiSvf2pMode::lowPass, 48, 16, false },
        { "serial_lp_bp", swaraxt::ShruthiSvfDualRouting::serial,
          swaraxt::ShruthiSvf2pMode::lowPass, swaraxt::ShruthiSvf2pMode::bandPass, 48, 16, false },
        { "serial_bp_lp", swaraxt::ShruthiSvfDualRouting::serial,
          swaraxt::ShruthiSvf2pMode::bandPass, swaraxt::ShruthiSvf2pMode::lowPass, 48, 16, false },
        { "serial_hp_lp", swaraxt::ShruthiSvfDualRouting::serial,
          swaraxt::ShruthiSvf2pMode::highPass, swaraxt::ShruthiSvf2pMode::lowPass, 48, 16, false },
        { "par_lp_hp", swaraxt::ShruthiSvfDualRouting::parallel,
          swaraxt::ShruthiSvf2pMode::lowPass, swaraxt::ShruthiSvf2pMode::highPass, 48, 16, false },
        { "par_lp_bp", swaraxt::ShruthiSvfDualRouting::parallel,
          swaraxt::ShruthiSvf2pMode::lowPass, swaraxt::ShruthiSvf2pMode::bandPass, 48, 16, false },
        { "par_bp_hp", swaraxt::ShruthiSvfDualRouting::parallel,
          swaraxt::ShruthiSvf2pMode::bandPass, swaraxt::ShruthiSvf2pMode::highPass, 48, 16, false },
    };
    for (const auto& job : jobs)
    {
        swaraxt::ShruthiSvfDual dual;
        dual.prepare(sr);
        dual.setRouting(job.routing);
        dual.setMode1(job.m1);
        dual.setMode2(job.m2);
        dual.setFirmwareCutoff1(panelCutoffByte(job.cut));
        dual.setFirmwareResonance1(panelResonanceByte(job.res));
        dual.setVcaTarget(1.0f);
        dual.snapControlsForTests();
        std::vector<float> y;
        y.reserve(4096);
        double maxBp = 0.0;
        for (int i = 0; i < 4096; ++i)
        {
            float x = 0.0f;
            if (job.impulse)
                x = i == 0 ? 0.2f : 0.0f;
            else
                x = static_cast<float>(0.1 * std::sin(2.0 * swaraxt::ShruthiSvf2pControl::kPi * 220.0 * i / sr));
            y.push_back(dual.process(x, 1.0f));
            maxBp = std::max(maxBp, std::abs(dual.section1().lastBpVolts()));
            maxBp = std::max(maxBp, std::abs(dual.section2().lastBpVolts()));
        }
        reportStats(job.name, y, maxBp, sr);
        expect(maxBp <= swaraxt::ShruthiSvf2pControl::kClampVolts + 1.0e-4, job.name);
    }
}

void testSilenceAndAutomation()
{
    ShruthiSvfSectionFixture svf;
    svf.prepare(swaraxt::ShruthiSvf2pControl::kNativeRate);
    svf.setFirmwareCutoff(panelCutoffByte(64));
    svf.setFirmwareResonance(panelResonanceByte(8));
    svf.snapControlsForTests();
    double peak = 0.0;
    for (int i = 0; i < 8192; ++i)
        peak = std::max(peak, std::abs(static_cast<double>(svf.process(0.0f, 1.0f))));
    expect(peak < 1.0e-6, "low-Q silence stays silent");

    int invalid = 0;
    const swaraxt::ShruthiSvf2pMode modes[] = {
        swaraxt::ShruthiSvf2pMode::lowPass,
        swaraxt::ShruthiSvf2pMode::bandPass,
        swaraxt::ShruthiSvf2pMode::highPass
    };
    for (int i = 0; i < 4096; ++i)
    {
        svf.setFirmwareCutoff(static_cast<std::uint8_t>(i & 255));
        svf.setFirmwareResonance(static_cast<std::uint8_t>((i * 3) & 255));
        svf.setMode(modes[i % 3]);
        const float x = static_cast<float>(((i * 17) & 255) / 128.0 - 1.0);
        const float y = svf.process(x, (i % 2) == 0 ? 1.0f : 0.2f);
        if (! std::isfinite(y) || std::abs(y) > 16.0f)
            ++invalid;
    }
    expect(invalid == 0, "rapid cutoff/res/mode automation stays finite");
}

void testExperimentalSvfPublication()
{
    using S = swaraxt::ExperimentalSvfSelection;
    S packed;
    packed.enabled = true;
    packed.routing = swaraxt::ShruthiSvfDualRouting::serial;
    packed.mode1 = swaraxt::ShruthiSvf2pMode::bandPass;
    packed.mode2 = swaraxt::ShruthiSvf2pMode::highPass;
    const auto historical = S::pack(packed) | (1u << S::kRetiredCalibrationBit);
    const auto roundTrip = S::unpack(historical);
    expect(roundTrip.enabled, "packed selection preserves enabled");
    expect(roundTrip.routing == packed.routing, "packed selection preserves routing");
    expect(roundTrip.mode1 == packed.mode1, "packed selection preserves mode1");
    expect(roundTrip.mode2 == packed.mode2, "packed selection preserves mode2");
    expect((S::pack(roundTrip) & (1u << S::kRetiredCalibrationBit)) == 0,
           "retired calibration bit is not rewritten into new state");

    swaraxt::ExperimentalSvfControl control;
    swaraxt::ShruthiSvfDual dual;
    dual.prepare(swaraxt::ShruthiSvf2pControl::kNativeRate);
    bool enabled = false;
    control.patch([](S& s) {
        s.enabled = true;
        s.routing = swaraxt::ShruthiSvfDualRouting::parallel;
        s.mode1 = swaraxt::ShruthiSvf2pMode::highPass;
    });
    control.patch([](S& s) {
        s.mode2 = swaraxt::ShruthiSvf2pMode::bandPass;
    });
    const auto sel = control.selection();
    expect(sel.enabled && sel.routing == swaraxt::ShruthiSvfDualRouting::parallel
               && sel.mode1 == swaraxt::ShruthiSvf2pMode::highPass
               && sel.mode2 == swaraxt::ShruthiSvf2pMode::bandPass,
           "CAS patch preserves unrelated selector fields");
    control.applyOnAudioThread(dual, enabled);
    expect(enabled, "audio apply publishes enabled");
    expect(dual.routing() == swaraxt::ShruthiSvfDualRouting::parallel, "audio apply routing");
    expect(dual.mode1() == swaraxt::ShruthiSvf2pMode::highPass, "audio apply mode1");

    dual.setFirmwareCutoff1(panelCutoffByte(64));
    dual.setFirmwareResonance1(panelResonanceByte(48));
    dual.setVcaTarget(1.0f);
    dual.snapControlsForTests();
    const double sr = swaraxt::ShruthiSvf2pControl::kNativeRate;
    for (int i = 0; i < 256; ++i)
    {
        const float x = static_cast<float>(0.1 * std::sin(2.0 * swaraxt::ShruthiSvf2pControl::kPi * 220.0 * i / sr));
        dual.process(x, 1.0f);
    }
    const double bpBefore = dual.section1().lastBpVolts();
    expect(std::abs(bpBefore) > 1.0e-6, "rendered section has state");
    control.patch([](S& s) {
        s.mode2 = swaraxt::ShruthiSvf2pMode::lowPass;
    });
    control.applyOnAudioThread(dual, enabled);
    expect(std::abs(dual.section1().lastBpVolts() - bpBefore) < 1.0e-15,
           "a mode-2 update does not reset section 1");
}

void testPrototype3CalibrationSonicReference()
{
    struct Case
    {
        const char* name;
        std::uint64_t expected;
        std::vector<float> audio;
    };
    const Case cases[] = {
        { "v03 S1 LP high resonance", 0x73c446eb5d9176f9ull,
          renderDual(swaraxt::ShruthiSvfDualRouting::section1,
                     swaraxt::ShruthiSvf2pMode::lowPass, swaraxt::ShruthiSvf2pMode::lowPass,
                     64, 48, false) },
        { "v03 serial LP>LP", 0x3bce33d3b4f92cfeull,
          renderDual(swaraxt::ShruthiSvfDualRouting::serial,
                     swaraxt::ShruthiSvf2pMode::lowPass, swaraxt::ShruthiSvf2pMode::lowPass,
                     48, 16, false) },
    };
    for (const auto& c : cases)
    {
        const auto h = hashSamples(c.audio);
        std::printf("PROTOTYPE 3 CALIBRATION SONIC REFERENCE %s hash=%016llx\n",
                    c.name, static_cast<unsigned long long>(h));
        if (c.expected != 0)
            expect(h == c.expected, c.name);
        else
            expect(h != 0, c.name);
        int invalid = 0;
        for (float s : c.audio)
            if (! std::isfinite(s))
                ++invalid;
        expect(invalid == 0, "prototype 3 reference finite");
    }
}

void testV2164ClosedLoop()
{
    using Board = swaraxt::DualSvfAnalogBoard;
    const double sr = swaraxt::ShruthiSvf2pControl::kNativeRate;
    // Independent of Board::steadyGain: KCL at the servo node with the cell
    // output sinking Iref*g, R14=R9 so Vf = Vpwm/2, and R53 pulling toward VEE.
    auto ratio = [](double pwmVolts, bool bleed) {
        const double vf = 0.5 * std::clamp(pwmVolts, 0.0, 5.0);
        const double g = vf / 5.0 - (bleed ? (100.0e3 / 2.2e6) : 0.0);
        return g > 0.0 ? g : 0.0;
    };
    expect(std::abs(Board::steadyGain(5.0) - ratio(5.0, true)) < 1.0e-15,
           "fitted full-scale gain matches the current ratio");
    expect(Board::steadyGain(0.0) == 0.0, "fitted R53 closes the cell at zero PWM");
    expect(ratio(5.0, false) > ratio(5.0, true), "omitting R53 raises the equilibrium gain");
    expect(Board::steadyGain(5.0 * 23.0 / 255.0) == 0.0, "PWM bytes through 23 stay closed");
    expect(Board::steadyGain(5.0 * 24.0 / 255.0) > 0.0, "PWM byte 24 is the first open gain");

    Board board;
    board.prepare(sr);
    board.snapControl(5.0);
    expect(std::abs(board.gain() - Board::steadyGain(5.0)) < 1.0e-15, "snap reaches equilibrium");
    double dc = 0.0;
    for (int i = 0; i < 400000; ++i)
        dc = board.processOutput(1.0, 1.0, 5.0);
    expect(std::abs(dc) < 1.0e-6, "both coupling branches reject a sustained DC input");

    board.reset();
    board.snapControl(5.0);
    double inputDc = 1.0;
    for (int i = 0; i < 400000; ++i)
        inputDc = board.conditionInput(1.0);
    expect(std::abs(inputDc) < 1.0e-6, "C34/R20 rejects a sustained mixer DC");

    board.reset();
    board.snapControl(5.0);
    const double g = board.gain();
    double inSq = 0.0, outSq = 0.0;
    const double hz = 1000.0;
    for (int i = 0; i < 80000; ++i)
    {
        const double x = std::sin(2.0 * swaraxt::ShruthiSvf2pControl::kPi * hz * i / sr);
        const double y = board.conditionInput(x);
        if (i >= 40000)
        {
            inSq += x * x;
            outSq += y * y;
        }
    }
    const double measured = std::sqrt(outSq / inSq);
    expect(std::abs(measured - (Board::kR13 / Board::kR20)) < 0.01,
           "digital input passband gain is R13/R20");
    expect(outSq > 0.0, "input network passes 1 kHz");

    board.reset();
    board.snapControl(5.0);
    inSq = outSq = 0.0;
    double polarity = 0.0;
    for (int i = 0; i < 80000; ++i)
    {
        const double x = std::sin(2.0 * swaraxt::ShruthiSvf2pControl::kPi * hz * i / sr);
        const double y = board.processOutput(x, 0.0, 5.0);
        if (i >= 40000)
        {
            inSq += x * x;
            outSq += y * y;
            polarity += x * y;
        }
    }
    const double branch = std::sqrt(outSq / inSq);
    expect(std::abs(branch - g) < 0.02, "one 33 k branch reproduces the current-ratio gain");
    expect(polarity < 0.0, "output I/V inverts");
    (void) g;
}

void testNonfiniteInputRecovery()
{
    const double sr = swaraxt::ShruthiSvf2pControl::kNativeRate;
    auto prepare = [&](swaraxt::ShruthiSvfDual& dual) {
        dual.prepare(sr);
        dual.setRouting(swaraxt::ShruthiSvfDualRouting::serial);
        dual.setMode1(swaraxt::ShruthiSvf2pMode::lowPass);
        dual.setMode2(swaraxt::ShruthiSvf2pMode::bandPass);
        dual.setFirmwareCutoff(160);
        dual.setFirmwareResonance(80);
    };
    swaraxt::ShruthiSvfDual poisoned;
    prepare(poisoned);
    const float nanOut = poisoned.process(std::numeric_limits<float>::quiet_NaN(), 1.0f);
    const float infOut = poisoned.process(std::numeric_limits<float>::infinity(), 0.5f);
    expect(nanOut == 0.0f && infOut == 0.0f, "nonfinite mixer input becomes exact silence");
    swaraxt::ShruthiSvfDual clean;
    prepare(clean);
    clean.process(0.0f, 1.0f);
    clean.process(0.0f, 0.5f);
    const float after = poisoned.process(0.2f, 0.8f);
    const float reference = clean.process(0.2f, 0.8f);
    expect(after == reference && std::isfinite(after), "nonfinite input leaves the same state as silence");
    int invalid = 0;
    for (int i = 0; i < 256; ++i)
    {
        const float y = poisoned.process(std::numeric_limits<float>::quiet_NaN(),
                                         (i & 1) != 0 ? std::numeric_limits<float>::infinity() : 0.3f);
        if (! std::isfinite(y))
            ++invalid;
    }
    expect(invalid == 0, "repeated nonfinite mixer and VCA inputs stay finite");
}

}  // namespace

int main()
{
    testV2164ClosedLoop();
    testNonfiniteInputRecovery();
    testControlLaws();
    testZener();
    testModesAndSlope();
    testSelfOscillation();
    testSilenceAndAutomation();
    testSection1SonicReference();
    testDualSection1MatchesFrozenReference();
    testPrototype2DualSonicReference();
    testSection2IndependentAndIdentical();
    testSerialAndParallelTopology();
    testRoutingSwitchSafety();
    testV03SelfOscillation();
    testApprovedVsHardwareMeasurements();
    testPrototype3CalibrationSonicReference();
    testExperimentalSvfPublication();
    if (gFailures != 0)
    {
        std::printf("%d failure(s)\n", gFailures);
        return 1;
    }
    std::printf("ShruthiSvf2p prototype tests passed\n");
    return 0;
}
