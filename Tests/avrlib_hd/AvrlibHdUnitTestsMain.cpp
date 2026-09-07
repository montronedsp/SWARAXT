// Copyright 2026 MontroneDSP.
// SPDX-License-Identifier: GPL-3.0-or-later
//
// avrlib_hd unit tests: float primitives, Phase, Random.
// Standalone (no JUCE / no engine). Compiled with -Werror in the HD gate.

#include <cmath>
#include <cstdio>
#include <cstdint>
#include <cstdlib>

#include "avrlib_hd/avrlib_hd.h"

namespace {

int g_failures = 0;

#define CHECK(cond)                                                       \
  do {                                                                    \
    if (!(cond)) {                                                        \
      std::printf("FAIL line %d: %s\n", __LINE__, #cond);                 \
      ++g_failures;                                                       \
    }                                                                     \
  } while (0)

#define CHECK_CLOSE(a, b, tol)                                            \
  do {                                                                    \
    const double va = static_cast<double>(a);                             \
    const double vb = static_cast<double>(b);                             \
    const double vt = static_cast<double>(tol);                           \
    if (std::fabs(va - vb) > vt) {                                        \
      std::printf("FAIL line %d: |%g - %g| > %g\n", __LINE__, va, vb, vt); \
      ++g_failures;                                                       \
    }                                                                     \
  } while (0)

// Reference implementations of the Classic fixed-point primitives, re-derived
// from op.h so the HD float forms are characterised against the original.
inline uint8_t ref_u8mix(uint8_t a, uint8_t b, uint8_t bal) {
  return static_cast<uint8_t>((a * (255u - bal) + b * bal) >> 8);
}

inline uint8_t ref_u8mix4(uint8_t a, uint8_t b, uint8_t ga, uint8_t gb) {
  return static_cast<uint8_t>((a * ga + b * gb) >> 8);
}

inline uint16_t ref_lfsr_update(uint16_t& s) {
  s = static_cast<uint16_t>((s >> 1) ^ (-(s & 1) & 0xb400));
  return s;
}

inline uint8_t ref_lfsr_byte(uint16_t& s) {
  ref_lfsr_update(s);
  return static_cast<uint8_t>(s >> 8);
}

void test_mix_characterization() {
  float max_err = 0.0f;
  for (int aa = 0; aa < 256; ++aa) {
    for (int bb = 0; bb < 256; ++bb) {
      const uint8_t a = static_cast<uint8_t>(aa);
      const uint8_t b = static_cast<uint8_t>(bb);
      for (int t = 0; t < 256; ++t) {
        const float hd = avrlib_hd::mix(static_cast<float>(a),
                                        static_cast<float>(b),
                                        static_cast<float>(t) / 255.0f);
        const float err = std::fabs(hd - static_cast<float>(ref_u8mix(a, b, static_cast<uint8_t>(t))));
        if (err > max_err) {
          max_err = err;
        }
      }
    }
  }
  std::printf("mix: max |hd - classic| over grid = %g\n", static_cast<double>(max_err));
  CHECK(max_err < 2.1f);
  CHECK_CLOSE(avrlib_hd::mix(1.0f, 1.0f, 0.5f), 1.0f, 0.0f);
  CHECK_CLOSE(avrlib_hd::mix(0.0f, 1.0f, 0.25f), 0.25f, 1e-7f);
  CHECK_CLOSE(avrlib_hd::mix(0.0f, 1.0f, 1.0f), 1.0f, 1e-7f);
}

void test_mix4_characterization() {
  float max_err = 0.0f;
  for (int aa = 0; aa < 256; ++aa) {
    for (int bb = 0; bb < 256; ++bb) {
      const uint8_t a = static_cast<uint8_t>(aa);
      const uint8_t b = static_cast<uint8_t>(bb);
      for (int gg = 0; gg < 256; ++gg) {
        const uint8_t ga = static_cast<uint8_t>(gg);
        const uint8_t gb = static_cast<uint8_t>(255 - gg);
        const float hd = avrlib_hd::mix4(static_cast<float>(a),
                                         static_cast<float>(b),
                                         static_cast<float>(ga),
                                         static_cast<float>(gb));
        const float err = std::fabs(hd - static_cast<float>(ref_u8mix4(a, b, ga, gb)));
        if (err > max_err) {
          max_err = err;
        }
      }
    }
  }
  std::printf("mix4: max |hd - classic| over grid = %g\n", static_cast<double>(max_err));
  CHECK(max_err < 2.2f);
  CHECK_CLOSE(avrlib_hd::mix4(0.5f, 0.5f, 1.0f, 1.0f), 0.5f, 1e-7f);
}

void test_clip() {
  CHECK_CLOSE(avrlib_hd::clip(-2.0f, -1.0f, 1.0f), -1.0f, 0.0f);
  CHECK_CLOSE(avrlib_hd::clip(2.0f, -1.0f, 1.0f), 1.0f, 0.0f);
  CHECK_CLOSE(avrlib_hd::clip(0.25f, -1.0f, 1.0f), 0.25f, 0.0f);
}

void test_phase_accumulation() {
  avrlib_hd::Phase ph;
  ph.Reset();
  CHECK(ph.state() == 0);
  const float sr = 44100.0f;
  const float freq = 440.0f;
  const int n = 1 << 20;
  ph.SetStep(avrlib_hd::Phase::Step(freq, sr));
  CHECK(ph.step() > 0);
  for (int i = 0; i < n; ++i) {
    ph.Add();
  }
  const uint64_t expected = static_cast<uint64_t>(n) * ph.step();
  CHECK(ph.state() == expected);
  const double actual_freq =
      static_cast<double>(ph.state()) / 4294967296.0 * static_cast<double>(sr) /
      static_cast<double>(n);
  CHECK_CLOSE(actual_freq, static_cast<double>(freq), 0.01);
  const double expected_int =
      std::floor(static_cast<double>(freq) * static_cast<double>(n) /
                 static_cast<double>(sr));
  CHECK_CLOSE(static_cast<double>(ph.intPart()), expected_int, 1.0);
}

void test_phase_wrap() {
  avrlib_hd::Phase ph;
  ph.Reset();
  ph.SetStep(1ull << 62);
  ph.Add();
  ph.Add();
  ph.Add();
  CHECK(!ph.Wrapped());
  ph.Add();
  CHECK(ph.Wrapped());
  CHECK(ph.state() == 0);
}

void test_phase_index_bits() {
  avrlib_hd::Phase ph;
  ph.Reset();
  ph.SetStep(avrlib_hd::Phase::Step(1000.0f, 44100.0f));
  for (int i = 0; i < 1000; ++i) {
    ph.Add();
  }
  const uint32_t frac = ph.fracPart();
  for (int bits = 1; bits <= 16; ++bits) {
    const uint16_t expected = static_cast<uint16_t>(frac >> (32 - bits));
    CHECK(ph.tableIndexBits(bits) == expected);
  }
  CHECK(ph.tableIndexBits(0) == ph.tableIndexBits(1));
  CHECK(ph.tableIndexBits(24) == ph.tableIndexBits(16));
}

void test_phase_step_guard() {
  CHECK(avrlib_hd::Phase::Step(0.0f, 44100.0f) == 0);
  CHECK(avrlib_hd::Phase::Step(-10.0f, 44100.0f) == 0);
  CHECK(avrlib_hd::Phase::Step(440.0f, 0.0f) == 0);
  CHECK(avrlib_hd::Phase::Step(1e-12f, 44100.0f) == 0);
  CHECK(avrlib_hd::Phase::Step(1.0f, 48000.0f, 32) > 0);
}

void test_random_lfsr_parity() {
  avrlib_hd::Random hd;
  uint16_t ref = 0x21;
  hd.Seed(0x21);
  CHECK(hd.state() == ref);
  for (int i = 0; i < 1000; ++i) {
    const uint8_t expected = ref_lfsr_byte(ref);
    const uint8_t actual = hd.GetByte();
    if (actual != expected) {
      std::printf("FAIL lfsr step %d: %u != %u\n", i, actual, expected);
      ++g_failures;
      break;
    }
  }
  CHECK(hd.state() == static_cast<uint16_t>(ref));
}

void test_random_determinism() {
  avrlib_hd::Random a;
  avrlib_hd::Random b;
  a.Seed(0x1234);
  b.Seed(0x1234);
  for (int i = 0; i < 256; ++i) {
    CHECK(a.GetWord() == b.GetWord());
    CHECK(a.state_msb() == b.state_msb());
  }
}

void test_random_float_range() {
  avrlib_hd::Random r;
  r.Seed(0xabcd);
  int in_range = 0;
  for (int i = 0; i < 100000; ++i) {
    const float v = r.next_float();
    CHECK(!std::isnan(v));
    if (v >= -1.0f && v <= 1.0f) {
      ++in_range;
    }
  }
  CHECK(in_range == 100000);
}

void test_norm_random_stats() {
  avrlib_hd::NormRandom r(42u);
  const int n = 200000;
  double sum = 0.0;
  double sq = 0.0;
  int in_range = 0;
  for (int i = 0; i < n; ++i) {
    const float v = r.next_float();
    CHECK(!std::isnan(v));
    if (v >= -1.0f && v < 1.0f) {
      ++in_range;
    }
    sum += static_cast<double>(v);
    sq += static_cast<double>(v) * static_cast<double>(v);
  }
  const double mean = sum / n;
  const double var = sq / n - mean * mean;
  std::printf("NormRandom: mean=%g stddev=%g in_range=%d\n",
              mean, std::sqrt(var), in_range);
  CHECK(in_range == n);
  CHECK_CLOSE(mean, 0.0, 0.01);
  CHECK_CLOSE(std::sqrt(var), 1.0 / std::sqrt(3.0), 0.02);
}

}  // namespace

int main() {
  std::printf("AvrlibHdUnitTests start\n");
  test_mix_characterization();
  test_mix4_characterization();
  test_clip();
  test_phase_accumulation();
  test_phase_wrap();
  test_phase_index_bits();
  test_phase_step_guard();
  test_random_lfsr_parity();
  test_random_determinism();
  test_random_float_range();
  test_norm_random_stats();
  if (g_failures == 0) {
    std::printf("AvrlibHdUnitTests: all passed\n");
    return EXIT_SUCCESS;
  }
  std::printf("AvrlibHdUnitTests: %d failure(s)\n", g_failures);
  return EXIT_FAILURE;
}