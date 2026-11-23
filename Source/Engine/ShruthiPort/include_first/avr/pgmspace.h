// Copyright 2026 MontroneDSP.
// Host-side AVR program-memory compatibility for SwaraXt.

#ifndef SWARAXT_AVR_PGMSPACE_H_
#define SWARAXT_AVR_PGMSPACE_H_

#include <stddef.h>
#include <stdint.h>
#include <string.h>

#ifndef PROGMEM
#define PROGMEM
#endif

#ifndef PSTR
#define PSTR(s) (s)
#endif

typedef char prog_char;
typedef uint8_t prog_uint8_t;
typedef int8_t prog_int8_t;
typedef uint16_t prog_uint16_t;
typedef int16_t prog_int16_t;
typedef uint32_t prog_uint32_t;
typedef int32_t prog_int32_t;

static inline uint8_t pgm_read_byte(const void* addr)
{
    return *static_cast<const uint8_t*>(addr);
}

static inline uint16_t pgm_read_word(const void* addr)
{
    uint16_t value;
    memcpy(&value, addr, sizeof(value));
    return value;
}

static inline uint32_t pgm_read_dword(const void* addr)
{
    uint32_t value;
    memcpy(&value, addr, sizeof(value));
    return value;
}

static inline const void* pgm_read_ptr(const void* addr)
{
    const void* value;
    memcpy(&value, addr, sizeof(value));
    return value;
}

static inline char* strncpy_P(char* dest, const char* src, size_t n)
{
    char* const result = dest;
    while (n != 0)
    {
        const char value = *src;
        *dest = value;
        ++dest;
        --n;
        if (value == '\0')
        {
            while (n != 0)
            {
                *dest = '\0';
                ++dest;
                --n;
            }
            break;
        }
        ++src;
    }
    return result;
}

static inline int strcmp_P(const char* a, const char* b)
{
    return strcmp(a, b);
}

static inline size_t strlen_P(const char* s)
{
    return strlen(s);
}

static inline void* memcpy_P(void* dest, const void* src, size_t n)
{
    return memcpy(dest, src, n);
}

#endif  // SWARAXT_AVR_PGMSPACE_H_
