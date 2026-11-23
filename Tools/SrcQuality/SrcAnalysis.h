// Copyright 2026 MontroneDSP.
// SPDX-License-Identifier: GPL-3.0-or-later
//
// Offline measurement support for the Hermite / polyphase-FIR sample-rate
// conversion study. Nothing here is compiled into the plugin.

#pragma once

#include <algorithm>
#include <cmath>
#include <complex>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

namespace srcq {

constexpr double kNativeRate = 20000000.0 / 510.0;  // 39215.6862745098 Hz
constexpr double kNativeNyquist = 0.5 * kNativeRate;
constexpr int kNativeBlock = 40;
constexpr double kPi = 3.14159265358979323846;

// ---------------------------------------------------------------- conversion

// Mirrors SwaraXtEngine::process(): native samples are produced one 40-sample
// control block at a time, only when the converter needs them, and exactly one
// host sample is pulled per output frame.
template <class Converter, class Producer>
void drive(Converter& converter, Producer&& produceNative, float* out, int numHostSamples)
{
    const int target = converter.queueTargetSize();
    for (int i = 0; i < numHostSamples; ++i)
    {
        int guard = 0;
        while (converter.size() < target && guard++ < 64)
            for (int b = 0; b < kNativeBlock; ++b)
                converter.push(produceNative());

        out[i] = converter.readInterpolated();
    }
}

// Same as drive(), but the native stream is a pre-computed buffer so both
// converters provably consume identical input samples. Returns how many native
// samples were consumed.
template <class Converter>
std::size_t driveFromBuffer(Converter& converter,
                            const std::vector<float>& native,
                            std::vector<float>& out,
                            int numHostSamples)
{
    out.assign(static_cast<std::size_t>(numHostSamples), 0.0f);
    std::size_t cursor = 0;
    drive(converter,
          [&]() -> float { return cursor < native.size() ? native[cursor++] : 0.0f; },
          out.data(),
          numHostSamples);
    return cursor;
}

// ---------------------------------------------------------------------- FFT

inline void fftRadix2(std::vector<std::complex<double>>& a)
{
    const std::size_t n = a.size();
    for (std::size_t i = 1, j = 0; i < n; ++i)
    {
        std::size_t bit = n >> 1;
        for (; j & bit; bit >>= 1)
            j ^= bit;
        j ^= bit;
        if (i < j)
            std::swap(a[i], a[j]);
    }

    for (std::size_t len = 2; len <= n; len <<= 1)
    {
        const double angle = -2.0 * kPi / static_cast<double>(len);
        const std::complex<double> step(std::cos(angle), std::sin(angle));
        for (std::size_t i = 0; i < n; i += len)
        {
            std::complex<double> w(1.0, 0.0);
            for (std::size_t k = 0; k < len / 2; ++k)
            {
                const std::complex<double> u = a[i + k];
                const std::complex<double> v = a[i + k + len / 2] * w;
                a[i + k] = u + v;
                a[i + k + len / 2] = u - v;
                w *= step;
            }
        }
    }
}

inline double besselI0(double x)
{
    double sum = 1.0;
    double term = 1.0;
    const double q = 0.25 * x * x;
    for (int k = 1; k < 200; ++k)
    {
        term *= q / (static_cast<double>(k) * static_cast<double>(k));
        sum += term;
        if (term < sum * 1.0e-18)
            break;
    }
    return sum;
}

// Kaiser window with very low sidelobes so image peaks 100+ dB down are still
// measurable.
inline std::vector<double> kaiserWindow(std::size_t n, double beta)
{
    std::vector<double> w(n);
    const double denominator = besselI0(beta);
    const double half = 0.5 * static_cast<double>(n - 1);
    for (std::size_t i = 0; i < n; ++i)
    {
        const double r = (static_cast<double>(i) - half) / half;
        const double inner = 1.0 - r * r;
        w[i] = besselI0(beta * std::sqrt(inner > 0.0 ? inner : 0.0)) / denominator;
    }
    return w;
}

struct Spectrum {
    std::vector<double> power;   // |X[k]|^2 for k = 0 .. n/2
    double windowEnergy = 0.0;   // sum(w^2)
    std::size_t fftSize = 0;
    double sampleRate = 0.0;

    double binHz() const { return sampleRate / static_cast<double>(fftSize); }
    std::size_t binOf(double hz) const
    {
        const double b = hz / binHz();
        return static_cast<std::size_t>(std::lround(b));
    }

