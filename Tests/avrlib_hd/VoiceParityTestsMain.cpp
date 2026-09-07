// Copyright 2026 MontroneDSP.
// SPDX-License-Identifier: GPL-3.0-or-later
//
// Voice parity: the faithful avrlib_hd::HdVoice (LoadSources +
// ProcessModulationMatrix + UpdateDestinations + RenderOscillators + mixer +
// noise) must reproduce the REAL adapted shruthi::Voice::ProcessBlock
// bit-for-bit on the audio output.
//
// The harness drives the real shruthi::Voice directly (not through
// Part::ProcessBlockInternal), so the parts of the pipeline that depend on
// the Part (LFO rendering, noexcept arp/seq) are not exercised. The LFO and
// fixed modulation sources are pre-set like Part::ProcessBlockInternal does.
// The voice output (the mixer writes kAudioBlockSize bytes into
// HostAudioRing) is captured and compared byte-by-byte with the HdVoice
// output buffer.
//
// RNG lockstep: both the avrlib::Random (classic) and the avrlib_hd::Random
// are seeded with the same value. The classic voice consumes exactly:
//   - 2 bytes in Voice::NoteOn ([[trigger]]) for the two oscillator resets,
//   - 1 byte per ProcessBlock for the mixer noise.
// HdVoice consumes the same bytes in the same order, so the two RNG streams
// stay in lockstep for the whole run. The LFO S&H waveform is not exercised
// here (it would advance the RNG from the LFO, which is explicitly out of
// scope for the voice parity -- the LFO sources are injected directly).

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
#include "avrlib_hd/envelope.h"
#include "avrlib_hd/voice.h"

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
    // Reset the final settled block and start a clean slate: the voices are
    // driven directly from here, so nothing else consumes the RNG.
    ring.Init();
  }
};

// Wire the classic tables into the HD voice. The classic voice uses the
// same tables via ResourcesManager::Lookup, so the pointers are authoritative.
avrlib_hd::OscillatorTables makeOscTables() {
  avrlib_hd::OscillatorTables t;
  t.waveform_table = shruthi::waveform_table;
  t.sine = shruthi::wav_res_sine;
  t.waves = shruthi::wav_res_waves;
  t.wavetables = shruthi::wav_res_wavetables;
  t.fm_frequency_ratios = shruthi::lut_res_fm_frequency_ratios;
  t.vowel_data = shruthi::wav_res_vowel_data;
  t.formant_sine = shruthi::wav_res_formant_sine;
  t.formant_square = shruthi::wav_res_formant_square;
  t.bandlimited_triangle_0 = shruthi::wav_res_bandlimited_triangle_0;
  return t;
}

avrlib_hd::EnvelopeTables makeEnvTables() {
  avrlib_hd::EnvelopeTables t;
  t.portamento_increments = shruthi::lut_res_env_portamento_increments;
  t.env_expo = shruthi::wav_res_env_expo;
  return t;
}

avrlib_hd::HdVoiceTables makeVoiceTables() {
  avrlib_hd::HdVoiceTables t;
  t.oscillator_increments = shruthi::lut_res_oscillator_increments;
  t.portamento_increments = shruthi::lut_res_env_portamento_increments;
  t.distortion = shruthi::wav_res_distortion;
  return t;
}

