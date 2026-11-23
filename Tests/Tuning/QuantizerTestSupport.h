// Copyright 2026 MontroneDSP.
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include "Tuning/TuningTable.h"

#include <cmath>
#include <cstdio>
#include <string>

namespace quantizer_test {

inline bool makeEdo(int divisions, swaraxt::tuning::TuningSnapshot& snapshot,
                    swaraxt::tuning::ParseError& error)
{
    std::string scl = "Deterministic EDO\n" + std::to_string(divisions) + "\n";
    for (int degree = 1; degree <= divisions; ++degree)
    {
        char line[64];
        std::snprintf(line, sizeof(line), "%.12f\n", 1200.0 * degree / divisions);
        scl += line;
    }
    return swaraxt::tuning::parseScalaAndKeyboard(scl.c_str(), nullptr, snapshot, error);
}

inline double midiHz(int note)
{
    return 440.0 * std::pow(2.0, (note - 69.0) / 12.0);
}

inline int edo256Degree(int note)
{
    return static_cast<int>(std::floor(9.0 + (note - 69.0) * 256.0 / 12.0 + 0.5));
}

inline double edo256Hz(int note)
{
    return 440.0 * std::pow(2.0, (edo256Degree(note) - 9.0) / 256.0);
}

} // namespace quantizer_test
