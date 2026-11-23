// Copyright 2026 MontroneDSP. SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
#include "Engine/Filter/SwaraXtFilter.h"
#include <limits>

namespace swaraxt {
// Offline access only. Prefix/commit replay is checked against actual process()
// state and output; only the reference's scalar feedback solve is different.
struct Ir3109SolverAccess {
    using Core = Ir3109BoardCore;
    struct Equation {
        Core board;
        double mixer, compensation, bias;
        double feedback(double x) const noexcept {
            return 68000 * bias * std::tanh((compensation - board.sharedAc_.previewHigh(x)
                * Core::kFeedbackDivider) / (2 * kOtaThermalVoltageVolts));
        }
        double evaluate(double x, double* derivative = nullptr) const noexcept {
            double y = .68 * mixer + feedback(x);
            if (derivative == nullptr) {
                for (const auto& pole : board.poles_) y = pole.preview(y, pole.snapshot()).output;
                return y;
            }
            const double t = std::tanh((compensation - board.sharedAc_.previewHigh(x)
                * Core::kFeedbackDivider) / (2 * kOtaThermalVoltageVolts));
            const double highGain = 1 - 1 / (1 + 2 * board.sampleRate_ * Core::kSharedOutputTau);
            double d = -68000 * bias * Core::kFeedbackDivider * highGain
                / (2 * kOtaThermalVoltageVolts) * (1 - t * t);
            for (const auto& pole : board.poles_) {
                const auto state = pole.snapshot();
                const auto& traits = pole.traits();
                const double a = traits.differentialInputAttenuation
                    / clampFinite(traits.saturationSoftness, .5, 2.0);
                const double tanhValue = std::tanh((y - state.integrator) * a / (2 * kOtaThermalVoltageVolts));
                const double equivalent = tanhValue * (2 * kOtaThermalVoltageVolts) / a;
                const double alpha = pole.tptGain() / (1 + pole.tptGain());
                const double unclipped = (state.integrator + alpha * clampFinite(equivalent, -8.0, 8.0)) * traits.bufferGain;
                d *= (std::abs(equivalent) < 8 && std::abs(unclipped) < 5) ? alpha * (1 - tanhValue * tanhValue) * traits.bufferGain : 0;
                y = pole.preview(y, state).output;
            }
            if (derivative != nullptr) *derivative = d;
            return y;
        }
        double slopeBound() const noexcept {
            double result = 68000 * bias * Core::kFeedbackDivider
                * (1 - 1 / (1 + 2 * board.sampleRate_ * Core::kSharedOutputTau)) / (2 * kOtaThermalVoltageVolts);
            for (const auto& pole : board.poles_) result *= pole.tptGain() / (1 + pole.tptGain());
            return result;
        }
    };
    struct Root { double x = 0, residual = 0; int iterations = 0; bool converged = false; };
    static Equation prepare(Core board, float input) noexcept {
        board.cutoffCv_ += board.cutoffAlpha_ * (board.cutoffTarget_ - board.cutoffCv_);
        board.resonanceCv_ += board.resonanceAlpha_ * (board.resonanceTarget_ - board.resonanceCv_);
        board.vcaCv_ += board.vcaAlpha_ * (board.vcaTarget_ - board.vcaCv_);
        if (board.vcaTarget_ == 0 && board.vcaCv_ < 1.e-12) board.vcaCv_ = 0;
        if (board.cutoffCv_ != board.lastCutoffCv_) {
            const double hz = Core::cutoffHzFromCv(board.cutoffCv_);
            for (auto& pole : board.poles_) pole.setCutoffHz(hz);
            board.lastCutoffCv_ = board.cutoffCv_;
        }
        const double mixer = board.inputPole_.low(board.inputAc_.high(input * Core::kSourceVoltsPerUnit));
        const double compensation = board.compensationAc_.high(mixer) * Core::kCompensationDivider;
        const double bias = board.resonanceCv_ / 33000;
        return {board, mixer, compensation, bias};
    }
    static Root root(const Equation& equation, bool bisection = false, double tolerance = 1.e-13) noexcept {
        double lo = -8, hi = 8, x = bisection ? 0 : equation.board.poles_[3].output();
        for (int i = 1; i <= 96; ++i) {
            double derivative = 0;
            const double r = equation.evaluate(x, bisection ? nullptr : &derivative) - x;
            if (! std::isfinite(r)) return {x, r, i, false};
            // F is nonincreasing, so |x-root| <= |F(x)-x|. No small
            // residual is accepted on the basis of iteration count alone.
            if (std::abs(r) <= tolerance) return {x, r, i, true};
            if (r > 0) lo = x; else hi = x;
            double next = bisection ? (lo + hi) * .5 : x + r / (1 - derivative);
            if (! std::isfinite(next) || next <= lo || next >= hi || next == x) next = (lo + hi) * .5;
            x = next;
        }
        const double r = equation.evaluate(x) - x;
        return {x, r, 96, std::abs(r) <= tolerance};
    }
    static double fixed(const Equation& equation, int budget, double tolerance = 1.e-9) noexcept {
        double x = equation.board.poles_[3].output();
        for (int i = 0; i < budget && equation.bias > 0; ++i) {
            const double residual = equation.evaluate(x) - x;
            x += .72 * residual;
            if (std::abs(residual) < tolerance) break;
        }
        return x;
    }
    static float commit(Core& board, const Equation& equation, double estimate, double* raw = nullptr) noexcept {
        double filtered = .68 * equation.mixer + equation.feedback(estimate);
        for (auto& pole : board.poles_) filtered = pole.process(filtered);
        const double coupled = board.sharedAc_.high(filtered);
        const double vca = 10000 * (board.vcaCv_ / 33000)
            * std::tanh(coupled * Core::kVcaDivider / (2 * kOtaThermalVoltageVolts));
        const double iv = board.vcaIvPole_.low(vca);
        const double output = board.outputPole_.low(board.outputAc_.high(iv)) * (68000.0 / 33000);
        if (raw != nullptr) *raw = output;
        board.lastOutput_ = clampFinite(output / Core::kSourceVoltsPerUnit, -8.0, 8.0);
        return static_cast<float>(board.lastOutput_);
    }
    static float reference(Core& board, float input, Root* observed = nullptr, bool bisection = false, double tolerance = 1.e-13) noexcept {
        auto equation = prepare(board, input);
        const auto solution = root(equation, bisection, tolerance);
        if (observed != nullptr) *observed = solution;
        board = equation.board;
        return commit(board, equation, solution.x);
    }
    static std::array<double, 23> state(const Core& board) noexcept {
        std::array<double, 23> result{};
        size_t i = 0;
        for (const auto& pole : board.poles_) { result[i++] = pole.integrator(); result[i++] = pole.output(); }
        for (const auto* pole : {&board.inputAc_, &board.inputPole_, &board.compensationAc_, &board.sharedAc_,
                                &board.vcaIvPole_, &board.outputAc_, &board.outputPole_}) result[i++] = pole->state();
        result[i++] = board.cutoffCv_; result[i++] = board.resonanceCv_; result[i++] = board.vcaCv_;
        result[i++] = board.lastCutoffCv_; result[i++] = board.lastOutput_;
        result[i++] = board.cutoffTarget_; result[i++] = board.resonanceTarget_; result[i] = board.vcaTarget_;
        return result;
    }
    static bool rawStagesFinite(const Equation& equation, double estimate) noexcept {
        double input = .68 * equation.mixer + equation.feedback(estimate);
        for (const auto& pole : equation.board.poles_) {
            const auto s = pole.snapshot();
            const auto& t = pole.traits();
            const double a = t.differentialInputAttenuation / clampFinite(t.saturationSoftness, .5, 2.0);
            const double equivalent = std::tanh((input - s.integrator) * a / (2 * kOtaThermalVoltageVolts)) * (2 * kOtaThermalVoltageVolts) / a;
            const double v = clampFinite(equivalent, -8.0, 8.0) / (1 + pole.tptGain());
            const double y = s.integrator + pole.tptGain() * v;
            const double next = y + pole.tptGain() * v;
            if (!std::isfinite(equivalent) || !std::isfinite(y) || !std::isfinite(next)) return false;
            input = pole.preview(input, s).output;
        }
        return true;
    }
};

// Reuses the production FIR and native control delay. Fixed-budget mode must
// match SwaraXtFilter sample-for-sample before converged mode is used for A/B.
class Ir3109ReferenceFilter {
public:
    void prepare(int factor, int iterations, bool converged, bool bisection = false, double tolerance = 1.e-13) {
        factor_ = factor; budget_ = iterations; converged_ = converged;
        bisection_ = bisection; tolerance_ = tolerance;
        converter_.prepare(factor); board_.prepare((20000000.0 / 510.0) * factor);
        head_ = 0; prime_ = true; referenceFailures_ = 0; maximumResidual_ = 0;
    }
    float process(float input, double cv, float resonance, float vca) noexcept {
        const Control target {cv, resonance, vca};
        if (prime_) {
            delay_.fill({cv, resonance, 0});
            board_.setTargets(1000, resonance, 0, cv); board_.reset(); prime_ = false;
        }
        const auto control = factor_ > 1 ? delay_[head_] : target;
        delay_[head_] = target; head_ = (head_ + 1) % delay_.size();
        board_.setTargets(1000, control.resonance, control.vca, control.cv);
        return clampFinite(converter_.process(clampFinite(input, -4.0f, 4.0f), [this](float x) noexcept {
            if (!converged_) return board_.process(x, budget_);
            Ir3109SolverAccess::Root solution;
            const float y = Ir3109SolverAccess::reference(board_, x, &solution, bisection_, tolerance_);
            if (!solution.converged) ++referenceFailures_;
            maximumResidual_ = std::max(maximumResidual_, std::abs(solution.residual));
            return y;
        }), -8.0f, 8.0f);
    }
    int referenceFailures() const noexcept { return referenceFailures_; }
    double maximumResidual() const noexcept { return maximumResidual_; }
    const Ir3109BoardCore& board() const noexcept { return board_; }
private:
    struct Control { double cv; float resonance, vca; };
    Ir3109BoardCore board_;
    FilterRateConverter converter_;
    std::array<Control, FilterRateConverter::kInputDelay> delay_{};
    size_t head_ = 0;
    int factor_ = 4, budget_ = 4;
    int referenceFailures_ = 0;
    double maximumResidual_ = 0;
    double tolerance_ = 1.e-13;
    bool prime_ = true, converged_ = true, bisection_ = false;
};
}
