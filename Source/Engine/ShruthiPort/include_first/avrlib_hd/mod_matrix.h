// Copyright 2026 MontroneDSP.
// SPDX-License-Identifier: GPL-3.0-or-later
//
// avrlib_hd: HD Shruthi modulation matrix. Created independently for the HD
// engine; not copied from Shruthi avrlib / Shruthi firmware (GPL-3.0,
// (c) 2009 Emilie Gillet).
//
// The Classic layout is mirrored numerically so the faithful path can be
// driven with the same {amount, source, destination} rows and the same
// operator descriptors the firmware stores in the patch:
//   - 32 modulation sources, 27 modulation destinations (src/patch.h
//     ModulationSource / ModulationDestination).
//   - Two CV operators (SUM..LAG, src/patch.h CvOperator) mixing source
//     bytes into MOD_SRC_OP_1 / MOD_SRC_OP_2.
//
// Two processing families:
//   - Faithful (byte): bit-for-bit replication of Voice::LoadSources
//     (operators) + Voice::ProcessModulationMatrix, using the same
//     primitives (S8S8Mul / S8U8Mul / S8U8MulShift8 / U8Mix / U8U8MulShift8 /
//     S16ClipU14) and the same special cases: the last matrix row scaled by
//     the wheel, the "relative" source class (LFO_1..LFO_2, pitch bend,
//     note, audio), the multiplicative VCA row, and the trigger-env side
//     flags.
//   - HD (float): the same routing as bounded float destinations in their
//     natural units. Sources are bipolar [-1, 1] for the relative class and
//     unipolar [0, 1] otherwise; rows add amount * source, clamped to the
//     destination range; the VCA row multiplies against the current value.
//
// See docs/HD_SHRUTHI_ARCHITECTURE.md (mod matrix) and HD_AVRLIB_PLAN.md.

#ifndef AVRLIB_HD_MOD_MATRIX_H_
#define AVRLIB_HD_MOD_MATRIX_H_

#include <cmath>
#include <cstdint>

#include "avrlib_hd/types.h"

namespace avrlib_hd {

constexpr int kNumModulationSources = 32;
constexpr int kNumModulationDestinations = 27;
// Classic matrix has always 12 rows; the wheel scales the 12th (last) row no
// matter how many rows are actually routed.
constexpr int kModulationMatrixSize = 12;

// Numeric mirrors of Classic src/patch.h ModulationSource.
enum ModSourceId {
  kModSourceLfo1 = 0,
  kModSourceLfo2,
  kModSourceSeq,
  kModSourceSeq1,
  kModSourceSeq2,
  kModSourceStep,
  kModSourceWheel,
  kModSourceAftertouch,
  kModSourcePitchBend,
  kModSourceOffset,
  kModSourceCv1,
  kModSourceCv2,
  kModSourceCv3,
  kModSourceCv4,
  kModSourceCcA,
  kModSourceCcB,
  kModSourceCcC,
  kModSourceCcD,
  kModSourceNoise,
  kModSourceEnv1,
  kModSourceEnv2,
  kModSourceVelocity,
  kModSourceRandom,
  kModSourceNote,
  kModSourceGate,
  kModSourceAudio,
  kModSourceOp1,
  kModSourceOp2,
  kModSourceValue4,
  kModSourceValue8,
  kModSourceValue16,
  kModSourceValue32,
  kModSourceLast = kNumModulationSources
};

// Numeric mirrors of Classic src/patch.h ModulationDestination.
enum ModDestination {
  kModDestFilterCutoff = 0,
  kModDestVca,
  kModDestPwm1,
  kModDestPwm2,
  kModDestVco1,
  kModDestVco2,
  kModDestVco1_2Coarse,
  kModDestVco1_2Fine,
  kModDestMixBalance,
  kModDestMixNoise,
  kModDestMixSubOsc,
  kModDestFilterResonance,
  kModDestCv1,
  kModDestCv2,
  kModDestAttack,
  kModDestLfo1,
  kModDestLfo2,
  kModDestTriggerEnv1,
  kModDestTriggerEnv2,
  kModDestAttack1,
  kModDestDecay1,
  kModDestSustain1,
  kModDestRelease1,
  kModDestAttack2,
  kModDestDecay2,
  kModDestSustain2,
  kModDestRelease2,
  kModDestLast = kNumModulationDestinations
};

// Numeric mirrors of Classic src/patch.h CvOperator.
enum CvOperator {
  kOpCvNone = 0,
  kOpCvSum,
  kOpCvProduct,
  kOpCvMax,
  kOpCvMin,
  kOpCvXor,
  kOpCvGe,
  kOpCvLe,
  kOpCvQuantize,
  kOpCvLagProcessor,
  kOpCvLast
};

// One modulation matrix row, Classic-compatible.
struct UniModulation {
  int8_t amount;
  uint8_t source;
  uint8_t destination;
};

// One classic CV operator stage.
struct UniOperator {
  uint8_t op;
  uint8_t operands[2];
};

class HdModMatrix {
 public:
  HdModMatrix() = default;

