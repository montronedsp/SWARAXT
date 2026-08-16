// Copyright 2026 MontroneDSP.
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include "Engine/Filter/Circuit/Ir3109Pole.h"
#include "Engine/Filter/Circuit/Ir3109Types.h"
#include "Engine/Filter/Circuit/ResonanceNetwork.h"
#include "Engine/Filter/Control/CutoffMapper.h"

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
    float envAmount = 0.0f;
    float modAmount = 0.0f;
    float drive = 1.0f;
    float noteNumber = 69.0f;
    float envValue = 0.0f; // 0..1
    float modValue = 0.0f; // bipolar-ish -1..1
};

class SwaraXtFilter {
 public:
    struct Trace
    {
        double filterInput = 0.0;
        double stageState[4] {};
        double stageOutput[4] {};
        double resonanceFeedbackSignal = 0.0;
        double phaseSplitInvertedPath = 0.0;
        double phaseSplitNonInvertedPath = 0.0;
        double diodeLimiterOutput = 0.0;
        double postResonanceMix = 0.0;
        double rawFilterOutputBeforeRecentering = 0.0;
        double recenteredFilterOutput = 0.0;
        double postFilterVcaInput = 0.0;
        double finalPluginOutput = 0.0;
        double solverResidual = 0.0;
        int solverIterations = 0;
        int solverFallbacks = 0;
        bool solverConverged = true;
    };

    SwaraXtFilter()
        : stage1_(kPassiveStage1Traits),
          stage2_(kActiveStage2Traits),
          stage3_(kPassiveStage3Traits),
          stage4_(kActiveStage4Traits)
    {
    }

    void prepare(double hostSampleRate) noexcept
    {
        hostSampleRate_ = hostSampleRate > 1.0 ? hostSampleRate : 44100.0;
        oversampleFactor_ = chooseOversampleFactor(hostSampleRate_);
        const double fs = hostSampleRate_ * static_cast<double>(oversampleFactor_);
        stage1_.setSampleRate(fs);
        stage2_.setSampleRate(fs);
        stage3_.setSampleRate(fs);
        stage4_.setSampleRate(fs);
        levels_ = CircuitLevelCalibration {};
        mapper_.setCalibration(FilterCalibration {});
        reset();
        updateCoefficients();
    }

    void reset() noexcept
    {
        stage1_.reset();
        stage2_.reset();
        stage3_.reset();
        stage4_.reset();
        resonance_.reset();
        previousInput_ = 0.0f;
        if constexpr (SWARAXT_FILTER_DIAGNOSTICS != 0)
        {
            selfOscMetric_ = 0.0f;
            trace_ = {};
        }
    }

    void setParams(const SwaraXtFilterParams& params) noexcept
    {
        if (params.cutoffHz == params_.cutoffHz
            && params.resonance == params_.resonance
            && params.keyTrack == params_.keyTrack
            && params.envAmount == params_.envAmount
            && params.modAmount == params_.modAmount
            && params.envValue == params_.envValue
            && params.modValue == params_.modValue
            && params.noteNumber == params_.noteNumber
            && params.drive == params_.drive)
        {
            return;
        }

        params_ = params;
        updateCoefficients();
    }

    CutoffMapper& cutoffMapper() noexcept { return mapper_; }
    const CutoffMapper& cutoffMapper() const noexcept { return mapper_; }
    const Trace& lastTrace() const noexcept { return trace_; }

    int oversamplingFactor() const noexcept { return oversampleFactor_; }
    float lastStageOutput(int stage) const noexcept
    {
        switch (stage)
        {
            case 0: return static_cast<float>(stage1_.output() / levels_.pluginFullScaleToVolts);
            case 1: return static_cast<float>(stage2_.output() / levels_.pluginFullScaleToVolts);
            case 2: return static_cast<float>(stage3_.output() / levels_.pluginFullScaleToVolts);
            case 3: return static_cast<float>(stage4_.output() / levels_.pluginFullScaleToVolts);
            default: return 0.0f;
        }
    }

    bool isSelfOscillating() const noexcept
    {
#if SWARAXT_FILTER_DIAGNOSTICS
        return selfOscMetric_ > 0.05f;
#else
        return false;
#endif
    }

