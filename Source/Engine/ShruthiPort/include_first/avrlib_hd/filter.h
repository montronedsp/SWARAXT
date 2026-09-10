// Copyright 2026 MontroneDSP.
// SPDX-License-Identifier: GPL-3.0-or-later
//
// avrlib_hd: HD Shruthi support library. Created independently for the HD
// engine; not copied from Shruthi avrlib (GPL-3.0, (c) 2009 Emilie Gillet).
// Only the interface concepts and the fixed-point -> float mapping described
// in docs/HD_AVRLIB_PLAN.md are expressed here in new code.
//
// HdFilter: HD VCF block for the M3 voice assembly.
//
// The Shruthi hardware filter is analogue and lived outside the DSP; Swara XT
// already ships a float model of it (swaraxt::SwaraXtFilter, an IR3109-family
// ladder with a feedback solver). docs/HD_SHRUTHI_ARCHITECTURE.md §5 records
// that the float filter is the HD filter, so this block is a thin facade that
// (a) exposes the exact parameter mapping the live engine uses to drive the
// filter from the Shruthi control path, and (b) lets the HD engine select a
// higher quality budget ("HD" mode) while keeping "faithful" mode bit-identical
// to the plugin's current behaviour.

#ifndef AVRLIB_HD_FILTER_H_
#define AVRLIB_HD_FILTER_H_

#include "Engine/Filter/FilterQuality.h"
#include "Engine/Filter/SwaraXtFilter.h"

namespace avrlib_hd {

// HD-neutral control set for the filter, in natural units. Committed values
// map directly to swaraxt::SwaraXtFilterParams. This optional facade does not
// calculate Shruthi control destinations. The production engine decodes the
// final firmware cutoff/resonance CV; callers must supply that decoded result.
//
//   envValue            = clamp01(env_source / 255)
//   modValue            = clamp((lfo2_source - 128) / 128, -1, 1)
//   cutoff_hz          = clamp(20000 * 2^((final_cutoff_cv - 254) / 24), 10, 20000)
//   resonance          = final_resonance_cv / 255
//   matrix_*           = optional extra offsets, never the native matrix twice
//   envAmount           = panelEnvAmount * 4    (up to ~4 octaves)
//   modAmount           = panelModAmount * 2
//   drive               = 1.0
struct FilterParams {
  float cutoff_hz = 1000.0f;
  float resonance = 0.0f;             // 0..1
  float key_track = 0.0f;             // 0..1
  float env_amount = 0.0f;            // octaves (post-panel-scale)
  float mod_amount = 0.0f;            // octaves (post-panel-scale)
  float drive = 1.0f;
  float note_number = 69.0f;          // MIDI note for key tracking
  float env_value = 0.0f;             // 0..1
  float mod_value = 0.0f;             // bipolar
  float matrix_cutoff_octaves = 0.0f;
  float matrix_resonance = 0.0f;      // bipolar additive on resonance
};

class HdFilter {
 public:
  enum class Mode {
    kFaithful,  // plugin-default quality budget (normal). Bit-identical audio.
    kHd,        // highest quality budget (high): full oversampling + 8 solver iters.
  };

  HdFilter() = default;

  // The filter always runs at the Shruthi internal rate (pre-SRC), exactly as
  // the live engine does (SwaraXtEngine::prepare -> filter_.prepare).
  void Prepare(double sampleRate) noexcept
  {
    sampleRate_ = sampleRate > 1.0 ? sampleRate : kInternalSampleRate;
    filter_.prepare(sampleRate_);
    UpdateModeBudget();
  }

  void Reset() noexcept { filter_.reset(); }

  void SetMode(Mode mode) noexcept
  {
    if (mode == mode_)
      return;
    mode_ = mode;
    UpdateModeBudget();
    filter_.reset();
  }

  Mode mode() const noexcept { return mode_; }

  void SetParams(const FilterParams& params) noexcept
  {
    swaraxt::SwaraXtFilterParams p;
    p.cutoffHz = params.cutoff_hz;
    p.resonance = swaraxt::clampFinite(
        params.resonance + params.matrix_resonance, 0.0f, 1.0f);
    p.keyTrack = swaraxt::clampFinite(params.key_track, 0.0f, 1.0f);
    p.envAmount = params.env_amount;
    p.modAmount = params.mod_amount;
    p.drive = params.drive;
    p.noteNumber = params.note_number;
    p.envValue = swaraxt::clampFinite(params.env_value, 0.0f, 1.0f);
    p.modValue = swaraxt::clampFinite(params.mod_value, -1.0f, 1.0f);
    p.matrixCutoffOctaves = params.matrix_cutoff_octaves;
    filter_.setParams(p);
  }

  float ProcessSample(float in) noexcept { return filter_.processSample(in); }

  void ProcessBlock(float* samples, int n) noexcept
  {
    for (int i = 0; i < n; ++i)
      samples[i] = ProcessSample(samples[i]);
  }

  int OversampleFactor() const noexcept { return filter_.oversamplingFactor(); }
  int SolverIterations() const noexcept { return filter_.solverIterationLimit(); }

 private:
  static constexpr double kInternalSampleRate = 20000000.0 / 510.0;

  void UpdateModeBudget() noexcept
  {
    filter_.setQuality(mode_ == Mode::kHd ? swaraxt::FilterQuality::high
                                          : swaraxt::FilterQuality::normal);
  }

  swaraxt::SwaraXtFilter filter_;
  double sampleRate_ = kInternalSampleRate;
  Mode mode_ = Mode::kFaithful;
};

}  // namespace avrlib_hd

#endif  // AVRLIB_HD_FILTER_H_
