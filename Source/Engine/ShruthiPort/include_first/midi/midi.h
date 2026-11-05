// Copyright 2026 MontroneDSP.
// SPDX-License-Identifier: GPL-3.0-or-later
//
// Host wrapper around the historical Shruthi MIDI parser. Default handler
// stubs leave unused parameters; that is firmware API, not SWARA logic.

#ifndef SWARAXT_HOST_MIDI_MIDI_H_
#define SWARAXT_HOST_MIDI_MIDI_H_

#ifdef _MSC_VER
#pragma warning(push)
#pragma warning(disable : 4100)
#endif

#if defined(__GNUC__)
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wunused-parameter"
#endif

#include "../../../../../third_party/shruthi-1/midi/midi.h"

#if defined(__GNUC__)
#pragma GCC diagnostic pop
#endif

#ifdef _MSC_VER
#pragma warning(pop)
#endif

#endif  // SWARAXT_HOST_MIDI_MIDI_H_
