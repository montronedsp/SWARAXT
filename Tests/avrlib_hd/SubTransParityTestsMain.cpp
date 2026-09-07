// Copyright 2026 MontroneDSP.
// SPDX-License-Identifier: GPL-3.0-or-later
//
// Faithful HD sub-oscillator + transient parity: HdSubOscillator and
// HdTransientGenerator renderers must reproduce the Classic
// shruthi::SubOscillator / shruthi::TransientGenerator bit-for-bit over a
// grid of SubOscillatorAlgorithm shapes (0..5 sub-osc, 6..10 transients),
// increments, amounts and trigger schedules.

#include <cstdint>
#include <cstdio>
#include <cstring>

#include "avrlib/base.h"
#include "shruthi/sub_oscillator.h"
#include "shruthi/transient_generator.h"
#include "avrlib_hd/sub_oscillator.h"
#include "avrlib_hd/transient_generator.h"

namespace {

constexpr int kNum = shruthi::kAudioBlockSize;

int gFailures = 0;

void expect(bool ok, const char* message) {
  if (!ok) {
    std::fprintf(stderr, "FAIL: %s\n", message);
    ++gFailures;
  }
}

uint24_t U24(uint32_t v) {
  uint24_t u;
  u.integral = static_cast<uint16_t>(v >> 8);
  u.fractional = static_cast<uint8_t>(v & 0xff);
  return u;
}

void checkSubOscCase(uint8_t shape, uint32_t increment, uint8_t amount,
                     int blocks) {
  uint8_t classic_buf[kNum];
  uint8_t hd_buf[kNum];
  shruthi::SubOscillator classic;
  classic.set_increment(U24(increment));
  avrlib_hd::HdSubOscillator hd;
  hd.set_increment(increment);
  for (int b = 0; b < blocks; ++b) {
    std::memset(classic_buf, 0, sizeof(classic_buf));
    std::memset(hd_buf, 0, sizeof(hd_buf));
    classic.Render(shape, classic_buf, amount);
    hd.RenderNaiveByte(shape, hd_buf, amount);
    if (std::memcmp(classic_buf, hd_buf, kNum) != 0) {
      int first = -1;
      for (int i = 0; i < kNum; ++i) {
        if (classic_buf[i] != hd_buf[i]) {
          first = i;
          break;
        }
      }
      std::fprintf(stderr,
                   "  sub mismatch shape=%d inc=0x%06X amount=%d b=%d i=%d "
                   "classic=%d hd=%d\n",
                   shape, increment, amount, b, first, classic_buf[first],
                   hd_buf[first]);
      expect(false, "faithful HD sub-oscillator matches Classic");
      return;
    }
  }
}

void testSubOscillatorParity() {
  const uint8_t shapes[] = {0, 1, 2, 3, 4, 5, 200, 255};
  const uint32_t increments[] = {0x000001u, 0x000123u, 0x00FF00u,
                                 0x400000u, 0xFFFFFFu};
  const uint8_t amounts[] = {0, 1, 32, 128, 255};
  for (uint8_t shape : shapes) {
    for (uint32_t inc : increments) {
      for (uint8_t amount : amounts) {
        checkSubOscCase(shape, inc, amount, 4);
      }
    }
  }
  // Stateful: a single instance across many blocks to exercise 24-bit phase
  // wraps across render boundaries.
  checkSubOscCase(0, 0x200000u, 255, 800);
  checkSubOscCase(4, 0x400000u, 77, 800);
}

void checkTransientCase(uint8_t shape, uint8_t amount, int blocks) {
  uint8_t classic_buf[kNum];
  uint8_t hd_buf[kNum];
  shruthi::TransientGenerator classic;
  avrlib_hd::HdTransientGenerator hd;
  for (int b = 0; b < blocks; ++b) {
    if (b == 1) {
      classic.Trigger();
      hd.Trigger();
    }
    std::memset(classic_buf, 0, sizeof(classic_buf));
    std::memset(hd_buf, 0, sizeof(hd_buf));
    classic.Render(shape, classic_buf, amount);
    hd.RenderNaiveByte(shape, hd_buf, amount);
    if (std::memcmp(classic_buf, hd_buf, kNum) != 0) {
      int first = -1;
      for (int i = 0; i < kNum; ++i) {
        if (classic_buf[i] != hd_buf[i]) {
          first = i;
          break;
        }
      }
      std::fprintf(stderr,
                   "  transient mismatch shape=%d amount=%d b=%d i=%d "
                   "classic=%d hd=%d\n",
                   shape, amount, b, first, classic_buf[first], hd_buf[first]);
      expect(false, "faithful HD transient generator matches Classic");
      return;
    }
  }
}

void testTransientParity() {
  // 5 (< click: untouched), 6..10 transients, 11 (> pop: clamped).
  const uint8_t shapes[] = {5, 6, 7, 8, 9, 10, 11};
  const uint8_t amounts[] = {64, 127, 255};
  for (uint8_t shape : shapes) {
    for (uint8_t amount : amounts) {
      // 130 blocks covers the full click/glitch/metallic/pop decays and the
      // much longer (decimated) blow decay.
      checkTransientCase(shape, amount, 130);
    }
  }
}

}  // namespace

int main() {
  testSubOscillatorParity();
  testTransientParity();
  if (gFailures == 0) {
    std::printf("AvrlibHdSubTransParityTests: all passed\n");
    return 0;
  }
  std::printf("AvrlibHdSubTransParityTests: %d FAILURES\n", gFailures);
  return 1;
}