// Mirror the voice-relevant patch fields from the classic patch into the
// HD patch struct.
void mirrorPatch(const shruthi::Part& part, avrlib_hd::HdVoicePatch* hd) {
  const shruthi::Patch& p = part.patch();
  for (int i = 0; i < 2; ++i) {
    hd->osc[i].shape = p.osc[i].shape;
    hd->osc[i].parameter = p.osc[i].parameter;
    hd->osc[i].range = p.osc[i].range;
    hd->osc[i].option = p.osc[i].option;
  }
  hd->mix_balance = p.mix_balance;
  hd->mix_sub_osc = p.mix_sub_osc;
  hd->mix_noise = p.mix_noise;
  hd->mix_sub_osc_shape = p.mix_sub_osc_shape;
  hd->filter_cutoff = p.filter_cutoff;
  hd->filter_resonance = p.filter_resonance;
  hd->filter_env = p.filter_env;
  hd->filter_lfo = p.filter_lfo;
  hd->filter_cutoff_2 = p.filter_cutoff_2;
  hd->filter_resonance_2 = p.filter_resonance_2;
  hd->filter_1_mode = p.filter_1_mode_;
  for (int i = 0; i < 2; ++i) {
    hd->env[i].attack = p.env[i].attack;
    hd->env[i].decay = p.env[i].decay;
    hd->env[i].sustain = p.env[i].sustain;
    hd->env[i].release = p.env[i].release;
  }
  for (int i = 0; i < shruthi::kModulationMatrixSize; ++i) {
    hd->mod[i].amount = p.modulation_matrix.modulation[i].amount;
    hd->mod[i].source = static_cast<uint8_t>(
        p.modulation_matrix.modulation[i].source);
    hd->mod[i].destination = static_cast<uint8_t>(
        p.modulation_matrix.modulation[i].destination);
  }
  for (int i = 0; i < 2; ++i) {
    hd->ops[i].op = p.ops_[i].op;
    hd->ops[i].operands[0] = p.ops_[i].operands[0];
    hd->ops[i].operands[1] = p.ops_[i].operands[1];
  }
}

void mirrorSystemSettings(const shruthi::Part& part,
                          avrlib_hd::HdVoiceSystemSettings* hd) {
  const shruthi::SystemSettings& s = part.system_settings();
  hd->expansion_filter_board = static_cast<uint8_t>(s.expansion_filter_board);
  hd->octave = static_cast<int8_t>(s.octave);
  hd->master_tuning = static_cast<int8_t>(s.master_tuning);
}

// One run: drive both voices with the same events and compare the audio
// output after every block. Returns the label prefix used by failures.
struct VoiceHarness {
  ShruthiRuntime runtime;
  avrlib_hd::Random hd_random;
  avrlib_hd::HdVoice hd_voice;
  avrlib_hd::OscillatorTables osc_tables;
  avrlib_hd::EnvelopeTables env_tables;
  avrlib_hd::HdVoiceTables voice_tables;
  avrlib_hd::HdVoicePatch hd_patch;
  avrlib_hd::HdVoiceSystemSettings hd_sys;
  uint8_t classic_out[shruthi::kAudioBlockSize];

  VoiceHarness()
      : osc_tables(makeOscTables()),
        env_tables(makeEnvTables()),
        voice_tables(makeVoiceTables()) {}

  void init() {
    runtime.init();
    // Part::Init -> ProcessBlock consumed one mixer-noise byte from the
    // classic RNG (the part-level init block). Reset it so both RNGs start
    // from the same state before the scenario begins.
    runtime.random.Seed(0x21);
    hd_random.Seed(0x21);
    hd_voice.Init(&hd_random);
    hd_voice.set_tables(&voice_tables);
    hd_voice.set_envelope_tables(env_tables);
    for (int i = 0; i < 2; ++i) {
      hd_voice.mutable_oscillator(i)->set_tables(osc_tables);
    }
    // Classic Voice::Init loads wav_res_waves into user_wavetable_;
    // HdVoice receives the same bytes so both wavetable families agree.
    hd_voice.set_user_wavetable(shruthi::wav_res_waves,
                                 kUserWavetableSize);

    hd_patch = avrlib_hd::HdVoicePatch{};
    hd_sys = avrlib_hd::HdVoiceSystemSettings{};
    mirrorPatch(runtime.part, &hd_patch);
    mirrorSystemSettings(runtime.part, &hd_sys);
    classic_out[0] = 0;
  }

  // Mirrors Part::ProcessBlockInternal's source feeding: LFOs (here: fixed
  // values) and MOD_SRC_NOISE / noise byte, right before ProcessBlock.
  void feedSources(uint8_t lfo1, uint8_t lfo2, uint8_t seq) {
    shruthi::Voice* classic = runtime.part.mutable_voice();
    classic->set_modulation_source(shruthi::MOD_SRC_LFO_1, lfo1);
    classic->set_modulation_source(shruthi::MOD_SRC_LFO_2, lfo2);
    classic->set_modulation_source(shruthi::MOD_SRC_SEQ, seq);
    classic->set_modulation_source(
        shruthi::MOD_SRC_NOISE, runtime.random.state_msb());
    hd_voice.set_modulation_source(avrlib_hd::kModSourceLfo1, lfo1);
    hd_voice.set_modulation_source(avrlib_hd::kModSourceLfo2, lfo2);
    hd_voice.set_modulation_source(avrlib_hd::kModSourceSeq, seq);
    hd_voice.set_modulation_source(
        avrlib_hd::kModSourceNoise, hd_random.state_msb());
  }

