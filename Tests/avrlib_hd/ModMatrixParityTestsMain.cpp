// Copyright 2026 MontroneDSP.
// SPDX-License-Identifier: GPL-3.0-or-later
//
// Mod-matrix parity: the faithful HdModMatrix byte pipeline
// (ProcessOperatorsByte + ProcessMatrixByte) must reproduce the REAL adapted
// shruthi::Voice::ProcessControlBlock bit-for-bit.
//
// The harness drives a real shruthi::Voice: it writes a crafted patch
// (matrix rows + CV operators), pre-sets the byte sources that LoadSources
// does not overwrite, runs control blocks, and captures every 14-bit
// destination via the debug taps together with all 32 source bytes and the
// multiplicative VCA byte. UpdateDestinations only consumes dst_ into the
// 8-bit destination bytes, so the captured 14-bit destinations are exactly
// the post-matrix values. The HD faithful path replays the same captured
// sources and the same patch-derived base destinations through
// HdModMatrix and must match on all 27 destinations and the VCA byte.

#include <cstdint>
#include <cstdio>
#include <cstring>

#include "avrlib/random.h"
#include "shruthi/audio_out.h"
#include "shruthi/midi_dispatcher.h"
#include "shruthi/part.h"
#include "shruthi/patch.h"
#include "shruthi/storage.h"
#include "shruthi/voice.h"
#include "avrlib_hd/mod_matrix.h"

#if !SWARAXT_ENABLE_SHRUTHI_DEBUG_TAPS
#error SWARAXT_ENABLE_SHRUTHI_DEBUG_TAPS must be enabled for these tests.
#endif

namespace {

int gFailures = 0;
int gChecks = 0;

void expect(bool ok, const char* message) {
  ++gChecks;
  if (!ok) {
    std::fprintf(stderr, "FAIL: %s\n", message);
    ++gFailures;
  }
}

void zeroMatrix(shruthi::Part& part) {
  auto* patch = part.mutable_patch();
  for (int row = 0; row < shruthi::kModulationMatrixSize; ++row) {
    patch->modulation_matrix.modulation[row].source =
        shruthi::MOD_SRC_OFFSET;
    patch->modulation_matrix.modulation[row].destination =
        shruthi::MOD_DST_VCA;
    patch->modulation_matrix.modulation[row].amount = 0;
  }
}

struct ShruthiRuntime {
  shruthi::HostAudioRing ring;
  avrlib::Random random;
  shruthi::MidiDispatcher midi;
  shruthi::Storage storage;
  shruthi::Part part;

