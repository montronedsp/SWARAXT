// Copyright 2026 MontroneDSP. SPDX-License-Identifier: GPL-3.0-or-later
//
// Experimental prototype: ONE section of the Mutable Instruments Shruthi Dual
// SVF (SSM2164 state-variable filter). Numerical realisation is TPT; cutoff,
// Q and BP limiting follow shruthi-1 svf_analysis.tex and
// ssm_2164_svf_transfer.py. This is not Classic IR3109, not MEERA, not Anushri.
#pragma once

#include <atomic>
#include <cmath>
#include <cstdint>

namespace swaraxt {

enum class ShruthiSvf2pMode : std::uint8_t
{
    lowPass = 0,
    bandPass = 1,
    highPass = 2
};

// Session-only A/B. Default paperApproved is the Prototype 1/2 listening reference.
enum class ShruthiSvfCalibration : std::uint8_t
{
    paperApproved = 0,
    eagleHardware = 1
};

// Control conversion from firmware PWM bytes to analog-law f0 / Q.
// Sources: pichenettes/shruthi-1 svf_analysis.tex and
// hardware_design/simulations/ssm_2164_svf_transfer.py (R, C, Rg, Ri, Rq).
struct ShruthiSvf2pControl
{
    static constexpr double kPi = 3.1415926535897932384626433832795;
    static constexpr double kROhms = 33000.0;
    static constexpr double kCFarads = 220.0e-12;
    static constexpr double kRgOhms = 33000.0;
    static constexpr double kRiOhms = 33000.0;
    static constexpr double kRqOhms = 15000.0;
    static constexpr double kRoOhms = 220000.0;
    // SSM2164 CV pin divider (0.5k / 4.5k) plus 22k PWM source impedance.
    static constexpr double kRho = 5.0 / 27.0;
    // 5 V PWM spans 128 MIDI notes => 2164 v_cv swing 2.141 V; gain 2.141/5.
    static constexpr double kCutoffCvGain = 2.141 / 5.0;
    static constexpr double kPwmFullScaleVolts = 5.0;
    static constexpr double kZenerVolts = 4.7;
    // Linear until 4.0 V, then C1-smooth approach to 4.7 V (head-to-head Zeners).
    static constexpr double kZenerKneeVolts = 0.7;
    static constexpr double kResonanceCvTauSeconds = 22000.0 * 68.0e-9;
    // Prototype PWM reconstruction for cutoff (active analog filter not extracted
    // from Eagle this pass). Same order as other Shruthi PWM CVs, not IR3109 map.
    static constexpr double kCutoffCvTauSeconds = 5600.0 * 33.0e-9;
    // Digital-board VCA PWM reconstruction. Prototype infrastructure, not SVF.
    static constexpr double kVcaCvTauSeconds = 10000.0 * 33.0e-9;
    // Mixer byte to volts: same source scaling Classic uses at the analog jack.
    static constexpr double kSourceVoltsPerUnit = 5.0 * 128.0 / 255.0;
    static constexpr double kNyquistRatio = 0.45;
    static constexpr double kNativeRate = 20000000.0 / 510.0;
    static constexpr double kMaxTptG = 4.0;

    static double analogF0Hz() noexcept
    {
        return 1.0 / (2.0 * kPi * kROhms * kCFarads);
    }

    static double passbandGain() noexcept
    {
        return kRgOhms / kRiOhms;
    }

    static double pwmVoltsFromByte(std::uint8_t byte) noexcept
    {
        return kPwmFullScaleVolts * (static_cast<double>(byte) / 255.0);
    }

    // Firmware cutoff() is 8-bit PWM after ENV1/LFO2/matrix/key-track.
    // Shared PWM convention with IR3109: larger byte => higher cutoff.
    // SSM2164 gain falls as v_cv rises, so the analog scaler inverts:
    //   v_cv = kCutoffCvGain * (5 V - v_pwm)
    //   f    = 1/(2πRC) * 10^(-3/2 v_cv)
    static double cutoffHzFromPwmVolts(double pwmVolts) noexcept
    {
        const double bounded = pwmVolts < 0.0 ? 0.0
            : (pwmVolts > kPwmFullScaleVolts ? kPwmFullScaleVolts : pwmVolts);
        const double vCv = kCutoffCvGain * (kPwmFullScaleVolts - bounded);
        const double hz = analogF0Hz() * std::pow(10.0, -1.5 * vCv);
        if (! std::isfinite(hz) || hz < 1.0)
            return 1.0;
        return hz;
    }

