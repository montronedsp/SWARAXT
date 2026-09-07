// Copyright 2026 MontroneDSP.
// SPDX-License-Identifier: GPL-3.0-or-later
//
// HD-only sub-oscillator + transient tests under the strict -Werror gate.
// No Classic tables; the analytic HD waves and the faithful paths are checked
// for range, duty, octave behaviour, determinism and one-shot exhaustion.

#include <cmath>
#include <cstdint>
#include <cstdio>
#include <vector>

#include "avrlib_hd/random.h"
#include "avrlib_hd/sub_oscillator.h"
#include "avrlib_hd/transient_generator.h"

namespace {

int gFailures = 0;

void expect(bool ok, const char* message) {
  if (!ok) {
    std::fprintf(stderr, "FAIL: %s\n", message);
    ++gFailures;
  }
}

int renderSubOscBound(const char* message, int blocks) {
  const float kIn[avrlib_hd::kAudioBlockSize] = {};
  float buf[avrlib_hd::kAudioBlockSize];
  int bad = 0;
  for (uint8_t shape = 0; shape < 6; ++shape) {
    avrlib_hd::HdSubOscillator o;
    o.set_increment(0x000400u);
    o.Reset();
    for (int b = 0; b < blocks; ++b) {
      for (int i = 0; i < avrlib_hd::kAudioBlockSize; ++i) {
        buf[i] = kIn[i];
      }
      o.RenderHd(shape, buf, avrlib_hd::kAudioBlockSize, 1.0f);
      for (int i = 0; i < avrlib_hd::kAudioBlockSize; ++i) {
        if (std::isnan(buf[i]) || !(buf[i] >= -1.0f && buf[i] <= 1.0f)) {
          ++bad;
        }
      }
    }
  }
  expect(bad == 0, message);
  return bad;
}

void testSubOscHdBounded() {
  renderSubOscBound("HD sub-osc stays in [-1,1] over all shapes", 100);
}

void testSubOscDutyAndOctave() {
  const int kNum = 48000;
  std::vector<float> x_sq(kNum);
  std::vector<float> x_pul(kNum);
  std::vector<float> x_tri(kNum);
  std::vector<float> x_sq2(kNum);

  const uint32_t kInc = 0x8000u;  // ~0.00195 cycles/sample at base.
  avrlib_hd::HdSubOscillator o;
  o.set_increment(kInc);
  o.Reset();
  for (int i = 0; i < kNum; ++i) {
    o.RenderHd(avrlib_hd::kSubOscSquare1, &x_sq[i], 1, 1.0f);
  }
  o.Reset();
  for (int i = 0; i < kNum; ++i) {
    o.RenderHd(avrlib_hd::kSubOscPulse1, &x_pul[i], 1, 1.0f);
  }
  o.Reset();
  for (int i = 0; i < kNum; ++i) {
    o.RenderHd(avrlib_hd::kSubOscTriangle1, &x_tri[i], 1, 1.0f);
  }
  o.Reset();
  for (int i = 0; i < kNum; ++i) {
    o.RenderHd(avrlib_hd::kSubOscSquare2, &x_sq2[i], 1, 1.0f);
  }

  int crossings_sq = 0;
  int crossings_sq2 = 0;
  double mean_sq = 0.0;
  double mean_pul = 0.0;
  double mean_tri = 0.0;
  double duty_pul = 0.0;
  for (int i = 1; i < kNum; ++i) {
    mean_sq += x_sq[i];
    mean_pul += x_pul[i];
    mean_tri += x_tri[i];
    if (x_pul[i] > 0.0f) duty_pul += 1.0;
    if (x_sq[i - 1] < 0.0f && x_sq[i] >= 0.0f) ++crossings_sq;
    if (x_sq2[i - 1] < 0.0f && x_sq2[i] >= 0.0f) ++crossings_sq2;
  }
  mean_sq /= kNum;
  mean_pul /= kNum;
  mean_tri /= kNum;
  duty_pul /= kNum;

  expect(std::fabs(mean_sq) < 0.05, "HD square is DC-balanced (50% duty)");
  expect(std::fabs(mean_tri) < 0.05, "HD triangle is DC-balanced");
  expect(duty_pul > 0.2 && duty_pul < 0.3,
         "HD pulse-25% has a ~25% positive duty");
  expect(mean_pul > -0.6 && mean_pul < -0.4,
         "HD pulse-25% mean is around -0.5");
  const double ratio = crossings_sq > 0
      ? static_cast<double>(crossings_sq) / static_cast<double>(crossings_sq2)
      : 0.0;
  expect(crossings_sq > 10 && ratio > 1.6 && ratio < 2.4,
         "HD square2 is one octave below square1");
}

void testSubOscFaithfulDeterminism() {
  avrlib_hd::HdSubOscillator a;
  a.set_increment(0x0A0000u);
  avrlib_hd::HdSubOscillator b;
  b.set_increment(0x0A0000u);
  uint8_t buf_a[avrlib_hd::kAudioBlockSize];
  uint8_t buf_b[avrlib_hd::kAudioBlockSize];
  for (int i = 0; i < 2000; ++i) {
    for (int j = 0; j < avrlib_hd::kAudioBlockSize; ++j) {
      buf_a[j] = static_cast<uint8_t>(i * 7 + j);
      buf_b[j] = static_cast<uint8_t>(i * 7 + j);
    }
    a.RenderNaiveByte(1, buf_a, 120);
    b.RenderNaiveByte(1, buf_b, 120);
    for (int j = 0; j < avrlib_hd::kAudioBlockSize; ++j) {
      if (buf_a[j] != buf_b[j]) {
        expect(false, "faithful sub-osc render is deterministic");
        return;
      }
    }
  }
}

void testTransientHdBounded() {
  avrlib_hd::Random rng;
  rng.Seed(0x21);
  avrlib_hd::HdTransientGenerator t;
  t.Init(&rng);
  t.Trigger();
  float buf[avrlib_hd::kAudioBlockSize];
  int rendered = 0;
  for (int b = 0; b < 200; ++b) {
    for (int i = 0; i < avrlib_hd::kAudioBlockSize; ++i) {
      buf[i] = 0.0f;
    }
    t.RenderHd(avrlib_hd::kSubOscBlow, buf, avrlib_hd::kAudioBlockSize, 1.0f);
    for (int i = 0; i < avrlib_hd::kAudioBlockSize; ++i) {
      if (std::isnan(buf[i]) || !(buf[i] >= -1.0f && buf[i] <= 1.0f)) {
        expect(false, "HD transient blow stays in [-1,1]");
        return;
      }
      ++rendered;
    }
  }
  expect(rendered > 0, "HD transient rendered audible samples");
}

void testTransientFaithfulExhaustion() {
  uint8_t buf[avrlib_hd::kAudioBlockSize];
  avrlib_hd::HdTransientGenerator t;
  t.Trigger();
  int nonzero = 0;
  bool tail_clean = true;
  for (int b = 0; b < 20; ++b) {
    for (int i = 0; i < avrlib_hd::kAudioBlockSize; ++i) {
      buf[i] = 0;
    }
    t.RenderNaiveByte(avrlib_hd::kSubOscGlitch, buf, 255);
    for (int i = 0; i < avrlib_hd::kAudioBlockSize; ++i) {
      if (buf[i] != 0) {
        ++nonzero;
        if (b >= 8) {
          tail_clean = false;
        }
      }
    }
  }
  // The one-shot counter runs 255..0 (one decrement per sample); with glitch
  // every sample of the run is non-zero except the occasional rng==0 hole, so
  // the run spans ~255 samples and everything past block 8 is silence.
  expect(nonzero >= 240 && nonzero <= 260,
         "faithful GLITCH transient is a contiguous ~255-sample run");
  expect(tail_clean, "faithful GLITCH transient is fully silent after block 8");
}

void testTransientFaithfulDeterminism() {
  avrlib_hd::HdTransientGenerator a;
  avrlib_hd::HdTransientGenerator b;
  a.Trigger();
  b.Trigger();
  uint8_t buf_a[avrlib_hd::kAudioBlockSize];
  uint8_t buf_b[avrlib_hd::kAudioBlockSize];
  for (int blk = 0; blk < 40; ++blk) {
    for (int i = 0; i < avrlib_hd::kAudioBlockSize; ++i) {
      buf_a[i] = 0;
      buf_b[i] = 0;
    }
    a.RenderNaiveByte(avrlib_hd::kSubOscGlitch, buf_a, 200);
    b.RenderNaiveByte(avrlib_hd::kSubOscGlitch, buf_b, 200);
    for (int i = 0; i < avrlib_hd::kAudioBlockSize; ++i) {
      if (buf_a[i] != buf_b[i]) {
        expect(false, "faithful transient render is deterministic");
        return;
      }
    }
  }
}

}  // namespace

int main() {
  testSubOscHdBounded();
  testSubOscDutyAndOctave();
  testSubOscFaithfulDeterminism();
  testTransientHdBounded();
  testTransientFaithfulExhaustion();
  testTransientFaithfulDeterminism();
  if (gFailures == 0) {
    std::printf("AvrlibHdSubTransUnitTests: all passed\n");
    return 0;
  }
  std::printf("AvrlibHdSubTransUnitTests: %d FAILURES\n", gFailures);
  return 1;
}