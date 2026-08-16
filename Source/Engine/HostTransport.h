// Copyright 2026 MontroneDSP.
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

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

}  // namespace swaraxt
