// Copyright 2026 MontroneDSP.
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include "Engine/Filter/Circuit/Ir3109Pole.h"
#include "Engine/Filter/Circuit/Ir3109BoardCore.h"
#include "Engine/Filter/Circuit/Ir3109Types.h"
#include "Engine/Filter/Control/CutoffMapper.h"
#include "Engine/Filter/FilterQuality.h"
#include "Engine/Filter/FilterRateConverter.h"

#include <array>
#include <cmath>

namespace swaraxt {

#ifndef SWARAXT_FILTER_DIAGNOSTICS
#define SWARAXT_FILTER_DIAGNOSTICS 0
#endif

struct SwaraXtFilterParams
{
    float cutoffHz = 1000.0f;
    float resonance = 0.0f;
    float keyTrack = 0.0f;
    float drive = 1.0f;
    float noteNumber = 69.0f;
    float matrixCutoffOctaves = 0.0f;
    // Explicit firmware-derived board CV; negative selects the diagnostic Hz
    // interface. This avoids RAW's 20 kHz panel clamp truncating CV byte 255.
    double boardCutoffCvVolts = -1.0;
};

class SwaraXtFilter {
 public:
    struct Trace
    {
        double finalPluginOutput = 0.0;
    };

    SwaraXtFilter() = default;

    void prepare(double hostSampleRate) noexcept
    {
        hostSampleRate_ = hostSampleRate > 1.0 ? hostSampleRate : 44100.0;
        applyQualityConfiguration(true);
        levels_ = CircuitLevelCalibration {};
        mapper_.setCalibration(FilterCalibration {});
        reset();
        updateCoefficients();
    }

    void reset() noexcept
    {
        rateConverter_.reset();
        boardCore_.reset();
        instrumentVcaGain_ = 0;
        instrumentFlushRemaining_ = 0;
        controlDelay_.fill(targetControl_);
        controlHead_ = 0;
        controlsNeedPrime_ = true;
        if constexpr (SWARAXT_FILTER_DIAGNOSTICS != 0)
        {
            trace_ = {};
        }
    }

    void setParams(const SwaraXtFilterParams& params) noexcept
    {
        if (params.cutoffHz == params_.cutoffHz
            && params.resonance == params_.resonance
            && params.keyTrack == params_.keyTrack
            && params.matrixCutoffOctaves == params_.matrixCutoffOctaves
            && params.noteNumber == params_.noteNumber
            && params.drive == params_.drive
            && params.boardCutoffCvVolts == params_.boardCutoffCvVolts)
        {
            return;
        }

        params_ = params;
        updateCoefficients();
    }

    CutoffMapper& cutoffMapper() noexcept { return mapper_; }
    const CutoffMapper& cutoffMapper() const noexcept { return mapper_; }
    const Trace& lastTrace() const noexcept { return trace_; }
#if SWARAXT_ENABLE_SHRUTHI_DEBUG_TAPS
    const SwaraXtFilterParams& paramsForTests() const noexcept { return params_; }
    double boardCutoffCvForTests() const noexcept { return boardCore_.cutoffCv(); }
#endif

    int oversamplingFactor() const noexcept { return oversampleFactor_; }
    int latencySamples() const noexcept { return rateConverter_.latency(); }
    int solverIterationLimit() const noexcept { return solverIterationLimit_; }
    FilterQuality quality() const noexcept { return quality_; }

    // Changing quality reconfigures oversampling/solver budget and resets state.
    void setQuality(FilterQuality quality) noexcept
    {
        if (quality == quality_)
            return;
        quality_ = quality;
        applyQualityConfiguration(true);
        reset();
        updateCoefficients();
    }

    void setOversampleFactorForTests(int factor) noexcept
    {
        applyOversampleFactor(factor);
        reset();
        updateCoefficients();
    }

    void setSolverIterationLimitForTests(int iterations) noexcept
    {
        solverIterationLimit_ = iterations < 1 ? 1 : (iterations > 8 ? 8 : iterations);
    }

