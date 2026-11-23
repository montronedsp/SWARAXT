// Copyright 2026 MontroneDSP.
// SPDX-License-Identifier: GPL-3.0-or-later

#include <array>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <cstdlib>

#include "avrlib/base.h"
#include "avrlib/random.h"
#include "avrlib/op.h"
#include "shruthi/envelope.h"
#include "shruthi/patch.h"
#include "shruthi/resources.h"
#include "shruthi/sequencer_settings.h"
#include "shruthi/shruthi.h"

#include "shruthi/lfo.h"

namespace shruthi {
struct LfoControlTestAccess {
  inline static constexpr std::array<uint16_t, 12> durations {{96, 48, 32, 24, 16, 12, 8, 6, 4, 3, 2, 1}};
  inline static constexpr std::array<uint16_t, 10> edges {{0, 1, 5, 6, 1039, 1040, 32767, 32768, 65534, 65535}};
  inline static uint64_t transitions = 0;
  inline static uint64_t outputHash = UINT64_C(14695981039346656037);

  [[noreturn]] static void fail(const char* message) {
    std::fprintf(stderr, "FAIL: %s at transition %llu\n", message,
                 static_cast<unsigned long long>(transitions));
    std::exit(1);
  }

  // Mathematical remainder and a representable signed conversion are independent
  // of the implementation's conversion of an out-of-range integer to int16_t.
  static int32_t signedPhase(int64_t value) {
    int64_t remainder = value % 65536;
    if (remainder < 0) remainder += 65536;
    return static_cast<int32_t>(remainder < 32768 ? remainder : remainder - 65536);
  }

  struct Correction { int32_t proportional, derivative, combined; uint16_t increment; };
  static Correction reference(const shruthi::Lfo& lfo, uint16_t step, uint16_t increment) {
    const int32_t p = signedPhase(static_cast<int64_t>(step) * increment - lfo.phase_);
    const int32_t d = signedPhase(static_cast<int64_t>(increment) - lfo.phase_ + lfo.previous_phase_);
    const int32_t error = p + d;
    // Floor division reproduces the supported compilers' arithmetic right shift.
    const int32_t adjustment = error >= 0 ? error / 256 : -((-error + 255) / 256);
    int32_t corrected = lfo.phase_increment_ + adjustment;
    if (corrected < 6) corrected = 6;
    if (corrected > 1040) corrected = 1040;
    return {p, d, error, static_cast<uint16_t>(corrected)};
  }

  static void checkCorrection(shruthi::Lfo& actual, uint16_t step, uint16_t increment,
                       bool originalDomain) {
    const auto expected = reference(actual, step, increment);
    const uint16_t previous = actual.phase_;
    if (originalDomain) {
      // Only executed when the original promoted product is below 65536.
      const int16_t p = static_cast<int16_t>(step * increment - actual.phase_);
      const int16_t d = static_cast<int16_t>(increment - (actual.phase_ - actual.previous_phase_));
      if (p != expected.proportional || d != expected.derivative ||
          static_cast<int32_t>(p) + d != expected.combined)
        fail("original valid-domain error signals differ from the independent model");
    }
    actual.set_target_phase(step, increment);
    ++transitions;
    if (actual.phase_increment_ != expected.increment || actual.previous_phase_ != previous ||
        actual.phase_ != previous)
      fail("phase correction or history differs from the independent model");
  }

  static void checkState(const shruthi::Lfo& a, const shruthi::Lfo& b) {
    if (a.phase_increment_ != b.phase_increment_ || a.previous_phase_ != b.previous_phase_ ||
        a.intensity_increment_ != b.intensity_increment_ || a.target_increment_ != b.target_increment_ ||
        a.intensity_ != b.intensity_ || a.phase_ != b.phase_ || a.shape_ != b.shape_ ||
        a.retrigger_mode_ != b.retrigger_mode_ || a.cycle_complete_ != b.cycle_complete_ ||
        a.running_ != b.running_ || a.value_ != b.value_ || a.one_shot_value_ != b.one_shot_value_ ||
        a.step_ != b.step_ || a.random_->state() != b.random_->state())
      fail("complete LFO state differs");
  }

  static void callerDomain() {
    // Exhaust the original signed-narrowing domain for both error expressions.
    for (int32_t value = -65535; value <= 131070; ++value) {
      if (static_cast<int16_t>(value) != signedPhase(value))
        fail("target compiler signed phase conversion differs from the reference");
    }
    uint32_t callerPairs = 0;
    shruthi::Lfo lfo;
    for (const auto duration : durations) {
      for (uint16_t rate = 1; rate <= 16; ++rate) {
        const uint16_t limit = static_cast<uint16_t>(duration * rate);
        const uint16_t increment = static_cast<uint16_t>(65536u / limit);
        for (uint16_t step = 0; step < limit; ++step) {
          ++callerPairs;
          if (static_cast<uint32_t>(step) * increment >= 65536u) fail("caller product invariant");
          for (const auto phase : edges) for (const auto previous : edges) {
            for (const auto current : edges) {
              lfo.phase_ = phase;
              lfo.previous_phase_ = previous;
              lfo.phase_increment_ = current;
              checkCorrection(lfo, step, increment, true);
            }
          }
        }
      }
    }
    if (callerPairs != 34272) fail("caller grid changed");
    std::printf("caller pairs=%u\n", callerPairs);
  }

