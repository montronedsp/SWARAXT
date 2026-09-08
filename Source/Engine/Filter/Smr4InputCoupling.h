// Copyright 2026 MontroneDSP. SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <cmath>

namespace swaraxt {

// SMR4 audio input AC-coupling: C=4.7 µF into R=100 kΩ (smr4_scaling.py /
// original analysis). This is the pre-VCF coupling, not the ~20 kHz output pole
// and not the DSP Board Chebyshev pre-ADC network.
class Smr4InputCoupling {
public:
    static constexpr double kResistanceOhms = 100000.0;
    static constexpr double kCapacitanceFarads = 4.7e-6;
    static constexpr double kCutoffHz = 1.0 / (2.0 * 3.1415926535897932384626433832795
        * kResistanceOhms * kCapacitanceFarads);

    void prepare(double sampleRate) noexcept
    {
        const double rate = sampleRate > 1.0 ? sampleRate : 39215.6862745098;
        pole_ = std::exp(-2.0 * 3.1415926535897932384626433832795 * kCutoffHz / rate);
        reset();
    }

    void reset() noexcept
    {
        previousInput_ = 0.0;
        coupling_ = 0.0;
    }

    float process(float sample) noexcept
    {
        const double in = std::isfinite(sample) ? sample : 0.0;
        coupling_ = (1.0 + pole_) * 0.5 * (in - previousInput_) + pole_ * coupling_;
        previousInput_ = in;
        if (! std::isfinite(coupling_))
        {
            reset();
            return 0.0f;
        }
        return static_cast<float>(coupling_);
    }

private:
    double pole_ = 0.9999458;
    double previousInput_ = 0.0;
    double coupling_ = 0.0;
};

}  // namespace swaraxt
