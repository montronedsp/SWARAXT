// Copyright 2026 MontroneDSP.
// SPDX-License-Identifier: GPL-3.0-or-later

#include "Engine/SampleRate/HostResampler.h"

#include <cmath>

namespace swaraxt {

void DcBlocker::prepare(double sampleRate) noexcept
{
    constexpr double kReferenceRate = 44100.0;
    constexpr double kReferencePole = 0.9999;
    const double validRate = sampleRate > 1.0 ? sampleRate : kReferenceRate;
    pole_ = static_cast<float>(std::pow(kReferencePole, kReferenceRate / validRate));
    reset();
}

void DcBlocker::reset() noexcept
{
    x1_ = 0.0f;
    y1_ = 0.0f;
}

float DcBlocker::process(float sample) noexcept
{
    const float y = sample - x1_ + pole_ * y1_;
    x1_ = sample;
    y1_ = y;
    return y;
}

}  // namespace swaraxt
