// Copyright 2026 MontroneDSP.
// SPDX-License-Identifier: GPL-3.0-or-later

#include "SonicQuality/SonicQualityMetrics.h"

#include "SonicQuality/SonicQualityThresholds.h"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <fstream>
#include <limits>
#include <numeric>
#include <sstream>

namespace swaraxt::sonic {
namespace {

double windowWeight(AnalysisWindow window, int index, int count) noexcept
{
    if (window == AnalysisWindow::rectangular || count <= 1)
        return 1.0;
    return 0.5 - 0.5 * std::cos(2.0 * kPi * static_cast<double>(index)
                                / static_cast<double>(count - 1));
}

double blackman(int index, int taps) noexcept
{
    if (taps <= 0)
        return 1.0;
    const double x = static_cast<double>(index) / static_cast<double>(taps);
    return 0.42 + 0.5 * std::cos(kPi * x) + 0.08 * std::cos(2.0 * kPi * x);
}

double sinc(double x) noexcept
{
    if (std::abs(x) < 1.0e-12)
        return 1.0;
    const double p = kPi * x;
    return std::sin(p) / p;
}

void fail(int& failures, std::ostringstream& detail, const char* message)
{
    ++failures;
    detail << "FAIL " << message << '\n';
}

}  // namespace

double db(double amplitude) noexcept
{
    return 20.0 * std::log10(std::max(std::abs(amplitude), 1.0e-15));
}

double foldFrequency(double frequencyHz, double sampleRate) noexcept
{
    if (!(sampleRate > 0.0) || !std::isfinite(frequencyHz))
        return 0.0;
    double wrapped = std::fmod(std::abs(frequencyHz), sampleRate);
    if (wrapped > sampleRate * 0.5)
        wrapped = sampleRate - wrapped;
    return wrapped;
}

int coherentLength(double sampleRate, double frequencyHz, int minimumSamples) noexcept
{
    if (!(sampleRate > 0.0) || !(frequencyHz > 0.0) || minimumSamples < 16)
        return std::max(16, minimumSamples);
    const double period = sampleRate / frequencyHz;
    const int periods = std::max(4, static_cast<int>(std::ceil(minimumSamples / period)));
    const auto length = static_cast<int>(std::llround(static_cast<double>(periods) * period));
    return std::max(16, length);
}

bool nearlyIntegerCycles(double sampleRate, double frequencyHz, int count) noexcept
{
    if (!(sampleRate > 0.0) || !(frequencyHz > 0.0) || count < 16)
        return false;
    const double cycles = frequencyHz * static_cast<double>(count) / sampleRate;
    return std::abs(cycles - std::round(cycles)) < 1.0e-6;
}

LevelStats measureLevels(const float* samples, int count)
{
    LevelStats stats;
    if (samples == nullptr || count <= 0)
        return stats;
    stats.samples = count;
    std::vector<double> steps;
    steps.reserve(static_cast<size_t>(std::max(0, count - 1)));
    double sum = 0.0;
    double energy = 0.0;
    int run = 0;
    for (int i = 0; i < count; ++i)
    {
        const float sample = samples[i];
        if (!std::isfinite(sample))
        {
            ++stats.nonfinite;
            run = 0;
            continue;
        }
        const double value = static_cast<double>(sample);
        const double magnitude = std::abs(value);
        stats.samplePeak = std::max(stats.samplePeak, magnitude);
        if (magnitude > 1.0)
            ++stats.aboveUnity;
        if (magnitude > static_cast<double>(kBoardSafetyLimit))
            ++stats.aboveBoardLimit;
        if (magnitude > static_cast<double>(kUnboundedLimit))
            ++stats.aboveUnbounded;
        if (magnitude >= 0.999)
        {
            ++stats.nearFullScale;
            ++run;
            stats.longestNearFullScaleRun = std::max(stats.longestNearFullScaleRun, run);
        }
        else
        {
            run = 0;
        }
        sum += value;
        energy += value * value;
        if (i > 0 && std::isfinite(samples[i - 1]))
            steps.push_back(std::abs(value - static_cast<double>(samples[i - 1])));
    }
    const double finiteCount = static_cast<double>(count - stats.nonfinite);
    if (finiteCount > 0.0)
    {
        stats.mean = sum / finiteCount;
        stats.rms = std::sqrt(energy / finiteCount);
    }
    if (stats.rms > 1.0e-15 && stats.samplePeak > 0.0)
        stats.crestDb = db(stats.samplePeak) - db(stats.rms);
    stats.maxAdjacentDifference = 0.0;
    for (double step : steps)
        stats.maxAdjacentDifference = std::max(stats.maxAdjacentDifference, step);
    if (!steps.empty())
    {
        const auto middle = steps.begin() + static_cast<std::ptrdiff_t>(steps.size() / 2);
        std::nth_element(steps.begin(), middle, steps.end());
        stats.medianAdjacentDifference = *middle;
    }
    return stats;
}

ToneComponent projectTone(const float* samples, int count, double sampleRate,
                          double frequencyHz, AnalysisWindow window)
{
    ToneComponent tone;
    tone.frequencyHz = frequencyHz;
    if (samples == nullptr || count < 16 || !(sampleRate > 0.0) || !std::isfinite(frequencyHz))
        return tone;
    if (frequencyHz < 0.0 || frequencyHz > sampleRate * 0.5)
        return tone;
    double real = 0.0;
    double imag = 0.0;
    double weight = 0.0;
    const double omega = 2.0 * kPi * frequencyHz / sampleRate;
    for (int i = 0; i < count; ++i)
    {
        if (!std::isfinite(samples[i]))
            continue;
        const double w = windowWeight(window, i, count);
        const double x = static_cast<double>(samples[i]) * w;
        real += x * std::cos(omega * static_cast<double>(i));
        imag -= x * std::sin(omega * static_cast<double>(i));
        weight += w;
    }
    if (!(weight > 0.0))
        return tone;
    const double magnitude = std::hypot(real, imag);
    const bool nyquistOrDc = frequencyHz < 1.0e-6
        || std::abs(frequencyHz - sampleRate * 0.5) < 1.0e-3;
    tone.peakAmplitude = nyquistOrDc ? magnitude / weight : 2.0 * magnitude / weight;
    tone.phaseRadians = std::atan2(imag, real);
    tone.measurable = std::isfinite(tone.peakAmplitude);
    return tone;
}

DistortionStats measureDistortion(const float* samples, int count, double sampleRate,
                                  double fundamentalHz, AnalysisWindow window)
{
    DistortionStats stats;
    stats.window = window;
    stats.coherent = nearlyIntegerCycles(sampleRate, fundamentalHz, count)
        && window == AnalysisWindow::rectangular;
    stats.fundamental = projectTone(samples, count, sampleRate, fundamentalHz, window);
    if (!stats.fundamental.measurable || stats.fundamental.peakAmplitude < 1.0e-15)
        return stats;
    double harmonicPower = 0.0;
    int stored = 0;
    for (int harmonic = 2; harmonic <= 12; ++harmonic)
    {
        const double raw = fundamentalHz * static_cast<double>(harmonic);
        const double folded = foldFrequency(raw, sampleRate);
        if (folded < 1.0 || folded >= sampleRate * 0.49)
            continue;
        if (std::abs(folded - fundamentalHz) < std::max(1.0, sampleRate / static_cast<double>(count)))
            continue;
        const auto partial = projectTone(samples, count, sampleRate, folded, window);
        if (!partial.measurable)
            continue;
        harmonicPower += partial.peakAmplitude * partial.peakAmplitude;
        if (stored < 4)
            stats.harmonicPeak[stored++] = partial.peakAmplitude;
        const bool aliased = raw >= sampleRate * 0.5;
        if (aliased && partial.peakAmplitude > stats.aliasPeak)
        {
            stats.aliasPeak = partial.peakAmplitude;
            stats.aliasFrequencyHz = folded;
        }
    }
    stats.thd = std::sqrt(harmonicPower) / stats.fundamental.peakAmplitude;
    if (stats.coherent)
    {
        const auto levels = measureLevels(samples, count);
        const double ac = std::max(0.0, levels.rms * levels.rms - levels.mean * levels.mean);
        const double fundamentalRms = stats.fundamental.peakAmplitude / std::sqrt(2.0);
        const double residual = std::max(0.0, ac - fundamentalRms * fundamentalRms);
        stats.thdN = std::sqrt(residual) / std::max(fundamentalRms, 1.0e-15);
        stats.thdNValid = std::isfinite(stats.thdN);
    }
    return stats;
}

TruePeakStats measureTruePeak(const float* samples, int count)
{
    TruePeakStats stats;
    stats.factor = 4;
    stats.taps = 48;
    if (samples == nullptr || count <= 0)
        return stats;
    const auto levels = measureLevels(samples, count);
    stats.samplePeak = levels.samplePeak;
    if (count < stats.taps * 2 + 8)
    {
        stats.truePeak = stats.samplePeak;
        return stats;
    }
    const int begin = stats.taps;
    const int end = count - stats.taps - 1;
    double peak = stats.samplePeak;
    for (int i = begin; i < end; ++i)
    {
        for (int phase = 0; phase < stats.factor; ++phase)
        {
            const double fraction = static_cast<double>(phase) / static_cast<double>(stats.factor);
            double sum = 0.0;
            double weight = 0.0;
            for (int tap = -stats.taps; tap <= stats.taps; ++tap)
            {
                const double kernel = sinc(static_cast<double>(tap) - fraction) * blackman(tap, stats.taps);
                const float sample = samples[i - tap];
                if (!std::isfinite(sample))
                    continue;
                sum += static_cast<double>(sample) * kernel;
                weight += kernel;
            }
            if (std::abs(weight) > 1.0e-12)
                peak = std::max(peak, std::abs(sum / weight));
        }
    }
    stats.truePeak = peak;
    return stats;
}

int bestIntegerLag(const std::vector<float>& reference, const std::vector<float>& other,
                   int maxLag, double& correlation)
{
    correlation = 0.0;
    const int count = static_cast<int>(std::min(reference.size(), other.size()));
    if (count < 32 || maxLag < 0)
        return 0;
    const int lagLimit = std::min(maxLag, count / 4);
    int best = 0;
    double bestScore = -std::numeric_limits<double>::infinity();
    for (int lag = -lagLimit; lag <= lagLimit; ++lag)
    {
        double score = 0.0;
        int used = 0;
        for (int i = 0; i < count; ++i)
        {
            const int j = i + lag;
            if (j < 0 || j >= count)
                continue;
            const float a = reference[static_cast<size_t>(i)];
            const float b = other[static_cast<size_t>(j)];
            if (!std::isfinite(a) || !std::isfinite(b))
                continue;
            score += static_cast<double>(a) * static_cast<double>(b);
            ++used;
        }
        if (used > 0)
            score /= static_cast<double>(used);
        const bool closerTie = std::abs(score - bestScore) <= 1.0e-15
            && std::abs(lag) < std::abs(best);
        if (score > bestScore || closerTie)
        {
            bestScore = score;
            best = lag;
        }
    }
    if (!(bestScore > 1.0e-12))
        best = 0;
    correlation = bestScore;
    return best;
}

double maxAbsDifference(const std::vector<float>& a, const std::vector<float>& b, int lag)
{
    const int count = static_cast<int>(std::min(a.size(), b.size()));
    double peak = 0.0;
    for (int i = 0; i < count; ++i)
    {
        const int j = i + lag;
        if (j < 0 || j >= count)
            continue;
        const float left = a[static_cast<size_t>(i)];
        const float right = b[static_cast<size_t>(j)];
        if (!std::isfinite(left) || !std::isfinite(right))
            return std::numeric_limits<double>::infinity();
        peak = std::max(peak, std::abs(static_cast<double>(left) - static_cast<double>(right)));
    }
    return peak;
}

double rmsDifference(const std::vector<float>& a, const std::vector<float>& b, int lag)
{
    const int count = static_cast<int>(std::min(a.size(), b.size()));
    double energy = 0.0;
    int used = 0;
    for (int i = 0; i < count; ++i)
    {
        const int j = i + lag;
        if (j < 0 || j >= count)
            continue;
        const float left = a[static_cast<size_t>(i)];
        const float right = b[static_cast<size_t>(j)];
        if (!std::isfinite(left) || !std::isfinite(right))
            return std::numeric_limits<double>::infinity();
        const double delta = static_cast<double>(left) - static_cast<double>(right);
        energy += delta * delta;
        ++used;
    }
    return used > 0 ? std::sqrt(energy / static_cast<double>(used)) : 0.0;
}

bool writeFloatWav(const std::string& path, const std::vector<float>& samples, int sampleRate)
{
    std::ofstream out(path, std::ios::binary);
    if (!out)
        return false;
    const auto write16 = [&out](uint16_t value) {
        const unsigned char bytes[2] = {
            static_cast<unsigned char>(value & 0xffu),
            static_cast<unsigned char>((value >> 8) & 0xffu)
        };
        out.write(reinterpret_cast<const char*>(bytes), 2);
    };
    const auto write32 = [&out](uint32_t value) {
        const unsigned char bytes[4] = {
            static_cast<unsigned char>(value & 0xffu),
            static_cast<unsigned char>((value >> 8) & 0xffu),
            static_cast<unsigned char>((value >> 16) & 0xffu),
            static_cast<unsigned char>((value >> 24) & 0xffu)
        };
        out.write(reinterpret_cast<const char*>(bytes), 4);
    };
    const uint32_t dataBytes = static_cast<uint32_t>(samples.size() * sizeof(float));
    out.write("RIFF", 4);
    write32(36u + dataBytes);
    out.write("WAVEfmt ", 8);
    write32(16);
    write16(3);
    write16(1);
    write32(static_cast<uint32_t>(sampleRate));
    write32(static_cast<uint32_t>(sampleRate * static_cast<int>(sizeof(float))));
    write16(static_cast<uint16_t>(sizeof(float)));
    write16(32);
    out.write("data", 4);
    write32(dataBytes);
    out.write(reinterpret_cast<const char*>(samples.data()), static_cast<std::streamsize>(dataBytes));
    return static_cast<bool>(out);
}

bool writeSpectrogramPpm(const std::string& path, const std::vector<float>& samples,
                         double sampleRate, int fftSize, int hop)
{
    if (fftSize < 64 || hop < 1 || samples.size() < static_cast<size_t>(fftSize) || !(sampleRate > 0.0))
        return false;
    const int frames = static_cast<int>((samples.size() - static_cast<size_t>(fftSize)) / static_cast<size_t>(hop));
    const int bins = std::min(fftSize / 2, static_cast<int>(8000.0 / (sampleRate / fftSize)) + 1);
    if (frames < 2 || bins < 8)
        return false;
    std::vector<unsigned char> image(static_cast<size_t>(frames * bins), 0);
    for (int frame = 0; frame < frames; ++frame)
    {
        const int origin = frame * hop;
        for (int bin = 0; bin < bins; ++bin)
        {
            const double frequency = sampleRate * static_cast<double>(bin) / static_cast<double>(fftSize);
            const auto tone = projectTone(samples.data() + origin, fftSize, sampleRate, frequency,
                                          AnalysisWindow::hann);
            const double decibels = std::clamp(db(tone.peakAmplitude), -100.0, 0.0);
            const auto shade = static_cast<unsigned char>(std::lround((decibels + 100.0) * 2.55));
            image[static_cast<size_t>((bins - 1 - bin) * frames + frame)] = shade;
        }
    }
    std::ofstream out(path, std::ios::binary);
    if (!out)
        return false;
    out << "P5\n" << frames << ' ' << bins << "\n255\n";
    out.write(reinterpret_cast<const char*>(image.data()), static_cast<std::streamsize>(image.size()));
    return static_cast<bool>(out);
}

int runMetricSelfTests(std::string& detail)
{
    int failures = 0;
    std::ostringstream text;
    constexpr double rate = 48000.0;
    constexpr int count = 4800;
    std::vector<float> sine(static_cast<size_t>(count));
    std::vector<float> harmonic(static_cast<size_t>(count));
    std::vector<float> dc(static_cast<size_t>(count), 0.25f);
    std::vector<float> clipped(static_cast<size_t>(count), 0.0f);
    std::vector<float> shifted(static_cast<size_t>(count));
    for (int i = 0; i < count; ++i)
    {
        const double t = static_cast<double>(i);
        sine[static_cast<size_t>(i)] = static_cast<float>(0.5 * std::sin(2.0 * kPi * 1000.0 * t / rate));
        harmonic[static_cast<size_t>(i)] = static_cast<float>(
            std::sin(2.0 * kPi * 1000.0 * t / rate) + 0.1 * std::sin(2.0 * kPi * 3000.0 * t / rate));
        clipped[static_cast<size_t>(i)] = i < 10 ? 1.2f : 0.1f;
        shifted[static_cast<size_t>(i)] = static_cast<float>(std::cos(2.0 * kPi * (t + 0.5) / 8.0));
    }
    clipped[20] = std::numeric_limits<float>::quiet_NaN();

    const auto sineLevel = measureLevels(sine.data(), count);
    const auto sineTone = projectTone(sine.data(), count, rate, 1000.0, AnalysisWindow::rectangular);
    if (std::abs(sineTone.peakAmplitude - 0.5) > 1.0e-6)
        fail(failures, text, "1 kHz rectangular projection is not 0.5 peak");
    if (std::abs(db(sineTone.peakAmplitude) - db(0.5)) > 1.0e-4)
        fail(failures, text, "dBFS of the 0.5 peak sine is wrong");
    if (sineLevel.nonfinite != 0 || sineLevel.aboveUnity != 0)
        fail(failures, text, "clean sine was classified as invalid or clipped");

    const auto dcLevel = measureLevels(dc.data(), count);
    if (std::abs(dcLevel.mean - 0.25) > 1.0e-7 || std::abs(dcLevel.samplePeak - 0.25) > 1.0e-7)
        fail(failures, text, "DC of 0.25 was subtracted or rescaled");

    const auto distortion = measureDistortion(harmonic.data(), count, rate, 1000.0,
                                              AnalysisWindow::rectangular);
    if (!distortion.thdNValid || std::abs(distortion.thd - 0.1) > 1.0e-4)
        fail(failures, text, "10 percent third harmonic was not recovered as THD");
    if (std::abs(distortion.thdN - 0.1) > 2.0e-3)
        fail(failures, text, "coherent THD+N missed the known third harmonic");
    const auto pure = measureDistortion(sine.data(), count, rate, 1000.0, AnalysisWindow::rectangular);
    if (!pure.thdNValid || pure.thd > 1.0e-5 || pure.thdN > 1.0e-5)
        fail(failures, text, "pure coherent sine reported leakage as distortion");

    const auto hannTone = projectTone(sine.data(), count, rate, 1000.0, AnalysisWindow::hann);
    if (std::abs(hannTone.peakAmplitude - 0.5) > 1.0e-4)
        fail(failures, text, "Hann coherent gain was not normalised");

    const auto clipLevel = measureLevels(clipped.data(), count);
    if (clipLevel.aboveUnity < 10 || clipLevel.nonfinite != 1 || clipLevel.longestNearFullScaleRun < 10)
        fail(failures, text, "clip run or nonfinite sample was not counted");

    const auto peak = measureTruePeak(shifted.data(), count);
    if (!(peak.samplePeak < 0.93) || !(peak.truePeak > 0.98) || std::abs(peak.truePeak - 1.0) > kTruePeakTolerance)
        fail(failures, text, "4x Blackman-sinc true peak missed the half-sample cosine crest");
    const auto dcPeak = measureTruePeak(dc.data(), count);
    if (std::abs(dcPeak.truePeak - 0.25) > 1.0e-3)
        fail(failures, text, "true-peak kernel does not preserve DC");

    const double folded = foldFrequency(15000.0 * 3.0, 48000.0);
    if (std::abs(folded - 3000.0) > 1.0e-6)
        fail(failures, text, "3rd harmonic of 15 kHz at 48 kHz did not fold to 3 kHz");
    if (coherentLength(rate, 1000.0, 1000) % 48 != 0)
        fail(failures, text, "coherent length is not an integer number of 1 kHz periods");

    std::vector<float> delayed(sine.size(), 0.0f);
    for (size_t i = 3; i < sine.size(); ++i)
        delayed[i] = sine[i - 3];
    double correlation = 0.0;
    const int lag = bestIntegerLag(sine, delayed, 8, correlation);
    if (lag != 3)
        fail(failures, text, "integer lag search did not find a 3-sample delay");

    if (failures == 0)
        text << "metric self-tests passed\n";
    detail = text.str();
    return failures;
}

}  // namespace swaraxt::sonic