    static double cutoffHzFromFirmwareByte(std::uint8_t cutoff) noexcept
    {
        return cutoffHzFromPwmVolts(pwmVoltsFromByte(cutoff));
    }

    static double rqOverRo() noexcept
    {
        return kRqOhms / kRoOhms;
    }

    // PWM voltage at which the R_O-compensated denominator is zero (Q → ∞).
    static double selfOscillationThresholdPwmVolts() noexcept
    {
        const double ratio = rqOverRo();
        return -std::log10(ratio) / (1.5 * kRho);
    }

    static std::uint8_t selfOscillationThresholdByte() noexcept
    {
        const double volts = selfOscillationThresholdPwmVolts();
        const double code = volts / kPwmFullScaleVolts * 255.0;
        if (code <= 0.0)
            return 0;
        if (code >= 255.0)
            return 255;
        return static_cast<std::uint8_t>(code + 0.5);
    }

    // Q from firmware resonance() PWM byte (0..255 after matrix; panel 0..63
    // becomes byte = (value << 8) >> 6). v_q is the 0–5 V PWM, then ρ loading.
    // Q = 1/2 * (10^(-3/2 ρ v_q) - R_Q/R_O)^(-1)
    // Denominator <= 0 => self-oscillation (lossless TPT k = 0).
    static double qFromPwmVolts(double pwmVolts) noexcept
    {
        const double vq = pwmVolts < 0.0 ? 0.0
            : (pwmVolts > kPwmFullScaleVolts ? kPwmFullScaleVolts : pwmVolts);
        const double loaded = std::pow(10.0, -1.5 * kRho * vq) - rqOverRo();
        if (! std::isfinite(loaded) || loaded <= 1.0e-12)
            return 0.0; // sentinel: lossless
        const double q = 0.5 / loaded;
        if (! std::isfinite(q) || q < 0.5)
            return 0.5;
        return q;
    }

    static double qFromFirmwareByte(std::uint8_t resonance) noexcept
    {
        return qFromPwmVolts(pwmVoltsFromByte(resonance));
    }

    static bool isSelfOscillatingPwmVolts(double pwmVolts) noexcept
    {
        return pwmVolts >= selfOscillationThresholdPwmVolts();
    }

    static bool isSelfOscillatingByte(std::uint8_t resonance) noexcept
    {
        return isSelfOscillatingPwmVolts(pwmVoltsFromByte(resonance));
    }

    // C1-smooth head-to-head Zener: identity below (Vz - knee), then
    // y = a + knee * t/(t+knee), a = 4.0 V, asymptote 4.7 V. Odd function.
    static double zenerLimit(double volts) noexcept
    {
        const double a = kZenerVolts - kZenerKneeVolts;
        const double ax = volts < 0.0 ? -volts : volts;
        if (ax <= a)
            return volts;
        const double t = ax - a;
        const double y = a + kZenerKneeVolts * t / (t + kZenerKneeVolts);
        return volts < 0.0 ? -y : y;
    }
};

// Eagle v03 / kit Dual-SVF: R_O = 330 kΩ, 3.6 V back-to-back Zeners.
// Same equations as the paper, only Ro and Vz change. Not the approved sound.
struct ShruthiSvfHardwareControl
{
    static constexpr double kRoOhms = 330000.0;
    static constexpr double kZenerVolts = 3.6;
    static constexpr double kZenerKneeVolts = 0.7;

    static double rqOverRo() noexcept
    {
        return ShruthiSvf2pControl::kRqOhms / kRoOhms;
    }

    static double selfOscillationThresholdPwmVolts() noexcept
    {
        return -std::log10(rqOverRo()) / (1.5 * ShruthiSvf2pControl::kRho);
    }

    static std::uint8_t selfOscillationThresholdByte() noexcept
    {
        const double code = selfOscillationThresholdPwmVolts()
            / ShruthiSvf2pControl::kPwmFullScaleVolts * 255.0;
        if (code <= 0.0)
            return 0;
        if (code >= 255.0)
            return 255;
        return static_cast<std::uint8_t>(code + 0.5);
    }