  void noteOn(uint16_t pitch, uint8_t velocity, uint8_t portamento,
              bool trigger) {
    runtime.part.mutable_voice()->NoteOn(pitch, velocity, portamento,
                                         trigger);
    hd_voice.NoteOn(pitch, velocity, portamento, trigger);
  }

  void noteOff() {
    runtime.part.mutable_voice()->NoteOff();
    hd_voice.NoteOff();
  }

  // Runs one classic block and compares with the HD output.
  void processBlock(int block) {
    runtime.part.mutable_voice()->ProcessBlock();
    for (int i = 0; i < shruthi::kAudioBlockSize; ++i) {
      classic_out[i] = runtime.ring.ImmediateRead();
    }
    hd_voice.ProcessBlock(hd_patch, hd_sys);
    for (int i = 0; i < shruthi::kAudioBlockSize; ++i) {
      char msg[128];
      std::snprintf(msg, sizeof(msg), "block %d sample %d: hd=%d classic=%d",
                    block, i, hd_voice.output_buffer()[i], classic_out[i]);
      if (hd_voice.output_buffer()[i] != classic_out[i]) {
        std::fprintf(stderr, "  VOICE MISMATCH: %s\n", msg);
      }
      expect(hd_voice.output_buffer()[i] == classic_out[i], msg);
    }
  }
};

// ---------------------------------------------------------------------------
// Scenario: a plain patch, no mod matrix, note on + a few blocks.
// ---------------------------------------------------------------------------
void testBasicNote() {
  VoiceHarness h;
  h.init();
  auto* patch = h.runtime.part.mutable_patch();
  patch->osc[0].shape = shruthi::WAVEFORM_SAW;
  patch->osc[0].parameter = 128;
  patch->osc[0].range = 2;
  patch->osc[0].option = shruthi::OP_SUM;
  patch->osc[1].shape = shruthi::WAVEFORM_SQUARE;
  patch->osc[1].parameter = 64;
  patch->osc[1].range = -2;
  patch->osc[1].option = shruthi::OP_SUM;
  patch->mix_balance = 96;
  patch->mix_noise = 0;
  patch->mix_sub_osc = 0;
  patch->mix_sub_osc_shape = shruthi::WAVEFORM_SUB_OSC_SQUARE_1;
  patch->filter_cutoff = 80;
  patch->filter_resonance = 40;
  h.runtime.part.mutable_voice()->set_volume(200);
  h.hd_voice.set_volume(200);
  mirrorPatch(h.runtime.part, &h.hd_patch);

  h.noteOn(76 << 7, 100, 0, true);
  for (int b = 0; b < 8; ++b) {
    h.feedSources(120, 12, 0);
    h.processBlock(b);
  }
}

