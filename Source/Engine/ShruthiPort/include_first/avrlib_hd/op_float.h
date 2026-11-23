// Copyright 2026 MontroneDSP.
// SPDX-License-Identifier: GPL-3.0-or-later
//
// avrlib_hd: HD Shruthi support library. Created independently for the HD
// engine; not copied from Shruthi avrlib (GPL-3.0, (c) 2009 Emilie Gillet).
// See docs/HD_AVRLIB_PLAN.md section 4.
//
// Float equivalents of the Classic avrlib fixed-point mix/clip primitives
// (docs/HD_AVRLIB_PLAN.md table): the balance is expressed as a normalised
// float instead of an 8/4-bit weight, removing the >>8 / >>4 quantisation.

#ifndef AVRLIB_HD_OP_FLOAT_H_
#define AVRLIB_HD_OP_FLOAT_H_

namespace avrlib_hd {

inline float mix(float a, float b, float t) {
  return a + (b - a) * t;
}

inline float mix4(float a, float b, float ga, float gb) {
  const float s = ga + gb;
  return (s == 0.0f) ? 0.5f * (a + b) : (a * ga + b * gb) / s;
}

inline float clip(float v, float lo, float hi) {
  return v < lo ? lo : (v > hi ? hi : v);
}

}  // namespace avrlib_hd

#endif  // AVRLIB_HD_OP_FLOAT_H_