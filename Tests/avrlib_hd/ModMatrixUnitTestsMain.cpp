// Copyright 2026 MontroneDSP.
// SPDX-License-Identifier: GPL-3.0-or-later
//
// HD-only mod-matrix tests under the strict -Werror gate. The byte-faithful
// primitives are spot-checked numerically (the Classic parity TU is the
// authoritative cross-check); the HD float path is checked for bounds,
// conservation and determinism.

#include <cmath>
#include <cstdint>
#include <cstdio>

#include "avrlib_hd/mod_matrix.h"

namespace {

int gFailures = 0;

void expect(bool ok, const char* message) {
  if (!ok) {
    std::fprintf(stderr, "FAIL: %s\n", message);
    ++gFailures;
  }
}

void testClipU14() {
  expect(avrlib_hd::HdModMatrix::ClipU14(0) == 0, "clip lower bound");
  expect(avrlib_hd::HdModMatrix::ClipU14(8192) == 8192, "clip mid value pass-through");
  expect(avrlib_hd::HdModMatrix::ClipU14(16383) == 16383, "clip upper bound stays");
  expect(avrlib_hd::HdModMatrix::ClipU14(16384) == 16383, "clip above 16383");
  expect(avrlib_hd::HdModMatrix::ClipU14(-1) == 0, "clip negative to zero");
  expect(avrlib_hd::HdModMatrix::ClipU14(32767) == 16383, "clip high positive");
  expect(avrlib_hd::HdModMatrix::ClipU14(-32768) == 0, "clip very negative");
}

void testSignedByteWraps() {
  // Classic S8S8Mul receives `source_value + 128` coerced to int8; the sum is
  // the wrapped byte (268 -> 12), so values above 127 become negative.
  const auto rel = [](uint8_t source) {
    return static_cast<uint8_t>(source + 128);
  };
  expect(avrlib_hd::HdModMatrix::S8S8Mul8(-30, rel(140)) == -30 * 12,
         "relative source 140 sums to +12");
  expect(avrlib_hd::HdModMatrix::S8S8Mul8(63, rel(255)) == 63 * 127,
         "relative source 255 sums to +127");
  expect(avrlib_hd::HdModMatrix::S8S8Mul8(63, rel(128)) == 63 * 0,
         "relative source centre is neutral");
  expect(avrlib_hd::HdModMatrix::S8S8Mul8(-63, rel(0)) == -63 * -128,
         "relative source 0 sums to -128");
}

void testOperatorsByte() {
  using M = avrlib_hd::HdModMatrix;
  expect(M::ApplyOperator(avrlib_hd::kOpCvSum, 64, 32, 0) == 48,
         "sum is the classic average");
  expect(M::ApplyOperator(avrlib_hd::kOpCvProduct, 200, 200, 0) ==
             static_cast<uint8_t>(200 * 200 >> 8),
         "product is U8U8MulShift8");
  expect(M::ApplyOperator(avrlib_hd::kOpCvMax, 10, 200, 0) == 200, "max");
  expect(M::ApplyOperator(avrlib_hd::kOpCvMin, 10, 200, 0) == 10, "min");
  expect(M::ApplyOperator(avrlib_hd::kOpCvXor, 0xF0, 0x55, 0) == 0xA5, "xor");
  expect(M::ApplyOperator(avrlib_hd::kOpCvGe, 200, 120, 0) == 255,
         "ge true yields 255");
  expect(M::ApplyOperator(avrlib_hd::kOpCvGe, 120, 200, 0) == 0,
         "ge false yields 0");
  expect(M::ApplyOperator(avrlib_hd::kOpCvLe, 200, 120, 0) == 0, "le false");
  expect(M::ApplyOperator(avrlib_hd::kOpCvLe, 120, 200, 0) == 255, "le true");
  // Quantize keeps the top bit-length of y.
  expect(M::ApplyOperator(avrlib_hd::kOpCvQuantize, 0x7F, 0x0E, 0) == 0x60,
         "quantize 3-bit mask (0xE0) on 0x7F");
  expect(M::ApplyOperator(avrlib_hd::kOpCvQuantize, 0xFF, 0x03, 0) == 0x80,
         "quantize 1-bit mask (0x80) on 0xFF");
  expect(M::ApplyOperator(avrlib_hd::kOpCvQuantize, 0xF0, 0, 0) == 0,
         "quantize by zero clears");
  // Lag: pos = prev + (x - prev) * (y/256); truncation leaves it within a few
  // LSBs of the target after enough steps.
  uint8_t lag = 0;
  for (int i = 0; i < 200; ++i) {
    lag = M::ApplyOperator(avrlib_hd::kOpCvLagProcessor, 200, 252, lag);
  }
  expect(lag >= 190 && lag <= 200, "lag processor converges to the target");
}

void testMatrixByteBasics() {
  using M = avrlib_hd::HdModMatrix;
  uint8_t sources[avrlib_hd::kNumModulationSources] = {};
  sources[avrlib_hd::kModSourceOffset] = 100;
  sources[avrlib_hd::kModSourceLfo1] = 200;
  int16_t dst[avrlib_hd::kNumModulationDestinations];
  for (int i = 0; i < avrlib_hd::kNumModulationDestinations; ++i) {
    dst[i] = 8192;
  }
  uint8_t vca = 200;
  avrlib_hd::UniModulation rows[2] = {
      {40, avrlib_hd::kModSourceOffset, avrlib_hd::kModDestPwm1},
      {-30, avrlib_hd::kModSourceLfo1, avrlib_hd::kModDestPwm2}};
  uint8_t flags[2] = {9, 9};
  M::ProcessMatrixByte(2, rows, sources, dst, &vca,
                       sources[avrlib_hd::kModSourceWheel], flags);
  // Absolute add: 8192 + 40*100 = 12192.
  expect(dst[avrlib_hd::kModDestPwm1] == 12192, "absolute row adds amount*source");
  // Relative add: source <= LFO_2, source_value 200 -> uint8(200+128=328) = 72.
  expect(dst[avrlib_hd::kModDestPwm2] == 8192 + (-30) * 72,
         "relative row uses the wrapped int8 source");
  expect(flags[0] == 0 && flags[1] == 0, "no trigger-env rows, flags clear");

  // Clipping (within int16 so the sum saturates instead of overflowing):
  // a large positive amount saturates at 16383, a large negative one at 0.
  rows[0] = {63, avrlib_hd::kModSourceOffset, avrlib_hd::kModDestVco1};
  sources[avrlib_hd::kModSourceOffset] = 255;
  M::ProcessMatrixByte(1, rows, sources, dst, &vca, 0, nullptr);
  expect(dst[avrlib_hd::kModDestVco1] == 16383, "additive row clips high");
  rows[0] = {-128, avrlib_hd::kModSourceOffset, avrlib_hd::kModDestVco1};
  M::ProcessMatrixByte(1, rows, sources, dst, &vca, 0, nullptr);
  expect(dst[avrlib_hd::kModDestVco1] == 0, "additive row clips low");

  // Trigger-env flags.
  rows[0] = {1, avrlib_hd::kModSourceOffset, avrlib_hd::kModDestTriggerEnv1};
  M::ProcessMatrixByte(1, rows, sources, dst, &vca, 0, flags);
  expect(flags[0] == 1 && flags[1] == 0, "trigger-env row sets its flag");

  // Wheel scaling applies to the 12th (last) matrix row only.
  int16_t dst2[avrlib_hd::kNumModulationDestinations];
  for (int i = 0; i < avrlib_hd::kNumModulationDestinations; ++i) {
    dst2[i] = 8192;
  }
  sources[avrlib_hd::kModSourceOffset] = 100;
  avrlib_hd::UniModulation wheel_rows[avrlib_hd::kModulationMatrixSize] = {};
  wheel_rows[avrlib_hd::kModulationMatrixSize - 1] = {
      63, avrlib_hd::kModSourceOffset, avrlib_hd::kModDestAttack};
  M::ProcessMatrixByte(avrlib_hd::kModulationMatrixSize, wheel_rows, sources,
                       dst2, &vca, 128, nullptr);
  const int8_t scaled = static_cast<int8_t>(static_cast<int16_t>(63) * 128 >> 8);
  expect(dst2[avrlib_hd::kModDestAttack] == 8192 + scaled * 100,
         "12th row amount is wheel-scaled");
  avrlib_hd::UniModulation early_row = {
      63, avrlib_hd::kModSourceOffset, avrlib_hd::kModDestVco1};
  M::ProcessMatrixByte(1, &early_row, sources, dst2, &vca, 255, nullptr);
  expect(dst2[avrlib_hd::kModDestVco1] == 8192 + 63 * 100,
         "non-last row ignores the wheel");
}

void testVcaByte() {
  using M = avrlib_hd::HdModMatrix;
  uint8_t sources[avrlib_hd::kNumModulationSources] = {};
  sources[avrlib_hd::kModSourceOffset] = 100;
  int16_t dst[avrlib_hd::kNumModulationDestinations] = {};
  avrlib_hd::UniModulation row = {63, avrlib_hd::kModSourceOffset,
                                  avrlib_hd::kModDestVca};
  uint8_t vca = 200;
  const uint8_t u8mix255 = static_cast<uint8_t>(
      (static_cast<uint16_t>(255) * (255 - 252) +
       static_cast<uint16_t>(100) * 252) >> 8);
  M::ProcessMatrixByte(1, &row, sources, dst, &vca,
                       sources[avrlib_hd::kModSourceWheel], nullptr);
  expect(vca == static_cast<uint8_t>(200 * u8mix255 >> 8),
         "VCA amount 63 blends source, then multiplies");

  vca = 200;
  row.amount = -63;
  const uint8_t inv = static_cast<uint8_t>(255 - 100);
  const uint8_t mix_inv = static_cast<uint8_t>(
      (static_cast<uint16_t>(255) * (255 - 252) +
       static_cast<uint16_t>(inv) * 252) >> 8);
  M::ProcessMatrixByte(1, &row, sources, dst, &vca,
                       sources[avrlib_hd::kModSourceWheel], nullptr);
  expect(vca == static_cast<uint8_t>(200 * mix_inv >> 8),
         "negative VCA amount inverts the source first");
}

void testVcaBalanceExhaustive() {
  using M = avrlib_hd::HdModMatrix;
  auto definedBalance = [](int8_t amount) {
    return static_cast<uint8_t>(static_cast<int16_t>(amount) * 4);
  };
  expect(definedBalance(63) == 252, "VCA balance 63 -> 252");
  expect(definedBalance(1) == 4, "VCA balance 1 -> 4");
  expect(definedBalance(-1) == 252, "VCA balance -1 -> 252");
  expect(definedBalance(-64) == 0, "VCA balance -64 -> 0");
  expect(definedBalance(static_cast<int8_t>(-128)) == 0, "VCA balance -128 -> 0");
  for (int i = -128; i <= 127; ++i) {
    const int8_t amount = static_cast<int8_t>(i);
    const uint8_t got = definedBalance(amount);
    const uint8_t ref = static_cast<uint8_t>(static_cast<int>(amount) * 4 & 0xff);
    expect(got == ref, "defined VCA balance matches AVR modulo-256");
  }

  uint8_t sources[avrlib_hd::kNumModulationSources] = {};
  sources[avrlib_hd::kModSourceOffset] = 255;
  int16_t dst[avrlib_hd::kNumModulationDestinations] = {};
  for (int i = -128; i <= 127; ++i) {
    if (i == 0) {
      continue;
    }
    avrlib_hd::UniModulation row = {static_cast<int8_t>(i),
                                    avrlib_hd::kModSourceOffset,
                                    avrlib_hd::kModDestVca};
    uint8_t vca = 255;
    M::ProcessMatrixByte(1, &row, sources, dst, &vca, 0, nullptr);
    expect(vca <= 255, "VCA destination stays in uint8 for every int8 amount");
  }
}

void testFloatOperators() {
  using M = avrlib_hd::HdModMatrix;
  const float eps = 1e-5f;
  expect(std::fabs(M::ApplyOperatorFloat(avrlib_hd::kOpCvSum, 0.2f, 0.8f, 0.0f) - 0.5f) < eps,
         "float sum is the midpoint");
  expect(std::fabs(M::ApplyOperatorFloat(avrlib_hd::kOpCvProduct, 0.5f, 0.9f, 0.0f) - 0.45f) < eps,
         "float product multiplies");
  expect(M::ApplyOperatorFloat(avrlib_hd::kOpCvMax, 0.2f, 0.8f, 0.0f) == 0.8f,
         "float max");
  expect(M::ApplyOperatorFloat(avrlib_hd::kOpCvMin, 0.2f, 0.8f, 0.0f) == 0.2f,
         "float min");
  expect(M::ApplyOperatorFloat(avrlib_hd::kOpCvXor, 0.5f, 0.5f, 0.0f) == 1.0f,
         "float xor peaks on equal inputs");
  expect(M::ApplyOperatorFloat(avrlib_hd::kOpCvGe, 0.9f, 0.1f, 0.0f) == 1.0f,
         "float ge step high");
  expect(M::ApplyOperatorFloat(avrlib_hd::kOpCvLe, 0.9f, 0.1f, 0.0f) == 0.0f,
         "float le step low");
  const float q = M::ApplyOperatorFloat(avrlib_hd::kOpCvQuantize, 0.37f, 0.9f, 0.0f);
  // y=0.9 -> ~7 bits -> a 127-level grid.
  expect(q >= 0.0f && q <= 1.0f && std::fabs(q * 127.0f - std::nearbyint(q * 127.0f)) < 1e-5f,
         "float quantize snaps to its own level grid");
  float lag = 0.2f;
  for (int i = 0; i < 50; ++i) {
    lag = M::ApplyOperatorFloat(avrlib_hd::kOpCvLagProcessor, 0.9f, 0.0f, lag);
  }
  expect(lag > 0.85f && lag <= 0.9f, "float lag converges toward the target");
  expect(M::ApplyOperatorFloat(5, -5.0f, 0.5f, 0.0f) >= 0.0f &&
             M::ApplyOperatorFloat(5, -5.0f, 0.5f, 0.0f) <= 1.0f,
         "float operators clamp into [0,1]");
}

void testFloatMatrix() {
  using M = avrlib_hd::HdModMatrix;
  float sources[avrlib_hd::kNumModulationSources] = {};
  sources[avrlib_hd::kModSourceLfo1] = -1.0f;  // bipolar relative swing
  sources[avrlib_hd::kModSourceOffset] = 0.5f;
  float dst[avrlib_hd::kNumModulationDestinations];
  for (int i = 0; i < avrlib_hd::kNumModulationDestinations; ++i) {
    dst[i] = 0.5f;
  }
  float vca = 0.5f;
  avrlib_hd::UniModulation rows[3] = {
      {40, avrlib_hd::kModSourceLfo1, avrlib_hd::kModDestFilterCutoff},
      {-127, avrlib_hd::kModSourceOffset, avrlib_hd::kModDestPwm1},
      {63, avrlib_hd::kModSourceOffset, avrlib_hd::kModDestVca}};
  float amts[3] = {0.4f, -2.0f, 0.5f};
  M::ProcessMatrixFloat(3, rows, sources, dst, &vca, 0.0f, amts);
  expect(dst[avrlib_hd::kModDestFilterCutoff] > 0.0f &&
             dst[avrlib_hd::kModDestFilterCutoff] < 1.0f,
         "relative source sweeps cutoff within bounds");
  expect(dst[avrlib_hd::kModDestPwm1] == 0.0f, "unipolar add clamps low");
  expect(vca >= 0.0f && vca <= 1.0f, "VCA stays in [0,1]");

  // Determinism: equal inputs give equal outputs.
  float dst_a[avrlib_hd::kNumModulationDestinations];
  float dst_b[avrlib_hd::kNumModulationDestinations];
  for (int i = 0; i < avrlib_hd::kNumModulationDestinations; ++i) {
    dst_a[i] = 0.3f;
    dst_b[i] = 0.3f;
  }
  float vca_a = 0.7f;
  float vca_b = 0.7f;
  avrlib_hd::UniModulation zero_row = {0, avrlib_hd::kModSourceOffset,
                                       avrlib_hd::kModDestPwm1};
  M::ProcessMatrixFloat(3, rows, sources, dst_a, &vca_a, 0.0f, amts);
  M::ProcessMatrixFloat(3, rows, sources, dst_b, &vca_b, 0.0f, amts);
  bool det = true;
  for (int i = 0; i < avrlib_hd::kNumModulationDestinations; ++i) {
    if (dst_a[i] != dst_b[i] || vca_a != vca_b) {
      det = false;
    }
  }
  expect(det, "float matrix is deterministic");

  // Zero-amount rows do nothing and out-of-range source pass clamps.
  avrlib_hd::UniModulation zero_rows[2] = {
      zero_row,
      {127, 31, avrlib_hd::kModDestAttack}};
  bool first = false;
  for (int i = 0; i < avrlib_hd::kNumModulationDestinations; ++i) {
    dst_a[i] = 0.5f;
  }
  vca_a = 0.5f;
  M::ProcessMatrixFloat(1, zero_rows, sources, dst_a, &vca_a, 0.0f, amts);
  for (int i = 0; i < avrlib_hd::kNumModulationDestinations; ++i) {
    if (dst_a[i] != 0.5f) {
      first = true;
    }
  }
  expect(!first && vca_a == 0.5f, "zero-amount rows are inert");
}

}  // namespace

int main() {
  testClipU14();
  testSignedByteWraps();
  testOperatorsByte();
  testMatrixByteBasics();
  testVcaByte();
  testVcaBalanceExhaustive();
  testFloatOperators();
  testFloatMatrix();
  if (gFailures == 0) {
    std::printf("AvrlibHdModMatrixTests: all passed\n");
    return 0;
  }
  std::printf("AvrlibHdModMatrixTests: %d FAILURES\n", gFailures);
  return 1;
}