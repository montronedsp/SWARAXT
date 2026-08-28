// Copyright 2026 MontroneDSP.
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include "Engine/Filter/Circuit/Ir3109Types.h"

#include <cmath>

namespace swaraxt {

class DiodeLimiter {
 public:
    void setLimitVolts(double volts) noexcept { limitVolts_ = volts > 0.01 ? volts : 0.01; }

    double process(double voltage) const noexcept
    {
        // Smooth anti-parallel diode-pair approximation. It is intentionally
        // odd and reference-free; diode bias belongs in the circuit operating
        // point, not in the plugin audio signal.
        const double limited = limitVolts_ * std::tanh(voltage / limitVolts_);
        return clampFinite(limited, -limitVolts_, limitVolts_);
    }

 private:
    double limitVolts_ = 1.35;
};

class PhaseSplitter {
 public:
    struct Output
    {
        double inverted = 0.0;
        double nonInverted = 0.0;
    };

    void reset() noexcept { last_ = {}; }

    Output process(double input) noexcept
    {
        last_ = evaluate(input);
        return last_;
    }

    Output evaluate(double input) const noexcept
    {
        return { -input * collectorGain_, input * emitterGain_ };
    }

    double inverted() const noexcept { return last_.inverted; }
    double nonInverted() const noexcept { return last_.nonInverted; }

    void setGains(double collector, double emitter) noexcept
    {
        collectorGain_ = collector;
        emitterGain_ = emitter;
    }

 private:
    double collectorGain_ = 1.0;
    double emitterGain_ = 0.85;
    Output last_ {};
};

class ResonanceNetwork {
 public:
    struct Evaluation
    {
        double phaseSplitInverted = 0.0;
        double phaseSplitNonInverted = 0.0;
        double diodeLimiterOutput = 0.0;
        double feedback = 0.0;
        double outputContribution = 0.0;
    };

    void reset() noexcept
    {
        splitter_.reset();
        last_ = {};
    }

    void setAmount(double amount01) noexcept
    {
        amount_ = clampFinite(amount01, 0.0, 1.0);

        // Resonance onset is deliberately spread across the upper third of the
        // control. The small-signal loop reaches self-oscillation just below the
        // top of the knob and remains bounded by the diode pair.
        const double shaped = std::pow(amount_, 1.35);
        feedbackGain_ = shaped * 4.45;

        // The output-side contribution is deliberately modest and centered.
        // It restores a little level at high resonance without acting as a DC
        // compensation or generic loudness boost.
        outputMix_ = amount_ * amount_ * 0.10;
    }

    Evaluation evaluate(double stage4Volts) const noexcept
    {
        const auto split = splitter_.evaluate(stage4Volts);
        const double limitedTap = diodes_.process(split.inverted);
        const double feedback = limitedTap * feedbackGain_;
        const double output = split.nonInverted * outputMix_;
        return { split.inverted, split.nonInverted, limitedTap, feedback, output };
    }

    Evaluation process(double stage4Volts) noexcept
    {
        last_ = evaluate(stage4Volts);
        splitter_.process(stage4Volts);
        return last_;
    }

    double outputContribution() const noexcept { return last_.outputContribution; }
    double feedback() const noexcept { return last_.feedback; }
    double diodeLimiterOutput() const noexcept { return last_.diodeLimiterOutput; }
    double phaseSplitInverted() const noexcept { return last_.phaseSplitInverted; }
    double phaseSplitNonInverted() const noexcept { return last_.phaseSplitNonInverted; }
    double feedbackGain() const noexcept { return feedbackGain_; }

 private:
    PhaseSplitter splitter_;
    DiodeLimiter diodes_;
    Evaluation last_ {};
    double amount_ = 0.0;
    double feedbackGain_ = 0.0;
    double outputMix_ = 0.0;
};

}  // namespace swaraxt
