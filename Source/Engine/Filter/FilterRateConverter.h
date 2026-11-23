// Copyright 2026 MontroneDSP. SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <memory>

namespace swaraxt {

// Linear-phase interpolation/decimation around a nonlinear native-rate stage.
// A 256-native-sample, Kaiser beta=10.06126 sinc is used in each direction.
// Its centre is native Nyquist; the transition straddles that frequency.
// The measured response, rather than beta alone, specifies usable rejection.
// Both filters together delay audio by 256 native samples, independent of L.
class FilterRateConverter {
public:
    static constexpr int kSpan = 256;
    static constexpr int kInputDelay = kSpan / 2;
    static constexpr int kLatency = kSpan;
    static constexpr int kMaxFactor = 8;
    static constexpr int kMaxTaps = kSpan * kMaxFactor + 1;

    void prepare(int factor) noexcept {
        factor_ = factor <= 1 ? 1 : factor <= 2 ? 2 : factor <= 4 ? 4 : 8;
        // Materialize all immutable kernels during prepare, so subsequent
        // quality switches have neither allocation nor coefficient design.
        const auto& bank = kernels();
        kernel_ = &bank[static_cast<size_t>(factor_ == 2 ? 0 : factor_ == 4 ? 1 : 2)];
        reset();
    }
    void reset() noexcept { history_->input.fill(0); history_->output.fill(0); inputHead_ = outputHead_ = 0; }
    int latency() const noexcept { return factor_ == 1 ? 0 : kLatency; }
#if SWARAXT_ENABLE_SHRUTHI_DEBUG_TAPS
    double coefficientForTests(int tap) const noexcept { return (*kernel_)[static_cast<size_t>(tap)]; }
#endif

    template<class Process>
    float process(float input, Process&& nonlinear) noexcept {
        if (factor_ == 1) return nonlinear(input);
        if (--inputHead_ < 0) inputHead_ = kSpan;
        auto& inputHistory = history_->input;
        auto& outputHistory = history_->output;
        inputHistory[static_cast<size_t>(inputHead_)] = input;
        inputHistory[static_cast<size_t>(inputHead_ + kSpan + 1)] = input;
        double result = 0;
        const int taps = factor_ * kSpan + 1;
        for (int phase = 0; phase < factor_; ++phase) {
            double interpolated = 0;
            if (phase == 0) {
                // Integer sinc zeros make this phase a single delayed sample.
                interpolated = inputHistory[static_cast<size_t>(inputHead_ + kInputDelay)]
                    * (*kernel_)[static_cast<size_t>((taps - 1) / 2)];
            } else {
                for (int j = 0, tap = phase; tap < taps; ++j, tap += factor_)
                    interpolated += inputHistory[static_cast<size_t>(inputHead_ + j)] * (*kernel_)[static_cast<size_t>(tap)];
            }
            const double processed = nonlinear(static_cast<float>(interpolated * factor_));
            if (--outputHead_ < 0) outputHead_ = taps - 1;
            outputHistory[static_cast<size_t>(outputHead_)] = processed;
            outputHistory[static_cast<size_t>(outputHead_ + taps)] = processed;
            // Phase zero makes the total delay an integer native sample count.
            if (phase == 0)
                for (int tap = 0; tap < taps; ++tap)
                    result += outputHistory[static_cast<size_t>(outputHead_ + tap)] * (*kernel_)[static_cast<size_t>(tap)];
        }
        return static_cast<float>(result);
    }
private:
    using Kernel = std::array<double, kMaxTaps>;
    static double bessel0(double x) noexcept {
        double sum = 1, term = 1;
        for (int k = 1; k < 48; ++k) { term *= x * x / (4.0 * k * k); sum += term; }
        return sum;
    }
    static const std::array<Kernel, 3>& kernels() noexcept {
        static const auto bank = [] {
            std::array<Kernel, 3> result{};
            constexpr double pi = 3.14159265358979323846, beta = 10.06126;
            const double norm = bessel0(beta);
            for (int index = 0, factor = 2; index < 3; ++index, factor *= 2) {
                auto& h = result[static_cast<size_t>(index)];
                const int length = factor * kSpan, centre = length / 2;
                double sum = 0;
                for (int tap = 0; tap <= length; ++tap) {
                    const double x = static_cast<double>(tap - centre) / factor;
                    const double position = static_cast<double>(tap - centre) / centre;
                    const double sinc = tap == centre ? 1.0 : (tap - centre) % factor == 0 ? 0.0 : std::sin(pi * x) / (pi * x);
                    h[static_cast<size_t>(tap)] = sinc / factor * bessel0(beta * std::sqrt(std::max(0.0, 1.0 - position * position))) / norm;
                    sum += h[static_cast<size_t>(tap)];
                }
                for (auto& coefficient : h) coefficient /= sum;
            }
            return result;
        }();
        return bank;
    }
    const Kernel* kernel_ = nullptr;
    struct History {
        std::array<double, 2 * (kSpan + 1)> input{};
        std::array<double, 2 * kMaxTaps> output{};
    };
    // Allocate once at object construction, never on prepare/reset/process or
    // quality change. Several processors can otherwise exhaust a host's stack.
    std::unique_ptr<History> history_ = std::make_unique<History>();
    int factor_ = 1, inputHead_ = 0, outputHead_ = 0;
};
} // namespace swaraxt
