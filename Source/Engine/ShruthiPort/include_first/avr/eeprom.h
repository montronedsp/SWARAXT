// Copyright 2026 MontroneDSP.
// SPDX-License-Identifier: GPL-3.0-or-later
// In-memory EEPROM stub for Shruthi system settings on desktop hosts.

#ifndef SWARAXT_AVR_EEPROM_H_
#define SWARAXT_AVR_EEPROM_H_

#include <stdint.h>
#include <string.h>

static inline void eeprom_write_byte(uint8_t* addr, uint8_t value)
{
    (void) addr;
    (void) value;
}

static inline uint8_t eeprom_read_byte(const uint8_t* addr)
{
    (void) addr;
    return 0;
}

static inline void eeprom_write_block(const void* src, void* dst, size_t n)
{
    (void) src;
    (void) dst;
    (void) n;
}

static inline void eeprom_read_block(void* dst, const void* src, size_t n)
{
    (void) src;
    memset(dst, 0, n);
}

#endif  // SWARAXT_AVR_EEPROM_H_