  // -------------------------------------------------------------------------
  // Faithful byte primitives (Classic src/avrlib/op.h, replicated).
  // -------------------------------------------------------------------------

  // S16ClipU14: clip an additive 14-bit modulation result to [0, 16383].
  static int16_t ClipU14(int16_t value) {
    uint8_t msb = static_cast<uint16_t>(value) >> 8;
    if (msb & 0x80) {
      return 0;
    }
    if (msb & 0x40) {
      return 16383;
    }
    return value;
  }

  // S8S8Mul truncated-signature replication: amount * (uint8 cast to int8).
  static int16_t S8S8Mul8(int8_t amount, uint8_t rhs) {
    return static_cast<int16_t>(amount) *
           static_cast<int8_t>(static_cast<uint8_t>(rhs));
  }

  // S8U8Mul: amount * rhs.
  static int16_t S8U8Mul8(int8_t amount, uint8_t rhs) {
    return static_cast<int16_t>(amount) * static_cast<uint8_t>(rhs);
  }

  // S8U8MulShift8: (amount * rhs) >> 8, kept as int8. This is the Shruthi
  // "muls + high byte" semantics (r1 = (a*b) >> 8).
  static int8_t S8U8MulShift8_8(int8_t amount, uint8_t rhs) {
    return static_cast<int8_t>(static_cast<int16_t>(amount) *
                               static_cast<uint8_t>(rhs) >> 8);
  }

  // U8Mix(a, b, balance): (a * (255 - balance) + b * balance) >> 8.
  static uint8_t MixU8(uint8_t a, uint8_t b, uint8_t balance) {
    return static_cast<uint8_t>(
        (static_cast<uint16_t>(a) * (255 - balance) +
         static_cast<uint16_t>(b) * balance) >>
        8);
  }

  // U8U8MulShift8.
  static uint8_t U8U8MulShift8(uint8_t a, uint8_t b) {
    return static_cast<uint8_t>(static_cast<uint16_t>(a) * b >> 8);
  }

  // -------------------------------------------------------------------------
  // Faithful operator (Voice::LoadSources operator stage), byte-exact.
  // lag_prev is the previous OP output byte kept by the caller.
  // -------------------------------------------------------------------------
  static uint8_t ApplyOperator(uint8_t op, uint8_t x, uint8_t y,
                               uint8_t lag_prev) {
    if (op <= kOpCvLe) {  // sum, product, max, min, xor, ge, le
      uint8_t ops[8] = {};
      ops[1] = static_cast<uint8_t>((x >> 1) + (y >> 1));
      ops[2] = U8U8MulShift8(x, y);
      ops[5] = static_cast<uint8_t>(x ^ y);
      if (x > y) {
        ops[3] = x;
        ops[4] = y;
        ops[6] = 255;
        ops[7] = 0;
      } else {
        ops[3] = y;
        ops[4] = x;
        ops[6] = 0;
        ops[7] = 255;
      }
      return ops[op];
    }
    if (op == kOpCvQuantize) {
      uint8_t mask = 0;
      while (y >>= 1) {
        mask = static_cast<uint8_t>(mask >> 1);
        mask = static_cast<uint8_t>(mask | 0x80);
      }
      return static_cast<uint8_t>(x & mask);
    }
    if (op == kOpCvLagProcessor) {
      y = static_cast<uint8_t>((y >> 2) + 1);
      uint16_t v = static_cast<uint16_t>(lag_prev) * (256 - y);
      v += static_cast<uint16_t>(x) * y;
      return static_cast<uint8_t>(v >> 8);
    }
    return 0;  // OP_CV_NONE is skipped by the caller.
  }