    float processSample(float in) noexcept
    {
        const float input = clampFinite(in, -4.0f, 4.0f);
        if (oversampleFactor_ <= 1)
        {
            previousInput_ = input;
            return processCore(input);
        }

        double sum = 0.0;
        const double start = previousInput_;
        for (int i = 0; i < oversampleFactor_; ++i)
        {
            const double t = static_cast<double>(i + 1) / static_cast<double>(oversampleFactor_);
            const float subInput = static_cast<float>(start + (static_cast<double>(input) - start) * t);
            sum += static_cast<double>(processCore(subInput));
        }

        previousInput_ = input;
        const float out = clampFinite(static_cast<float>(sum / static_cast<double>(oversampleFactor_)), -8.0f, 8.0f);
        if constexpr (SWARAXT_FILTER_DIAGNOSTICS != 0)
            trace_.finalPluginOutput = out;
        return out;
    }

    void processBlock(float* samples, int n) noexcept
    {
        for (int i = 0; i < n; ++i)
            samples[i] = processSample(samples[i]);
    }

 private:
    struct CascadeResult
    {
        double state[4] {};
        double output[4] {};
    };

    static int chooseOversampleFactor(double hostHz) noexcept
    {
        if (hostHz <= 48000.0 + 1.0)
            return 4;
        if (hostHz <= 96000.0 + 1.0)
            return 2;
        return 1;
    }

    void updateCoefficients() noexcept
    {
        const double modOct = static_cast<double>(params_.envAmount) * static_cast<double>(params_.envValue)
                            + static_cast<double>(params_.modAmount) * static_cast<double>(params_.modValue);
        const double musical = mapper_.mapCutoffHz(params_.cutoffHz,
                                                   params_.keyTrack,
                                                   params_.noteNumber,
                                                   modOct);
        const double stageHz = mapper_.stageCutoffFromMusicalCutoff(musical);
        stage1_.setCutoffHz(stageHz);
        stage2_.setCutoffHz(stageHz);
        stage3_.setCutoffHz(stageHz);
        stage4_.setCutoffHz(stageHz);
        resonance_.setAmount(params_.resonance);
    }

    CascadeResult evaluateCascade(double inputVolts) const noexcept
    {
        Ir3109Pole p1 = stage1_;
        Ir3109Pole p2 = stage2_;
        Ir3109Pole p3 = stage3_;
        Ir3109Pole p4 = stage4_;
        return runCascade(inputVolts, p1, p2, p3, p4);
    }

    CascadeResult commitCascade(double inputVolts) noexcept
    {
        return runCascade(inputVolts, stage1_, stage2_, stage3_, stage4_);
    }

    CascadeResult runCascade(double inputVolts,
                             Ir3109Pole& p1,
                             Ir3109Pole& p2,
                             Ir3109Pole& p3,
                             Ir3109Pole& p4) const noexcept
    {
        CascadeResult result;

        result.output[0] = p1.process(inputVolts);
        result.output[1] = p2.process(result.output[0]);
        result.output[2] = p3.process(result.output[1]);
        result.output[3] = p4.process(result.output[2]);
        result.state[0] = p1.integrator();
        result.state[1] = p2.integrator();
        result.state[2] = p3.integrator();
        result.state[3] = p4.integrator();

        return result;
    }

    ResonanceNetwork::Evaluation solveFeedback(double inputVolts, Trace& localTrace) const noexcept
    {
        ResonanceNetwork::Evaluation network = resonance_.evaluate(stage4_.output());
        double estimate = stage4_.output();
        double previous = estimate;
        if constexpr (SWARAXT_FILTER_DIAGNOSTICS != 0)
        {
            localTrace.solverResidual = 0.0;
            localTrace.solverIterations = 0;
            localTrace.solverFallbacks = 0;
            localTrace.solverConverged = true;
        }

        for (int iter = 0; iter < 8; ++iter)
        {
            network = resonance_.evaluate(estimate);
            const CascadeResult preview = evaluateCascade(inputVolts + network.feedback);
            const double next = preview.output[3];
            const double residual = next - estimate;
            previous = estimate;
            estimate += residual * 0.72;
            if constexpr (SWARAXT_FILTER_DIAGNOSTICS != 0)
            {
                localTrace.solverResidual = std::abs(residual);
                localTrace.solverIterations = iter + 1;
            }

            if (! std::isfinite(estimate))
            {
                estimate = previous;
                if constexpr (SWARAXT_FILTER_DIAGNOSTICS != 0)
                {
                    localTrace.solverFallbacks = 1;
                    localTrace.solverConverged = false;
                }
                break;
            }

            if (std::abs(residual) < 1.0e-9)
                break;
        }

        if constexpr (SWARAXT_FILTER_DIAGNOSTICS != 0)
        {
            if (localTrace.solverResidual > 1.0e-5)
                localTrace.solverConverged = false;
        }

        return resonance_.evaluate(estimate);
    }

