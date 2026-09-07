// Copyright 2026 MontroneDSP.
// SPDX-License-Identifier: GPL-3.0-or-later
//
// avrlib_hd resource tests: the ResourcesLoader must convert the real Classic
// Shruthi prog_uint8_t tables into float arrays with exact equality, and the
// FloatTable/Lookup reads must be exact on integer indices and linear on
// fractional ones. Links the genuine Shruthi tables (resources.cc) as the
// reference data.

#include <cmath>
#include <cstdio>
#include <cstdint>
#include <cstdlib>
#include <vector>

#include "avrlib_hd/avrlib_hd.h"

// Classic Shruthi resource tables, host-compiled into AvrlibHdResourceData.
// prog_uint8_t == uint8_t on the host toolchain; the tables live in the shruthi
// namespace (resources.cc).
namespace shruthi {
extern const uint8_t wav_res_formant_sine[];
extern const uint8_t wav_res_formant_square[];
extern const uint8_t wav_res_vowel_data[];
extern const uint8_t wav_res_env_expo[];
extern const uint8_t wav_res_waves[];
}  // namespace shruthi

using shruthi::wav_res_formant_sine;
using shruthi::wav_res_formant_square;
using shruthi::wav_res_vowel_data;
using shruthi::wav_res_env_expo;
using shruthi::wav_res_waves;

namespace {

const uint32_t kFormantSineLen = 256;
const uint32_t kFormantSquareLen = 256;
const uint32_t kVowelDataLen = 63;  // 9 vowels x 7 bytes [f0 f1 f2 a0 a1 a2 noise]
const uint32_t kEnvExpoLen = 257;
const uint32_t kWavesLen = 16383;

int g_failures = 0;

#define CHECK(cond)                                                 \
  do {                                                              \
    if (!(cond)) {                                                  \
      std::printf("FAIL line %d: %s\n", __LINE__, #cond);           \
      ++g_failures;                                                 \
    }                                                               \
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

void convert_and_assert_exact(const uint8_t* src, uint32_t len, const char* name) {
  std::vector<float> dst(len);
  const uint32_t n = avrlib_hd::ResourcesLoader::ConvertU8(src, len, dst.data());
  CHECK(n == len);
  for (uint32_t i = 0; i < len; ++i) {
    if (dst[i] != static_cast<float>(src[i])) {
      std::printf("FAIL %s[%u]: %g != %u\n", name, i,
                  static_cast<double>(dst[i]), src[i]);
      ++g_failures;
      return;
    }
  }
}

void test_conversion_exact() {
  convert_and_assert_exact(shruthi::wav_res_formant_sine, kFormantSineLen,
                           "wav_res_formant_sine");
  convert_and_assert_exact(shruthi::wav_res_formant_square, kFormantSquareLen,
                           "wav_res_formant_square");
  convert_and_assert_exact(shruthi::wav_res_vowel_data, kVowelDataLen,
                           "wav_res_vowel_data");
  convert_and_assert_exact(shruthi::wav_res_env_expo, kEnvExpoLen,
                           "wav_res_env_expo");
  convert_and_assert_exact(shruthi::wav_res_waves, kWavesLen, "wav_res_waves");
}

void test_vowel_data_structure() {
  std::vector<float> vowel(kVowelDataLen);
  avrlib_hd::ResourcesLoader::ConvertU8(wav_res_vowel_data, kVowelDataLen,
                                        vowel.data());
  for (int row = 0; row < 9; ++row) {
    const int off = row * 7;
    for (int k = 0; k < 7; ++k) {
      CHECK(vowel[static_cast<size_t>(off) + static_cast<size_t>(k)] ==
            static_cast<float>(wav_res_vowel_data[off + k]));
    }
  }
  CHECK(vowel[6] == static_cast<float>(wav_res_vowel_data[6]));
  CHECK(vowel[13] == static_cast<float>(wav_res_vowel_data[13]));
}

void test_lookup_integer() {
  std::vector<float> sine(kFormantSineLen);
  avrlib_hd::ResourcesLoader::ConvertU8(wav_res_formant_sine, kFormantSineLen,
                                        sine.data());
  const avrlib_hd::FloatTable table(sine.data(), kFormantSineLen);
  CHECK(table.valid());
  CHECK(table.size() == kFormantSineLen);
  for (int i = 0; i < static_cast<int>(kFormantSineLen); ++i) {
    CHECK(avrlib_hd::Lookup(table, i) == sine[static_cast<size_t>(i)]);
  }
  CHECK(avrlib_hd::Lookup(table, 0) == static_cast<float>(wav_res_formant_sine[0]));
  CHECK(avrlib_hd::Lookup(table, 255) == static_cast<float>(wav_res_formant_sine[255]));
}

void test_lookup_lerp() {
  std::vector<float> sine(kFormantSineLen);
  avrlib_hd::ResourcesLoader::ConvertU8(wav_res_formant_sine, kFormantSineLen,
                                        sine.data());
  const avrlib_hd::FloatTable table(sine.data(), kFormantSineLen);
  const float mid = 0.5f * (sine[64] + sine[65]);
  CHECK_CLOSE(avrlib_hd::Lookup(table, 64.5f), mid, 1e-6f);
  CHECK_CLOSE(avrlib_hd::Lookup(table, 0.0f), sine[0], 1e-7f);
  CHECK_CLOSE(avrlib_hd::Lookup(table, 255.0f), sine[255], 1e-7f);
  const float t = 0.25f;
  const float raw = avrlib_hd::Lookup(sine.data(), 3, t);
  CHECK_CLOSE(raw, sine[3] + t * (sine[4] - sine[3]), 1e-6f);
}

void test_sample_last_entry_clamp() {
  std::vector<float> square(kFormantSquareLen);
  avrlib_hd::ResourcesLoader::ConvertU8(wav_res_formant_square,
                                        kFormantSquareLen, square.data());
  const avrlib_hd::FloatTable table(square.data(), kFormantSquareLen);
  CHECK_CLOSE(avrlib_hd::Lookup(table, 255.75f), square[255], 1e-7f);
}

}  // namespace

int main() {
  std::printf("AvrlibHdResourceTests start\n");
  test_conversion_exact();
  test_vowel_data_structure();
  test_lookup_integer();
  test_lookup_lerp();
  test_sample_last_entry_clamp();
  if (g_failures == 0) {
    std::printf("AvrlibHdResourceTests: all passed\n");
    return EXIT_SUCCESS;
  }
  std::printf("AvrlibHdResourceTests: %d failure(s)\n", g_failures);
  return EXIT_FAILURE;
}