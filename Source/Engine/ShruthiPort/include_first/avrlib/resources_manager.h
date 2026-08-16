// Copyright 2026 MontroneDSP.
// SPDX-License-Identifier: GPL-3.0-or-later
// Host-safe ResourcesManager: never load native pointers via pgm_read_word.
// Derived from pichenettes/avril resources_manager.h (GPLv3).

#ifndef AVRLIB_RESOURCES_MANAGER_H_
#define AVRLIB_RESOURCES_MANAGER_H_

#include "avrlib/base.h"

#include <string.h>
#include <avr/pgmspace.h>

namespace avrlib {

template<
  const prog_char* const* strings,
  const prog_uint16_t* const* lookup_tables>
struct ResourcesTables {
  static inline const prog_char* const* string_table() { return strings; }
  static inline const prog_uint16_t* const* lookup_table_table() {
      return lookup_tables;
  }
};

struct NoResourcesTables {
  static inline const prog_char* const* string_table() { return NULL; }
  static inline const prog_uint16_t* const* lookup_table_table() { return NULL; }
};

template<typename ResourceId = uint8_t, typename Tables = NoResourcesTables>
class ResourcesManager {
 public:
  static inline void LoadStringResource(ResourceId resource, char* buffer,
                                        uint8_t buffer_size) {
    if (!Tables::string_table()) {
      return;
    }
    // Host: string_table stores native pointers — do not truncate with pgm_read_word.
    const char* address = Tables::string_table()[resource];
    if (address == nullptr || buffer == nullptr || buffer_size == 0)
      return;
    strncpy_P(buffer, address, buffer_size);
  }

  template<typename ResultType, typename IndexType>
  static inline ResultType Lookup(ResourceId resource, IndexType i) {
    if (!Tables::lookup_table_table()) {
      return 0;
    }
    // Host: lookup_table_table stores native pointers (64-bit on Win64).
    // AVR code used pgm_read_word() which truncates addresses to 16 bits and
    // causes access violations once ResourceId-based lookups run (e.g. groove
    // tables when the internal sequencer clock wraps).
    const prog_uint16_t* address = Tables::lookup_table_table()[resource];
    if (address == nullptr)
      return 0;
    return ResultType(pgm_read_word(address + i));
  }

  template<typename ResultType, typename IndexType>
  static inline ResultType Lookup(const prog_char* p, IndexType i) {
    return ResultType(pgm_read_byte(p + i));
  }

  template<typename ResultType, typename IndexType>
  static inline ResultType Lookup(const prog_uint8_t* p, IndexType i) {
    return ResultType(pgm_read_byte(p + i));
  }

  template<typename ResultType, typename IndexType>
  static inline ResultType Lookup(const prog_uint16_t* p, IndexType i) {
    return ResultType(pgm_read_word(p + i));
  }

  template<typename T>
  static void Load(const prog_char* p, uint8_t i, T* destination) {
    memcpy_P(destination, p + i * sizeof(T), sizeof(T));
  }

  template<typename T, typename U>
  static void Load(const T* p, uint8_t i, U* destination) {
    STATIC_ASSERT(sizeof(T) == sizeof(U));
    memcpy_P(destination, p + i, sizeof(T));
  }

  template<typename T>
  static void Load(const T* p, uint8_t* destination, uint16_t size) {
    memcpy_P(destination, p, size);
  }
};

typedef ResourcesManager<> SimpleResourcesManager;

}  // namespace avrlib

#endif  // AVRLIB_RESOURCES_MANAGER_H_
