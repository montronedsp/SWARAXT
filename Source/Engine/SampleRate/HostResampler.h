// Copyright 2026 MontroneDSP.
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

namespace swaraxt {

// Safety-only DC blocker on the host output path (not part of Shruthi DSP).
class DcBlocker {
 public:
    void prepare(double sampleRate) noexcept;
    void reset() noexcept;
    float process(float sample) noexcept;

#if defined(SWARAXT_ENABLE_IDLE_CPU_TESTS)
    float poleForTests() const noexcept { return pole_; }
#endif

 private:
    // 3.5 Hz at 44.1 kHz; prepare() overwrites before processing.
    float pole_ = 0.9995014588f;
    float x1_ = 0.0f;
    float y1_ = 0.0f;
};

}  // namespace swaraxt