    static double qFromPwmVolts(double pwmVolts) noexcept
    {
        const double vq = pwmVolts < 0.0 ? 0.0
            : (pwmVolts > ShruthiSvf2pControl::kPwmFullScaleVolts
                ? ShruthiSvf2pControl::kPwmFullScaleVolts : pwmVolts);
        const double loaded = std::pow(10.0, -1.5 * ShruthiSvf2pControl::kRho * vq) - rqOverRo();
        if (! std::isfinite(loaded) || loaded <= 1.0e-12)
            return 0.0;
        const double q = 0.5 / loaded;
        if (! std::isfinite(q) || q < 0.5)
            return 0.5;
        return q;
    }

    static double qFromFirmwareByte(std::uint8_t resonance) noexcept
    {
        return qFromPwmVolts(ShruthiSvf2pControl::pwmVoltsFromByte(resonance));
    }

    static bool isSelfOscillatingPwmVolts(double pwmVolts) noexcept
    {
        return pwmVolts >= selfOscillationThresholdPwmVolts();
    }

    static bool isSelfOscillatingByte(std::uint8_t resonance) noexcept
    {
        return isSelfOscillatingPwmVolts(ShruthiSvf2pControl::pwmVoltsFromByte(resonance));
    }

    // Same C1-smooth odd clip as the paper, with kit Vz = 3.6 V.
    // Linear below 2.9 V, asymptote 3.6 V. Analog anti-series pair would
    // add ~0.7 V forward drop; that is documented, not added here, so the
    // A/B isolates the kit Vz rating against the paper 4.7 V model.
    static double zenerLimit(double volts) noexcept
    {
        const double a = kZenerVolts - kZenerKneeVolts;
        const double ax = volts < 0.0 ? -volts : volts;
        if (ax <= a)
            return volts;
        const double t = ax - a;
        const double y = a + kZenerKneeVolts * t / (t + kZenerKneeVolts);
        return volts < 0.0 ? -y : y;
    }
};

// One Dual-SVF section at native Shruthi rate. LP, BP and HP share state.
class ShruthiSvf2p
{
 public:
    void prepare(double nativeRate) noexcept
    {
        sampleRate_ = nativeRate > 1.0 && std::isfinite(nativeRate)
            ? nativeRate
            : ShruthiSvf2pControl::kNativeRate;
        cutoffAlpha_ = 1.0 - std::exp(-1.0 / (sampleRate_ * ShruthiSvf2pControl::kCutoffCvTauSeconds));
        resonanceAlpha_ = 1.0 - std::exp(-1.0 / (sampleRate_ * ShruthiSvf2pControl::kResonanceCvTauSeconds));
        vcaAlpha_ = 1.0 - std::exp(-1.0 / (sampleRate_ * ShruthiSvf2pControl::kVcaCvTauSeconds));
        reset();
        updateCoefficients();
    }

    void reset() noexcept
    {
        ic1_ = 0.0;
        ic2_ = 0.0;
        lastLp_ = 0.0;
        lastBp_ = 0.0;
        lastHp_ = 0.0;
        lastOutput_ = 0.0;
        cutoffCv_ = cutoffTarget_;
        resonanceCv_ = resonanceTarget_;
        vcaCv_ = 0.0;
        updateCoefficients();
    }

    void setFirmwareCutoff(std::uint8_t cutoff) noexcept
    {
        cutoffTarget_ = ShruthiSvf2pControl::pwmVoltsFromByte(cutoff);
    }

    void setFirmwareResonance(std::uint8_t resonance) noexcept
    {
        resonanceTarget_ = ShruthiSvf2pControl::pwmVoltsFromByte(resonance);
    }

    void setMode(ShruthiSvf2pMode mode) noexcept { mode_ = mode; }
    ShruthiSvf2pMode mode() const noexcept { return mode_; }

    void setCalibration(ShruthiSvfCalibration calibration) noexcept
    {
        if (calibration_ == calibration)
            return;
        calibration_ = calibration;
        updateCoefficients();
    }

    ShruthiSvfCalibration calibration() const noexcept { return calibration_; }

    void setVcaTarget(float vca01) noexcept
    {
        const double v = static_cast<double>(vca01);
        vcaTarget_ = v < 0.0 ? 0.0 : (v > 1.0 ? 1.0 : v);
    }

    void advanceControls(float vcaTarget) noexcept
    {
        setVcaTarget(vcaTarget);
        cutoffCv_ += cutoffAlpha_ * (cutoffTarget_ - cutoffCv_);
        resonanceCv_ += resonanceAlpha_ * (resonanceTarget_ - resonanceCv_);
        vcaCv_ += vcaAlpha_ * (vcaTarget_ - vcaCv_);
        if (vcaTarget_ == 0.0 && vcaCv_ < 1.0e-12)
            vcaCv_ = 0.0;
        updateCoefficients();
    }

