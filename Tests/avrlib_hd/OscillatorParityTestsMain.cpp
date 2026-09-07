// Copyright 2026 MontroneDSP.
// SPDX-License-Identifier: GPL-3.0-or-later
//
// Faithful HD oscillator parity: HdOscillator must reproduce the Classic
// shruthi::Oscillator bit-for-bit over the full shape set (0..34), over a grid
// of notes, parameters, secondary parameters, phase increments and sync
// patterns. The output float must equal (byte - 128) / 128 exactly, including
// the output sync-carry stream and the stateful renderers (FM doubled
// parameter + zeroed fractional phase, vowel control-rate update and the
// reset/alias collision, crushed-sine decimation, quad-saw phases, filtered
// noise LFSR).

#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <vector>

#include "avrlib/base.h"
#include "avrlib/random.h"
#include "shruthi/oscillator.h"
#include "shruthi/resources.h"
#include "avrlib_hd/oscillator.h"

namespace {

int gFailures = 0;

void expect(bool ok, const char* message) {
  if (!ok) {
    std::fprintf(stderr, "FAIL: %s\n", message);
    ++gFailures;
  }
}

constexpr int kNum = shruthi::kAudioBlockSize;
constexpr int kPasses = 3;
constexpr uint8_t kGuard = 0xA5;

// The Classic RenderSweepingWavetableRam reads past the logical 8*129-byte
// user wavetable when the sweeping parameter's high nibble is > 7 (wave index
// up to 15 maps to byte offsets up to 1935, and InterpolateSampleRam reads
// offsets [offset, offset+128]). Hand the oscillator a larger buffer so the
// faithful OOB reads stay inside the allocation (the bytes are filled with the
// same deterministic pattern as the logical wavetable).
constexpr int kUserWavetableBufferSize =
    avrlib_hd::HdOscillator::kUserWavetableSize + 2064;

float classicByteToFloat(uint8_t b) {
  return static_cast<float>(static_cast<int>(b) - 128) * (1.0f / 128.0f);
}

class ParityRunner {
 public:
  ParityRunner() {
    random_.Seed(0x1337);
    hdRandom_.Seed(0x1337);
    classic_.set_random(&random_);
    classic_.Reset();
    for (int i = 0; i < kUserWavetableBufferSize; ++i) {
      userWavetable_[i] = static_cast<uint8_t>((i * 37) & 0xff);
    }
    classic_.set_user_wavetable(userWavetable_);
    hd_.set_user_wavetable(userWavetable_);
    tables_.waveform_table = shruthi::waveform_table;
    tables_.sine = shruthi::wav_res_sine;
    tables_.waves = shruthi::wav_res_waves;
    tables_.wavetables = shruthi::wav_res_wavetables;
    tables_.fm_frequency_ratios = shruthi::lut_res_fm_frequency_ratios;
    tables_.vowel_data = shruthi::wav_res_vowel_data;
    tables_.formant_sine = shruthi::wav_res_formant_sine;
    tables_.formant_square = shruthi::wav_res_formant_square;
    tables_.bandlimited_triangle_0 = shruthi::wav_res_bandlimited_triangle_0;
    hd_.set_tables(tables_);
    hd_.set_random(&hdRandom_);
    hd_.Reset();
  }

  avrlib_hd::OscWave mapShape(uint8_t shape) {
    switch (shape) {
      case shruthi::WAVEFORM_SAW: return avrlib_hd::OscWave::kSaw;
      case shruthi::WAVEFORM_TRIANGLE: return avrlib_hd::OscWave::kTriangle;
      case shruthi::WAVEFORM_SQUARE: return avrlib_hd::OscWave::kSquare;
      default: return avrlib_hd::OscWave::kSilence;
    }
  }

  // Drives HdOscillator::RenderNaive (the three-wave faithful slot) against
  // the Classic oscillator over a note/param/increment grid.
  void run(uint8_t shape, uint8_t note, uint32_t increment24, uint8_t parameter,
           const uint8_t* syncFlags = nullptr) {
    ++blocks_;
    uint24_t increment{};
    increment.integral = static_cast<uint16_t>(increment24 >> 8);
    increment.fractional = static_cast<uint8_t>(increment24 & 0xff);
    avrlib_hd::OscWave wave = mapShape(shape);

    clearBuffers();
    for (int pass = 0; pass < kPasses; ++pass) {
      prepareSync(syncFlags);
      classic_.set_parameter(parameter);
      classic_.Render(shape, note, increment, syncIn_ + 4, syncOut_ + 4,
                      buffer_ + 4);
      hd_.RenderNaive(wave, note, increment24, parameter, kNum, hdOut_,
                      hdSyncIn_ + 4, hdSyncOut_ + 4);
      if (compare(pass, shape, note, increment24, parameter)) {
        return;
      }
    }
  }

