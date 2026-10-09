// Copyright 2026 MontroneDSP.
// SPDX-License-Identifier: GPL-3.0-or-later
// Experimental Shruthi Dual-SVF section: control laws, modes, stability.

#include "Engine/Filter/ShruthiSvf2p.h"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstring>
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

void testControlLaws()
{
    using C = swaraxt::ShruthiSvf2pControl;
    const double expectedF0 = 1.0 / (2.0 * C::kPi * 33000.0 * 220.0e-12);
    expect(std::abs(C::analogF0Hz() - expectedF0) < 1.0e-9, "f0 = 1/(2 pi R C) from Python R=33k C=220pF");
    expect(std::abs(C::passbandGain() - 1.0) < 1.0e-12, "G = Rg/Ri = 1");
    expect(std::abs(C::kRho - 5.0 / 27.0) < 1.0e-15, "rho = 5/27");
    expect(std::abs(C::kRoOhms - 220000.0) < 1.0e-9, "R_O = 220k");
    expect(std::abs(C::rqOverRo() - 15000.0 / 220000.0) < 1.0e-15, "R_Q/R_O");

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
    expect(vCrit > 4.1 && vCrit < 4.3, "R_O singularity near 4.20 V PWM");
    expect(C::selfOscillationThresholdByte() == 214, "singularity ~ PWM byte 214");

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
    expect(! C::isSelfOscillatingByte(panelResonanceByte(52)), "low-Q region still finite at panel 52");
    expect(C::isSelfOscillatingByte(panelResonanceByte(54)), "steep landing by panel 54");
}

void testZener()
{
    using C = swaraxt::ShruthiSvf2pControl;
    expect(C::zenerLimit(0.0) == 0.0, "zener odd: zero");
    expect(std::abs(C::zenerLimit(1.0) - 1.0) < 1.0e-15, "zener linear at 1 V");
    expect(std::abs(C::zenerLimit(4.0) - 4.0) < 1.0e-15, "zener linear through 4.0 V");
    expect(C::zenerLimit(4.35) > 4.0 && C::zenerLimit(4.35) < 4.7, "zener knee below 4.7 V");
    expect(C::zenerLimit(20.0) < 4.7 && C::zenerLimit(20.0) > 4.6, "zener asymptote 4.7 V");
    expect(std::abs(C::zenerLimit(-4.35) + C::zenerLimit(4.35)) < 1.0e-15, "zener odd symmetry");
}

