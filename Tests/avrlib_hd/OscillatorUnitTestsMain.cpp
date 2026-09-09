// Copyright 2026 MontroneDSP.
// SPDX-License-Identifier: GPL-3.0-or-later
//
// HD-only oscillator tests: the band-limited / analytic waves. These run under
// the strict -Werror gate and do NOT depend on the Classic tables. Range, DC,
// pitch (zero crossings) and phase-geometry checks.

#include <cmath>
#include <cstdint>
#include <cstdio>
#include <vector>

#include "avrlib_hd/oscillator.h"

namespace {

int gFailures = 0;

void expect(bool ok, const char* message) {
  if (!ok) {
    std::fprintf(stderr, "FAIL: %s\n", message);
    ++gFailures;
  }
}

struct Stats {
  double mean = 0.0;
  double rms = 0.0;
  double peak = 0.0;
  double pitch = 0.0;
};

Stats analyze(const std::vector<float>& x, double sample_rate) {
  Stats s;
  double mean = 0.0;
  double rms = 0.0;
  int up_crossings = 0;
  for (size_t i = 0; i < x.size(); ++i) {
    double v = x[i];
    mean += v;
    rms += v * v;
    if (std::fabs(v) > s.peak) s.peak = std::fabs(v);
    if (i > 0 && x[i - 1] < 0.0 && v >= 0.0) ++up_crossings;
    if (std::isnan(v)) s.peak = std::nan("bad");
  }
  s.mean = mean / static_cast<double>(x.size());
  s.rms = std::sqrt(rms / static_cast<double>(x.size()));
  s.pitch = up_crossings > 0 ? sample_rate * static_cast<double>(up_crossings) / static_cast<double>(x.size()) : 0.0;
  return s;
}

constexpr uint32_t FrequencyToIncrement(double freq, double rate) {
  const double per_sample = (freq * 65536.0) / rate;
  return static_cast<uint32_t>(per_sample * 256.0 + 0.5);
}

void testHdWavesWithinRange() {
  const double rate = 48000.0;
  const int n = static_cast<int>(rate * 2.0);
  std::vector<float> x(n);
  avrlib_hd::HdOscillator osc;
  for (avrlib_hd::OscWave wave :
       {avrlib_hd::OscWave::kSine, avrlib_hd::OscWave::kSawBlp,
        avrlib_hd::OscWave::kSquareBlp, avrlib_hd::OscWave::kTriangleHd}) {
    osc.Reset();
    uint32_t inc = FrequencyToIncrement(220.0, rate);
    int left = n;
    float* cursor = x.data();
    while (left > 0) {
      int chunk = left > 512 ? 512 : left;
      osc.RenderHd(wave, 57, inc, 128, chunk, cursor);
      cursor += chunk;
      left -= chunk;
    }
    Stats s = analyze(x, rate);
    expect(!std::isnan(s.peak) && s.peak <= 1.25, "HD wave stays within range");
    expect(s.rms > 0.05, "HD wave carries energy");
    if (wave == avrlib_hd::OscWave::kSquareBlp) {
      expect(std::fabs(s.mean) < 0.01, "50% duty pulse has no DC");
    } else {
      expect(std::fabs(s.mean) < 0.05, "HD wave has negligible DC");
    }
    expect(std::fabs(s.pitch - 220.0) < 2.0, "HD wave keeps its pitch");
  }
}

void testHdSinePitchGrid() {
  const double rate = 48000.0;
  const int n = static_cast<int>(rate);
  std::vector<float> x(n);
  avrlib_hd::HdOscillator osc;
  for (double freq : {55.0, 220.0, 1000.0, 5000.0, 12000.0}) {
    osc.Reset();
    uint32_t inc = FrequencyToIncrement(freq, rate);
    osc.RenderHd(avrlib_hd::OscWave::kSine, 50, inc, 0, n, x.data());
    Stats s = analyze(x, rate);
    expect(std::fabs(s.pitch - freq) < freq * 0.02 + 1.0,
           "sine pitch matches across the audible range");
    expect(s.peak <= 1.000001, "sine never exceeds unity");
  }
}

void testHdPhaseProgression() {
  avrlib_hd::HdOscillator osc;
  osc.Reset();
  const uint32_t inc = 0x010201u;
  const int n = 1000;
  std::vector<float> x(n);
  osc.RenderHd(avrlib_hd::OscWave::kSine, 60, inc, 0, n, x.data());
  uint32_t expected = (static_cast<uint64_t>(inc) * n) & 0xffffffu;
  expect(osc.phase_for_tests() == expected, "phase advances by block in 24 bits");
}

void testHdPulseDuty() {
  const double rate = 48000.0;
  const int n = static_cast<int>(rate);
  std::vector<float> x(n);
  avrlib_hd::HdOscillator osc;
  const uint32_t inc = FrequencyToIncrement(98.0, rate);
  for (uint8_t parameter : {uint8_t(0), uint8_t(128), uint8_t(255)}) {
    osc.Reset();
    osc.RenderHd(avrlib_hd::OscWave::kSquareBlp, 50, inc, parameter, n, x.data());
    int high = 0;
    int low = 0;
    for (float v : x) {
      if (v > 0.0f) ++high;
      if (v < 0.0f) ++low;
    }
    double duty = static_cast<double>(high) / (high + low);
    if (parameter == 128) {
      expect(std::fabs(duty - 0.5) < 0.02, "mid parameter keeps 50% duty");
    } else {
      double expected = 0.5 + (static_cast<double>(parameter) - 128.0) * (0.45 / 128.0);
      expect(std::fabs(duty - expected) < 0.02, "pulse duty follows the parameter");
    }
  }
}

void testHdDeterministic() {
  avrlib_hd::HdOscillator a;
  avrlib_hd::HdOscillator b;
  const uint32_t inc = 0x000400u;
  std::vector<float> x(64), y(64);
  a.RenderHd(avrlib_hd::OscWave::kSawBlp, 60, inc, 0, 64, x.data());
  b.RenderHd(avrlib_hd::OscWave::kSawBlp, 60, inc, 0, 64, y.data());
  bool identical = x.size() == y.size();
  for (size_t i = 0; i < x.size() && identical; ++i) {
    identical = x[i] == y[i];
  }
  expect(identical, "HD rendering is deterministic");
}

}  // namespace

int main() {
  testHdWavesWithinRange();
  testHdSinePitchGrid();
  testHdPhaseProgression();
  testHdPulseDuty();
  testHdDeterministic();
  if (gFailures == 0) {
    std::printf("AvrlibHdOscillatorTests: all passed\n");
    return 0;
  }
  std::printf("AvrlibHdOscillatorTests: %d FAILURES\n", gFailures);
  return 1;
}