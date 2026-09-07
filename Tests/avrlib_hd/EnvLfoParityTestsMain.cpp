// Copyright 2026 MontroneDSP.
// SPDX-License-Identifier: GPL-3.0-or-later
//
// Faithful HD env/LFO parity: HdEnvelope::RenderNaiveByte and
// HdLfo::RenderNaiveByte must reproduce the Classic shruthi::Envelope and
// shruthi::Lfo bit-for-bit. Envelopes are driven over the full parameter grid
// with trigger schedules and mid-run parameter edits; LFOs are driven over
// shapes x increments x attack x retrigger modes, including sample-and-hold
// lockstep through the exact Classic LFSR stream.

#include <cstdint>
#include <cstdio>
#include <cstring>
#include <vector>

#include "avrlib/base.h"
#include "avrlib/random.h"
#include "shruthi/envelope.h"
#include "shruthi/lfo.h"
#include "shruthi/resources.h"
#include "shruthi/sequencer_settings.h"
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

avrlib_hd::EnvelopeTables EnvelopeTablesFromClassic() {
  avrlib_hd::EnvelopeTables tables;
  tables.portamento_increments = shruthi::lut_res_env_portamento_increments;
  tables.env_expo = shruthi::wav_res_env_expo;
  return tables;
}

avrlib_hd::LfoTables LfoTablesFromClassic() {
  avrlib_hd::LfoTables tables;
  tables.portamento_increments = shruthi::lut_res_env_portamento_increments;
  tables.waves = shruthi::wav_res_waves;
  return tables;
}

struct EnvParams {
  uint8_t attack;
  uint8_t decay;
  uint8_t sustain;
  uint8_t release;
};

// A trigger either (re)starts the attack or starts the release of the env.
void checkEnvCase(const EnvParams& p, const EnvParams& p2, int blocks,
                  const std::vector<std::pair<int, uint8_t> >& schedule) {
  shruthi::Envelope classic;
  classic.Init();
  classic.Update(p.attack, p.decay, p.sustain, p.release);

  avrlib_hd::HdEnvelope hd;
  hd.set_tables(EnvelopeTablesFromClassic());
  hd.Init();
  hd.Update(p.attack, p.decay, p.sustain, p.release);

  for (int i = 0; i < blocks; ++i) {
    for (size_t j = 0; j < schedule.size(); ++j) {
      if (schedule[j].first == i) {
        if (schedule[j].second == 1) {
          classic.Trigger(shruthi::ATTACK);
          hd.Trigger(avrlib_hd::kEnvelopeAttack);
        } else {
          classic.Trigger(shruthi::RELEASE);
          hd.Trigger(avrlib_hd::kEnvelopeRelease);
        }
      }
    }
    if (i == 300) {
      classic.Update(p2.attack, p2.decay, p2.sustain, p2.release);
      hd.Update(p2.attack, p2.decay, p2.sustain, p2.release);
    }
    if (i == 1000) {
      classic.Update(p.attack, p.decay, p.sustain, p.release);
      hd.Update(p.attack, p.decay, p.sustain, p.release);
    }
    uint8_t cb = classic.Render();
    uint8_t hb = hd.RenderNaiveByte();
    if (cb != hb) {
      std::fprintf(stderr,
                   "  env mismatch block=%d a=%d d=%d s=%d r=%d "
                   "classic=%d hd=%d\n",
                   i, p.attack, p.decay, p.sustain, p.release, cb, hb);
      expect(cb == hb, "faithful HD envelope matches Classic");
      return;
    }
  }
}

void testEnvelopeParity() {
  const EnvParams cases[] = {
      {127, 127, 127, 127},
      {64, 96, 8, 32},
      {1, 32, 96, 64},
      {8, 16, 64, 127},
      {0, 0, 0, 0},
      {32, 32, 32, 32},
      {127, 1, 127, 1},
      {45, 110, 3, 90},
  };
  const int kBlocks = 1500;
  for (size_t i = 0; i < sizeof(cases) / sizeof(cases[0]); ++i) {
    // Attack, sustain, release, retrigger (attack from level), release to
    // dead, then a final retrigger at the very end.
    std::vector<std::pair<int, uint8_t> > schedule;
    schedule.push_back(std::make_pair(16, 1));
    schedule.push_back(std::make_pair(500, 2));
    schedule.push_back(std::make_pair(900, 1));
    schedule.push_back(std::make_pair(1200, 2));
    checkEnvCase(cases[i], cases[(i + 1) % 8], kBlocks, schedule);
  }
}

