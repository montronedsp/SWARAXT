// Copyright 2026 MontroneDSP. SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
#include "Engine/Filter/Circuit/Ir3109Pole.h"
#include <array>
#include <algorithm>

namespace swaraxt {

// Schematic-derived Shruthi-Analog-IR3109-v01 surrounding circuit. This is an
// idealized OTA/op-amp model, not calibration against a physical instrument.
class Ir3109BoardCore {
public:
    static constexpr double kInputTau = 100000.0 * 4.7e-6; // C10/R7
    static constexpr double kInputPoleTau = 100000.0 * 68e-12; // C6/R4
    static constexpr double kOutputPoleTau = 68000.0 * 100e-12; // C7/R6
    static constexpr double kCutoffCvTau = 5600.0 * 33e-9; // C31/R31
    static constexpr double kResonanceCvTau = 10000.0 * 33e-9; // C30/R25
    static constexpr double kVcaCvTau = 10000.0 * 33e-9; // C14/R19
    static constexpr double kResonanceTrimOhms = 2500.0; // R39 midpoint assumption
    static constexpr double kFeedbackDivider = 220.0 / (4700 + kResonanceTrimOhms + 220);
    static constexpr double kCompensationDivider = 220.0 / (47000 + 220);
    static constexpr double kVcaDivider = 220.0 / (10000 + 220);
    static constexpr double kSharedOutputTau = 4.7e-6 / (1.0 / 10220 + 1.0 / (4700 + kResonanceTrimOhms + 220));
    static constexpr double kSourceVoltsPerUnit = 5.0 * 128.0 / 255.0;