    float processCore(float inPlugin) noexcept
    {
        Trace localTrace;
        const double drive = clampFinite(static_cast<double>(params_.drive), 0.25, 4.0);
        const double inV = static_cast<double>(inPlugin) * levels_.pluginFullScaleToVolts * drive;

        const ResonanceNetwork::Evaluation appliedNetwork = solveFeedback(inV, localTrace);
        const double stageInput = inV + appliedNetwork.feedback;
        const CascadeResult cascade = commitCascade(stageInput);
        const ResonanceNetwork::Evaluation outputNetwork = resonance_.process(cascade.output[3]);

        const double rawOutV = cascade.output[3] + outputNetwork.outputContribution;
        float out = static_cast<float>(rawOutV / levels_.pluginFullScaleToVolts);
        out = clampFinite(out, -8.0f, 8.0f);

        if constexpr (SWARAXT_FILTER_DIAGNOSTICS != 0)
        {
            localTrace.filterInput = inV;
            for (int i = 0; i < 4; ++i)
            {
                localTrace.stageState[i] = cascade.state[i];
                localTrace.stageOutput[i] = cascade.output[i];
            }
            localTrace.resonanceFeedbackSignal = appliedNetwork.feedback;
            localTrace.phaseSplitInvertedPath = appliedNetwork.phaseSplitInverted;
            localTrace.phaseSplitNonInvertedPath = appliedNetwork.phaseSplitNonInverted;
            localTrace.diodeLimiterOutput = appliedNetwork.diodeLimiterOutput;
            localTrace.postResonanceMix = stageInput;
            localTrace.rawFilterOutputBeforeRecentering = rawOutV;
            localTrace.recenteredFilterOutput = rawOutV;
            localTrace.postFilterVcaInput = out;
            localTrace.finalPluginOutput = out;
            trace_ = localTrace;

            const float absOut = std::fabs(out);
            selfOscMetric_ = selfOscMetric_ * 0.995f + absOut * 0.005f;
        }
        return out;
    }

    Ir3109Pole stage1_;
    Ir3109Pole stage2_;
    Ir3109Pole stage3_;
    Ir3109Pole stage4_;
    ResonanceNetwork resonance_;
    CutoffMapper mapper_;
    CircuitLevelCalibration levels_ {};
    SwaraXtFilterParams params_ {};
    Trace trace_ {};
    double hostSampleRate_ = 44100.0;
    int oversampleFactor_ = 4;
    float previousInput_ = 0.0f;
    float selfOscMetric_ = 0.0f;
};

// Offline double-precision reference with higher oversampling and independent
// state variables. It intentionally does not delegate to SwaraXtFilter.
class Ir3109ReferenceModel {
 public:
    using Trace = SwaraXtFilter::Trace;

    struct Features
    {
        bool stageAsymmetry = true;
        bool currentLimits = false;
        bool slew = false;
        bool diodes = true;
        bool phaseSplitter = true;
        bool outputContribution = true;
    };

    void prepare(double sampleRate) noexcept
    {
        sampleRate_ = sampleRate > 1.0 ? sampleRate : 44100.0;
        reset();
        updateCoefficients();
    }

    void reset() noexcept
    {
        states_.fill(0.0);
        previousInput_ = 0.0;
        trace_ = {};
    }

    void setFeatures(const Features& f) noexcept { features_ = f; }

    void setParams(const SwaraXtFilterParams& p) noexcept
    {
        params_ = p;
        updateCoefficients();
    }

    double process(double input) noexcept
    {
        constexpr int kReferenceOversample = 16;
        double sum = 0.0;
        for (int os = 0; os < kReferenceOversample; ++os)
        {
            const double t = static_cast<double>(os + 1) / static_cast<double>(kReferenceOversample);
            const double xPlugin = previousInput_ + (input - previousInput_) * t;
            sum += processCore(xPlugin, sampleRate_ * static_cast<double>(kReferenceOversample));
        }
        previousInput_ = input;
        trace_.finalPluginOutput = sum / static_cast<double>(kReferenceOversample);
        return trace_.finalPluginOutput;
    }

    const Trace& trace() const noexcept { return trace_; }