// ---------------------------------------------------------------------------
// Scenario: every mixer operator, including the sequence-rhythmic ones.
// ---------------------------------------------------------------------------
void testMixerOperators() {
const uint8_t operators[] = {
        shruthi::OP_SUM,        shruthi::OP_SYNC,  shruthi::OP_RING_MOD,
        shruthi::OP_XOR,        shruthi::OP_FUZZ,  shruthi::OP_CRUSH_4,
        shruthi::OP_CRUSH_8,    shruthi::OP_FOLD,  shruthi::OP_BITS,
        shruthi::OP_DUO,        shruthi::OP_PING_PONG_2,
        shruthi::OP_PING_PONG_4, shruthi::OP_PING_PONG_8,
        shruthi::OP_PING_PONG_SEQ};

    for (uint8_t operator_ : operators) {
      VoiceHarness h;
      h.init();
      auto* patch = h.runtime.part.mutable_patch();
      patch->osc[0].shape = shruthi::WAVEFORM_SAW;
      patch->osc[0].parameter = 110;
      patch->osc[0].range = 0;
      patch->osc[0].option = operator_;
      patch->osc[1].shape = shruthi::WAVEFORM_TRIANGLE;
      patch->osc[1].parameter = 190;
      patch->osc[1].range = 0;
      patch->osc[1].option = shruthi::OP_SUM;
      patch->mix_balance = 64;
      patch->mix_noise = 32;
      patch->mix_sub_osc = 128;
      patch->mix_sub_osc_shape = shruthi::WAVEFORM_SUB_OSC_SQUARE_1;
      patch->filter_cutoff = 90;
      patch->filter_resonance = 30;
      h.runtime.part.mutable_voice()->set_volume(200);
      h.hd_voice.set_volume(200);
      if (operator_ == shruthi::OP_DUO) {
        // Duo mode mixes in a bass voice (the lowest held note).
        h.runtime.part.mutable_voice()->set_bass_note(48 << 7);
        h.hd_voice.set_bass_note(48 << 7);
      }
      mirrorPatch(h.runtime.part, &h.hd_patch);

      h.noteOn(64 << 7, 100, 0, true);
      for (int b = 0; b < 12; ++b) {
        // Sequence sources flip like a live step sequencer.
        h.feedSources(static_cast<uint8_t>(60 + b * 7),
                      static_cast<uint8_t>(130 - b * 3),
                      static_cast<uint8_t>(0xf0));
        h.processBlock(b);
      }
      h.noteOn(70 << 7, 100, 0, true);
      for (int b = 12; b < 20; ++b) {
        h.feedSources(static_cast<uint8_t>(40 + b * 5),
                      static_cast<uint8_t>(200 - b * 4),
                      static_cast<uint8_t>(0x30));
        h.processBlock(b);
      }
    }
  }

// ---------------------------------------------------------------------------
// Scenario: modulating parameters through the mod matrix (cutoff, osc, mix).
// ---------------------------------------------------------------------------
void testModulatedCutoff() {
  VoiceHarness h;
  h.init();
  auto* patch = h.runtime.part.mutable_patch();
  patch->osc[0].shape = shruthi::WAVEFORM_SAW;
  patch->osc[1].shape = shruthi::WAVEFORM_SAW;
  patch->osc[0].parameter = 96;
  patch->osc[1].parameter = 32;
  patch->mix_balance = 255;
  patch->mix_noise = 64;
  patch->filter_cutoff = 60;
  patch->filter_resonance = 90;
  for (int i = 0; i < shruthi::kModulationMatrixSize; ++i) {
    patch->modulation_matrix.modulation[i].source = shruthi::MOD_SRC_OFFSET;
    patch->modulation_matrix.modulation[i].destination =
        shruthi::MOD_DST_VCA;
    patch->modulation_matrix.modulation[i].amount = 0;
  }
  patch->modulation_matrix.modulation[0].source = shruthi::MOD_SRC_LFO_1;
  patch->modulation_matrix.modulation[0].destination =
      shruthi::MOD_DST_FILTER_CUTOFF;
  patch->modulation_matrix.modulation[0].amount = 127;
  patch->modulation_matrix.modulation[1].source = shruthi::MOD_SRC_ENV_1;
  patch->modulation_matrix.modulation[1].destination =
      shruthi::MOD_DST_MIX_NOISE;
  patch->modulation_matrix.modulation[1].amount = 63;
  patch->modulation_matrix.modulation[2].source = shruthi::MOD_SRC_WHEEL;
  patch->modulation_matrix.modulation[2].destination =
      shruthi::MOD_DST_VCO_1;
  patch->modulation_matrix.modulation[2].amount = 40;
  h.runtime.part.mutable_voice()->set_volume(200);
  h.hd_voice.set_volume(200);
  mirrorPatch(h.runtime.part, &h.hd_patch);

  h.noteOn(60 << 7, 120, 0, true);
  for (int b = 0; b < 16; ++b) {
    h.feedSources(static_cast<uint8_t>(90 + b * 6),
                  static_cast<uint8_t>(50),
                  static_cast<uint8_t>(0));
    // Move the wheel between blocks through the voice ControlChange.
    h.runtime.part.mutable_voice()->ControlChange(midi::kModulationWheelMsb,
                                                  static_cast<uint8_t>(b * 3));
    h.hd_voice.ControlChange(avrlib_hd::kMidiModulationWheelMsb,
                             static_cast<uint8_t>(b * 3));
    h.processBlock(b);
  }
}

