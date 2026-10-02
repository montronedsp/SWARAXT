// Copyright 2026 MontroneDSP. SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
#include "Plugin/SwaraXtParameterLayout.h"
#include <array>

namespace swaraxt {
inline constexpr std::array<const char*, 9> sequenceParameterIds {
    IDs::seqMode, IDs::seqTempo, IDs::seqSwing, IDs::seqClockMode, IDs::seqGate,
    IDs::arpDirection, IDs::arpPattern, IDs::arpOctaves, IDs::arpGate
};
inline bool isSequenceParameter(juce::StringRef id) noexcept
{
    for (const auto* candidate : sequenceParameterIds)
        if (id == juce::StringRef(candidate)) return true;
    return false;
}
}