    // Core tick without the prototype VCA. Dual routing uses this so VCA is applied once.
    double processSelectedVolts(double inVolts) noexcept
    {
        processVolts(inVolts);
        return mode_ == ShruthiSvf2pMode::bandPass
            ? lastBp_
            : (mode_ == ShruthiSvf2pMode::highPass ? lastHp_ : lastLp_);
    }

    void snapControlsForTests() noexcept
    {
        cutoffCv_ = cutoffTarget_;
        resonanceCv_ = resonanceTarget_;
        vcaCv_ = vcaTarget_;
        updateCoefficients();
    }

    float vcaControl() const noexcept { return static_cast<float>(vcaCv_); }

    bool tailActive() const noexcept
    {
        return std::abs(lastOutput_) > 1.0e-8
            || std::abs(ic1_) > 1.0e-8
            || std::abs(ic2_) > 1.0e-8
            || vcaCv_ > 1.0e-8;
    }

    double cutoffHz() const noexcept { return cutoffHz_; }
    double q() const noexcept { return q_; }
    double lastLpVolts() const noexcept { return lastLp_; }
    double lastBpVolts() const noexcept { return lastBp_; }
    double lastHpVolts() const noexcept { return lastHp_; }

    // mixerUnit is the native mixer float (same domain as Classic filter in).
    // Returns plugin-domain audio after prototype VCA.
    float process(float mixerUnit, float vcaTarget) noexcept
    {
        advanceControls(vcaTarget);
        const double inVolts = static_cast<double>(mixerUnit) * ShruthiSvf2pControl::kSourceVoltsPerUnit;
        const double selected = processSelectedVolts(inVolts);
        const double outUnit = selected / ShruthiSvf2pControl::kSourceVoltsPerUnit * vcaCv_;
        lastOutput_ = std::isfinite(outUnit) ? outUnit : 0.0;
        if (! std::isfinite(lastOutput_))
        {
            reset();
            return 0.0f;
        }
        return static_cast<float>(lastOutput_);
    }

 private:
    void updateCoefficients() noexcept
    {
        cutoffHz_ = ShruthiSvf2pControl::cutoffHzFromPwmVolts(cutoffCv_);
        const bool hardware = calibration_ == ShruthiSvfCalibration::eagleHardware;
        const double q = hardware
            ? ShruthiSvfHardwareControl::qFromPwmVolts(resonanceCv_)
            : ShruthiSvf2pControl::qFromPwmVolts(resonanceCv_);
        const bool lossless = (hardware
            ? ShruthiSvfHardwareControl::isSelfOscillatingPwmVolts(resonanceCv_)
            : ShruthiSvf2pControl::isSelfOscillatingPwmVolts(resonanceCv_)) || q <= 0.0;
        q_ = lossless ? 0.0 : q;

        const double nyquistSafe = sampleRate_ * ShruthiSvf2pControl::kNyquistRatio;
        double fc = cutoffHz_;
        if (! std::isfinite(fc) || fc < 1.0)
            fc = 1.0;
        if (fc > nyquistSafe)
            fc = nyquistSafe;
        g_ = std::tan(ShruthiSvf2pControl::kPi * fc / sampleRate_);
        if (! std::isfinite(g_) || g_ < 1.0e-6)
            g_ = 1.0e-6;
        if (g_ > ShruthiSvf2pControl::kMaxTptG)
            g_ = ShruthiSvf2pControl::kMaxTptG;
        k_ = lossless ? 0.0 : 1.0 / q_;
        if (! std::isfinite(k_) || k_ < 0.0)
            k_ = 0.0;
    }