  void init() {
    random.Seed(0x21);
    ring.Init();
    part.Init(ring, random, midi, storage);
    part.ProcessBlock();
    ring.Init();
  }
};

// Patch fields that drive LoadSources' destination/parameter initialisation.
struct PatchBase {
  uint8_t filter_cutoff;
  uint8_t filter_resonance;
  uint8_t osc1_parameter;
  uint8_t osc2_parameter;
  uint8_t mix_balance;
  uint8_t mix_noise;
  uint8_t mix_sub_osc;
  uint8_t env0_attack, env0_decay, env0_sustain, env0_release;
  uint8_t env1_attack, env1_decay, env1_sustain, env1_release;
  uint8_t volume;
};

PatchBase kPatchBase = {
    80, 40, 96, 32, 64, 16, 200,
    100, 200, 60, 90,
    20, 150, 120, 30,
    220};

void applyPatchBase(shruthi::Part& part, const PatchBase& base) {
  auto* patch = part.mutable_patch();
  patch->filter_cutoff = base.filter_cutoff;
  patch->filter_resonance = base.filter_resonance;
  patch->osc[0].parameter = base.osc1_parameter;
  patch->osc[1].parameter = base.osc2_parameter;
  patch->mix_balance = base.mix_balance;
  patch->mix_noise = base.mix_noise;
  patch->mix_sub_osc = base.mix_sub_osc;
  patch->env[0].attack = base.env0_attack;
  patch->env[0].decay = base.env0_decay;
  patch->env[0].sustain = base.env0_sustain;
  patch->env[0].release = base.env0_release;
  patch->env[1].attack = base.env1_attack;
  patch->env[1].decay = base.env1_decay;
  patch->env[1].sustain = base.env1_sustain;
  patch->env[1].release = base.env1_release;
  patch->ops_[0].op = shruthi::OP_CV_NONE;
  patch->ops_[1].op = shruthi::OP_CV_NONE;
  part.mutable_voice()->set_volume(base.volume);
}

// Faithful replication of Voice::LoadSources destination initialisation.
void initDst14(const PatchBase& base, int16_t dst14[27]) {
  dst14[avrlib_hd::kModDestFilterCutoff] =
      static_cast<int16_t>(static_cast<uint16_t>(base.filter_cutoff) * 128);
  // VCA is multiplicative byte, not a 14-bit destination; classic dst_[VCA] stays 0.
  dst14[avrlib_hd::kModDestVca] = 0;
  dst14[avrlib_hd::kModDestPwm1] =
      static_cast<int16_t>(static_cast<uint16_t>(base.osc1_parameter) * 128);
  dst14[avrlib_hd::kModDestPwm2] =
      static_cast<int16_t>(static_cast<uint16_t>(base.osc2_parameter) * 128);
  for (int d : {avrlib_hd::kModDestVco1, avrlib_hd::kModDestVco2,
                avrlib_hd::kModDestVco1_2Coarse, avrlib_hd::kModDestVco1_2Fine,
                avrlib_hd::kModDestAttack, avrlib_hd::kModDestLfo1,
                avrlib_hd::kModDestLfo2}) {
    dst14[d] = 8192;
  }
  dst14[avrlib_hd::kModDestMixBalance] =
      static_cast<int16_t>(base.mix_balance << 8);
  dst14[avrlib_hd::kModDestMixNoise] =
      static_cast<int16_t>(base.mix_noise << 8);
  dst14[avrlib_hd::kModDestMixSubOsc] =
      static_cast<int16_t>(base.mix_sub_osc << 8);
  dst14[avrlib_hd::kModDestFilterResonance] =
      static_cast<int16_t>(base.filter_resonance << 8);
  dst14[avrlib_hd::kModDestCv1] = 0;
  dst14[avrlib_hd::kModDestCv2] = 0;
  dst14[avrlib_hd::kModDestTriggerEnv1] = 0;
  dst14[avrlib_hd::kModDestTriggerEnv2] = 0;
  const uint8_t env_stages[4] = {base.env0_attack, base.env0_decay,
                                 base.env0_sustain, base.env0_release};
  for (int i = 0; i < 4; ++i) {
    dst14[avrlib_hd::kModDestAttack1 + i] =
        static_cast<int16_t>(static_cast<uint16_t>(env_stages[i]) * 128);
  }
  const uint8_t env1_stages[4] = {base.env1_attack, base.env1_decay,
                                  base.env1_sustain, base.env1_release};
  for (int i = 0; i < 4; ++i) {
    dst14[avrlib_hd::kModDestAttack2 + i] =
        static_cast<int16_t>(static_cast<uint16_t>(env1_stages[i]) * 128);
  }
}

void setStableSource(shruthi::Part& part, uint8_t source, uint8_t value) {
  if (source != shruthi::MOD_SRC_ENV_1 && source != shruthi::MOD_SRC_ENV_2 &&
      source != shruthi::MOD_SRC_NOTE && source != shruthi::MOD_SRC_GATE &&
      source != shruthi::MOD_SRC_AUDIO &&
      source != shruthi::MOD_SRC_VELOCITY &&
      source != shruthi::MOD_SRC_RANDOM) {
    part.mutable_voice()->set_modulation_source(source, value);
  }
}

void capture(const shruthi::Part& part, uint8_t sources[32], int16_t dst14[27],
             uint8_t* vca) {
  const shruthi::Voice& voice = part.voice();
  for (int i = 0; i < 32; ++i) {
    sources[i] = voice.modulation_source(static_cast<uint8_t>(i));
  }
  for (int d = 0; d < 27; ++d) {
    dst14[d] = voice.debug_destination14(static_cast<uint8_t>(d));
  }
  *vca = voice.modulation_destination(shruthi::MOD_DST_VCA);
}

// Replays one classic control block through the faithful path and compares.
void replayAndCompare(const shruthi::Part& part, const PatchBase& base,
                      int num_rows,
                      const shruthi::Modulation* rows, int blocks) {
  uint8_t sources[32];
  int16_t classic_dst[27];
  uint8_t classic_vca;
  capture(part, sources, classic_dst, &classic_vca);

  avrlib_hd::UniModulation uni[shruthi::kModulationMatrixSize];
  for (int i = 0; i < num_rows; ++i) {
    uni[i].amount = rows[i].amount;
    uni[i].source = static_cast<uint8_t>(rows[i].source);
    uni[i].destination = static_cast<uint8_t>(rows[i].destination);
  }

  int16_t hd_dst[27];
  initDst14(base, hd_dst);
  uint8_t hd_vca = base.volume;
  avrlib_hd::HdModMatrix::ProcessMatrixByte(
      num_rows, uni, sources, hd_dst, &hd_vca, sources[shruthi::MOD_SRC_WHEEL],
      nullptr);

  for (int d = 0; d < 27; ++d) {
    char msg[96];
    std::snprintf(msg, sizeof(msg), "block %d dst[%d]=%d classic=%d", blocks,
                  d, hd_dst[d], classic_dst[d]);
    if (hd_dst[d] != classic_dst[d]) {
      std::fprintf(stderr,
                   "  dst14 mismatch d=%d hd=%d classic=%d (block %d, rows=%d)\n",
                   d, hd_dst[d], classic_dst[d], blocks, num_rows);
    }
    expect(hd_dst[d] == classic_dst[d], msg);
  }
  if (hd_vca != classic_vca) {
    std::fprintf(stderr, "  vca mismatch hd=%d classic=%d\n", hd_vca,
                 classic_vca);
  }
  expect(hd_vca == classic_vca, "VCA byte matches");
  static_cast<void>(blocks);
}

// Runs a grid of single matrix rows with the given source palette.
void testSingleRows() {
  const uint8_t destinations[] = {
      shruthi::MOD_DST_FILTER_CUTOFF,     shruthi::MOD_DST_PWM_1,
      shruthi::MOD_DST_PWM_2,             shruthi::MOD_DST_VCO_1,
      shruthi::MOD_DST_VCO_1_2_COARSE,    shruthi::MOD_DST_MIX_BALANCE,
      shruthi::MOD_DST_MIX_NOISE,         shruthi::MOD_DST_MIX_SUB_OSC,
      shruthi::MOD_DST_FILTER_RESONANCE,  shruthi::MOD_DST_CV_1,
      shruthi::MOD_DST_CV_2,              shruthi::MOD_DST_ATTACK,
      shruthi::MOD_DST_LFO_1,             shruthi::MOD_DST_LFO_2,
      shruthi::MOD_DST_TRIGGER_ENV_1,     shruthi::MOD_DST_TRIGGER_ENV_2,
      shruthi::MOD_DST_ATTACK_1,          shruthi::MOD_DST_DECAY_2,
      shruthi::MOD_DST_SUSTAIN_1,         shruthi::MOD_DST_RELEASE_2};
  const uint8_t relative_sources[] = {
      shruthi::MOD_SRC_LFO_1, shruthi::MOD_SRC_LFO_2,
      shruthi::MOD_SRC_PITCH_BEND, shruthi::MOD_SRC_NOTE,
      shruthi::MOD_SRC_AUDIO};
  const int8_t relative_amounts[] = {1, -1, 31, -31, 63, -63, 127, -128};
  const uint8_t absolute_sources[] = {
      shruthi::MOD_SRC_OFFSET, shruthi::MOD_SRC_CC_A,
      shruthi::MOD_SRC_ENV_1,  shruthi::MOD_SRC_RANDOM};
  const int8_t absolute_amounts[] = {1, -1, 31, -31, 63, -63, 127, -128};
  const uint8_t source_values[] = {0, 1, 64, 128, 129, 140, 200, 255};

  for (uint8_t destination : destinations) {
    for (uint8_t source : relative_sources) {
      for (int8_t amount : relative_amounts) {
        ShruthiRuntime runtime;
        runtime.init();
        zeroMatrix(runtime.part);
        applyPatchBase(runtime.part, kPatchBase);
        runtime.part.mutable_voice()->set_modulation_source(source, 140);
        runtime.part.mutable_patch()->modulation_matrix.modulation[0].source =
            source;
        runtime.part.mutable_patch()->modulation_matrix.modulation[0]
            .destination = destination;
        runtime.part.mutable_patch()->modulation_matrix.modulation[0].amount =
            amount;
        for (int block = 0; block < 2; ++block) {
          const uint8_t value = source_values[(block * 5) % 8];
          setStableSource(runtime.part, source, value);
          runtime.part.mutable_voice()->ProcessControlBlock();
          replayAndCompare(runtime.part, kPatchBase, 1,
                           runtime.part.patch().modulation_matrix.modulation,
                           block);
        }
      }
    }
    for (uint8_t source : absolute_sources) {
      for (int8_t amount : absolute_amounts) {
        ShruthiRuntime runtime;
        runtime.init();
        zeroMatrix(runtime.part);
        applyPatchBase(runtime.part, kPatchBase);
        runtime.part.mutable_patch()->modulation_matrix.modulation[0].source =
            source;
        runtime.part.mutable_patch()->modulation_matrix.modulation[0]
            .destination = destination;
        runtime.part.mutable_patch()->modulation_matrix.modulation[0].amount =
            amount;
        for (int block = 0; block < 2; ++block) {
          const uint8_t value = source_values[(block * 3 + 2) % 8];
          setStableSource(runtime.part, source, value);
          runtime.part.mutable_voice()->ProcessControlBlock();
          replayAndCompare(runtime.part, kPatchBase, 1,
                           runtime.part.patch().modulation_matrix.modulation,
                           block);
        }
      }
    }
  }
}

void testVcaRows() {
  static const shruthi::Modulation vca_rows[] = {
      {shruthi::MOD_SRC_OFFSET, shruthi::MOD_DST_VCA, 63},
      {shruthi::MOD_SRC_OFFSET, shruthi::MOD_DST_VCA, -63},
      {shruthi::MOD_SRC_ENV_1, shruthi::MOD_DST_VCA, 31},
      {shruthi::MOD_SRC_ENV_1, shruthi::MOD_DST_VCA, -31},
      {shruthi::MOD_SRC_CC_A, shruthi::MOD_DST_VCA, 127},
      {shruthi::MOD_SRC_CC_A, shruthi::MOD_DST_VCA, -128},
      {shruthi::MOD_SRC_RANDOM, shruthi::MOD_DST_VCA, 1},
      {shruthi::MOD_SRC_RANDOM, shruthi::MOD_DST_VCA, -1}};
  const uint8_t values[] = {0, 64, 128, 200, 255};
  for (const auto& row : vca_rows) {
    for (uint8_t value : values) {
      ShruthiRuntime runtime;
      runtime.init();
      zeroMatrix(runtime.part);
      applyPatchBase(runtime.part, kPatchBase);
      setStableSource(runtime.part, row.source, value);
      runtime.part.mutable_patch()->modulation_matrix.modulation[0] = row;
      runtime.part.mutable_voice()->ProcessControlBlock();
      replayAndCompare(runtime.part, kPatchBase, 1,
                       runtime.part.patch().modulation_matrix.modulation, 0);
    }
  }
}

void testWheelLastRowScaling() {
  ShruthiRuntime runtime;
  runtime.init();
  zeroMatrix(runtime.part);
  applyPatchBase(runtime.part, kPatchBase);
  auto* rows = runtime.part.mutable_patch()->modulation_matrix.modulation;
  for (int i = 0; i < shruthi::kModulationMatrixSize; ++i) {
    rows[i].destination = shruthi::MOD_DST_PWM_1;
  }
  rows[shruthi::kModulationMatrixSize - 1].source = shruthi::MOD_SRC_OFFSET;
  rows[shruthi::kModulationMatrixSize - 1].destination =
      shruthi::MOD_DST_PWM_1;
  rows[shruthi::kModulationMatrixSize - 1].amount = 63;
  const uint8_t wheels[] = {0, 64, 128, 255};
  for (uint8_t wheel : wheels) {
    runtime.part.mutable_voice()->set_modulation_source(
        shruthi::MOD_SRC_WHEEL, wheel);
    runtime.part.mutable_voice()->ProcessControlBlock();
    replayAndCompare(runtime.part, kPatchBase, shruthi::kModulationMatrixSize,
                     runtime.part.patch().modulation_matrix.modulation, wheel);
  }
}

void testStackedRows() {
  ShruthiRuntime runtime;
  runtime.init();
  zeroMatrix(runtime.part);
  applyPatchBase(runtime.part, kPatchBase);
  auto* rows = runtime.part.mutable_patch()->modulation_matrix.modulation;
  rows[0] = {shruthi::MOD_SRC_LFO_1, shruthi::MOD_DST_FILTER_CUTOFF, -63};
  rows[1] = {shruthi::MOD_SRC_OFFSET, shruthi::MOD_DST_FILTER_CUTOFF, 127};
  rows[2] = {shruthi::MOD_SRC_ENV_1, shruthi::MOD_DST_MIX_NOISE, 63};
  rows[3] = {shruthi::MOD_SRC_CC_A, shruthi::MOD_DST_PWM_2, -31};
  rows[4] = {shruthi::MOD_SRC_LFO_2, shruthi::MOD_DST_LFO_2, 31};
  rows[5] = {shruthi::MOD_SRC_SEQ, shruthi::MOD_DST_ATTACK, -127};
  rows[6] = {shruthi::MOD_SRC_NOTE, shruthi::MOD_DST_CV_2, 63};
  for (int block = 0; block < 3; ++block) {
    setStableSource(runtime.part, shruthi::MOD_SRC_LFO_1,
                    static_cast<uint8_t>(200 - block * 40));
    setStableSource(runtime.part, shruthi::MOD_SRC_LFO_2,
                    static_cast<uint8_t>(40 + block * 50));
    setStableSource(runtime.part, shruthi::MOD_SRC_CC_A,
                    static_cast<uint8_t>(12 + block * 70));
    setStableSource(runtime.part, shruthi::MOD_SRC_SEQ,
                    static_cast<uint8_t>(block * 90));
    runtime.part.mutable_voice()->ProcessControlBlock();
    replayAndCompare(runtime.part, kPatchBase, 7,
                     runtime.part.patch().modulation_matrix.modulation, block);
  }
}

void testOperators() {
  static const uint8_t kOps[] = {
      shruthi::OP_CV_SUM,      shruthi::OP_CV_PRODUCT, shruthi::OP_CV_MAX,
      shruthi::OP_CV_MIN,      shruthi::OP_CV_XOR,     shruthi::OP_CV_GE,
      shruthi::OP_CV_LE,       shruthi::OP_CV_QUANTIZE};
  for (uint8_t op : kOps) {
    ShruthiRuntime runtime;
    runtime.init();
    zeroMatrix(runtime.part);
    applyPatchBase(runtime.part, kPatchBase);
    auto* patch = runtime.part.mutable_patch();
    patch->ops_[0].op = op;
    patch->ops_[0].operands[0] = shruthi::MOD_SRC_LFO_1;
    patch->ops_[0].operands[1] = shruthi::MOD_SRC_CC_A;
    uint8_t prev = 0;
    for (int block = 0; block < 6; ++block) {
      const uint8_t x = static_cast<uint8_t>(30 + block * 37);
      const uint8_t y = static_cast<uint8_t>(block * 53 + 21);
      setStableSource(runtime.part, shruthi::MOD_SRC_LFO_1, x);
      setStableSource(runtime.part, shruthi::MOD_SRC_CC_A, y);
      runtime.part.mutable_voice()->ProcessControlBlock();
      const uint8_t classic_out =
          runtime.part.voice().modulation_source(shruthi::MOD_SRC_OP_1);
      const uint8_t hd_out = avrlib_hd::HdModMatrix::ApplyOperator(
          op, x, y, prev);
      if (classic_out != hd_out) {
        std::fprintf(stderr,
                     "  op mismatch op=%u block=%d x=%u y=%u hd=%u classic=%u\n",
                     static_cast<unsigned>(op), block,
                     static_cast<unsigned>(x), static_cast<unsigned>(y),
                     static_cast<unsigned>(hd_out),
                     static_cast<unsigned>(classic_out));
      }
      expect(classic_out == hd_out, "CV operator matches Classic");
      prev = classic_out;
    }
  }

  // Lag processor is stateful and must track across blocks.
  ShruthiRuntime runtime;
  runtime.init();
  zeroMatrix(runtime.part);
  applyPatchBase(runtime.part, kPatchBase);
  runtime.part.mutable_patch()->ops_[0].op = shruthi::OP_CV_LAG_PROCESSOR;
  runtime.part.mutable_patch()->ops_[0].operands[0] = shruthi::MOD_SRC_LFO_1;
  runtime.part.mutable_patch()->ops_[0].operands[1] = shruthi::MOD_SRC_CC_A;
  uint8_t prev = 0;
  for (int block = 0; block < 10; ++block) {
    const uint8_t x = static_cast<uint8_t>(15 + block * 23);
    const uint8_t y = static_cast<uint8_t>(100 + block * 9);
    setStableSource(runtime.part, shruthi::MOD_SRC_LFO_1, x);
    setStableSource(runtime.part, shruthi::MOD_SRC_CC_A, y);
    runtime.part.mutable_voice()->ProcessControlBlock();
    const uint8_t classic_out =
        runtime.part.voice().modulation_source(shruthi::MOD_SRC_OP_1);
    const uint8_t hd_out =
        avrlib_hd::HdModMatrix::ApplyOperator(shruthi::OP_CV_LAG_PROCESSOR, x,
                                              y, prev);
    expect(classic_out == hd_out, "lag operator tracks Classic state");
    prev = classic_out;
  }
}

void testLaunchedVoice() {
  ShruthiRuntime runtime;
  runtime.init();
  zeroMatrix(runtime.part);
  applyPatchBase(runtime.part, kPatchBase);
  auto* rows = runtime.part.mutable_patch()->modulation_matrix.modulation;
  rows[0] = {shruthi::MOD_SRC_AUDIO, shruthi::MOD_DST_MIX_BALANCE, 63};
  rows[1] = {shruthi::MOD_SRC_AUDIO, shruthi::MOD_DST_PWM_1, -127};
  rows[2] = {shruthi::MOD_SRC_NOTE, shruthi::MOD_DST_FILTER_CUTOFF, 63};
  rows[3] = {shruthi::MOD_SRC_GATE, shruthi::MOD_DST_ATTACK, 63};
  rows[4] = {shruthi::MOD_SRC_ENV_1, shruthi::MOD_DST_MIX_NOISE, -31};
  runtime.part.mutable_patch()->modulation_matrix.modulation[4] = rows[4];
  runtime.part.NoteOn(0, 60, 104);
  for (int block = 0; block < 4; ++block) {
    runtime.part.ProcessBlock();
    runtime.part.mutable_voice()->ProcessControlBlock();
    replayAndCompare(runtime.part, kPatchBase, 5,
                     runtime.part.patch().modulation_matrix.modulation, block);
  }
  runtime.part.NoteOff(0, 60);
  for (int block = 0; block < 3; ++block) {
    runtime.part.ProcessBlock();
    runtime.part.mutable_voice()->ProcessControlBlock();
    replayAndCompare(runtime.part, kPatchBase, 5,
                     runtime.part.patch().modulation_matrix.modulation,
                     block + 4);
  }
}

}  // namespace

int main() {
  testSingleRows();
  testVcaRows();
  testWheelLastRowScaling();
  testStackedRows();
  testOperators();
  testLaunchedVoice();
  std::printf("AvrlibHdModMatrixParityTests: %d checks, %d failures\n",
              gChecks, gFailures);
  return gFailures == 0 ? 0 : 1;
}