    // Complete analog path: VCA and output network are before anti-alias
    // decimation. The input coupling/network always precedes the filter core.
    float processInstrumentSample(float in, float vcaTarget) noexcept
    {
        return processWithControls(in, vcaTarget);
    }
    float instrumentVcaControl() const noexcept { return instrumentVcaGain_; }
    bool instrumentTailActive() const noexcept { return instrumentFlushRemaining_ > 0; }
    const Ir3109BoardCore& boardCoreForTests() const noexcept { return boardCore_; }
    void advanceDormantControls(int nativeSamples) noexcept {
        boardCore_.setTargets(targetControl_.cutoff, targetControl_.resonance, 0, targetControl_.boardCutoffCvVolts);
        boardCore_.advanceControls(nativeSamples / hostSampleRate_);
    }

private:
    float processWithControls(float in, float vcaTarget) noexcept
    {
        const float input = clampFinite(in, -4.0f, 4.0f);
        auto target = targetControl_;
        target.vca = vcaTarget;
        if (controlsNeedPrime_) {
            // Reset may precede SetParams. Seed the silent prehistory from the
            // first committed controls, never the previous render's patch.
            // The VCA remains closed until its delayed first target arrives.
            auto initial = target;
            initial.vca = 0;
            controlDelay_.fill(initial);
            boardCore_.setTargets(initial.cutoff, initial.resonance, 0, initial.boardCutoffCvVolts);
            boardCore_.reset();
            controlsNeedPrime_ = false;
        }
        // Align held native controls with the interpolation filter's delay.
        const auto control = oversampleFactor_ > 1 ? controlDelay_[controlHead_] : target;
        controlDelay_[controlHead_] = target;
        controlHead_ = (controlHead_ + 1) % controlDelay_.size();
        boardCore_.setTargets(control.cutoff, control.resonance, control.vca, control.boardCutoffCvVolts);
        const float out = clampFinite(rateConverter_.process(input,
            [this, control](float sample) noexcept {
                return boardCore_.process(sample * control.drive, solverIterationLimit_);
            }), -8.0f, 8.0f);
        instrumentVcaGain_ = static_cast<float>(boardCore_.vcaControl());
        const bool active = vcaTarget > 0 || control.vca > 0 || instrumentVcaGain_ > 1.e-8f
            || boardCore_.outputTailActive();
        instrumentFlushRemaining_ = active ? FilterRateConverter::kSpan : std::max(0, instrumentFlushRemaining_ - 1);
        if constexpr (SWARAXT_FILTER_DIAGNOSTICS != 0)
            trace_.finalPluginOutput = out;
        return out;
    }

public:
    void processBlock(float* samples, int n) noexcept
    {
        for (int i = 0; i < n; ++i)
            samples[i] = processInstrumentSample(samples[i], 1);
    }

 private:
    static int chooseOversampleFactor(double hostHz) noexcept
    {
        if (hostHz <= 48000.0 + 1.0)
            return 4;
        if (hostHz <= 96000.0 + 1.0)
            return 2;
        return 1;
    }

    static int oversampleForQuality(FilterQuality quality, double hostHz) noexcept
    {
        const int full = chooseOversampleFactor(hostHz);
        if (quality == FilterQuality::eco)
            return full > 1 ? full / 2 : 1;
        return full;
    }

    static int solverLimitForQuality(FilterQuality quality) noexcept
    {
        return quality == FilterQuality::high ? 8 : 4;
    }

    void applyOversampleFactor(int factor) noexcept
    {
        const int clamped = factor <= 1 ? 1 : factor <= 2 ? 2 : factor <= 4 ? 4 : 8;
        oversampleFactor_ = clamped;
        rateConverter_.prepare(clamped);
        const double fs = hostSampleRate_ * static_cast<double>(oversampleFactor_);
        boardCore_.prepare(fs);
    }

    void applyQualityConfiguration(bool forceOversample) noexcept
    {
        solverIterationLimit_ = solverLimitForQuality(quality_);
        const int desired = oversampleForQuality(quality_, hostSampleRate_);
        if (forceOversample || desired != oversampleFactor_)
            applyOversampleFactor(desired);
    }

    void updateCoefficients() noexcept
    {
        const double modOct = static_cast<double>(params_.matrixCutoffOctaves);
        const double musical = mapper_.mapCutoffHz(params_.cutoffHz,
                                                    params_.keyTrack,
                                                    params_.noteNumber,
                                                    modOct);
        const double stageHz = mapper_.stageCutoffFromMusicalCutoff(musical);
        targetControl_ = {stageHz, params_.resonance, params_.drive};
        targetControl_.boardCutoffCvVolts = params_.boardCutoffCvVolts;
    }

    CutoffMapper mapper_;
    CircuitLevelCalibration levels_ {};
    SwaraXtFilterParams params_ {};
    Trace trace_ {};
    double hostSampleRate_ = 44100.0;
    int oversampleFactor_ = 4;
    int solverIterationLimit_ = 4;
    FilterQuality quality_ = FilterQuality::normal;
    struct Control {
        double cutoff = 1000;
        float resonance = 0, drive = 1, vca = 0;
        double boardCutoffCvVolts = -1;
    };
    Control targetControl_{};
    std::array<Control, FilterRateConverter::kInputDelay> controlDelay_{};
    size_t controlHead_ = 0;
    bool controlsNeedPrime_ = true;
    FilterRateConverter rateConverter_;
    Ir3109BoardCore boardCore_;
    float instrumentVcaGain_ = 0;
    int instrumentFlushRemaining_ = 0;
};

}  // namespace swaraxt
