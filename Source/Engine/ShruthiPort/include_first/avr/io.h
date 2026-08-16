// Copyright 2026 MontroneDSP.
// SPDX-License-Identifier: GPL-3.0-or-later
// Host stub replacing AVR <avr/io.h> for desktop Shruthi compilation.

#ifndef SWARAXT_AVR_IO_H_
#define SWARAXT_AVR_IO_H_

#include <stdint.h>

#ifndef _BV
#define _BV(bit) (1u << (bit))
#endif

#ifndef _SFR_BYTE
#define _SFR_BYTE(addr) (*(volatile uint8_t*)(addr))
#endif

#ifndef _SFR_WORD
#define _SFR_WORD(addr) (*(volatile uint16_t*)(addr))
#endif

#endif  // SWARAXT_AVR_IO_H_
