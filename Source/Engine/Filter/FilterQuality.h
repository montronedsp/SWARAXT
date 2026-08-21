// Copyright 2026 MontroneDSP.
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include <cstdint>

namespace swaraxt {

enum class FilterQuality : uint8_t
{
    high = 0,
    normal = 1,
    eco = 2
};

}  // namespace swaraxt
