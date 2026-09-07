// Copyright 2026 MontroneDSP.
// SPDX-License-Identifier: GPL-3.0-or-later
//
// HD vowel unit tests under the strict -Werror gate with synthetic tables: the
// faithful render's arithmetic is spot-checked (the Classic parity TU is the
// authoritative cross-check), and the HD float path is checked for bounds,
// determinism, cadence and NaN-freedom.

#include <cmath>
#include <cstdint>
#include <cstdio>

#include "avrlib_hd/vowel.h"

namespace {

int gFailures = 0;

void expect(bool ok, const char* message) {
  if (!ok) {
    std::fprintf(stderr, "FAIL: %s\n", message);
    ++gFailures;
  }
}

// Synthetic but structurally faithful tables: 9 vowels x 7 bytes
// (3 increments, 3 amplitudes, 1 noise) and small formant sign tables.
const uint8_t kSyntheticVowel[avrlib_hd::HdVowel::kNumVowels * 7] = {
    27, 40, 89, 15, 13, 1, 0,  18, 51, 62, 13, 12, 6, 0,
    15, 69, 93, 14, 12, 7, 0,  10, 84, 110, 13, 10, 8, 0,
    23, 44, 87, 15, 12, 1, 0,  13, 29, 80, 13, 8, 0, 0,
    6,  46, 81, 12, 3, 0, 0,   9,  51, 95, 15, 3, 0, 3,
    6,  73, 99, 7,  3, 14, 9,
};

const uint8_t kSyntheticSine[256] = {
    0,
};
const uint8_t kSyntheticSquare[256] = {
    0,
};

avrlib_hd::VowelTables syntheticTables() {
  avrlib_hd::VowelTables t;
  t.vowel_data = kSyntheticVowel;
  t.formant_sine = kSyntheticSine;
  t.formant_square = kSyntheticSquare;
  return t;
}

void testToSample() {
  expect(avrlib_hd::HdVowel::ToSample(0) == -1.0f, "byte 0 -> -1");
  expect(avrlib_hd::HdVowel::ToSample(128) == 0.0f, "byte 128 -> 0");
  expect(avrlib_hd::HdVowel::ToSample(255) == 127.0f / 128.0f, "byte 255 -> 127/128");
}

void testS8U8MulShift8() {
  using M = avrlib_hd::HdVowel;
  expect(M::S8U8MulShift8(63, 64) == static_cast<int8_t>((63 * 64) >> 8),
         "wheel-like scale matches the host op semantics");
  expect(M::S8U8MulShift8(-128, 255) == static_cast<int8_t>((-128 * 255) >> 8),
         "clamp-negative scale matches arithmetic shift");
}

void testVowelInterpolation() {
  avrlib_hd::HdVowel v;
  v.set_tables(syntheticTables());
  // Exercise the 4-block control cadence, then verify the faithful 16-bit
  // phase integral advances once per two-sample pair per render.
  float out[avrlib_hd::kAudioBlockSize];
  const uint16_t kIncrement = 0x0080;
  const int kBlocks = 12;
  for (int b = 0; b < kBlocks; ++b) {
    v.RenderNaive(kIncrement, 0x30, 0, avrlib_hd::kAudioBlockSize, out);
  }
  uint16_t expected_phase =
      static_cast<uint16_t>(kIncrement * (avrlib_hd::kAudioBlockSize / 2));
  expected_phase = static_cast<uint16_t>(expected_phase * kBlocks);
  expect(v.phase_for_tests() == expected_phase,
         "faithful phase integral advances 20 increments/block");
}

void testFaithfulInvariants() {
  avrlib_hd::VowelTables t = syntheticTables();
  avrlib_hd::HdVowel v;
  v.set_tables(t);
  float out[avrlib_hd::kAudioBlockSize];
  for (int b = 0; b < 50; ++b) {
    v.RenderNaive(0x0010, 0x6a, 0x3c, avrlib_hd::kAudioBlockSize, out);
    for (int i = 0; i < avrlib_hd::kAudioBlockSize; ++i) {
      expect(out[i] >= -1.0f && out[i] <= 1.0f, "faithful output stays in [-1,1]");
      if (out[i] < -1.0f || out[i] > 1.0f) return;
    }
  }
}

void testHdBoundsAndDeterminism() {
  avrlib_hd::VowelTables t = syntheticTables();
  avrlib_hd::HdVowel a;
  avrlib_hd::HdVowel b;
  a.set_tables(t);
  b.set_tables(t);
  float oa[avrlib_hd::kAudioBlockSize];
  float ob[avrlib_hd::kAudioBlockSize];
  bool finite = true;
  for (int blk = 0; blk < 100; ++blk) {
    a.RenderHd(0x008000u, 0x6a, 0.5f, avrlib_hd::kAudioBlockSize, oa);
    b.RenderHd(0x008000u, 0x6a, 0.5f, avrlib_hd::kAudioBlockSize, ob);
    for (int i = 0; i < avrlib_hd::kAudioBlockSize; ++i) {
      if (!std::isfinite(oa[i]) || !std::isfinite(ob[i])) finite = false;
      expect(oa[i] >= -1.0f && oa[i] <= 1.0f, "HD output stays in [-1,1]");
      expect(oa[i] == ob[i], "HD render is deterministic");
    }
  }
  expect(finite, "HD output is finite");

  // Guard: zero increment (no fundamental) must not divide by zero.
  avrlib_hd::HdVowel z;
  z.set_tables(t);
  float oz[avrlib_hd::kAudioBlockSize];
  z.RenderHd(0, 0x30, 0.0f, avrlib_hd::kAudioBlockSize, oz);
  expect(std::isfinite(oz[0]) && oz[0] >= -1.0f && oz[0] <= 1.0f,
         "zero-increment HD render is safe");
}

}  // namespace

int main() {
  testToSample();
  testS8U8MulShift8();
  testVowelInterpolation();
  testFaithfulInvariants();
  testHdBoundsAndDeterminism();
  if (gFailures == 0) {
    std::printf("AvrlibHdVowelTests: all passed\n");
    return 0;
  }
  std::printf("AvrlibHdVowelTests: %d FAILURES\n", gFailures);
  return 1;
}