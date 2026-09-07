// Copyright 2026 MontroneDSP.
// SPDX-License-Identifier: GPL-3.0-or-later
//
// HD-only env/LFO tests under the strict -Werror gate. These do NOT depend on
// the Classic tables: synthetic table banks exercise the bounds, ranges,
// monotonic segments, sample-and-hold, one-shot latch, dead state and
// determinism of the HD envelope and LFO.

#include <cmath>
#include <cstdint>
#include <cstdio>
#include <vector>

#include "avrlib_hd/envelope.h"
#include "avrlib_hd/lfo.h"
#include "avrlib_hd/random.h"

namespace {

int gFailures = 0;

void expect(bool ok, const char* message) {
  if (!ok) {
    std::fprintf(stderr, "FAIL: %s\n", message);
    ++gFailures;
  }
}

// Synthetic env_expo: linear ramp 0..255, and per-index increments that grow
// with the index so the low indexes are slow and the high ones wrap quickly.
// Modifiable static storage (default zero-init), written once in initTables().
static uint8_t kEnvExpo[257];
static uint16_t kPortamento[128];
int gExpoInit = 0;

void initTables() {
  if (gExpoInit) return;
  // env_expo[0..255] = 0..255, env_expo[256] = 255 (interp guard sample).
  for (int i = 0; i < 256; ++i) {
    kEnvExpo[i] = static_cast<uint8_t>(i);
  }
  kEnvExpo[256] = 255;
  for (int i = 0; i < 128; ++i) {
    kPortamento[i] = static_cast<uint16_t>(i == 0 ? 0 : 1 + 256 * i);
  }
  gExpoInit = 1;
}

avrlib_hd::EnvelopeTables envTables() {
  avrlib_hd::EnvelopeTables t;
  t.portamento_increments = kPortamento;
  t.env_expo = kEnvExpo;
  return t;
}

void testEnvelopeHdSegmentPath() {
  avrlib_hd::HdEnvelope e;
  e.set_tables(envTables());
  e.Init();
  e.Update(4, 4, 63, 4);  // fast attack/decay to sustain 63<<1 = 126.
  e.Trigger(avrlib_hd::kEnvelopeAttack);
  for (int i = 0; i < 5000; ++i) {
    float v = e.RenderHd();
    if (!(v >= 0.0f && v <= 1.0f)) {
      expect(false, "HD envelope stays in [0,1]");
      return;
    }
  }
  expect(e.stage() == avrlib_hd::kEnvelopeSustain ||
             e.stage() == avrlib_hd::kEnvelopeDecay ||
             e.stage() == avrlib_hd::kEnvelopeRelease,
         "fast faders reach sustain/decay and hold");

  // From sustain, release to silence then dead.
  e.Trigger(avrlib_hd::kEnvelopeRelease);
  int dead_at = -1;
  for (int i = 0; i < 5000; ++i) {
    float v = e.RenderHd();
    expect(v >= 0.0f && v <= 1.0f, "HD envelope bounded during release");
    if (e.dead()) {
      dead_at = i;
      break;
    }
  }
  expect(dead_at >= 0, "HD envelope reaches the dead state at end of release");
  if (dead_at >= 0) {
    for (int i = 0; i < 16; ++i) {
      float v = e.RenderHd();
      expect(v == 0.0f, "HD envelope holds silence when dead");
    }
  }
}

void testEnvelopeFaithfulDeterminism() {
  avrlib_hd::HdEnvelope a;
  a.set_tables(envTables());
  a.Init();
  a.Update(64, 16, 127, 32);
  a.Trigger(avrlib_hd::kEnvelopeAttack);
  std::vector<uint8_t> bytes;
  for (int i = 0; i < 2000; ++i) {
    bytes.push_back(a.RenderNaiveByte());
  }
  avrlib_hd::HdEnvelope b;
  b.set_tables(envTables());
  b.Init();
  b.Update(64, 16, 127, 32);
  b.Trigger(avrlib_hd::kEnvelopeAttack);
  for (int i = 0; i < 2000; ++i) {
    if (bytes[static_cast<size_t>(i)] != b.RenderNaiveByte()) {
      expect(false, "faithful ENVELOPE render is deterministic");
      return;
    }
  }
}

// Synthetic LFO waveform bank: 5000 bytes, enough for the WAVE_16 base
// (32*129 + 127 maximal wave lookup).
static uint8_t kWaveBank[5000];
int gWaveInit = 0;

void initWaveBank() {
  if (gWaveInit) return;
  for (int i = 0; i < 5000; ++i) {
    kWaveBank[i] = static_cast<uint8_t>((i * 13) & 0xff);
  }
  gWaveInit = 1;
}

avrlib_hd::LfoTables lfoTables() {
  avrlib_hd::LfoTables t;
  t.portamento_increments = kPortamento;
  t.waves = kWaveBank;
  return t;
}

void testLfoShapesBounded() {
  const uint8_t shapes[] = {
      avrlib_hd::kLfoRamp, avrlib_hd::kLfoSH, avrlib_hd::kLfoTriangle,
      avrlib_hd::kLfoSquare, avrlib_hd::kLfoStepSequencer,
      avrlib_hd::kLfoWave1, avrlib_hd::kLfoWave4, avrlib_hd::kLfoWave16,
  };
  uint8_t step_cc[16] = {3, 1, 4, 1, 5, 9, 2, 6, 5, 3, 5, 8, 9, 7, 9, 3};
  for (uint8_t shape : shapes) {
    for (int mode = avrlib_hd::kLfoModeFree;
         mode < avrlib_hd::kLfoModeLast; ++mode) {
      avrlib_hd::Random rng;
      rng.Seed(0x21);
      avrlib_hd::HdLfo l;
      l.Init(&rng);
      l.set_tables(lfoTables());
      l.Reset();
      l.Update(shape, 0x0400, 127, static_cast<uint8_t>(mode));
      for (int i = 0; i < 3000; ++i) {
        float v = l.RenderHd(step_cc, 8);
        if (std::isnan(v) || !(v >= 0.0f && v <= 1.0f)) {
          std::fprintf(stderr, "  shape=%d mode=%d block=%d v=%f\n", shape,
                       mode, i, v);
          expect(false, "HD LFO stays in [0,1] over all shapes/modes");
          return;
        }
      }
    }
  }
}

void testLfoFaithfulDeterminism() {
  uint8_t step_cc[16] = {0, 1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12, 13, 14, 15};
  avrlib_hd::Random ra;
  ra.Seed(0x21);
  avrlib_hd::HdLfo a;
  a.Init(&ra);
  a.set_tables(lfoTables());
  a.Reset();
  a.Update(avrlib_hd::kLfoSH, 0x0400, 16, avrlib_hd::kLfoModeFree);
  std::vector<uint8_t> bytes;
  for (int i = 0; i < 2000; ++i) {
    bytes.push_back(a.RenderNaiveByte(step_cc, 16));
  }
  avrlib_hd::Random rb;
  rb.Seed(0x21);
  avrlib_hd::HdLfo b;
  b.Init(&rb);
  b.set_tables(lfoTables());
  b.Reset();
  b.Update(avrlib_hd::kLfoSH, 0x0400, 16, avrlib_hd::kLfoModeFree);
  for (int i = 0; i < 2000; ++i) {
    if (bytes[static_cast<size_t>(i)] != b.RenderNaiveByte(step_cc, 16)) {
      expect(false, "faithful LFO (S&H) render is deterministic");
      return;
    }
  }
}

void testLfoRampMonotonicAndSquareBinary() {
  avrlib_hd::HdLfo ramp;
  ramp.set_tables(lfoTables());
  ramp.Reset();
  ramp.Update(avrlib_hd::kLfoRamp, 0x0080, 127, avrlib_hd::kLfoModeFree);
  uint8_t prev = ramp.RenderNaiveByte(nullptr, 8);
  int wraps = 0;
  for (int i = 0; i < 60000; ++i) {
    uint8_t cur = ramp.RenderNaiveByte(nullptr, 8);
    if (static_cast<int>(prev) - static_cast<int>(cur) > 200) {
      ++wraps;
    } else {
      expect(cur >= prev, "faithful RAMP LFO is monotonic within a cycle");
    }
    prev = cur;
  }
  expect(wraps > 0, "faithful RAMP LFO wraps across cycles");

  avrlib_hd::HdLfo sq;
  sq.set_tables(lfoTables());
  sq.Reset();
  sq.Update(avrlib_hd::kLfoSquare, 0x0080, 127, avrlib_hd::kLfoModeFree);
  int seen_zero = 0;
  int seen_max = 0;
  for (int i = 0; i < 60000; ++i) {
    uint8_t cur = sq.RenderNaiveByte(nullptr, 8);
    // The Classic intensity ramp stalls just under 16383 with attack=127
    // (increment 16256 -> depth byte 254), so the two full-depth rails are
    // 1 and 254: (0-128)*254 >> 8 + 128 == 1 and (255-128)*254 >> 8 + 128
    // == 254.
    if (cur == 1) {
      seen_zero = 1;
    } else if (cur == 254) {
      seen_max = 1;
    } else {
      expect(false,
             "faithful SQUARE LFO only outputs the two full-depth rails");
      return;
    }
  }
  expect(seen_zero && seen_max,
         "faithful SQUARE LFO visits both rails at full depth");
}

void testLfoOneShotLatch() {
  avrlib_hd::Random rng;
  rng.Seed(0x21);
  avrlib_hd::HdLfo l;
  l.Init(&rng);
  l.set_tables(lfoTables());
  l.Reset();
  l.Update(avrlib_hd::kLfoTriangle, 0x0400, 127, avrlib_hd::kLfoModeOneShot);
  uint8_t last = l.RenderNaiveByte(nullptr, 8);
  int constant_blocks = 0;
  for (int i = 1; i < 20000; ++i) {
    uint8_t cur = l.RenderNaiveByte(nullptr, 8);
    if (cur == last) {
      ++constant_blocks;
      if (constant_blocks > 2000) break;
    } else {
      constant_blocks = 0;
    }
    last = cur;
  }
  expect(constant_blocks > 2000,
         "faithful ONE_SHOT LFO latches its end-of-cycle value");
}

}  // namespace

int main() {
  initTables();
  initWaveBank();
  testEnvelopeHdSegmentPath();
  testEnvelopeFaithfulDeterminism();
  testLfoShapesBounded();
  testLfoFaithfulDeterminism();
  testLfoRampMonotonicAndSquareBinary();
  testLfoOneShotLatch();
  if (gFailures == 0) {
    std::printf("AvrlibHdEnvLfoUnitTests: all passed\n");
    return 0;
  }
  std::printf("AvrlibHdEnvLfoUnitTests: %d FAILURES\n", gFailures);
  return 1;
}