// Copyright 2026 MontroneDSP.
// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include "shruthi/patch.h"
#include <algorithm>
#include <cstdint>

namespace swaraxt {

struct ShruthiSvfSection2Cv {
    std::uint8_t cutoff, resonance;
};

// shruthi-1 56bfe78 voice.cc: LoadSources, ProcessModulationMatrix,
// UpdateDestinations. Replay only the two CV lanes with the transaction's
// original sources. The live voice's board identity and FX CVs stay untouched.
template <typename Voice>
ShruthiSvfSection2Cv reconstructSvfSection2(const shruthi::Patch& patch, const Voice& voice,
    std::uint8_t cutoff, std::uint8_t resonance, bool relative) noexcept
{
    std::int32_t cv1 = std::min<int>(cutoff, 127) * 128;
    std::int32_t cv2 = std::min<int>(resonance, 63) * 256;
    for (int i = 0; i < shruthi::kModulationMatrixSize; ++i)
    {
        const auto& row = patch.modulation_matrix.modulation[i];
        if (row.destination != shruthi::MOD_DST_CV_1 && row.destination != shruthi::MOD_DST_CV_2)
            continue;
        if (row.source >= shruthi::kNumModulationSources) continue;
        int amount = row.amount;
        if (i == shruthi::kModulationMatrixSize - 1)
        {
            const int product = amount * voice.modulation_source(shruthi::MOD_SRC_WHEEL);
            // AVR arithmetic right shift rounds negative wheel products down.
            amount = product >= 0 ? product / 256 : -((-product + 255) / 256);
        }
        int source = voice.modulation_source(row.source);
        if (row.source <= shruthi::MOD_SRC_LFO_2 || row.source == shruthi::MOD_SRC_PITCH_BEND
            || row.source == shruthi::MOD_SRC_NOTE || row.source == shruthi::MOD_SRC_AUDIO)
            source -= 128;
        auto& destination = row.destination == shruthi::MOD_DST_CV_1 ? cv1 : cv2;
        // Valid firmware amounts are -63..63. int32 also bounds every int8
        // amount, source byte and 14-bit accumulator without signed overflow.
        destination = std::clamp(destination + amount * source, 0, 16383);
    }
    if (relative)
        cv1 = std::clamp(cv1 + static_cast<int>(voice.filter_cutoff14()) - 8192, 0, 16383);
    return { static_cast<std::uint8_t>(cv1 / 64), static_cast<std::uint8_t>(cv2 / 64) };
}

} // namespace swaraxt