  // Drives HdOscillator::RenderNaiveFull (the full 0..34 shape domain) against
  // the Classic oscillator, mirroring the Classic member-state protocol (the
  // parameters are set with the setters, matching the voice UpdateSources).
  void runFull(uint8_t shape, uint8_t note, uint32_t increment24,
               uint8_t parameter, uint8_t secondary_parameter,
               const uint8_t* syncFlags = nullptr) {
    ++blocks_;
    uint24_t increment{};
    increment.integral = static_cast<uint16_t>(increment24 >> 8);
    increment.fractional = static_cast<uint8_t>(increment24 & 0xff);

    clearBuffers();
    for (int pass = 0; pass < kPasses; ++pass) {
      prepareSync(syncFlags);
      classic_.set_parameter(parameter);
      classic_.set_secondary_parameter(secondary_parameter);
      classic_.Render(shape, note, increment, syncIn_ + 4, syncOut_ + 4,
                      buffer_ + 4);
      hd_.set_parameter(parameter);
      hd_.set_secondary_parameter(secondary_parameter);
      hd_.RenderNaiveFull(shape, note, increment24, kNum, hdOut_,
                          hdSyncIn_ + 4, hdSyncOut_ + 4);
      if (compare(pass, shape, note, increment24, parameter)) {
        return;
      }
    }
  }

  void protectedBuffers() {
    for (int i = 0; i < 4; ++i) {
      expect(syncIn_[i] == 0 && syncOut_[i] == 0 && buffer_[i] == 0,
             "sync/audio guard area untouched (low)");
    }
    expect(syncIn_[kNum + 4] == 0 && syncOut_[kNum + 4] == 0 &&
               buffer_[kNum + 4] == 0,
           "sync/audio guard area untouched (high)");
    expect(hdSyncIn_[0] == 0 && hdSyncIn_[kNum + 4] == 0,
           "HD sync_input guard area untouched");
    expect(hdSyncOut_[0] == 0 && hdSyncOut_[kNum + 4] == 0,
           "HD sync_output guard area untouched");
  }

  int renderedBlocks() const { return blocks_; }

 private:
  void clearBuffers() {
    std::memset(syncIn_, 0, sizeof(syncIn_));
    std::memset(syncOut_, 0, sizeof(syncOut_));
    std::memset(buffer_, 0, sizeof(buffer_));
    std::memset(hdSyncIn_, 0, sizeof(hdSyncIn_));
    std::memset(hdSyncOut_, 0, sizeof(hdSyncOut_));
    std::memset(hdOut_, 0, sizeof(hdOut_));
  }

  void prepareSync(const uint8_t* syncFlags) {
    if (syncFlags != nullptr) {
      std::memcpy(syncIn_ + 4, syncFlags, kNum);
      std::memcpy(hdSyncIn_, syncIn_, sizeof(syncIn_));
    }
  }

  // Compares the HD output against the Classic buffer; returns true on the
  // first mismatch (caller aborts the pass loop then).
  bool compare(int pass, uint8_t shape, uint8_t note, uint32_t increment24,
               uint8_t parameter) {
    for (int i = 0; i < kNum; ++i) {
      float expected = classicByteToFloat(buffer_[4 + i]);
      if (expected != hdOut_[i]) {
        std::fprintf(stderr,
                     "  mismatch pass=%d shape=%d note=%d inc=0x%06X param=%d "
                     "i=%d classic=%f hd=%f\n",
                     pass, shape, note, increment24, parameter, i, expected,
                     hdOut_[i]);
        expect(expected == hdOut_[i], "faithful HD sample matches Classic");
        return true;
      }
    }
    for (int i = 0; i < kNum; ++i) {
      if (hdSyncOut_[4 + i] != syncOut_[4 + i]) {
        std::fprintf(stderr,
                     "  syncOUT mismatch pass=%d shape=%d i=%d classic=%d hd=%d\n",
                     pass, shape, i, syncOut_[4 + i], hdSyncOut_[4 + i]);
        expect(false, "faithful HD sync carry stream matches Classic");
        return true;
      }
    }
    return false;
  }