// ---------------------------------------------------------------------------
// Scenario: note off, release, retrigger, portamento glide.
// ---------------------------------------------------------------------------
void testPortamentoAndReleases() {
  VoiceHarness h;
  h.init();
  auto* patch = h.runtime.part.mutable_patch();
  patch->osc[0].shape = shruthi::WAVEFORM_SAW;
  patch->osc[1].shape = shruthi::WAVEFORM_TRIANGLE;
  patch->osc[0].parameter = 128;
  patch->osc[1].parameter = 128;
  patch->mix_balance = 96;
  patch->env[0].decay = 64;
  patch->env[0].sustain = 64;
  patch->env[0].release = 160;
  patch->env[1].decay = 64;
  patch->env[1].sustain = 64;
  patch->env[1].release = 160;
  patch->filter_cutoff = 100;
  patch->filter_resonance = 40;
  h.runtime.part.mutable_voice()->set_volume(200);
  h.hd_voice.set_volume(200);
  mirrorPatch(h.runtime.part, &h.hd_patch);

  h.noteOn(60 << 7, 100, 0, true);
  for (int b = 0; b < 4; ++b) {
    h.feedSources(100, 40, 0);
    h.processBlock(b);
  }
  h.noteOff();
  for (int b = 4; b < 10; ++b) {
    h.feedSources(100, 40, 0);
    h.processBlock(b);
  }
  // New note with portamento (no retrigger).
  h.noteOn(72 << 7, 100, 200, false);
  for (int b = 10; b < 16; ++b) {
    h.feedSources(100, 40, 0);
    h.processBlock(b);
  }
  // Retrigger a new note.
  h.noteOn(48 << 7, 80, 0, true);
  for (int b = 16; b < 20; ++b) {
    h.feedSources(100, 40, 0);
    h.processBlock(b);
  }
}

// ---------------------------------------------------------------------------
// Scenario: transient + sub shapes, noise, filter modes, master tuning.
// ---------------------------------------------------------------------------
void testSubTransientNoise() {
  const uint8_t shapes[] = {
      shruthi::WAVEFORM_SUB_OSC_SQUARE_1, shruthi::WAVEFORM_SUB_OSC_TRIANGLE_1,
      shruthi::WAVEFORM_SUB_OSC_PULSE_1,  shruthi::WAVEFORM_SUB_OSC_SQUARE_2,
      shruthi::WAVEFORM_SUB_OSC_TRIANGLE_2, shruthi::WAVEFORM_SUB_OSC_PULSE_2,
      shruthi::WAVEFORM_SUB_OSC_CLICK,    shruthi::WAVEFORM_SUB_OSC_GLITCH,
      shruthi::WAVEFORM_SUB_OSC_BLOW,     shruthi::WAVEFORM_SUB_OSC_METALLIC,
      shruthi::WAVEFORM_SUB_OSC_POP};

  for (uint8_t shape : shapes) {
    VoiceHarness h;
    h.init();
    auto* patch = h.runtime.part.mutable_patch();
    patch->osc[0].shape = shruthi::WAVEFORM_SAW;
    patch->osc[0].parameter = 80;
    patch->osc[1].shape = shruthi::WAVEFORM_SAW;
    patch->osc[1].parameter = 80;
    patch->mix_balance = 64;
    patch->mix_noise = 64;
    patch->mix_sub_osc = 200;
    patch->mix_sub_osc_shape = shape;
    patch->filter_cutoff = 80;
    patch->filter_resonance = 50;
    h.runtime.part.mutable_voice()->set_volume(200);
    h.hd_voice.set_volume(200);
    mirrorPatch(h.runtime.part, &h.hd_patch);

    h.noteOn(60 << 7, 100, 0, true);
    for (int b = 0; b < 8; ++b) {
      h.feedSources(static_cast<uint8_t>(80 + b), static_cast<uint8_t>(40),
                    static_cast<uint8_t>(0));
      h.processBlock(b);
    }
  }
}

