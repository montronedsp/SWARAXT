// Copyright 2026 MontroneDSP. SPDX-License-Identifier: GPL-3.0-or-later
//
// Shruthi Analog SVF v03 input mixer, V2164 control servo and output I/V.
// Connectivity is the eagle-exhumer a3ae95b XML of the v03 schematic and PCB.
// The matched reference cell cancels the exponential, so the audio gain is a
// current ratio. Nominal build includes the schematic R53 2.2 MΩ bleed.
#pragma once

#include <algorithm>
#include <cmath>

namespace swaraxt {

class DualSvfAnalogBoard
{
 public:
    // v03 values. R53 is fitted on the schematic; the kit text treats an
    // equivalent bleed as a builder option. Production uses the fitted part.
    static constexpr double kC34 = 4.7e-6;
    static constexpr double kR20 = 100.0e3;
    static constexpr double kR13 = 33.0e3;
    static constexpr double kC13 = 100.0e-12;
    static constexpr double kR14 = 100.0e3;
    static constexpr double kR9 = 100.0e3;
    static constexpr double kC9 = 6.8e-9;
    static constexpr double kR24 = 100.0e3;
    static constexpr double kR53 = 2.2e6;
    static constexpr double kRail = 5.0;
    static constexpr double kR31 = 33.0e3;
    static constexpr double kR32 = 33.0e3;
    static constexpr double kR12 = 33.0e3;
    static constexpr double kC20 = 1.0e-6;
    static constexpr double kC21 = 1.0e-6;
    static constexpr double kC14 = 100.0e-12;

    // Equilibrium audio gain for a PWM voltage, with the schematic bleed.
    // Negative results are the cell driven past closure; the audio gain is 0.
    static double steadyGain(double pwmVolts) noexcept
    {
        const double vf = kControlGain * clampPwm(pwmVolts);
        const double ratio = vf / kRail - kBleed;
        return ratio > 0.0 ? ratio : 0.0;
    }

    // Same equilibrium with R53 omitted. Used by the reference check only.
    static double steadyGainUnpopulated(double pwmVolts) noexcept
    {
        const double ratio = kControlGain * clampPwm(pwmVolts) / kRail;
        return ratio > 0.0 ? ratio : 0.0;
    }

    void prepare(double sampleRate) noexcept
    {
        sampleRate_ = sampleRate > 1.0 && std::isfinite(sampleRate) ? sampleRate : 39215.6862745098;
        inputHp_.set(sampleRate_, kR20, kC34);
        inputLp_.set(sampleRate_, kR13, kC13);
        branch1_.set(sampleRate_, kR31, kC20);
        branch2_.set(sampleRate_, kR32, kC21);
        outputLp_.set(sampleRate_, kR12, kC14);
        const double rParallel = 1.0 / (1.0 / kR14 + 1.0 / kR9);
        control_ = std::exp(-1.0 / (sampleRate_ * kC9 * rParallel));
        if (! std::isfinite(control_) || control_ < 0.0 || control_ > 1.0)
            control_ = 0.0;
        reset();
    }

    void reset() noexcept
    {
        inputHp_.reset();
        inputLp_.reset();
        branch1_.reset();
        branch2_.reset();
        outputLp_.reset();
        controlVolts_ = 0.0;
        gain_ = 0.0;
        lastVolts_ = 0.0;
    }

    // Place the PWM reconstruction at equilibrium. Audio capacitors stay put.
    void snapControl(double pwmVolts) noexcept
    {
        controlVolts_ = kControlGain * clampPwm(pwmVolts);
        gain_ = gainFromControl(controlVolts_);
    }

    double gain() const noexcept { return gain_; }
    double lastVolts() const noexcept { return lastVolts_; }

    bool audioMemory() const noexcept
    {
        return inputHp_.active() || inputLp_.active() || branch1_.active()
            || branch2_.active() || outputLp_.active() || std::abs(lastVolts_) > 1.0e-8;
    }

    // Volts into the SVF, after C34/R20 and the inverting R13 feedback.
    double conditionInput(double sourceVolts) noexcept
    {
        if (! std::isfinite(sourceVolts))
            sourceVolts = 0.0;
        const double across = inputHp_.highpass(sourceVolts);
        const double limited = inputLp_.lowpass(across);
        return -kInputGain * limited;
    }

    // branch*Volts are the routed filter outputs. An unused mux branch is 0 V,
    // which discharges that capacitor through its 33 kΩ resistor.
    double processOutput(double branch1Volts, double branch2Volts, double pwmVolts) noexcept
    {
        if (! std::isfinite(branch1Volts))
            branch1Volts = 0.0;
        if (! std::isfinite(branch2Volts))
            branch2Volts = 0.0;
        const double target = kControlGain * clampPwm(pwmVolts);
        controlVolts_ += (1.0 - control_) * (target - controlVolts_);
        if (pwmVolts == 0.0 && controlVolts_ < 1.0e-12)
            controlVolts_ = 0.0;
        gain_ = gainFromControl(controlVolts_);

        const double i1 = branch1_.highpass(branch1Volts);
        const double i2 = branch2_.highpass(branch2Volts);
        // R31 = R32 = R12, so the I/V ratio is 1 and the sum is in volts.
        const double summed = outputLp_.lowpass(i1 + i2);
        const double volts = std::isfinite(summed) ? -gain_ * summed : 0.0;
        lastVolts_ = volts;
        return volts;
    }

 private:
    struct Pole
    {
        double coeff = 0.0;
        double state = 0.0;
        double previous = 0.0;

        void set(double sampleRate, double ohms, double farads) noexcept
        {
            const double a = std::exp(-1.0 / (sampleRate * ohms * farads));
            coeff = std::isfinite(a) ? std::clamp(a, 0.0, 1.0) : 0.0;
            reset();
        }

        void reset() noexcept
        {
            state = 0.0;
            previous = 0.0;
        }

        bool active() const noexcept
        {
            return std::abs(state) > 1.0e-8 || std::abs(previous) > 1.0e-8;
        }

        double highpass(double x) noexcept
        {
            state = coeff * (state + x - previous);
            previous = x;
            if (std::abs(state) < 1.0e-20)
                state = 0.0;
            return state;
        }

        double lowpass(double x) noexcept
        {
            state += (1.0 - coeff) * (x - state);
            if (std::abs(state) < 1.0e-20)
                state = 0.0;
            return state;
        }
    };

    static double clampPwm(double pwmVolts) noexcept
    {
        if (! std::isfinite(pwmVolts) || pwmVolts <= 0.0)
            return 0.0;
        return pwmVolts > kRail ? kRail : pwmVolts;
    }

    static double gainFromControl(double controlVolts) noexcept
    {
        const double ratio = controlVolts / kRail - kBleed;
        return ratio > 0.0 ? ratio : 0.0;
    }

    // R14 and R9 are equal, so the loaded reconstruction gain is 1/2.
    static constexpr double kControlGain = kR9 / (kR14 + kR9);
    // |I_bleed/I_ref| = R24/R53. VEE polarity makes this a closure offset.
    static constexpr double kBleed = kR24 / kR53;
    static constexpr double kInputGain = kR13 / kR20;

    double sampleRate_ = 39215.6862745098;
    double control_ = 0.0;
    double controlVolts_ = 0.0;
    double gain_ = 0.0;
    double lastVolts_ = 0.0;
    Pole inputHp_;
    Pole inputLp_;
    Pole branch1_;
    Pole branch2_;
    Pole outputLp_;
};

} // namespace swaraxt
