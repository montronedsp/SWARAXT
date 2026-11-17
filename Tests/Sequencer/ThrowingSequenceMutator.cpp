// Copyright 2026 MontroneDSP.
// SPDX-License-Identifier: GPL-3.0-or-later

#include "ThrowingSequenceMutator.h"

namespace swaraxt::test_support {

void throwingSequenceMutator(SequenceSnapshot&)
{
    throw 1;
}

}  // namespace swaraxt::test_support
