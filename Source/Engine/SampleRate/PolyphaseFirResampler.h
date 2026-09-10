// Copyright 2026 MontroneDSP.
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include <array>
#include <cmath>
#include <cstddef>
#include <memory>
#include <algorithm>

namespace swaraxt {

// Band-limited polyphase reconstruction filter for the native -> host rate
// conversion.
//
// The source clock stays at 20 MHz / 510. For hosts below that rate, setStep()
// designs a lower cutoff with an explicit Kaiser transition before host Nyquist.
// Rate configuration belongs to prepare; unchanged-rate resets/wakes reuse the
// kernel without allocating or redesigning. Production uses 256 taps/phases:
// sampled passband error <0.001 dB through 19 kHz at/above native Fs. Below
// native, passband ends at host/2 - 8*native/256; stopband starts at host/2
// (sampled coefficient rejection >95 dB). HostSrcTests checks streamed images.
// These are bounded test specifications, not universal near-Nyquist claims.
//
// The interface mirrors InternalSampleQueue so the engine can hold either
// converter behind the same push/read protocol.
template <int TapsPerPhase, int PhaseCount, bool InterpolatePhase = true>
class PolyphaseFirResampler {
 public:
    static constexpr int kTaps = TapsPerPhase;
    static constexpr int kPhases = PhaseCount;
    // One extra row so both nearest-phase rounding and linear phase blending
    // can index kPhases without a wrap test in the inner loop.
    static constexpr int kRows = PhaseCount + 1;
    static constexpr int kCapacity = 16384;
    static constexpr int kCapacityMask = kCapacity - 1;

    static_assert(TapsPerPhase >= 8 && TapsPerPhase % 2 == 0,
                  "an even tap count keeps the kernel centred between two input samples");
    static_assert(PhaseCount >= 8 && (PhaseCount & (PhaseCount - 1)) == 0,
                  "a power-of-two phase count keeps the phase index cheap");
    static_assert(TapsPerPhase < kCapacity / 2, "kernel must fit the history ring");

    // stopbandDb drives the Kaiser window; cutoffNyquist is in units of native
    // Nyquist (1.0 = 19607.84 Hz).
    explicit PolyphaseFirResampler(double stopbandDb = 100.0, double cutoffNyquist = 1.0)
        : stopbandDb_(stopbandDb), requestedCutoff_(cutoffNyquist)
    {
        design(stopbandDb_, requestedCutoff_);
    }

    void reset() noexcept;
    void setStep(double internalRate, double hostRate) noexcept;
    void push(float sample) noexcept;
    // Grows with the whole-sample phase debt, exactly like the Hermite reader.
    // Once the queue can no longer satisfy this, the caller knows the stream
    // has run dry and can shut the converter down instead of spinning on a
    // permanently minimal queue.
    int minimumReadableSize() const noexcept;
    // Must stay above minimumReadableSize(): the read drains down to the
    // minimum, so a target equal to it would stall the producer.
    int queueTargetSize() const noexcept { return kTaps + 8; }
    float readInterpolated() noexcept;
    int size() const noexcept { return size_; }

    // Causal streaming delay in native samples, including startup. The engine
    // must report it to the host rather than concealing it with future audio.
    static constexpr double groupDelayNativeSamples() noexcept
    {
        return 0.5 * static_cast<double>(kTaps);
    }

    static constexpr std::size_t coefficientBytes() noexcept
    {
        return sizeof(float) * static_cast<std::size_t>(kRows) * static_cast<std::size_t>(kTaps);
    }

    static constexpr std::size_t historyBytes() noexcept
    {
        return sizeof(float) * static_cast<std::size_t>(kCapacity);
    }

    const float* coefficientRow(int phase) const noexcept
    {
        return coeff_->data() + static_cast<std::size_t>(phase) * static_cast<std::size_t>(kTaps);
    }
    double cutoffNyquist() const noexcept { return designedCutoff_; }
    unsigned kernelDesignCount() const noexcept { return kernelDesignCount_; }

