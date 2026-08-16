// Copyright 2026 MontroneDSP.
// SPDX-License-Identifier: GPL-3.0-or-later
//
// One IR3109-style OTA integrator stage.
//
// The state is a small-signal capacitor voltage, not an absolute chip pin
// voltage. The OTA nonlinearity is an odd, physically scaled approximation of
// the differential-pair current law, so it cannot rectify silence into DC.

#pragma once

#include "Engine/Filter/Circuit/Ir3109Types.h"

#include <cmath>

namespace swaraxt {

class Ir3109Pole {
 public:
    struct Evaluation
    {
        double nextState = 0.0;
        double output = 0.0;
    };

    explicit Ir3109Pole(Ir3109StageTraits traits) noexcept
        : traits_(traits)
    {
    }

    void reset() noexcept
    {
        state_ = 0.0;
        bufferOut_ = 0.0;
    }

    void setSampleRate(double sampleRate) noexcept
    {
        sampleRate_ = sampleRate > 1.0 ? sampleRate : 44100.0;
        updateTptGain();
    }

    // gm/C = 2*pi*f, with the 240 pF capacitor retained as provenance for the
    // cutoff mapping even though it cancels in the normalized integrator.
    void setCutoffHz(double cutoffHz) noexcept
    {
        cutoffHz_ = clampFinite(cutoffHz * traits_.frequencyScale, 1.0, sampleRate_ * 0.45);
        updateTptGain();
    }

    Evaluation evaluate(double inputVolts) const noexcept
    {
        return evaluateFromState(inputVolts, state_);
    }

    Evaluation evaluateFromState(double inputVolts, double state) const noexcept
    {
        const double diff = inputVolts - state;
        const double shapedDiff = shapeOtaInput(diff);

        // TPT one-pole low-pass written as an integrator receiving the OTA
        // differential signal. For small signals shapedDiff ~= diff.
        const double v = shapedDiff / (1.0 + tptG_);
        const double y = state + tptG_ * v;
        const double next = y + tptG_ * v;
        const double out = applyLoadSoftness(y);

        if (! std::isfinite(next) || ! std::isfinite(out))
            return { state * 0.999, 0.0 };
        return { next, out };
    }

    double process(double inputVolts) noexcept
    {
        const Evaluation e = evaluate(inputVolts);
        state_ = clampFinite(e.nextState, -8.0, 8.0);
        const double slewed = applySlew(e.output);
        bufferOut_ = clampFinite(slewed, -8.0, 8.0);
        return bufferOut_;
    }

    double output() const noexcept { return bufferOut_; }
    double integrator() const noexcept { return state_; }
    double cutoffHz() const noexcept { return cutoffHz_; }
    double tptGain() const noexcept { return tptG_; }
    LoadTopology topology() const noexcept { return traits_.loadTopology; }
    const Ir3109StageTraits& traits() const noexcept { return traits_; }

 private:
    void updateTptGain() noexcept
    {
        const double nyquistSafe = sampleRate_ * 0.45;
        const double f = clampFinite(cutoffHz_, 1.0, nyquistSafe);
        tptG_ = std::tan(kPi * f / sampleRate_);
        if (! std::isfinite(tptG_) || tptG_ <= 0.0)
            tptG_ = 1.0e-6;
    }

    double shapeOtaInput(double diffVolts) const noexcept
    {
        const double attenuation = kOtaDifferentialInputAttenuation
                                 / clampFinite(traits_.saturationSoftness, 0.5, 2.0);
        const double differential = diffVolts * attenuation;
        const double pairCurrent = std::tanh(differential / (2.0 * kOtaThermalVoltageVolts));
        const double equivalentVolts = pairCurrent * (2.0 * kOtaThermalVoltageVolts) / attenuation;
        return clampFinite(equivalentVolts, -8.0, 8.0);
    }

    double applyLoadSoftness(double signal) const noexcept
    {
        // Passive and active loads differ only once the stage is driven hard.
        // With no explicit load impedance, the published current limits are not
        // converted into voltage clipping. This odd softening preserves DC.
        const double headroom = traits_.loadTopology == LoadTopology::activeCurrentSource ? 4.25 : 3.75;
        const double y = headroom * std::tanh((signal * traits_.bufferGain) / headroom);
        return clampFinite(y, -8.0, 8.0);
    }

    double applySlew(double target) noexcept
    {
        const double maxDelta = traits_.slewRateVoltsPerSecond / sampleRate_;
        const double delta = target - bufferOut_;
        if (delta > maxDelta)
            return bufferOut_ + maxDelta;
        if (delta < -maxDelta)
            return bufferOut_ - maxDelta;
        return target;
    }

    Ir3109StageTraits traits_;
    double sampleRate_ = 44100.0;
    double cutoffHz_ = kAs3109ReferenceCutoffHz;
    double tptG_ = 0.01;
    double state_ = 0.0;
    double bufferOut_ = 0.0;
};

}  // namespace swaraxt
