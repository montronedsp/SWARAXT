// Copyright 2026 MontroneDSP.
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include <cmath>

namespace swaraxt {

struct HostTransportSnapshot {
    double bpm = 120.0;
    double ppqPosition = 0.0;
    bool isPlaying = true;
    bool hasPpqPosition = false;
    bool hasHostTransport = false;
};

inline double sanitizeBpm(double bpm) noexcept
{
    return bpm >= 20.0 && bpm <= 400.0 ? bpm : 120.0;
}

inline bool isValidHostPpq(double ppq) noexcept
{
    // Bound tick conversion and preserve sub-sample precision during alignment.
    // This accommodates more than six decades at 120 BPM.
    return std::isfinite(ppq) && ppq >= 0.0 && ppq <= 4294967296.0;
}

}  // namespace swaraxt