    void processVolts(double inVolts) noexcept
    {
        const double v0 = inVolts * ShruthiSvf2pControl::passbandGain() - ic2_;
        const double den = 1.0 + g_ * (g_ + k_);
        double v1 = (ic1_ + g_ * v0) / den;
        if (calibration_ == ShruthiSvfCalibration::eagleHardware)
        {
            v1 = ShruthiSvfHardwareControl::zenerLimit(v1);
            ic1_ = ShruthiSvfHardwareControl::zenerLimit(2.0 * v1 - ic1_);
        }
        else
        {
            v1 = ShruthiSvf2pControl::zenerLimit(v1);
            ic1_ = ShruthiSvf2pControl::zenerLimit(2.0 * v1 - ic1_);
        }
        const double v2 = ic2_ + g_ * v1;
        ic2_ = 2.0 * v2 - ic2_;
        lastLp_ = v2;
        lastBp_ = v1;
        // Analog SVF: HP = G*in - k*BP - LP. v0 is the TPT delayed residual, not in.
        lastHp_ = inVolts * ShruthiSvf2pControl::passbandGain() - k_ * v1 - v2;
        if (! std::isfinite(ic1_) || ! std::isfinite(ic2_)
            || ! std::isfinite(lastLp_) || ! std::isfinite(lastBp_) || ! std::isfinite(lastHp_))
        {
            ic1_ = ic2_ = lastLp_ = lastBp_ = lastHp_ = 0.0;
        }
    }

    double sampleRate_ = ShruthiSvf2pControl::kNativeRate;
    double cutoffAlpha_ = 1.0;
    double resonanceAlpha_ = 1.0;
    double vcaAlpha_ = 1.0;
    double cutoffTarget_ = 0.0;
    double resonanceTarget_ = 0.0;
    double vcaTarget_ = 0.0;
    double cutoffCv_ = 0.0;
    double resonanceCv_ = 0.0;
    double vcaCv_ = 0.0;
    double cutoffHz_ = 1.0;
    double q_ = 0.5;
    double g_ = 1.0e-6;
    double k_ = 2.0;
    double ic1_ = 0.0;
    double ic2_ = 0.0;
    double lastLp_ = 0.0;
    double lastBp_ = 0.0;
    double lastHp_ = 0.0;
    double lastOutput_ = 0.0;
    ShruthiSvf2pMode mode_ = ShruthiSvf2pMode::lowPass;
    ShruthiSvfCalibration calibration_ = ShruthiSvfCalibration::paperApproved;
};

// Dual-SVF routing from FILTER_BOARD_SVF firmware + Analog-SVF-v03.
// Hardware exposes serial (MIX→F1→F2→VCA, F1 not mixed) and parallel
// (MIX→F1 and MIX→F2, analog sum→VCA). section1 is the Prototype 1
// listening reference, not a Dual-board firmware mode.
enum class ShruthiSvfDualRouting : std::uint8_t
{
    section1 = 0,
    serial = 1,
    parallel = 2
};

class ShruthiSvfDual
{
 public:
    void prepare(double nativeRate) noexcept
    {
        s1_.prepare(nativeRate);
        s2_.prepare(nativeRate);
        const double rate = nativeRate > 1.0 && std::isfinite(nativeRate)
            ? nativeRate
            : ShruthiSvf2pControl::kNativeRate;
        vcaAlpha_ = 1.0 - std::exp(-1.0 / (rate * ShruthiSvf2pControl::kVcaCvTauSeconds));
        reset();
    }

    void reset() noexcept
    {
        s1_.reset();
        s2_.reset();
        vcaCv_ = 0.0;
        lastOutput_ = 0.0;
    }

    void setRouting(ShruthiSvfDualRouting routing) noexcept
    {
        if (routing_ == routing)
            return;
        routing_ = routing;
        reset();
    }

    ShruthiSvfDualRouting routing() const noexcept { return routing_; }

    void setMode1(ShruthiSvf2pMode mode) noexcept { s1_.setMode(mode); }
    void setMode2(ShruthiSvf2pMode mode) noexcept { s2_.setMode(mode); }
    void setMode(ShruthiSvf2pMode mode) noexcept { setMode1(mode); }
    ShruthiSvf2pMode mode1() const noexcept { return s1_.mode(); }
    ShruthiSvf2pMode mode2() const noexcept { return s2_.mode(); }

    void setFirmwareCutoff1(std::uint8_t cutoff) noexcept
    {
        s1_.setFirmwareCutoff(cutoff);
        if (s2FollowsS1_)
            s2_.setFirmwareCutoff(cutoff);
    }

    void setFirmwareResonance1(std::uint8_t resonance) noexcept
    {
        s1_.setFirmwareResonance(resonance);
        if (s2FollowsS1_)
            s2_.setFirmwareResonance(resonance);
    }

    void setFirmwareCutoff(std::uint8_t cutoff) noexcept { setFirmwareCutoff1(cutoff); }
    void setFirmwareResonance(std::uint8_t resonance) noexcept { setFirmwareResonance1(resonance); }

