// Copyright 2026 MontroneDSP.
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include <array>
#include <cmath>
#include <cstddef>

namespace swaraxt {

// Band-limited polyphase reconstruction filter for the native -> host rate
// conversion.
//
// The Shruthi core always runs at 20 MHz / 510 (~39.2156 kHz) and every host
// rate Swara XT supports is at or above 44.1 kHz, so this converter is only
// ever an interpolator. That makes the anti-imaging cutoff a constant: native
// Nyquist. The coefficient table is therefore host-rate independent and is
// built once at construction; setStep() only changes the phase increment.
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
    {
        design(stopbandDb, cutoffNyquist);
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

    // Delay from the newest queued input sample to the sample the next read
    // will reconstruct, in native samples. This is the converter's group delay.
    static constexpr double groupDelayNativeSamples() noexcept
    {
        return 0.5 * static_cast<double>(kTaps - 1);
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
        return &coeff_[static_cast<std::size_t>(phase) * static_cast<std::size_t>(kTaps)];
    }

 private:
    void design(double stopbandDb, double cutoffNyquist);
    static double besselI0(double x) noexcept;

    float at(int index) const noexcept
    {
        return data_[static_cast<std::size_t>((readIndex_ + index) & kCapacityMask)];
    }

    std::array<float, static_cast<std::size_t>(kRows) * static_cast<std::size_t>(TapsPerPhase)> coeff_ {};
    std::array<float, kCapacity> data_ {};
    int readIndex_ = 0;
    int writeIndex_ = 0;
    int size_ = 0;
    double fraction_ = 0.0;
    double step_ = 1.0;
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
        float* row = &coeff_[static_cast<std::size_t>(phase) * static_cast<std::size_t>(kTaps)];

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

    // The kernel is centred kTaps/2 - 1 samples into its window. Priming that
    // much silence makes the first output sample reconstruct the first input
    // sample rather than skipping past it, so a note-on is neither delayed nor
    // clipped of its attack, and the converter's own group delay never appears
    // at the plugin output.
    for (int i = 0; i < kTaps / 2 - 1; ++i)
        push(0.0f);
}

template <int T, int P, bool I>
void PolyphaseFirResampler<T, P, I>::setStep(double internalRate, double hostRate) noexcept
{
    const double safeHost = hostRate > 1.0 ? hostRate : 44100.0;
    const double safeInternal = internalRate > 1.0 ? internalRate : safeHost;
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