  // Faithful CV-operator stage over a source byte array (32 entries).
  // Writes OP result bytes into sources[kModSourceOp1] / kModSourceOp2 and
  // persists the lag state between calls inside the same source array.
  void ProcessOperatorsByte(uint8_t* sources, const UniOperator* ops) const {
    for (int i = 0; i < 2; ++i) {
      const UniOperator& op = ops[i];
      if (op.op == kOpCvNone) {
        continue;
      }
      uint8_t current = sources[kModSourceOp1 + i];
      current = ApplyOperator(op.op, sources[op.operands[0]],
                              sources[op.operands[1]], current);
      sources[kModSourceOp1 + i] = current;
    }
  }

  // -------------------------------------------------------------------------
  // Faithful matrix pass (Voice::ProcessModulationMatrix), byte-exact.
  //
  //   dst14[i]    : 14-bit destination working values in/out (0..16383).
  //   vca         : multiplicative VCA byte in/out (0..255).
  //   wheel       : MOD_SRC_WHEEL byte; scales the last matrix row.
  //   retrigger   : optional 2-byte side output, 1 when a row targets a
  //                 trigger-env destination (classic auto-retrigger disable).
  // -------------------------------------------------------------------------
  static void ProcessMatrixByte(int num_rows, const UniModulation* matrix,
                                const uint8_t* sources, int16_t* dst14,
                                uint8_t* vca, uint8_t wheel,
                                uint8_t* retrigger) {
    if (retrigger) {
      retrigger[0] = retrigger[1] = 0;
    }
    for (int i = 0; i < num_rows; ++i) {
      int8_t amount = matrix[i].amount;
      if (amount == 0) {
        continue;
      }
      if (i == kModulationMatrixSize - 1) {  // last row scaled by the wheel.
        amount = S8U8MulShift8_8(amount, wheel);
      }
      const uint8_t source = matrix[i].source;
      const uint8_t destination = matrix[i].destination;
      if (destination >= kModDestTriggerEnv1 &&
          destination <= kModDestTriggerEnv2) {
        if (retrigger) {
          retrigger[destination - kModDestTriggerEnv1] = 1;
        }
      }
      uint8_t source_value = sources[source];
      if (destination != kModDestVca) {
        int16_t modulation = dst14[destination];
        if (source <= kModSourceLfo2 || source == kModSourcePitchBend ||
            source == kModSourceNote || source == kModSourceAudio) {
          modulation += S8S8Mul8(amount, static_cast<uint8_t>(
                                             source_value + 128));
        } else {
          modulation += S8U8Mul8(amount, source_value);
        }
        dst14[destination] = ClipU14(modulation);
      } else {
        // Multiplicative VCA row.
        if (amount < 0) {
          amount = static_cast<int8_t>(-amount);
          source_value = static_cast<uint8_t>(255 - source_value);
        }
        if (amount != 63) {
          // amount * 4 replicates the AVR `amount << 2` (truncate to uint8)
          // without UB for negative amounts.
          source_value = MixU8(255, source_value,
                               static_cast<uint8_t>(static_cast<int16_t>(amount) * 4));
        }
        *vca = U8U8MulShift8(*vca, source_value);
      }
    }
  }

