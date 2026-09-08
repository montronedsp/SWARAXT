// Copyright 2026 MontroneDSP. SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
#include "BoardControl.h"
#include "BoardFilter.h"
#include "BoardInputModel.h"
#include "BoardOutputModel.h"

namespace swaraxt::board {
using FloatBlock = std::array<float, blockSize>;
class BoardProcessor {
public:
    void reset(Effect nextEffect = Effect::off) noexcept;
    void processBoard(FloatBlock& samples, const BoardControl& controls,
                      const float* hardwareMix = nullptr) noexcept;
    void processClassicFx(FloatBlock& samples, const BoardControl& controls) noexcept;
    bool hasValidLoop() const noexcept { return effects_.hasRecordedLoop(); }
    const BoardInputModel& inputModel() const noexcept { return input_; }
    static double tailSeconds(const BoardControl& controls) noexcept;
    bool needsAudio(const BoardControl& controls) const noexcept;
private:
    void processEffects(Block& samples, const BoardControl& controls) noexcept;
    void observeTail(const FloatBlock& samples, const BoardControl& controls, bool driven) noexcept;
    BoardInputModel input_;
    BoardOutputModel output_;
    BoardFilter filter_;
    BoardEffects effects_;
    double sinceInput_ = 0;
    double quietTime_ = 0;
    float lastOutput_ = 0;
    bool tailActive_ = false;
};
}