 private:
    void design(double stopbandDb, double cutoffNyquist);
    static double besselI0(double x) noexcept;

    float at(int index) const noexcept
    {
        return data_[static_cast<std::size_t>((readIndex_ + index) & kCapacityMask)];
    }

    using Coefficients = std::array<float, static_cast<std::size_t>(kRows) * static_cast<std::size_t>(TapsPerPhase)>;
    std::unique_ptr<Coefficients> coeff_ = std::make_unique<Coefficients>();
    std::array<float, kCapacity> data_ {};
    int readIndex_ = 0;
    int writeIndex_ = 0;
    int size_ = 0;
    double fraction_ = 0.0;
    double step_ = 1.0;
    double stopbandDb_ = 100, requestedCutoff_ = 1, designedCutoff_ = 1;
    unsigned kernelDesignCount_ = 0;
};

template <int T, int P, bool I>
double PolyphaseFirResampler<T, P, I>::besselI0(double x) noexcept
{
    // Series expansion; converges quickly for the Kaiser beta range we use.
    double sum = 1.0;
    double term = 1.0;
    const double halfSquared = 0.25 * x * x;
    for (int k = 1; k < 64; ++k)
    {
        term *= halfSquared / (static_cast<double>(k) * static_cast<double>(k));
        sum += term;
        if (term < sum * 1.0e-18)
            break;
    }
    return sum;
}

template <int T, int P, bool I>
void PolyphaseFirResampler<T, P, I>::design(double stopbandDb, double cutoffNyquist)
{
    designedCutoff_ = cutoffNyquist;
    ++kernelDesignCount_;
    constexpr double kResamplerPi = 3.14159265358979323846;

    double beta = 0.0;
    if (stopbandDb > 50.0)
        beta = 0.1102 * (stopbandDb - 8.7);
    else if (stopbandDb >= 21.0)
        beta = 0.5842 * std::pow(stopbandDb - 21.0, 0.4) + 0.07886 * (stopbandDb - 21.0);

    const double normalisation = besselI0(beta);
    const double halfWidth = 0.5 * static_cast<double>(kTaps);
    // Kernel centre sits between taps kTaps/2-1 and kTaps/2, so a fractional
    // phase in [0, 1) never leaves the tap span.
    const double centre = static_cast<double>(kTaps) * 0.5 - 1.0;

    for (int phase = 0; phase < kRows; ++phase)
    {
        const double fraction = static_cast<double>(phase) / static_cast<double>(kPhases);
        double sum = 0.0;
        float* row = coeff_->data() + static_cast<std::size_t>(phase) * static_cast<std::size_t>(kTaps);

        for (int tap = 0; tap < kTaps; ++tap)
        {
            const double x = centre + fraction - static_cast<double>(tap);
            const double scaled = cutoffNyquist * x;
            const double sinc = std::abs(scaled) < 1.0e-12
                ? 1.0
                : std::sin(kResamplerPi * scaled) / (kResamplerPi * scaled);

            const double ratio = x / halfWidth;
            const double window = std::abs(ratio) >= 1.0
                ? 0.0
                : besselI0(beta * std::sqrt(1.0 - ratio * ratio)) / normalisation;

            const double value = cutoffNyquist * sinc * window;
            row[tap] = static_cast<float>(value);
            sum += value;
        }

        // Force unity DC gain per phase. A linear blend of two unity-gain rows
        // is still unity gain, so phase interpolation cannot introduce ripple
        // at DC.
        const double scale = std::abs(sum) > 1.0e-12 ? 1.0 / sum : 1.0;
        for (int tap = 0; tap < kTaps; ++tap)
            row[tap] = static_cast<float>(static_cast<double>(row[tap]) * scale);
    }
}

template <int T, int P, bool I>
void PolyphaseFirResampler<T, P, I>::reset() noexcept
{
    data_.fill(0.0f);
    readIndex_ = 0;
    writeIndex_ = 0;
    size_ = 0;
    fraction_ = 0.0;

    // Silent causal history preserves the complete attack and gives the same
    // T/2 delay at startup and during streaming. No unreported lookahead.
    for (int i = 0; i < kTaps - 1; ++i)
        push(0.0f);
}

template <int T, int P, bool I>
void PolyphaseFirResampler<T, P, I>::setStep(double internalRate, double hostRate) noexcept
{
    const double safeHost = std::isfinite(hostRate) && hostRate > 1.0 ? hostRate : 44100.0;
    const double safeInternal = std::isfinite(internalRate) && internalRate > 1.0 ? internalRate : safeHost;
    // A half-transition of 4 Fs/T places the 100 dB Kaiser stopband before
    // the lower output Nyquist. Production T=256 supports hosts >= 8 kHz.
    const double outputCutoff = safeHost < safeInternal
        ? std::max(0.001, safeHost / safeInternal - 8.0 / kTaps) : 1.0;
    const double desiredCutoff = std::min(requestedCutoff_, outputCutoff);
    if (desiredCutoff != designedCutoff_) design(stopbandDb_, desiredCutoff);
    step_ = safeInternal / safeHost;
    if (! std::isfinite(step_) || step_ <= 0.0)
        step_ = 1.0;
    if (! std::isfinite(fraction_) || fraction_ < 0.0)
        fraction_ = 0.0;
    while (fraction_ >= 1.0)
        fraction_ -= 1.0;
}

template <int T, int P, bool I>
void PolyphaseFirResampler<T, P, I>::push(float sample) noexcept
{
    if (size_ >= kCapacity - kTaps)
    {
        readIndex_ = (readIndex_ + 1) & kCapacityMask;
        --size_;
    }

    data_[static_cast<std::size_t>(writeIndex_)] = std::isfinite(sample) ? sample : 0.0f;
    writeIndex_ = (writeIndex_ + 1) & kCapacityMask;
    ++size_;
}

template <int T, int P, bool I>
int PolyphaseFirResampler<T, P, I>::minimumReadableSize() const noexcept
{
    if (! std::isfinite(fraction_) || fraction_ <= 0.0)
        return kTaps;

    return kTaps + static_cast<int>(std::floor(fraction_));
}

template <int T, int P, bool I>
float PolyphaseFirResampler<T, P, I>::readInterpolated() noexcept
{
    if (size_ < kTaps)
        return 0.0f;

    const int maxAdvances = 2 + static_cast<int>(std::ceil(step_)) + 8;
    int advances = 0;
    while (fraction_ >= 1.0 && size_ > kTaps && advances < maxAdvances)
    {
        fraction_ -= 1.0;
        readIndex_ = (readIndex_ + 1) & kCapacityMask;
        --size_;
        ++advances;
    }
    if (fraction_ >= 1.0 || size_ < kTaps)
        return 0.0f;

    const double phasePosition = fraction_ * static_cast<double>(kPhases);
    float accumulator = 0.0f;

    if constexpr (I)
    {
        const int phase = static_cast<int>(phasePosition);
        const float mu = static_cast<float>(phasePosition - static_cast<double>(phase));
        const float* lower = coefficientRow(phase);
        const float* upper = lower + kTaps;
        for (int tap = 0; tap < kTaps; ++tap)
            accumulator += at(tap) * (lower[tap] + mu * (upper[tap] - lower[tap]));
    }
    else
    {
        const int phase = static_cast<int>(phasePosition + 0.5);
        const float* row = coefficientRow(phase);
        for (int tap = 0; tap < kTaps; ++tap)
            accumulator += at(tap) * row[tap];
    }

    fraction_ += step_;
    return std::isfinite(accumulator) ? accumulator : 0.0f;
}

}  // namespace swaraxt
