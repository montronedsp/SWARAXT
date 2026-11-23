// Copyright 2026 MontroneDSP. SPDX-License-Identifier: GPL-3.0-or-later
// Exhaustive SequenceSnapshot::pack domain: all 256 x 256 uint8 pairs.
#include "Engine/SequenceState.h"

#include <cstdint>
#include <cstdio>

namespace {

// Independent reference: low byte = dataA, high byte = dataB, via multiply
// rather than the production shift expression.
constexpr std::uint16_t independentPack(std::uint8_t dataA, std::uint8_t dataB) noexcept
{
    return static_cast<std::uint16_t>(
        static_cast<unsigned>(dataA) + static_cast<unsigned>(dataB) * 256u);
}

}

int main()
{
    int failures = 0;
    std::uint32_t cases = 0;
    for (int a = 0; a < 256; ++a)
    {
        for (int b = 0; b < 256; ++b)
        {
            const auto dataA = static_cast<std::uint8_t>(a);
            const auto dataB = static_cast<std::uint8_t>(b);
            const auto packed = swaraxt::SequenceSnapshot::pack(dataA, dataB);
            const auto reference = independentPack(dataA, dataB);
            const auto roundA = swaraxt::SequenceSnapshot::dataA(packed);
            const auto roundB = swaraxt::SequenceSnapshot::dataB(packed);
            if (packed != reference || roundA != dataA || roundB != dataB)
            {
                std::printf("FAIL pack a=%u b=%u got=%04x ref=%04x round=%u,%u\n",
                            static_cast<unsigned>(dataA), static_cast<unsigned>(dataB),
                            packed, reference, roundA, roundB);
                ++failures;
                if (failures > 8)
                {
                    std::printf("Sequence pack exhaustive aborted after extra failures\n");
                    return 1;
                }
            }
            ++cases;
        }
    }
    std::printf("Sequence pack exhaustive cases=%u failures=%d\n", cases, failures);
    return failures == 0 && cases == 65536u ? 0 : 1;
}