    void setFirmwareCutoff2(std::uint8_t cutoff) noexcept
    {
        s2FollowsS1_ = false;
        s2_.setFirmwareCutoff(cutoff);
    }

    void setFirmwareResonance2(std::uint8_t resonance) noexcept
    {
        s2FollowsS1_ = false;
        s2_.setFirmwareResonance(resonance);
    }

    void setSection2FollowsSection1(bool follow) noexcept { s2FollowsS1_ = follow; }

    void setCalibration(ShruthiSvfCalibration calibration) noexcept
    {
        s1_.setCalibration(calibration);
        s2_.setCalibration(calibration);
    }

    ShruthiSvfCalibration calibration() const noexcept { return s1_.calibration(); }

    void setVcaTarget(float vca01) noexcept
    {
        s1_.setVcaTarget(vca01);
        s2_.setVcaTarget(vca01);
        const double v = static_cast<double>(vca01);
        vcaTarget_ = v < 0.0 ? 0.0 : (v > 1.0 ? 1.0 : v);
    }

    ShruthiSvf2p& section1() noexcept { return s1_; }
    const ShruthiSvf2p& section1() const noexcept { return s1_; }
    ShruthiSvf2p& section2() noexcept { return s2_; }
    const ShruthiSvf2p& section2() const noexcept { return s2_; }

    void snapControlsForTests() noexcept
    {
        s1_.snapControlsForTests();
        s2_.snapControlsForTests();
        vcaCv_ = vcaTarget_;
    }

    float vcaControl() const noexcept
    {
        if (routing_ == ShruthiSvfDualRouting::section1)
            return s1_.vcaControl();
        return static_cast<float>(vcaCv_);
    }

    bool tailActive() const noexcept
    {
        return s1_.tailActive() || s2_.tailActive()
            || std::abs(lastOutput_) > 1.0e-8 || vcaCv_ > 1.0e-8;
    }

    float process(float mixerUnit, float vcaTarget) noexcept
    {
        if (routing_ == ShruthiSvfDualRouting::section1)
            return s1_.process(mixerUnit, vcaTarget);

        const double v = static_cast<double>(vcaTarget);
        vcaTarget_ = v < 0.0 ? 0.0 : (v > 1.0 ? 1.0 : v);
        s1_.advanceControls(vcaTarget);
        s2_.advanceControls(vcaTarget);
        vcaCv_ += vcaAlpha_ * (vcaTarget_ - vcaCv_);
        if (vcaTarget_ == 0.0 && vcaCv_ < 1.0e-12)
            vcaCv_ = 0.0;

        const double inVolts = static_cast<double>(mixerUnit) * ShruthiSvf2pControl::kSourceVoltsPerUnit;
        const double y1 = s1_.processSelectedVolts(inVolts);
        const double y2In = routing_ == ShruthiSvfDualRouting::serial ? y1 : inVolts;
        const double y2 = s2_.processSelectedVolts(y2In);
        // Parallel: equal 1>VCA and 2>VCA paths (R1/R2 are unity jumpers).
        // Mix Rf/Rin is not readable from the binary Eagle file; identical
        // sections imply analog sum, not an extra 1/2 average.
        const double mixed = routing_ == ShruthiSvfDualRouting::serial ? y2 : (y1 + y2);
        const double outUnit = mixed / ShruthiSvf2pControl::kSourceVoltsPerUnit * vcaCv_;
        lastOutput_ = std::isfinite(outUnit) ? outUnit : 0.0;
        if (! std::isfinite(lastOutput_))
        {
            reset();
            return 0.0f;
        }
        return static_cast<float>(lastOutput_);
    }

 private:
    ShruthiSvf2p s1_;
    ShruthiSvf2p s2_;
    ShruthiSvfDualRouting routing_ = ShruthiSvfDualRouting::section1;
    bool s2FollowsS1_ = true;
    double vcaAlpha_ = 1.0;
    double vcaTarget_ = 0.0;
    double vcaCv_ = 0.0;
    double lastOutput_ = 0.0;
};

// Session-only Dual-SVF selector published from the message thread.
// One packed word is a complete consistent choice so readers never observe
// mixed routing/mode/calibration/enable from independent stores.
struct ExperimentalSvfSelection
{
    bool enabled = false;
    ShruthiSvfDualRouting routing = ShruthiSvfDualRouting::section1;
    ShruthiSvf2pMode mode1 = ShruthiSvf2pMode::lowPass;
    ShruthiSvf2pMode mode2 = ShruthiSvf2pMode::lowPass;
    ShruthiSvfCalibration calibration = ShruthiSvfCalibration::paperApproved;

