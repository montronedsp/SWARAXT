// Copyright 2026 MontroneDSP.
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include "Engine/SequenceState.h"

namespace swaraxt::test_support {

// Defined out of line so optimized MSVC cannot treat SequenceState::revise's
// publish path as unreachable when this mutator always throws (C4702 under /WX).
void throwingSequenceMutator(SequenceSnapshot& snapshot);

}  // namespace swaraxt::test_support