    // Amplitude of a sinusoid whose main lobe is centred in [lo, hi].
    double amplitudeInBins(std::size_t lo, std::size_t hi) const
    {
        if (lo > hi || hi >= power.size())
            return 0.0;
        double energy = 0.0;
        for (std::size_t k = lo; k <= hi; ++k)
            energy += power[k];
        const double denominator = static_cast<double>(fftSize) * windowEnergy;
        return denominator > 0.0 ? 2.0 * std::sqrt(energy / denominator) : 0.0;
    }

    double amplitudeAt(double hz, int halfWidthBins) const
    {
        const long centre = static_cast<long>(std::lround(hz / binHz()));
        const long lo = std::max<long>(0, centre - halfWidthBins);
        const long hi = std::min<long>(static_cast<long>(power.size()) - 1, centre + halfWidthBins);
        if (lo > hi)
            return 0.0;
        return amplitudeInBins(static_cast<std::size_t>(lo), static_cast<std::size_t>(hi));
    }
};

inline Spectrum analyse(const float* x, std::size_t n, double sampleRate, double beta = 20.0)
{
    std::size_t fftSize = 1;
    while (fftSize * 2 <= n)
        fftSize *= 2;

    const std::vector<double> w = kaiserWindow(fftSize, beta);
    std::vector<std::complex<double>> buffer(fftSize);
    double windowEnergy = 0.0;
    for (std::size_t i = 0; i < fftSize; ++i)
    {
        buffer[i] = std::complex<double>(static_cast<double>(x[i]) * w[i], 0.0);
        windowEnergy += w[i] * w[i];
    }

    fftRadix2(buffer);

    Spectrum s;
    s.fftSize = fftSize;
    s.sampleRate = sampleRate;
    s.windowEnergy = windowEnergy;
    s.power.resize(fftSize / 2 + 1);
    for (std::size_t k = 0; k < s.power.size(); ++k)
        s.power[k] = std::norm(buffer[k]);
    return s;
}

struct ToneMetrics {
    double amplitude = 0.0;
    double passbandErrorDb = 0.0;
    double measuredHz = 0.0;
    double frequencyErrorHz = 0.0;
    double worstImageDbc = -200.0;
    double worstImageHz = 0.0;
    double thdPlusNDbc = -200.0;
    double rms = 0.0;
    double peak = 0.0;
    double dcMean = 0.0;
};

// The native input is a mathematically exact sine below native Nyquist, so any
// non-fundamental energy in the host stream was created by the converter.
inline ToneMetrics measureTone(const std::vector<float>& host,
                               double hostRate,
                               double toneHz,
                               double inputAmplitude)
{
    ToneMetrics m;

    double sum = 0.0;
    double sumSquares = 0.0;
    double peak = 0.0;
    for (float v : host)
    {
        sum += v;
        sumSquares += static_cast<double>(v) * v;
        peak = std::max(peak, std::abs(static_cast<double>(v)));
    }
    m.dcMean = host.empty() ? 0.0 : sum / static_cast<double>(host.size());
    m.rms = host.empty() ? 0.0 : std::sqrt(sumSquares / static_cast<double>(host.size()));
    m.peak = peak;

    const Spectrum s = analyse(host.data(), host.size(), hostRate);
    constexpr int kLobe = 24;  // Kaiser beta=20 main lobe is ~13 bins wide

    const long centre = static_cast<long>(std::lround(toneHz / s.binHz()));
    const long lo = std::max<long>(1, centre - kLobe);
    const long hi = std::min<long>(static_cast<long>(s.power.size()) - 1, centre + kLobe);

    m.amplitude = s.amplitudeInBins(static_cast<std::size_t>(lo), static_cast<std::size_t>(hi));
    m.passbandErrorDb = (m.amplitude > 0.0 && inputAmplitude > 0.0)
        ? 20.0 * std::log10(m.amplitude / inputAmplitude)
        : -200.0;

    // Energy-weighted centroid gives sub-bin frequency accuracy.
    double weight = 0.0;
    double weighted = 0.0;
    for (long k = lo; k <= hi; ++k)
    {
        weight += s.power[static_cast<std::size_t>(k)];
        weighted += s.power[static_cast<std::size_t>(k)] * static_cast<double>(k);
    }
    m.measuredHz = weight > 0.0 ? (weighted / weight) * s.binHz() : 0.0;
    m.frequencyErrorHz = m.measuredHz - toneHz;

    // Worst residual peak outside DC and the fundamental lobe.
    const long dcGuard = kLobe;
    double fundamentalEnergy = 0.0;
    for (long k = lo; k <= hi; ++k)
        fundamentalEnergy += s.power[static_cast<std::size_t>(k)];

    double residualEnergy = 0.0;
    double worstPower = 0.0;
    long worstBin = 0;
    for (long k = dcGuard; k < static_cast<long>(s.power.size()); ++k)
    {
        if (k >= lo && k <= hi)
            continue;
        residualEnergy += s.power[static_cast<std::size_t>(k)];
        if (s.power[static_cast<std::size_t>(k)] > worstPower)
        {
            worstPower = s.power[static_cast<std::size_t>(k)];
            worstBin = k;
        }
    }

    if (worstPower > 0.0)
    {
        const long wlo = std::max<long>(dcGuard, worstBin - kLobe);
        const long whi = std::min<long>(static_cast<long>(s.power.size()) - 1, worstBin + kLobe);
        const double imageAmplitude =
            s.amplitudeInBins(static_cast<std::size_t>(wlo), static_cast<std::size_t>(whi));
        m.worstImageHz = static_cast<double>(worstBin) * s.binHz();
        m.worstImageDbc = (imageAmplitude > 0.0 && m.amplitude > 0.0)
            ? 20.0 * std::log10(imageAmplitude / m.amplitude)
            : -200.0;
    }

    m.thdPlusNDbc = (fundamentalEnergy > 0.0 && residualEnergy > 0.0)
        ? 10.0 * std::log10(residualEnergy / fundamentalEnergy)
        : -200.0;

    return m;
}

// ------------------------------------------------------------------- signals

inline std::vector<float> nativeSine(double hz, double amplitude, std::size_t n, double phase = 0.0)
{
    std::vector<float> x(n);
    for (std::size_t i = 0; i < n; ++i)
        x[i] = static_cast<float>(amplitude * std::sin(2.0 * kPi * hz * static_cast<double>(i) / kNativeRate + phase));
    return x;
}

inline std::vector<float> nativeImpulse(std::size_t n, std::size_t position, float amplitude = 1.0f)
{
    std::vector<float> x(n, 0.0f);
    if (position < n)
        x[position] = amplitude;
    return x;
}

inline std::vector<float> nativeStep(std::size_t n, std::size_t position, float amplitude = 1.0f)
{
    std::vector<float> x(n, 0.0f);
    for (std::size_t i = position; i < n; ++i)
        x[i] = amplitude;
    return x;
}

// Deterministic broadband source (xorshift32, fixed seed) band-limited only by
// the native rate itself.
inline std::vector<float> nativeNoise(std::size_t n, uint32_t seed = 0x5772A11u, float amplitude = 0.5f)
{
    std::vector<float> x(n);
    uint32_t state = seed;
    for (std::size_t i = 0; i < n; ++i)
    {
        state ^= state << 13;
        state ^= state >> 17;
        state ^= state << 5;
        x[i] = amplitude * (static_cast<float>(state) / 2147483648.0f - 1.0f);
    }
    return x;
}

// ---------------------------------------------------------------- WAV output

inline bool writeWavFloat32(const std::string& path,
                            const std::vector<float>& mono,
                            double sampleRate)
{
    FILE* f = std::fopen(path.c_str(), "wb");
    if (f == nullptr)
        return false;

    const uint32_t dataBytes = static_cast<uint32_t>(mono.size() * sizeof(float));
    const uint32_t rate = static_cast<uint32_t>(std::lround(sampleRate));
    const uint16_t channels = 1;
    const uint16_t bits = 32;
    const uint32_t byteRate = rate * channels * bits / 8;
    const uint16_t blockAlign = static_cast<uint16_t>(channels * bits / 8);

    auto u32 = [&](uint32_t v) { std::fwrite(&v, 4, 1, f); };
    auto u16 = [&](uint16_t v) { std::fwrite(&v, 2, 1, f); };

    std::fwrite("RIFF", 1, 4, f);
    u32(36 + dataBytes);
    std::fwrite("WAVE", 1, 4, f);
    std::fwrite("fmt ", 1, 4, f);
    u32(16);
    u16(3);  // IEEE float
    u16(channels);
    u32(rate);
    u32(byteRate);
    u16(blockAlign);
    u16(bits);
    std::fwrite("data", 1, 4, f);
    u32(dataBytes);
    std::fwrite(mono.data(), 1, dataBytes, f);
    std::fclose(f);
    return true;
}

inline bool writeRawFloat32(const std::string& path, const std::vector<float>& mono)
{
    FILE* f = std::fopen(path.c_str(), "wb");
    if (f == nullptr)
        return false;
    std::fwrite(mono.data(), sizeof(float), mono.size(), f);
    std::fclose(f);
    return true;
}

}  // namespace srcq