struct LfoCaseParams {
  uint8_t shape;
  uint16_t increment;
  uint8_t attack;
  uint8_t retrigger_mode;
};

void checkLfoCase(const LfoCaseParams& p, int blocks) {
  avrlib::Random classic_rng;
  classic_rng.Seed(0x21);
  shruthi::Lfo classic;
  classic.Init(&classic_rng);
  classic.Reset();
  classic.Update(p.shape, p.increment, p.attack, p.retrigger_mode);

  avrlib_hd::Random hd_rng;
  hd_rng.Seed(0x21);
  avrlib_hd::HdLfo hd;
  hd.Init(&hd_rng);
  hd.set_tables(LfoTablesFromClassic());
  hd.Reset();
  hd.Update(p.shape, p.increment, p.attack, p.retrigger_mode);

  shruthi::SequencerSettings sequence;
  std::memset(&sequence, 0, sizeof(sequence));
  sequence.pattern_size = 8;
  uint8_t step_cc[16] = {3, 1, 4, 1, 5, 9, 2, 6, 5, 3, 5, 8, 9, 7, 9, 3};
  for (int i = 0; i < 16; ++i) {
    sequence.steps[i].set_controller(step_cc[i]);
  }

  for (int i = 0; i < blocks; ++i) {
    if (i == 300) {
      classic.Update(p.shape, p.increment, p.attack, p.retrigger_mode);
      hd.Update(p.shape, p.increment, p.attack, p.retrigger_mode);
    }
    if (i == 600) {
      classic.Trigger();
      hd.Trigger();
    }
    uint8_t cb = classic.Render(sequence);
    uint8_t hb = hd.RenderNaiveByte(step_cc, sequence.pattern_size);
    if (cb != hb) {
      std::fprintf(stderr,
                   "  lfo mismatch block=%d shape=0x%02x inc=%d a=%d rm=%d "
                   "classic=%d hd=%d\n",
                   i, p.shape, p.increment, p.attack, p.retrigger_mode, cb, hb);
      expect(cb == hb, "faithful HD LFO matches Classic");
      return;
    }
  }
}

void testLfoParity() {
  const uint8_t shapes[] = {
      shruthi::LFO_WAVEFORM_RAMP, shruthi::LFO_WAVEFORM_S_H,
      shruthi::LFO_WAVEFORM_TRIANGLE, shruthi::LFO_WAVEFORM_SQUARE,
      shruthi::LFO_WAVEFORM_STEP_SEQUENCER, shruthi::LFO_WAVEFORM_WAVE_1,
      shruthi::LFO_WAVEFORM_WAVE_4, shruthi::LFO_WAVEFORM_WAVE_7,
      shruthi::LFO_WAVEFORM_WAVE_11, shruthi::LFO_WAVEFORM_WAVE_16,
  };
  const uint16_t increments[] = {
      0x0005u, 0x000Au, 0x00FFu, 0x0400u, 0x1000u, 0x8000u, 0xFFFFu,
  };
  const uint8_t attacks[] = {0, 16, 40, 63, 96, 127};
  const uint8_t retriggers[] = {shruthi::LFO_MODE_FREE,
                                shruthi::LFO_MODE_ONE_SHOT};

  int total_blocks = 0;
  for (uint8_t shape : shapes) {
    for (uint16_t inc : increments) {
      for (uint8_t attack : attacks) {
        for (uint8_t retrigger : retriggers) {
          LfoCaseParams p = {shape, inc, attack, retrigger};
          checkLfoCase(p, 1000);
          total_blocks += 1000;
        }
      }
    }
  }
  std::printf("  lfo parity: %d block-pairs\n", total_blocks);
}

}  // namespace

int main() {
  testEnvelopeParity();
  testLfoParity();
  if (gFailures == 0) {
    std::printf("AvrlibHdEnvLfoParityTests: all passed\n");
    return 0;
  }
  std::printf("AvrlibHdEnvLfoParityTests: %d FAILURES\n", gFailures);
  return 1;
}