    static constexpr std::uint32_t kEnabledMask = 1u;
    static constexpr int kRoutingShift = 1;
    static constexpr int kMode1Shift = 3;
    static constexpr int kMode2Shift = 5;
    static constexpr int kCalibrationShift = 7;
    static constexpr std::uint32_t kFieldMask = 3u;

    static std::uint32_t pack(const ExperimentalSvfSelection& s) noexcept
    {
        std::uint32_t v = s.enabled ? kEnabledMask : 0u;
        v |= (static_cast<std::uint32_t>(s.routing) & kFieldMask) << kRoutingShift;
        v |= (static_cast<std::uint32_t>(s.mode1) & kFieldMask) << kMode1Shift;
        v |= (static_cast<std::uint32_t>(s.mode2) & kFieldMask) << kMode2Shift;
        if (s.calibration == ShruthiSvfCalibration::eagleHardware)
            v |= 1u << kCalibrationShift;
        return v;
    }

    static ExperimentalSvfSelection unpack(std::uint32_t v) noexcept
    {
        ExperimentalSvfSelection s;
        s.enabled = (v & kEnabledMask) != 0;
        const std::uint32_t routing = (v >> kRoutingShift) & kFieldMask;
        s.routing = routing == 2 ? ShruthiSvfDualRouting::parallel
            : (routing == 1 ? ShruthiSvfDualRouting::serial
                            : ShruthiSvfDualRouting::section1);
        const std::uint32_t m1 = (v >> kMode1Shift) & kFieldMask;
        s.mode1 = m1 == 2 ? ShruthiSvf2pMode::highPass
            : (m1 == 1 ? ShruthiSvf2pMode::bandPass : ShruthiSvf2pMode::lowPass);
        const std::uint32_t m2 = (v >> kMode2Shift) & kFieldMask;
        s.mode2 = m2 == 2 ? ShruthiSvf2pMode::highPass
            : (m2 == 1 ? ShruthiSvf2pMode::bandPass : ShruthiSvf2pMode::lowPass);
        s.calibration = ((v >> kCalibrationShift) & 1u) != 0
            ? ShruthiSvfCalibration::eagleHardware
            : ShruthiSvfCalibration::paperApproved;
        return s;
    }
};

// Control-thread publication / audio-thread application for Dual SVF.
// Dual DSP state is owned exclusively by the audio (or prepare/reset) thread.
class ExperimentalSvfControl
{
 public:
    ExperimentalSvfSelection selection() const noexcept
    {
        return ExperimentalSvfSelection::unpack(requested_.load(std::memory_order_acquire));
    }

    void request(ExperimentalSvfSelection next) noexcept
    {
        requested_.store(ExperimentalSvfSelection::pack(next), std::memory_order_release);
    }

    template <typename Patch>
    void patch(Patch&& fn) noexcept
    {
        std::uint32_t old = requested_.load(std::memory_order_relaxed);
        std::uint32_t neu;
        do
        {
            auto sel = ExperimentalSvfSelection::unpack(old);
            fn(sel);
            neu = ExperimentalSvfSelection::pack(sel);
        } while (! requested_.compare_exchange_weak(old, neu,
                                                    std::memory_order_release,
                                                    std::memory_order_relaxed));
    }

    // Audio / prepare / reset thread only. Calibration stays state-continuous.
    // Enable-on and routing changes keep the previous Dual reset semantics.
    void applyOnAudioThread(ShruthiSvfDual& dual, bool& enabledOut) noexcept
    {
        const auto next = selection();
        if (next.enabled != applied_.enabled && next.enabled)
            dual.reset();
        if (next.routing != applied_.routing)
            dual.setRouting(next.routing);
        dual.setMode1(next.mode1);
        dual.setMode2(next.mode2);
        dual.setCalibration(next.calibration);
        applied_ = next;
        enabledOut = next.enabled;
    }

 private:
    static_assert(std::atomic<std::uint32_t>::is_always_lock_free,
                  "Experimental Dual-SVF publication must be realtime lock-free");
    std::atomic<std::uint32_t> requested_ { 0 };
    ExperimentalSvfSelection applied_ {};
};

}  // namespace swaraxt
