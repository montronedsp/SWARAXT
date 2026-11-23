// SPDX-FileCopyrightText: 2026 MontroneDSP
// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
#include <cstdio>

inline bool prepareNativeOutput()
{
    return std::setvbuf(stdout, nullptr, _IONBF, 0) == 0;
}