 private:
    double poleStep(double input, double& state, double sampleRate, int stage) const noexcept
    {
        const double g = std::tan(kPi * clampFinite(stageCutoffHz_, 1.0, sampleRate * 0.45) / sampleRate);
        const double softness = features_.stageAsymmetry
            ? (stage == 1 || stage == 3 ? 0.98 : 1.06)
            : 1.0;
        const double attenuation = kOtaDifferentialInputAttenuation / softness;
        const double diff = input - state;
        const double shaped = std::tanh((diff * attenuation) / (2.0 * kOtaThermalVoltageVolts))
                            * (2.0 * kOtaThermalVoltageVolts) / attenuation;
        const double v = shaped / (1.0 + g);
        const double y = state + g * v;
        state = y + g * v;
        return y;
    }

    double cascadePreview(double input, const std::array<double, 4>& sourceStates, double sampleRate) const noexcept
    {
        std::array<double, 4> s = sourceStates;
        double x = input;
        for (int stage = 0; stage < 4; ++stage)
            x = poleStep(x, s[static_cast<size_t>(stage)], sampleRate, stage + 1);
        return x;
    }

    ResonanceNetwork::Evaluation referenceNetwork(double stage4) const noexcept
    {
        const double amount = clampFinite(static_cast<double>(params_.resonance), 0.0, 1.0);
        const double gain = std::pow(amount, 1.35) * 4.45;
        const double inverted = features_.phaseSplitter ? -stage4 : stage4;
        const double nonInverted = features_.phaseSplitter ? stage4 * 0.85 : stage4;
        const double limited = features_.diodes ? 1.35 * std::tanh(inverted / 1.35) : inverted;
        const double output = features_.outputContribution ? nonInverted * amount * amount * 0.10 : 0.0;
        return { inverted, nonInverted, limited, limited * gain, output };
    }

    double processCore(double inputPlugin, double sampleRate) noexcept
    {
        Trace local;
        const double drive = clampFinite(static_cast<double>(params_.drive), 0.25, 4.0);
        const double inV = inputPlugin * levels_.pluginFullScaleToVolts * drive;

        double estimate = states_[3];
        ResonanceNetwork::Evaluation network = referenceNetwork(estimate);
        for (int iter = 0; iter < 12; ++iter)
        {
            network = referenceNetwork(estimate);
            const double next = cascadePreview(inV + network.feedback, states_, sampleRate);
            const double residual = next - estimate;
            estimate += residual * 0.75;
            local.solverResidual = std::abs(residual);
            local.solverIterations = iter + 1;
            if (std::abs(residual) < 1.0e-10)
                break;
        }

        network = referenceNetwork(estimate);
        double x = inV + network.feedback;
        for (int stage = 0; stage < 4; ++stage)
        {
            x = poleStep(x, states_[static_cast<size_t>(stage)], sampleRate, stage + 1);
            local.stageState[stage] = states_[static_cast<size_t>(stage)];
            local.stageOutput[stage] = x;
        }

        const ResonanceNetwork::Evaluation outputNetwork = referenceNetwork(x);
        const double raw = x + outputNetwork.outputContribution;
        const double out = clampFinite(raw / levels_.pluginFullScaleToVolts, -8.0, 8.0);

        local.filterInput = inV;
        local.resonanceFeedbackSignal = network.feedback;
        local.phaseSplitInvertedPath = network.phaseSplitInverted;
        local.phaseSplitNonInvertedPath = network.phaseSplitNonInverted;
        local.diodeLimiterOutput = network.diodeLimiterOutput;
        local.postResonanceMix = inV + network.feedback;
        local.rawFilterOutputBeforeRecentering = raw;
        local.recenteredFilterOutput = raw;
        local.postFilterVcaInput = out;
        local.finalPluginOutput = out;
        local.solverConverged = local.solverResidual < 1.0e-5;
        trace_ = local;
        return out;
    }

    void updateCoefficients() noexcept
    {
        const double modOct = static_cast<double>(params_.envAmount) * static_cast<double>(params_.envValue)
                            + static_cast<double>(params_.modAmount) * static_cast<double>(params_.modValue);
        const double musical = mapper_.mapCutoffHz(params_.cutoffHz,
                                                   params_.keyTrack,
                                                   params_.noteNumber,
                                                   modOct);
        stageCutoffHz_ = mapper_.stageCutoffFromMusicalCutoff(musical);
    }

    CutoffMapper mapper_;
    CircuitLevelCalibration levels_ {};
    SwaraXtFilterParams params_ {};
    Features features_ {};
    Trace trace_ {};
    std::array<double, 4> states_ {};
    double sampleRate_ = 44100.0;
    double stageCutoffHz_ = 1000.0;
    double previousInput_ = 0.0;
};

}  // namespace swaraxt
