// Copyright 2026 MontroneDSP.
// SPDX-License-Identifier: GPL-3.0-or-later
//
// Regression: vowel formant reset must use AVR's 16-bit unsigned wrap.
// A signed widening add (the previous desktop expression) resets when
// phase + noise is a small negative, which the firmware does not.
//
// Trigger: phase 40, noise -96, increment 10.
//   AVR:    uint16(40 + 65440) = 65480 < 10  -> no reset
//   signed: 40 + -96 = -56 < 10              -> reset
// The following sample then reads formant index 16 instead of 0.

#include <cstdint>
#include <cstdio>
#include <cstring>

#include "avrlib_hd/oscillator.h"
#include "avrlib_hd/vowel.h"

namespace {

int gFailures = 0;

void expect(bool ok, const char* message) {
  if (!ok) {
    std::fprintf(stderr, "FAIL: %s\n", message);
    ++gFailures;
  }
}

uint8_t gSine[256];
uint8_t gSquare[256];
uint8_t gVowel[21];

void initTables() {
  std::memset(gSine, 0, sizeof(gSine));
  std::memset(gSquare, 0, sizeof(gSquare));
  std::memset(gVowel, 0, sizeof(gVowel));
  gSine[0] = 80;
  gSine[16] = 200;
  gVowel[7] = 32;   // vowel 1, formant 0 increment
  gVowel[13] = 9;   // vowel 1, noise byte
}

// Independent of the production helpers. Mirrors the integer sample path
// with an explicit 16-bit wrap on the breath compare.
float avrExpectedObservation() {
  uint16_t phase = 0;
  uint16_t formant = 0;
  bool updated = false;
  uint8_t noiseMod = 0;
  const uint16_t increment = 10;
  const int8_t rng = static_cast<int8_t>(244);
  float sample = 0.0f;
  for (int block = 0; block < 5; ++block) {
    const uint8_t update = static_cast<uint8_t>((block + 1) & 3);
    if (update == 0) {
      // balance 15 against vowel 1: (32 * 15) << 3, noise (9 * 15) >> 4.
      updated = true;
      noiseMod = static_cast<uint8_t>((9 * 15) >> 4);
    }
    const uint16_t formantInc = updated ? static_cast<uint16_t>((32 * 15) << 3) : 0;
    formant = static_cast<uint16_t>(formant + formantInc);
    const uint8_t index = static_cast<uint8_t>(((formant >> 8) & 0xf0u) | 0u);
    const int sum = static_cast<int>(gSine[index]) + static_cast<int>(gSine[0]) + 0;
    int8_t r = static_cast<int8_t>(static_cast<uint8_t>(sum & 0xff));
    const uint8_t scale = static_cast<uint8_t>(~static_cast<uint8_t>(phase >> 8));
    r = static_cast<int8_t>((static_cast<int16_t>(r) * scale) >> 8);
    phase = static_cast<uint16_t>(phase + increment);
    const int16_t noise = static_cast<int16_t>(rng * static_cast<int8_t>(noiseMod));
    const uint16_t wrapped = static_cast<uint16_t>(phase + static_cast<uint16_t>(noise));
    if (wrapped < increment) {
      formant = 0;
    }
    int x = 4 * static_cast<int>(r);
    if (x < -128) x = -128;
    if (x > 127) x = 127;
    const int byte = x + 128;
    sample = static_cast<float>(byte - 128) * (1.0f / 128.0f);
  }
  return sample;
}

float renderHdVowel() {
  avrlib_hd::HdVowel v;
  avrlib_hd::VowelTables tables;
  tables.vowel_data = gVowel;
  tables.formant_sine = gSine;
  tables.formant_square = gSquare;
  v.set_tables(tables);
  float out[2] = {};
  for (int i = 0; i < 5; ++i) {
    v.RenderNaive(10, 0x0f, 244, 2, out);
  }
  return out[0];
}

float renderProductionOscillator() {
  avrlib_hd::HdOscillator osc;
  avrlib_hd::OscillatorTables tables{};
  tables.vowel_data = gVowel;
  tables.formant_sine = gSine;
  tables.formant_square = gSquare;
  osc.set_tables(tables);
  avrlib_hd::Random rng;
  rng.Seed(0xF400);  // state_msb() == 244
  osc.set_random(&rng);
  osc.set_parameter(0x0f);
  float out[2] = {};
  const uint32_t increment = 10u << 8;
  for (int i = 0; i < 5; ++i) {
    osc.RenderNaiveFull(avrlib_hd::kRealVowel, 60, increment, 2, out, nullptr, nullptr);
  }
  return out[0];
}

}  // namespace

int main() {
  initTables();
  const float expected = avrExpectedObservation();
  expect(expected == 92.0f / 128.0f, "independent AVR model yields byte 220");
  expect(renderHdVowel() == expected, "HdVowel breath reset matches AVR wrap");
  expect(renderProductionOscillator() == expected,
         "production vowel oscillator matches AVR wrap");
  // The old signed compare produces silence (-1) on this input.
  expect(expected != -1.0f, "the triggering input is not the signed-desktop result");
  if (gFailures == 0) {
    std::printf("VowelPhaseWrapTests: all passed\n");
    return 0;
  }
  std::printf("VowelPhaseWrapTests: %d FAILURES\n", gFailures);
  return 1;
}