  avrlib::Random random_;
  avrlib_hd::Random hdRandom_;
  shruthi::Oscillator classic_;
  avrlib_hd::OscillatorTables tables_{};
  avrlib_hd::HdOscillator hd_;
  uint8_t userWavetable_[kUserWavetableBufferSize] = {};
  uint8_t syncIn_[kNum + 8];
  uint8_t syncOut_[kNum + 8];
  uint8_t buffer_[kNum + 8];
  uint8_t hdSyncIn_[kNum + 8];
  uint8_t hdSyncOut_[kNum + 8];
  float hdOut_[kNum];
  int blocks_ = 0;
};

void testNaiveParityGrid() {
  const uint8_t kFirstNote = 12;
  const uint8_t kLastNote = 127;
  const uint8_t params[] = {0, 1, 16, 31, 64, 96, 127, 128, 200, 255};
  const uint32_t increments[] = {0x000001u, 0x000400u, 0x001000u, 0x040000u,
                                 0x004E20u, 0x008000u, 0x00A000u, 0x00FFFFu};
  ParityRunner runner;
  const uint8_t shapes[] = {shruthi::WAVEFORM_SAW, shruthi::WAVEFORM_TRIANGLE,
                            shruthi::WAVEFORM_SQUARE};
  for (uint8_t shape : shapes) {
    for (int note = kFirstNote; note <= kLastNote; ++note) {
      for (uint8_t parameter : params) {
        for (uint32_t increment : increments) {
          runner.run(shape, static_cast<uint8_t>(note), increment, parameter,
                     nullptr);
        }
      }
    }
  }
  const int kExpectedBlocks = 3 * (kLastNote - kFirstNote + 1) * 10 * 8;
  if (runner.renderedBlocks() != kExpectedBlocks) {
    std::printf("FAIL naive grid: rendered %d blocks, expected %d\n",
                runner.renderedBlocks(), kExpectedBlocks);
    ++gFailures;
  }
}

void testNaiveParityWithSync() {
  ParityRunner runner;
  uint8_t flags[kNum];
  std::memset(flags, 0, sizeof(flags));
  for (int i = 0; i < kNum; ++i) {
    if (i % 7 == 0) flags[i] = 1;
  }
  flags[kNum - 4] = 1;
  flags[kNum - 3] = 1;
  const uint8_t shapes[] = {shruthi::WAVEFORM_SAW, shruthi::WAVEFORM_TRIANGLE,
                            shruthi::WAVEFORM_SQUARE};
  for (uint8_t shape : shapes) {
    for (uint8_t note : {uint8_t(24), uint8_t(60), uint8_t(100)}) {
      for (uint32_t increment : {0x000400u, 0x001000u, 0x007000u}) {
        runner.run(shape, note, increment, 64, flags);
      }
    }
    runner.protectedBuffers();
  }
}

void testFullShapeParityGrid() {
  const uint8_t kFirstShape = 0;
  const uint8_t kLastShape = 34;
  const uint8_t notes[] = {12, 24, 36, 60, 96, 124, 127};
  const uint8_t params[] = {0, 1, 64, 128, 200, 255};
  const uint8_t secondary[] = {0, 24};
  const uint32_t increments[] = {0x000400u, 0x001000u, 0x007000u};
  ParityRunner runner;
  for (uint8_t shape = kFirstShape; shape <= kLastShape; ++shape) {
    for (uint8_t note : notes) {
      for (uint8_t parameter : params) {
        // The vowel renderer indexes wav_res_vowel_data (63 bytes) with
        // (parameter >> 4) * 7 + 7 + 6; parameter >= 0x80 walks past the table.
        // The firmware constrains the vowel parameter to 0..127.
        if (shape == shruthi::WAVEFORM_VOWEL && parameter > 0x7f) {
          parameter = 0x7f;
        }
        for (uint8_t secondary_parameter : secondary) {
          for (uint32_t increment : increments) {
            runner.runFull(shape, note, increment, parameter,
                           secondary_parameter);
          }
        }
      }
    }
  }
  const int kExpectedBlocks = (kLastShape - kFirstShape + 1) * 7 * 6 * 2 * 3;
  if (runner.renderedBlocks() != kExpectedBlocks) {
    std::printf("FAIL full grid: rendered %d blocks, expected %d\n",
                runner.renderedBlocks(), kExpectedBlocks);
    ++gFailures;
  }
}

void testFullShapeParityWithSync() {
  ParityRunner runner;
  uint8_t flags[kNum];
  std::memset(flags, 0, sizeof(flags));
  for (int i = 0; i < kNum; ++i) {
    if (i % 5 == 0) flags[i] = 1;
  }
  flags[0] = 1;
  flags[kNum - 2] = 1;
  const uint8_t shapes[] = {1, 2, 3, 4, 7, 8, 9, 10, 11, 20, 21, 22, 23, 24};
  for (uint8_t shape : shapes) {
    for (uint8_t parameter : {uint8_t(0), uint8_t(64), uint8_t(255)}) {
      if (shape == shruthi::WAVEFORM_VOWEL && parameter > 0x7f) {
        parameter = 0x7f;
      }
      runner.runFull(shape, 60, 0x001000u, parameter, 24, flags);
    }
  }
  runner.protectedBuffers();
}

// The Classic OscillatorState union aliases the filtered-noise reset value's
// bytes with the vowel synthesizer's formant increment 2 and the quad-saw-pad
// phases. After a note-on reset and before the first vowel control-rate
// update, the vowel renderer consumes that aliased value; the HdOscillatorState
// union in HdOscillator must yield the same output.
void testVowelResetAlias() {
  ParityRunner runner;
  runner.runFull(24, 36, 0x004E20u, 64, 24);
  runner.protectedBuffers();
}

}  // namespace

int main() {
  testNaiveParityGrid();
  testNaiveParityWithSync();
  testFullShapeParityGrid();
  testFullShapeParityWithSync();
  testVowelResetAlias();
  if (gFailures == 0) {
    std::printf("AvrlibHdOscillatorParityTests: all passed\n");
    return 0;
  }
  std::printf("AvrlibHdOscillatorParityTests: %d FAILURES\n", gFailures);
  return 1;
}