// ---------------------------------------------------------------------------
// Scenario: filter board expansion modes (PVK cutoff, SVF coupled, SSM caps).
// ---------------------------------------------------------------------------
void testFilterBoards() {
  const uint8_t boards[] = {
      shruthi::FILTER_BOARD_LPF, shruthi::FILTER_BOARD_SSM,
      shruthi::FILTER_BOARD_SVF, shruthi::FILTER_BOARD_PVK,
      shruthi::FILTER_BOARD_4PM};
  const uint8_t ssf_filter_modes[] = {
      shruthi::FILTER_MODE_LP, shruthi::FILTER_MODE_LP_COUPLED};

  for (uint8_t board : boards) {
    for (uint8_t fs_mode : ssf_filter_modes) {
      VoiceHarness h;
      h.init();
      auto* patch = h.runtime.part.mutable_patch();
      patch->osc[0].shape = shruthi::WAVEFORM_SAW;
      patch->osc[0].parameter = 100;
      patch->osc[1].shape = shruthi::WAVEFORM_SAW;
      patch->osc[1].parameter = 100;
      patch->mix_balance = 96;
      patch->mix_noise = 32;
      patch->filter_cutoff = 70;
      patch->filter_resonance = 60;
      patch->filter_1_mode_ = fs_mode;
      patch->filter_cutoff_2 = 90;
      patch->filter_resonance_2 = 40;
      auto* sys = h.runtime.part.mutable_system_settings();
      sys->expansion_filter_board = static_cast<shruthi::FilterBoard>(board);
      sys->octave = -1;
      sys->master_tuning = 7;
      h.runtime.part.mutable_voice()->set_volume(200);
      h.hd_voice.set_volume(200);
      mirrorPatch(h.runtime.part, &h.hd_patch);
      mirrorSystemSettings(h.runtime.part, &h.hd_sys);

      h.noteOn(64 << 7, 100, 0, true);
      for (int b = 0; b < 10; ++b) {
        h.feedSources(static_cast<uint8_t>(70 + b * 3),
                      static_cast<uint8_t>(30),
                      static_cast<uint8_t>(0));
        h.processBlock(b);
      }
    }
  }
}

void testLongRenderStress() {
  VoiceHarness h;
  h.init();
  auto* patch = h.runtime.part.mutable_patch();
  patch->osc[0].shape = shruthi::WAVEFORM_SAW;
  patch->osc[0].parameter = 128;
  patch->osc[0].range = 0;
  patch->osc[0].option = shruthi::OP_SUM;
  patch->osc[1].shape = shruthi::WAVEFORM_SQUARE;
  patch->osc[1].parameter = 64;
  patch->osc[1].range = 0;
  patch->osc[1].option = shruthi::OP_SUM;
  patch->mix_balance = 64;
  patch->mix_noise = 20;
  patch->mix_sub_osc = 32;
  patch->mix_sub_osc_shape = shruthi::WAVEFORM_SUB_OSC_SQUARE_1;
  h.runtime.part.mutable_voice()->set_volume(220);
  h.hd_voice.set_volume(220);
  mirrorPatch(h.runtime.part, &h.hd_patch);
  h.noteOn(60 << 7, 100, 0, true);
  constexpr int kBlocks = 25000;  // 1,000,000 samples at kAudioBlockSize=40
  for (int b = 0; b < kBlocks; ++b) {
    h.feedSources(static_cast<uint8_t>(64 + (b & 127)),
                  static_cast<uint8_t>(32 + ((b >> 3) & 63)),
                  static_cast<uint8_t>(b & 255));
    h.processBlock(b);
    if (gFailures != 0) {
      return;
    }
  }
}

}  // namespace

int main() {
  testBasicNote();
  testMixerOperators();
  testModulatedCutoff();
  testPortamentoAndReleases();
  testSubTransientNoise();
  testFilterBoards();
  testLongRenderStress();

  if (gFailures == 0) {
    std::printf("VOICE PARITY: PASS (%d checks)\n", gChecks);
  } else {
    std::printf("VOICE PARITY: FAIL (%d/%d)\n", gFailures, gChecks);
  }
  return gFailures == 0 ? 0 : 1;
}