  static void fullOperandAxes() {
    shruthi::Lfo lfo;
    for (const auto duration : durations) for (uint16_t rate = 1; rate <= 16; ++rate) {
      const uint16_t limit = static_cast<uint16_t>(duration * rate);
      const uint16_t increment = static_cast<uint16_t>(65536u / limit);
      for (uint16_t step = static_cast<uint16_t>(limit - 1); step <= limit + 1; ++step) {
        for (int delta = -1; delta <= 1; ++delta) for (const auto phase : edges) {
          const int perturbed = static_cast<int>(increment) + delta;
          if (perturbed < 0 || perturbed > 65535) continue;
          lfo.phase_ = phase;
          lfo.previous_phase_ = static_cast<uint16_t>(65535u - phase);
          lfo.phase_increment_ = 500;
          checkCorrection(lfo, step, static_cast<uint16_t>(perturbed), false);
        }
      }
    }
    // Exhaust each operand and every phase code against the other boundaries.
    for (uint32_t value = 0; value < 65536u; ++value) {
      for (const auto a : edges) for (const auto b : edges) {
        for (int axis = 0; axis < 3; ++axis) {
          lfo.phase_ = axis == 2 ? static_cast<uint16_t>(value) : b;
          lfo.previous_phase_ = a;
          lfo.phase_increment_ = b;
          const uint16_t step = axis == 0 ? static_cast<uint16_t>(value) : a;
          const uint16_t increment = axis == 1 ? static_cast<uint16_t>(value) : b;
          checkCorrection(lfo, step, increment, false);
        }
      }
    }
  }

  static void rendering() {
    shruthi::SequencerSettings sequence {};
    sequence.pattern_size = 16;
    for (uint8_t i = 0; i < 16; ++i) sequence.steps[i].set_controller(i);
    for (uint8_t shape = 0; shape < shruthi::LFO_WAVEFORM_LAST; ++shape) {
      for (uint8_t mode = 0; mode <= shruthi::LFO_MODE_ONE_SHOT; ++mode) {
        for (const uint8_t attack : {uint8_t{0}, uint8_t{1}, uint8_t{64}, uint8_t{127}}) {
          avrlib::Random actualRandom, expectedRandom;
          actualRandom.Seed(0x21); expectedRandom.Seed(0x21);
          shruthi::Lfo actual, expected;
          actual.Init(&actualRandom); expected.Init(&expectedRandom);
          actual.Update(shape, 227, attack, mode); expected.Update(shape, 227, attack, mode);
          actual.Reset(); expected.Reset();
          for (uint32_t block = 0; block < 65536u; ++block) {
            if (block % 997u == 0) { actual.Reset(); expected.Reset(); }
            if (block % 509u == 0) { actual.Trigger(); expected.Trigger(); }
            if (block % 131u == 0) {
              const uint16_t inc = static_cast<uint16_t>((block * 17u) % 65536u);
              actual.set_target_increment(inc); expected.set_target_increment(inc);
            }
            const uint16_t limit = static_cast<uint16_t>(durations[(block / 4096u) % durations.size()] * (block % 16u + 1u));
            const uint16_t step = static_cast<uint16_t>(block % limit);
            const uint16_t increment = static_cast<uint16_t>(65536u / limit);
            const auto correction = reference(expected, step, increment);
            expected.phase_increment_ = correction.increment;
            expected.previous_phase_ = expected.phase_;
            checkCorrection(actual, step, increment, true);
            checkState(actual, expected);
            const uint8_t a = actual.Render(sequence), b = expected.Render(sequence);
            if (a != b) fail("rendered byte differs");
            outputHash = (outputHash ^ a) * UINT64_C(1099511628211);
            checkState(actual, expected);
          }
        }
      }
    }
  }
  static int run(bool validOnly) {
    callerDomain();
    rendering();
    if (!validOnly) fullOperandAxes();
    std::printf("transitions=%llu output-hash=%016llx valid-mismatches=0\n",
                static_cast<unsigned long long>(transitions), static_cast<unsigned long long>(outputHash));
    return 0;
  }
};
}  // namespace shruthi

int main(int argc, char** argv) {
  const bool validOnly = argc == 2 && std::strcmp(argv[1], "--valid-only") == 0;
  return shruthi::LfoControlTestAccess::run(validOnly);
}