  // -------------------------------------------------------------------------
  // HD (float) processing.
  //
  // Matrix rows: float sources are unipolar [0, 1] for the absolute class and
  // bipolar [-1, 1] for the relative class (LFO_1..LFO_2, pitch bend, note,
  // audio); either way a row adds amt * source to the destination. A row's
  // amount is a float scale amt (callers usually derive it as the classic
  // amount / 127); the last row is additionally scaled by wheel_f (0..1).
  // Destinations are floats in their natural units ([0, 1], VCA in [0, 1]
  // multiplicative): additive rows clamp to [0, 1]; the VCA row interpolates
  // the current value toward the (optionally inverted) source by |amt|.
  // Trigger-env destinations receive a scalar; > 0.5 latches a trigger in
  // the caller.
  // -------------------------------------------------------------------------

  // Operator stage over unipolar [0, 1] float operands; the classic op menu
  // with an analytic xor analog (1 - |x - y|) and a y-driven low-pass lag.
  static float ApplyOperatorFloat(uint8_t op, float x, float y,
                                  float lag_prev) {
    x = Clamp01(x);
    y = Clamp01(y);
    switch (op) {
      case kOpCvSum:
        return 0.5f * (x + y);
      case kOpCvProduct:
        return x * y;
      case kOpCvMax:
        return x > y ? x : y;
      case kOpCvMin:
        return x < y ? x : y;
      case kOpCvXor:
        return 1.0f - std::fabs(x - y);
      case kOpCvGe:
        return x >= y ? 1.0f : 0.0f;
      case kOpCvLe:
        return x <= y ? 1.0f : 0.0f;
      case kOpCvQuantize: {
        float bits = 1.0f + 7.0f * y;
        float levels = std::ldexp(1.0f, static_cast<int>(bits)) - 1.0f;
        return std::nearbyint(x * levels) / levels;
      }
      case kOpCvLagProcessor: {
        // alpha in [0.25, 1] driven by the y operand (more smoothing at low y).
        float alpha = 0.75f * y + 0.25f;
        return lag_prev * (1.0f - alpha) + x * alpha;
      }
      default:
        return x;
    }
  }

  // HD operator stage over 32 float sources; writes into index 26/27 and
  // keeps the lag state there.
  void ProcessOperatorsFloat(float* sources, const UniOperator* ops) const {
    for (int i = 0; i < 2; ++i) {
      const UniOperator& op = ops[i];
      if (op.op == kOpCvNone) {
        continue;
      }
      float current = sources[kModSourceOp1 + i];
      sources[kModSourceOp1 + i] =
          ApplyOperatorFloat(op.op, sources[op.operands[0]],
                             sources[op.operands[1]], current);
    }
  }

  // HD matrix pass.
  static void ProcessMatrixFloat(int num_rows, const UniModulation* matrix,
                                 const float* sources, float* dst,
                                 float* vca, float wheel_f,
                                 const float* amount_f) {
    for (int i = 0; i < num_rows; ++i) {
      int8_t amount = matrix[i].amount;
      if (amount == 0) {
        continue;
      }
      float amt = amount_f ? amount_f[i] : static_cast<float>(amount) / 127.0f;
      if (i == kModulationMatrixSize - 1) {
        amt *= wheel_f;
      }
      const uint8_t source = matrix[i].source;
      const uint8_t destination = matrix[i].destination;
      if (destination != kModDestVca) {
        float modulation = dst[destination] + amt * sources[source];
        dst[destination] = Clamp01(modulation);
      } else {
        float src = sources[source];
        if (amount < 0) {
          src = 1.0f - src;
        }
        float weight = amt < 0.0f ? -amt : amt;
        *vca *= weight;
        *vca += src * (1.0f - weight);
        *vca = Clamp01(*vca);
      }
    }
  }

 private:
  static float Clamp01(float x) {
    return x < 0.0f ? 0.0f : (x > 1.0f ? 1.0f : x);
  }
};

}  // namespace avrlib_hd

#endif  // AVRLIB_HD_MOD_MATRIX_H_