    Ir3109BoardCore() : poles_ {Ir3109Pole(traits(true)), Ir3109Pole(traits(false)),
                                Ir3109Pole(traits(false)), Ir3109Pole(traits(false))} {}
    void prepare(double rate) noexcept {
        sampleRate_ = rate;
        for (auto& pole : poles_) pole.setSampleRate(rate);
        inputAc_.prepare(rate, kInputTau); inputPole_.prepare(rate, kInputPoleTau);
        compensationAc_.prepare(rate, (47000 + 220) * 100e-9); // C34/R26/R27
        sharedAc_.prepare(rate, kSharedOutputTau); // C13, both branches load it
        vcaIvPole_.prepare(rate, 10000 * 10e-12); // C19/R22
        outputAc_.prepare(rate, 33000 * 4.7e-6); // C12/R8, R2 at full volume
        outputPole_.prepare(rate, kOutputPoleTau);
        cutoffAlpha_ = 1 - std::exp(-1 / (rate * kCutoffCvTau));
        resonanceAlpha_ = 1 - std::exp(-1 / (rate * kResonanceCvTau));
        vcaAlpha_ = 1 - std::exp(-1 / (rate * kVcaCvTau));
        reset();
    }
    void reset() noexcept {
        for (auto& pole : poles_) pole.reset();
        inputAc_.reset(); inputPole_.reset(); compensationAc_.reset(); sharedAc_.reset();
        vcaIvPole_.reset(); outputAc_.reset(); outputPole_.reset();
        cutoffCv_ = cutoffTarget_; resonanceCv_ = resonanceTarget_; vcaCv_ = 0;
        lastCutoffCv_ = -1; lastOutput_ = 0;
    }
    void setTargets(double decodedCutoffHz, double resonance, double vca, double explicitCutoffCv = -1) noexcept {
        // Invert the adaptation's byte-CV decode. Plugin octave additions are
        // additional CV, clipped to the physical 0..5 V control domain.
        cutoffTarget_ = explicitCutoffCv >= 0
            ? std::clamp(explicitCutoffCv, 0.0, 5.0)
            : std::clamp((254 + 24 * std::log2(std::max(decodedCutoffHz, 1.0) / 20000)) * 5 / 255, 0.0, 5.0);
        resonanceTarget_ = std::clamp(resonance, 0.0, 1.0) * 5;
        vcaTarget_ = std::clamp(vca, 0.0, 1.0) * 5;
    }
    void advanceControls(double seconds) noexcept {
        cutoffCv_ = cutoffTarget_ + (cutoffCv_ - cutoffTarget_) * std::exp(-seconds / kCutoffCvTau);
        resonanceCv_ = resonanceTarget_ + (resonanceCv_ - resonanceTarget_) * std::exp(-seconds / kResonanceCvTau);
        vcaCv_ = vcaTarget_ + (vcaCv_ - vcaTarget_) * std::exp(-seconds / kVcaCvTau);
    }
    double cutoffCv() const noexcept { return cutoffCv_; }
    double resonanceCv() const noexcept { return resonanceCv_; }
    double vcaControl() const noexcept { return vcaCv_ / 5; }
    bool outputTailActive() const noexcept { return std::abs(lastOutput_) > 1.e-7 || std::abs(outputAc_.state()) > 1.e-7; }
    static double cutoffHzFromCv(double cv) noexcept {
        // Recovered ir3109_scaling.py empirical transfer prior. Selected trim
        // settings: R40=3k (30k+3k input), R41=0 (68k bias). Both are within the
        // schematic ranges; these are explicit calibration assumptions.
        const double chipCv = -5600 * (cv / 33000 - 5.0 / 68000) * 2000 / 8800;
        return 1760 * std::exp2(-(chipCv + .0352) / .0908 * 5);
    }
    float process(float input, int iterations) noexcept {
        cutoffCv_ += cutoffAlpha_ * (cutoffTarget_ - cutoffCv_);
        resonanceCv_ += resonanceAlpha_ * (resonanceTarget_ - resonanceCv_);
        vcaCv_ += vcaAlpha_ * (vcaTarget_ - vcaCv_);
        if (vcaTarget_ == 0 && vcaCv_ < 1.e-12) vcaCv_ = 0;
        if (cutoffCv_ != lastCutoffCv_) {
            const auto hz = cutoffHzFromCv(cutoffCv_);
            for (auto& pole : poles_) pole.setCutoffHz(hz);
            lastCutoffCv_ = cutoffCv_;
        }
        // The input and output op-amp inversions cancel at the output jack.
        // Alternating inverting IR stages are expressed in sign-normalized states.
        const double mixer = inputPole_.low(inputAc_.high(input * kSourceVoltsPerUnit));
        const double compensation = compensationAc_.high(mixer) * kCompensationDivider;
        const double biasCurrent = resonanceCv_ / 33000; // R24 ideal servo, Q2
        const auto feedback = [&](double output) noexcept {
            const double differential = compensation - sharedAc_.previewHigh(output) * kFeedbackDivider;
            return 68000 * biasCurrent * std::tanh(differential / (2 * kOtaThermalVoltageVolts));
        };
        double estimate = poles_[3].output();
        for (int iteration = 0; iteration < iterations && biasCurrent > 0; ++iteration) {
            double predicted = .68 * mixer + feedback(estimate); // R35/R33
            for (auto& pole : poles_) predicted = pole.preview(predicted, pole.snapshot()).output;
            const double residual = predicted - estimate;
            estimate += .72 * residual;
            if (std::abs(residual) < 1.e-9) break;
        }
        double filtered = .68 * mixer + feedback(estimate);
        for (auto& pole : poles_) filtered = pole.process(filtered);
        const double coupled = sharedAc_.high(filtered);
        // LM13700 OTA2 followed by R22 I/V; its two inversions cancel.
        // Offset trimmer R23 is assumed calibrated for zero bias at zero CV.
        const double vca = 10000 * (vcaCv_ / 33000)
            * std::tanh(coupled * kVcaDivider / (2 * kOtaThermalVoltageVolts));
        const double iv = vcaIvPole_.low(vca);
        const double output = outputPole_.low(outputAc_.high(iv)) * (68000.0 / 33000);
        lastOutput_ = clampFinite(output / kSourceVoltsPerUnit, -8.0, 8.0);
        return static_cast<float>(lastOutput_);
    }
private:
    class Pole {
    public:
        void prepare(double rate, double tau) noexcept { gain_ = 1 / (1 + 2 * rate * tau); reset(); }
        void reset() noexcept { state_ = 0; }
        double previewLow(double x) const noexcept { return state_ + gain_ * (x - state_); }
        double previewHigh(double x) const noexcept { return x - previewLow(x); }
        double low(double x) noexcept { const double y = previewLow(x); state_ = 2 * y - state_; return y; }
        double high(double x) noexcept { return x - low(x); }
        double state() const noexcept { return state_; }
    private:
        double gain_ = 0, state_ = 0;
    };
    static Ir3109StageTraits traits(bool first) noexcept {
        Ir3109StageTraits t;
        // INx summing-node divider: 560R to ground, 68k feedback, input 100k
        // on stage one / 68k on the following stages. Capacitors are 220 pF.
        t.differentialInputAttenuation = 1 / (1 / 560.0 + 1 / 68000.0 + 1 / (first ? 100000.0 : 68000.0)) / 68000;
        t.linearBufferToRails = true;
        return t;
    }
    std::array<Ir3109Pole, 4> poles_;
    Pole inputAc_, inputPole_, compensationAc_, sharedAc_, vcaIvPole_, outputAc_, outputPole_;
    double sampleRate_ = 1;
    double cutoffAlpha_ = 1, resonanceAlpha_ = 1, vcaAlpha_ = 1;
    double cutoffTarget_ = 0, resonanceTarget_ = 0, vcaTarget_ = 0;
    double cutoffCv_ = 0, resonanceCv_ = 0, vcaCv_ = 0, lastCutoffCv_ = -1;
    double lastOutput_ = 0;
};
} // namespace swaraxt