void testModesAndSlope()
{
    swaraxt::ShruthiSvf2p svf;
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
    swaraxt::ShruthiSvf2p svf;
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
    expect(peak > 0.01 && peak < 8.0, "self-osc bounded and alive after impulse");
    expect(std::abs(svf.lastBpVolts()) <= 4.7 + 1.0e-6, "BP capacitor within Zener");
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
    swaraxt::ShruthiSvf2p svf;
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
        { "S1 LP low resonance", 0xb12a7b6fe14f50a3ull, renderSection1(swaraxt::ShruthiSvf2pMode::lowPass, 64, 8, false, false, false) },
        { "S1 LP high resonance", 0x44a4fe703707c9baull, renderSection1(swaraxt::ShruthiSvf2pMode::lowPass, 64, 48, false, false, false) },
        { "S1 LP self-oscillation", 0x5055b05892a50940ull, renderSection1(swaraxt::ShruthiSvf2pMode::lowPass, 80, 63, true, false, false) },
        { "S1 BP", 0x1b1d6a0ca0fc7916ull, renderSection1(swaraxt::ShruthiSvf2pMode::bandPass, 64, 24, false, false, false) },
        { "S1 HP", 0x0360ab996aa498ecull, renderSection1(swaraxt::ShruthiSvf2pMode::highPass, 64, 16, false, false, false) },
        { "S1 cutoff automation", 0x006f74e9a52fae2full, renderSection1(swaraxt::ShruthiSvf2pMode::lowPass, 64, 16, false, true, false) },
        { "S1 resonance automation", 0x351762b97a841514ull, renderSection1(swaraxt::ShruthiSvf2pMode::lowPass, 64, 16, false, false, true) },
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
                              bool impulseThenSilence,
                              swaraxt::ShruthiSvfCalibration calibration = swaraxt::ShruthiSvfCalibration::paperApproved)
{
    swaraxt::ShruthiSvfDual dual;
    dual.prepare(swaraxt::ShruthiSvf2pControl::kNativeRate);
    dual.setCalibration(calibration);
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
        { "Dual S1 LP low resonance", 0xb12a7b6fe14f50a3ull,
          renderDual(swaraxt::ShruthiSvfDualRouting::section1,
                     swaraxt::ShruthiSvf2pMode::lowPass, swaraxt::ShruthiSvf2pMode::lowPass,
                     64, 8, false) },
        { "Dual S1 LP high resonance", 0x44a4fe703707c9baull,
          renderDual(swaraxt::ShruthiSvfDualRouting::section1,
                     swaraxt::ShruthiSvf2pMode::lowPass, swaraxt::ShruthiSvf2pMode::lowPass,
                     64, 48, false) },
        { "Dual S1 LP self-oscillation", 0x5055b05892a50940ull,
          renderDual(swaraxt::ShruthiSvfDualRouting::section1,
                     swaraxt::ShruthiSvf2pMode::lowPass, swaraxt::ShruthiSvf2pMode::lowPass,
                     80, 63, true) },
        { "Dual S1 BP", 0x1b1d6a0ca0fc7916ull,
          renderDual(swaraxt::ShruthiSvfDualRouting::section1,
                     swaraxt::ShruthiSvf2pMode::bandPass, swaraxt::ShruthiSvf2pMode::lowPass,
                     64, 24, false) },
        { "Dual S1 HP", 0x0360ab996aa498ecull,
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
        { "P2 serial LP>LP", 0x60b76744f830cfe9ull,
          renderDual(swaraxt::ShruthiSvfDualRouting::serial,
                     swaraxt::ShruthiSvf2pMode::lowPass, swaraxt::ShruthiSvf2pMode::lowPass,
                     48, 16, false) },
        { "P2 serial LP>BP", 0xbff625db74c9addfull,
          renderDual(swaraxt::ShruthiSvfDualRouting::serial,
                     swaraxt::ShruthiSvf2pMode::lowPass, swaraxt::ShruthiSvf2pMode::bandPass,
                     48, 16, false) },
        { "P2 serial BP>LP", 0xbff625db74c9addfull,
          renderDual(swaraxt::ShruthiSvfDualRouting::serial,
                     swaraxt::ShruthiSvf2pMode::bandPass, swaraxt::ShruthiSvf2pMode::lowPass,
                     48, 16, false) },
        { "P2 serial HP>LP", 0x4b4f755e3ce99103ull,
          renderDual(swaraxt::ShruthiSvfDualRouting::serial,
                     swaraxt::ShruthiSvf2pMode::highPass, swaraxt::ShruthiSvf2pMode::lowPass,
                     48, 16, false) },
        { "P2 parallel LP+HP", 0xba004ae74418c201ull,
          renderDual(swaraxt::ShruthiSvfDualRouting::parallel,
                     swaraxt::ShruthiSvf2pMode::lowPass, swaraxt::ShruthiSvf2pMode::highPass,
                     48, 16, false) },
        { "P2 parallel LP+BP", 0xf82e913fbcde2d97ull,
          renderDual(swaraxt::ShruthiSvfDualRouting::parallel,
                     swaraxt::ShruthiSvf2pMode::lowPass, swaraxt::ShruthiSvf2pMode::bandPass,
                     48, 16, false) },
        { "P2 parallel BP+HP", 0x34e6c5e4c6a7e7d7ull,
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
    swaraxt::ShruthiSvf2p s2;
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

void testHardwareResonanceLaw()
{
    using P = swaraxt::ShruthiSvf2pControl;
    using H = swaraxt::ShruthiSvfHardwareControl;
    expect(std::abs(H::kRoOhms - 330000.0) < 1.0e-9, "hardware R_O = 330k");
    expect(std::abs(H::rqOverRo() - 15000.0 / 330000.0) < 1.0e-15, "hardware R_Q/R_O");
    const double vHw = H::selfOscillationThresholdPwmVolts();
    const double vPaper = P::selfOscillationThresholdPwmVolts();
    expect(vHw > vPaper, "330k oscillates at a higher PWM than 220k");
    expect(vHw > 4.8 && vHw < 4.9, "330k singularity near 4.83 V PWM");
    expect(H::selfOscillationThresholdByte() == 246, "330k singularity ~ PWM byte 246");
    expect(! H::isSelfOscillatingByte(panelResonanceByte(54)), "hardware still finite at panel 54");
    expect(! H::isSelfOscillatingByte(panelResonanceByte(60)), "hardware still finite at panel 60");
    expect(H::isSelfOscillatingByte(panelResonanceByte(63)), "hardware crosses lossless by panel 63");

    std::printf("panel  paperQ     hwQ        paperOsc hwOsc\n");
    for (int panel = 0; panel <= 63; ++panel)
    {
        const auto byte = panelResonanceByte(panel);
        const double qp = P::qFromFirmwareByte(byte);
        const double qh = H::qFromFirmwareByte(byte);
        const bool oscP = P::isSelfOscillatingByte(byte);
        const bool oscH = H::isSelfOscillatingByte(byte);
        if (panel <= 50 || panel >= 52)
            std::printf("%5d  %9.3f  %9.3f  %d        %d\n",
                        panel, oscP ? 0.0 : qp, oscH ? 0.0 : qh,
                        oscP ? 1 : 0, oscH ? 1 : 0);
    }
    const double qP50 = P::qFromFirmwareByte(panelResonanceByte(50));
    const double qH50 = H::qFromFirmwareByte(panelResonanceByte(50));
    const double qP53 = P::qFromFirmwareByte(panelResonanceByte(53));
    const double qH60 = H::qFromFirmwareByte(panelResonanceByte(60));
    expect(qH50 < qP50 * 0.5, "at panel 50, 330k Q is much lower than 220k");
    expect(qP53 > qP50 * 5.0, "220k Q has a steep landing toward oscillation");
    expect(qH50 < 20.0 && qH60 > 50.0, "330k stays milder through 0-50 then steepens near max");
}

void testHardwareZener()
{
    using H = swaraxt::ShruthiSvfHardwareControl;
    expect(H::zenerLimit(0.0) == 0.0, "hw zener odd: zero");
    expect(std::abs(H::zenerLimit(1.0) - 1.0) < 1.0e-15, "hw zener linear at 1 V");
    expect(std::abs(H::zenerLimit(2.9) - 2.9) < 1.0e-15, "hw zener linear through 2.9 V");
    expect(H::zenerLimit(3.2) > 2.9 && H::zenerLimit(3.2) < 3.6, "hw zener knee below 3.6 V");
    expect(H::zenerLimit(20.0) < 3.6 && H::zenerLimit(20.0) > 3.5, "hw zener asymptote 3.6 V");
    expect(std::abs(H::zenerLimit(-3.2) + H::zenerLimit(3.2)) < 1.0e-15, "hw zener odd symmetry");
}

void testCalibrationSwitchAndHardwareAudio()
{
    swaraxt::ShruthiSvfDual dual;
    dual.prepare(swaraxt::ShruthiSvf2pControl::kNativeRate);
    dual.setFirmwareCutoff1(panelCutoffByte(64));
    dual.setFirmwareResonance1(panelResonanceByte(48));
    dual.setVcaTarget(1.0f);
    dual.snapControlsForTests();
    expect(dual.calibration() == swaraxt::ShruthiSvfCalibration::paperApproved,
           "default calibration is Approved/Paper");

    const auto paper = renderDual(swaraxt::ShruthiSvfDualRouting::section1,
                                  swaraxt::ShruthiSvf2pMode::lowPass, swaraxt::ShruthiSvf2pMode::lowPass,
                                  64, 48, false, swaraxt::ShruthiSvfCalibration::paperApproved);
    const auto hardware = renderDual(swaraxt::ShruthiSvfDualRouting::section1,
                                     swaraxt::ShruthiSvf2pMode::lowPass, swaraxt::ShruthiSvf2pMode::lowPass,
                                     64, 48, false, swaraxt::ShruthiSvfCalibration::eagleHardware);
    expect(hashSamples(paper) == 0x44a4fe703707c9baull, "paper high-res still matches S1 reference");
    expect(hashSamples(hardware) != hashSamples(paper), "hardware calibration changes high-res LP");

    swaraxt::ShruthiSvf2p svf;
    svf.prepare(swaraxt::ShruthiSvf2pControl::kNativeRate);
    svf.setFirmwareCutoff(panelCutoffByte(80));
    svf.setFirmwareResonance(panelResonanceByte(63));
    svf.setMode(swaraxt::ShruthiSvf2pMode::bandPass);
    svf.setCalibration(swaraxt::ShruthiSvfCalibration::eagleHardware);
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
    expect(invalid == 0, "hardware self-osc 1 s finite");
    expect(peak > 0.01 && peak < 8.0, "hardware self-osc bounded and alive");
    expect(maxBp <= 3.6 + 1.0e-6, "hardware BP within 3.6 V limiter");

    int switchInvalid = 0;
    for (int i = 0; i < 2048; ++i)
    {
        dual.setCalibration((i & 1) != 0
            ? swaraxt::ShruthiSvfCalibration::eagleHardware
            : swaraxt::ShruthiSvfCalibration::paperApproved);
        dual.setFirmwareCutoff1(static_cast<std::uint8_t>(i & 255));
        dual.setFirmwareResonance1(static_cast<std::uint8_t>((i * 7) & 255));
        const float x = static_cast<float>(0.25 * std::sin(2.0 * swaraxt::ShruthiSvf2pControl::kPi * 220.0 * i / sr));
        const float y = dual.process(x, 1.0f);
        if (! std::isfinite(y) || std::abs(y) > 16.0f)
            ++switchInvalid;
    }
    expect(switchInvalid == 0, "calibration switch torture stays finite without forced reset");
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
        for (auto cal : { swaraxt::ShruthiSvfCalibration::paperApproved,
                          swaraxt::ShruthiSvfCalibration::eagleHardware })
        {
            swaraxt::ShruthiSvfDual dual;
            dual.prepare(sr);
            dual.setCalibration(cal);
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
            char label[96];
            std::snprintf(label, sizeof(label), "%s_%s",
                          cal == swaraxt::ShruthiSvfCalibration::paperApproved ? "approved" : "hardware",
                          job.name);
            reportStats(label, y, maxBp, sr);
        }
    }
}

void testSilenceAndAutomation()
{
    swaraxt::ShruthiSvf2p svf;
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
    packed.calibration = swaraxt::ShruthiSvfCalibration::eagleHardware;
    const auto roundTrip = S::unpack(S::pack(packed));
    expect(roundTrip.enabled, "packed selection preserves enabled");
    expect(roundTrip.routing == packed.routing, "packed selection preserves routing");
    expect(roundTrip.mode1 == packed.mode1, "packed selection preserves mode1");
    expect(roundTrip.mode2 == packed.mode2, "packed selection preserves mode2");
    expect(roundTrip.calibration == packed.calibration, "packed selection preserves calibration");

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
        s.calibration = swaraxt::ShruthiSvfCalibration::eagleHardware;
    });
    const auto sel = control.selection();
    expect(sel.enabled && sel.routing == swaraxt::ShruthiSvfDualRouting::parallel
               && sel.mode1 == swaraxt::ShruthiSvf2pMode::highPass
               && sel.calibration == swaraxt::ShruthiSvfCalibration::eagleHardware,
           "CAS patch preserves unrelated selector fields");
    control.applyOnAudioThread(dual, enabled);
    expect(enabled, "audio apply publishes enabled");
    expect(dual.routing() == swaraxt::ShruthiSvfDualRouting::parallel, "audio apply routing");
    expect(dual.mode1() == swaraxt::ShruthiSvf2pMode::highPass, "audio apply mode1");
    expect(dual.calibration() == swaraxt::ShruthiSvfCalibration::eagleHardware,
           "audio apply calibration");

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
    expect(std::abs(bpBefore) > 1.0e-6, "pre-calibration switch has state");
    control.patch([](S& s) {
        s.calibration = swaraxt::ShruthiSvfCalibration::paperApproved;
    });
    control.applyOnAudioThread(dual, enabled);
    expect(std::abs(dual.section1().lastBpVolts() - bpBefore) < 1.0e-15,
           "calibration apply is state-continuous");
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
        { "P3 paper S1 LP high resonance", 0x44a4fe703707c9baull,
          renderDual(swaraxt::ShruthiSvfDualRouting::section1,
                     swaraxt::ShruthiSvf2pMode::lowPass, swaraxt::ShruthiSvf2pMode::lowPass,
                     64, 48, false, swaraxt::ShruthiSvfCalibration::paperApproved) },
        { "P3 hardware S1 LP high resonance", 0x5f9802c78ca67b35ull,
          renderDual(swaraxt::ShruthiSvfDualRouting::section1,
                     swaraxt::ShruthiSvf2pMode::lowPass, swaraxt::ShruthiSvf2pMode::lowPass,
                     64, 48, false, swaraxt::ShruthiSvfCalibration::eagleHardware) },
        { "P3 paper serial LP>LP", 0x60b76744f830cfe9ull,
          renderDual(swaraxt::ShruthiSvfDualRouting::serial,
                     swaraxt::ShruthiSvf2pMode::lowPass, swaraxt::ShruthiSvf2pMode::lowPass,
                     48, 16, false, swaraxt::ShruthiSvfCalibration::paperApproved) },
        { "P3 hardware serial LP>LP", 0xe8f811fa4dbd553dull,
          renderDual(swaraxt::ShruthiSvfDualRouting::serial,
                     swaraxt::ShruthiSvf2pMode::lowPass, swaraxt::ShruthiSvf2pMode::lowPass,
                     48, 16, false, swaraxt::ShruthiSvfCalibration::eagleHardware) },
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

}  // namespace

int main()
{
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
    testHardwareResonanceLaw();
    testHardwareZener();
    testCalibrationSwitchAndHardwareAudio();
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
