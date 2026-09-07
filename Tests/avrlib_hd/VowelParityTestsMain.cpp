// Copyright 2026 MontroneDSP.
// SPDX-License-Identifier: GPL-3.0-or-later
//
// Faithful HD vowel parity: HdVowel::RenderNaive must reproduce Classic
// shruthi::Oscillator::RenderVowel bit-for-bit over a grid of vowels,
// balances, phase increments and RNG msb bytes. The output float must equal
// (byte - 128) / 128 exactly, with the vowel parameter update cadence,
// formant-phase reset and sync a no-op faithfully mirrored.

#include <cstdint>
#include <cstdio>
#include <cstring>

#include "avrlib/base.h"
#include "avrlib/random.h"
#include "shruthi/oscillator.h"
#include "shruthi/resources.h"
#include "avrlib_hd/vowel.h"

namespace {

int gFailures = 0;

void expect(bool ok, const char* message) {
  if (!ok) {
    std::fprintf(stderr, "FAIL: %s\n", message);
    ++gFailures;
  }
}

constexpr int kNum = avrlib_hd::kAudioBlockSize;

float classicByteToFloat(uint8_t b) {
  return static_cast<float>(static_cast<int>(b) - 128) * (1.0f / 128.0f);
}

class ParityRunner {
 public:
  ParityRunner() {
    tables_.vowel_data = shruthi::wav_res_vowel_data;
    tables_.formant_sine = shruthi::wav_res_formant_sine;
    tables_.formant_square = shruthi::wav_res_formant_square;
  }

  // Seed the Classic RNG and Reset both oscillators so they share the same
  // rng msb byte (which Classic's Reset() embeds in the vowel union residue
  // and RenderVowel samples as its noise source). The Classic oscillator union
  // is zeroed first so render timing (update cadence) is deterministic,
  // matching a freshly spawned voice on the device.
  void seedAndReset(uint16_t seed) {
    std::memset(&classic_, 0, sizeof(classic_));
    random_.Seed(seed);
    classic_.set_random(&random_);
    classic_.Reset();  // advances the LFSR one step.
    noise_ = random_.state_msb();
    hd_.set_tables(tables_);
    hd_.Reset(noise_);
    ++resets_;
  }

  // Render one block with the already-seeded RNG; the Classic Oscillator never
  // advances the RNG inside RenderVowel, so the noise byte is constant across
  // blocks within a reset.
  void runBlock(uint8_t vowel_param, uint16_t increment16) {
    ++blocks_;
    uint24_t increment{};
    increment.integral = increment16;
    increment.fractional = 0;
    std::memset(buffer_, 0, sizeof(buffer_));
    std::memset(hdOut_, 0, sizeof(hdOut_));
    classic_.set_parameter(vowel_param);
    classic_.Render(shruthi::WAVEFORM_VOWEL, 60, increment, nullptr, nullptr,
                    buffer_ + 4);
    hd_.RenderNaive(increment16, vowel_param, noise_, kNum, hdOut_);
    for (int i = 0; i < kNum; ++i) {
      float expected = classicByteToFloat(buffer_[4 + i]);
      if (expected != hdOut_[i]) {
        std::fprintf(stderr,
                     "  mismatch vowel=0x%02x inc16=%d noise=%d i=%d "
                     "classic=%f hd=%f\n",
                     vowel_param, increment16, noise_, i, expected, hdOut_[i]);
        expect(expected == hdOut_[i], "faithful HD vowel matches Classic");
        return;
      }
    }
  }

  void protectedBuffers() {
    expect(buffer_[0] == 0 && buffer_[3] == 0 && buffer_[kNum + 4] == 0,
           "audio guard area untouched");
  }

  int renderedBlocks() const { return blocks_; }

  uint8_t noise() const { return noise_; }

 private:
  avrlib::Random random_;
  shruthi::Oscillator classic_;
  avrlib_hd::VowelTables tables_{};
  avrlib_hd::HdVowel hd_;
  uint8_t noise_ = 0;
  uint8_t buffer_[kNum + 8];
  float hdOut_[kNum];
  int blocks_ = 0;
  int resets_ = 0;
};

void testVowelParityGrid() {
  ParityRunner runner;
  // Valid vowel parameters only: the device exposes a 7-bit oscillator
  // parameter, so the high nibble is capped at 7 and offset_2 stays inside
  // the 63-byte vowel table (0x80+ would index past it, OOB in Classic).
  const uint8_t params[] = {0x00, 0x07, 0x11, 0x28, 0x33, 0x44,
                            0x58, 0x66, 0x77, 0x7f};
  const uint16_t increments[] = {0x0001, 0x0040, 0x0080, 0x0100,
                                 0x0400, 0x1000, 0x8000, 0xFFFF};
  const uint16_t seeds[] = {0x0000, 0x0021, 0x1234, 0x3c5a,
                            0x8000, 0xB400, 0xFFFF};
  for (uint16_t seed : seeds) {
    runner.seedAndReset(seed);
    for (uint8_t vowel_param : params) {
      for (uint16_t increment : increments) {
        runner.runBlock(vowel_param, increment);
      }
    }
  }
  const int kExpected = static_cast<int>(sizeof(seeds) / sizeof(seeds[0])) *
                        static_cast<int>(sizeof(params) / sizeof(params[0])) *
                        static_cast<int>(sizeof(increments) /
                                         sizeof(increments[0]));
  if (runner.renderedBlocks() != kExpected) {
    std::printf("FAIL vowel grid: rendered %d blocks, expected %d\n",
                runner.renderedBlocks(), kExpected);
    ++gFailures;
  }
}

void testVowelParityContinuity() {
  // A single seed, then a long run that crosses several control decimation
  // boundaries: formant increments are the first-updated values, so the run
  // also covers the "corrupted first 3 blocks" residue vanishing at block 4.
  ParityRunner runner;
  runner.seedAndReset(0x55aa);
  const uint8_t noise0 = runner.noise();
  for (int pass = 0; pass < 12; ++pass) {
    runner.runBlock(0x3a, 0x0010);
  }
  if (runner.noise() != noise0) {
    std::printf("FAIL continuity: RNG advanced during vowel render\n");
    ++gFailures;
  }
  runner.protectedBuffers();
}

}  // namespace

int main() {
  testVowelParityGrid();
  testVowelParityContinuity();
  if (gFailures == 0) {
    std::printf("AvrlibHdVowelParityTests: all passed\n");
    return 0;
  }
  std::printf("AvrlibHdVowelParityTests: %d FAILURES\n", gFailures);
  return 1;
}