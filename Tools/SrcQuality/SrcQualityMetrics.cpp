// Copyright 2026 MontroneDSP.
// SPDX-License-Identifier: GPL-3.0-or-later
//
// Hermite vs polyphase-FIR sample-rate conversion study.
//
// Both converters are driven through the exact protocol SwaraXtEngine uses and
// are fed from the same pre-computed native buffer, so any difference in the
// results comes from the converter alone.

#include "Engine/SampleRate/InternalSampleQueue.h"
#include "Engine/SampleRate/PolyphaseFirResampler.h"

#include "SrcAnalysis.h"

#include <chrono>
#include <cstdlib>
#include <filesystem>
#include <functional>
#include <iostream>
#include <map>
#include <memory>
#include <numeric>
#include <string>
#include <vector>

namespace {

using srcq::kNativeRate;
using srcq::kNativeBlock;

const std::vector<double> kHostRates { 44100.0, 48000.0, 88200.0, 96000.0, 176400.0, 192000.0 };
const std::vector<double> kTones { 20.0, 100.0, 1000.0, 2000.0, 5000.0, 8000.0,
                                   10000.0, 12000.0, 15000.0, 18000.0, 19000.0 };

constexpr int kFftHost = 1 << 17;
constexpr int kWarmupHost = 8192;
constexpr double kToneAmplitude = 0.5;

std::string artifactRoot()
{
    if (const char* configured = std::getenv("SWARAXT_SRC_ARTIFACT_DIR");
        configured != nullptr && *configured != '\0')
        return configured;

    return (std::filesystem::current_path() / "artifacts" / "src-quality").string();
}

// ------------------------------------------------------------------ registry

// Every candidate is described by a set of type-erased operations. The CPU
// benchmark deliberately runs through a templated lambda so the inner loop is
// still a direct, inlinable call.
struct Candidate {
    std::string name;
    int taps = 4;
    int phases = 0;
    bool phaseInterpolation = false;
    double stopbandDb = 0.0;
    std::size_t coefficientBytes = 0;
    double groupDelayNative = 0.0;

    // Runs the converter over `native` producing `numHost` output samples and
    // returns how many native samples were consumed.
    std::function<std::size_t(const std::vector<float>&, std::vector<float>&, int, double)> run;
    // Same, but the host loop is split into chunks of `chunk` samples.
    std::function<void(const std::vector<float>&, std::vector<float>&, int, double, int)> runChunked;
    // Returns median and p95 nanoseconds per output sample.
    std::function<std::pair<double, double>(const std::vector<float>&, int, double, int)> benchmark;
};

// Converters own multi-hundred-kilobyte tables, so every instance lives on the
// heap even though the production engine holds one as a plain member.
template <class Conv, class Factory>
Candidate makeCandidate(const std::string& name, Factory factory,
                        int taps, int phases, bool interpolate, double stopbandDb,
                        std::size_t coefficientBytes, double groupDelay)
{
    Candidate c;
    c.name = name;
    c.taps = taps;
    c.phases = phases;
    c.phaseInterpolation = interpolate;
    c.stopbandDb = stopbandDb;
    c.coefficientBytes = coefficientBytes;
    c.groupDelayNative = groupDelay;

    c.run = [factory](const std::vector<float>& native, std::vector<float>& out,
                      int numHost, double hostRate) -> std::size_t {
        auto conv = factory();
        conv->reset();
        conv->setStep(kNativeRate, hostRate);
        return srcq::driveFromBuffer(*conv, native, out, numHost);
    };

    c.runChunked = [factory](const std::vector<float>& native, std::vector<float>& out,
                             int numHost, double hostRate, int chunk) {
        auto conv = factory();
        conv->reset();
        conv->setStep(kNativeRate, hostRate);
        out.assign(static_cast<std::size_t>(numHost), 0.0f);
        std::size_t cursor = 0;
        auto produce = [&]() -> float { return cursor < native.size() ? native[cursor++] : 0.0f; };
        int done = 0;
        while (done < numHost)
        {
            const int n = std::min(chunk, numHost - done);
            srcq::drive(*conv, produce, out.data() + done, n);
            done += n;
        }
    };

    c.benchmark = [factory](const std::vector<float>& native, int numHost,
                            double hostRate, int repeats) -> std::pair<double, double> {
        auto conv = factory();
        std::vector<float> out(static_cast<std::size_t>(numHost));
        std::vector<double> samples;
        samples.reserve(static_cast<std::size_t>(repeats));

        for (int r = 0; r < repeats + 2; ++r)
        {
            conv->reset();
            conv->setStep(kNativeRate, hostRate);
            std::size_t cursor = 0;
            auto produce = [&]() -> float { return cursor < native.size() ? native[cursor++] : 0.0f; };

            const auto start = std::chrono::steady_clock::now();
            srcq::drive(*conv, produce, out.data(), numHost);
            const auto end = std::chrono::steady_clock::now();

            // Keep the optimiser honest.
            volatile float sink = out[static_cast<std::size_t>(numHost) - 1];
            (void) sink;

            if (r >= 2)  // first two passes warm the caches
            {
                const double ns = std::chrono::duration<double, std::nano>(end - start).count();
                samples.push_back(ns / static_cast<double>(numHost));
            }
        }

        std::sort(samples.begin(), samples.end());
        const std::size_t p95 = std::min(samples.size() - 1,
                                         static_cast<std::size_t>(0.95 * static_cast<double>(samples.size())));
        return { samples[samples.size() / 2], samples[p95] };
    };

    return c;
}

Candidate hermiteCandidate()
{
    using Conv = swaraxt::InternalSampleQueue;
    return makeCandidate<Conv>("Hermite",
                               []() { return std::make_unique<Conv>(); },
                               4, 0, false, 0.0, 0, 1.5);
}

template <int Taps, int Phases, bool Interp>
Candidate firCandidate(const std::string& name, double stopbandDb, double cutoff = 1.0)
{
    using Conv = swaraxt::PolyphaseFirResampler<Taps, Phases, Interp>;
    return makeCandidate<Conv>(name,
                               [stopbandDb, cutoff]() { return std::make_unique<Conv>(stopbandDb, cutoff); },
                               Taps, Phases, Interp, stopbandDb,
                               Conv::coefficientBytes(), Conv::groupDelayNativeSamples());
}

std::vector<Candidate> designSearchCandidates()
{
    std::vector<Candidate> v;
    v.push_back(hermiteCandidate());
    // Tap-count knee at a fixed, generous design attenuation.
    v.push_back(firCandidate<32, 256, true>("FIR32-P256-lin-A100", 100.0));
    v.push_back(firCandidate<48, 256, true>("FIR48-P256-lin-A100", 100.0));
    v.push_back(firCandidate<64, 256, true>("FIR64-P256-lin-A100", 100.0));
    v.push_back(firCandidate<96, 256, true>("FIR96-P256-lin-A100", 100.0));
    v.push_back(firCandidate<128, 256, true>("FIR128-P256-lin-A100", 100.0));
    // Phase-table density at the likely knee.
    v.push_back(firCandidate<64, 64, true>("FIR64-P64-lin-A100", 100.0));
    v.push_back(firCandidate<64, 128, true>("FIR64-P128-lin-A100", 100.0));
    v.push_back(firCandidate<64, 512, true>("FIR64-P512-lin-A100", 100.0));
    // Nearest phase instead of blending between phase kernels.
    v.push_back(firCandidate<64, 256, false>("FIR64-P256-near-A100", 100.0));
    v.push_back(firCandidate<64, 4096, false>("FIR64-P4096-near-A100", 100.0));
    // Design attenuation trades transition width against stopband depth: a
    // narrower transition pushes the stopband edge below the first image of an
    // 18 kHz tone, at the cost of a shallower floor everywhere else.
    v.push_back(firCandidate<64, 256, true>("FIR64-P256-lin-A75", 75.0));
    v.push_back(firCandidate<64, 256, true>("FIR64-P256-lin-A80", 80.0));
    v.push_back(firCandidate<64, 256, true>("FIR64-P256-lin-A85", 85.0));
    v.push_back(firCandidate<64, 256, true>("FIR64-P256-lin-A90", 90.0));
    v.push_back(firCandidate<64, 256, true>("FIR64-P256-lin-A120", 120.0));
    v.push_back(firCandidate<48, 256, true>("FIR48-P256-lin-A80", 80.0));
    v.push_back(firCandidate<80, 256, true>("FIR80-P256-lin-A100", 100.0));
    v.push_back(firCandidate<96, 256, true>("FIR96-P256-lin-A120", 120.0));
    return v;
}

// ------------------------------------------------------------------- reports

void sweep(const std::vector<Candidate>& candidates,
           const std::vector<double>& rates,
           const std::string& csvPath)
{
    FILE* csv = std::fopen(csvPath.c_str(), "w");
    if (csv == nullptr)
    {
        std::cerr << "cannot open " << csvPath << "\n";
        return;
    }
    std::fprintf(csv, "candidate,taps,phases,phase_interp,stopband_db,host_rate,tone_hz,"
                      "passband_err_db,freq_err_hz,worst_image_dbc,worst_image_hz,"
                      "thdn_dbc,rms,peak,dc_mean\n");

    const int numHost = kWarmupHost + kFftHost;
    std::map<std::pair<long, long>, std::vector<float>> nativeCache;

    for (double rate : rates)
    {
        for (double tone : kTones)
        {
            if (tone >= 0.5 * kNativeRate)
                continue;

            const auto key = std::make_pair(static_cast<long>(std::lround(rate)),
                                            static_cast<long>(std::lround(tone)));
            if (nativeCache.find(key) == nativeCache.end())
            {
                const std::size_t nativeNeeded =
                    static_cast<std::size_t>(numHost * (kNativeRate / rate)) + 4 * kNativeBlock + 64;
                nativeCache[key] = srcq::nativeSine(tone, kToneAmplitude, nativeNeeded);
            }
            const std::vector<float>& native = nativeCache[key];

            for (const Candidate& c : candidates)
            {
                std::vector<float> host;
                c.run(native, host, numHost, rate);
                std::vector<float> analysed(host.begin() + kWarmupHost, host.end());
                const srcq::ToneMetrics m = srcq::measureTone(analysed, rate, tone, kToneAmplitude);

                std::fprintf(csv,
                             "%s,%d,%d,%d,%.1f,%.1f,%.1f,%.4f,%.6f,%.2f,%.1f,%.2f,%.6f,%.6f,%.3e\n",
                             c.name.c_str(), c.taps, c.phases, c.phaseInterpolation ? 1 : 0,
                             c.stopbandDb, rate, tone,
                             m.passbandErrorDb, m.frequencyErrorHz, m.worstImageDbc, m.worstImageHz,
                             m.thdPlusNDbc, m.rms, m.peak, m.dcMean);
            }
            std::fflush(csv);
            std::cerr << "  swept " << rate << " Hz / " << tone << " Hz\n";
        }
    }
    std::fclose(csv);
    std::cout << "wrote " << csvPath << "\n";
}

struct ImpulseMetrics {
    double delayHostSamples = 0.0;
    double peakHostIndex = 0.0;
    double peakValue = 0.0;
    double preRingingDb = -200.0;
    double postRingingDb = -200.0;
    double preRingingSpanMs = 0.0;
    double postRingingSpanMs = 0.0;
    double dcGainImpulse = 0.0;
    double dcGainConstant = 0.0;
};

ImpulseMetrics measureImpulse(const Candidate& c, double hostRate)
{
    constexpr std::size_t kImpulsePosition = 4096;
    const int numHost = 1 << 15;
    const std::size_t nativeNeeded =
        static_cast<std::size_t>(numHost * (kNativeRate / hostRate)) + 4 * kNativeBlock + 64;

    std::vector<float> native = srcq::nativeImpulse(nativeNeeded, kImpulsePosition, 1.0f);
    std::vector<float> host;
    c.run(native, host, numHost, hostRate);

    ImpulseMetrics m;

    double peak = 0.0;
    std::size_t peakIndex = 0;
    for (std::size_t i = 0; i < host.size(); ++i)
    {
        if (std::abs(static_cast<double>(host[i])) > peak)
        {
            peak = std::abs(static_cast<double>(host[i]));
            peakIndex = i;
        }
    }
    m.peakValue = static_cast<double>(host[peakIndex]);
    m.peakHostIndex = static_cast<double>(peakIndex);

    // Energy centroid gives the true group delay of a symmetric kernel.
    double energy = 0.0;
    double weighted = 0.0;
    const std::size_t lo = peakIndex > 2048 ? peakIndex - 2048 : 0;
    const std::size_t hi = std::min(host.size() - 1, peakIndex + 2048);
    for (std::size_t i = lo; i <= hi; ++i)
    {
        const double p = static_cast<double>(host[i]) * host[i];
        energy += p;
        weighted += p * static_cast<double>(i);
    }
    const double centroid = energy > 0.0 ? weighted / energy : static_cast<double>(peakIndex);

    // Ideal arrival time of the impulse in host samples, with no converter delay.
    const double idealHostIndex = static_cast<double>(kImpulsePosition) * hostRate / kNativeRate;
    m.delayHostSamples = centroid - idealHostIndex;

    // Ringing: largest excursion more than one host period either side of the
    // main lobe, relative to the peak. The main lobe is taken as the first zero
    // crossings around the peak.
    std::size_t leftEdge = peakIndex;
    while (leftEdge > lo && host[leftEdge] * host[peakIndex] > 0.0f)
        --leftEdge;
    std::size_t rightEdge = peakIndex;
    while (rightEdge < hi && host[rightEdge] * host[peakIndex] > 0.0f)
        ++rightEdge;

    double preMax = 0.0;
    double postMax = 0.0;
    std::size_t preFirst = leftEdge;
    std::size_t postLast = rightEdge;
    const double ringingFloor = peak * 1.0e-4;  // -80 dB relative to the peak
    for (std::size_t i = lo; i < leftEdge; ++i)
    {
        const double a = std::abs(static_cast<double>(host[i]));
        preMax = std::max(preMax, a);
        if (a > ringingFloor && i < preFirst)
            preFirst = i;
    }
    for (std::size_t i = rightEdge + 1; i <= hi; ++i)
    {
        const double a = std::abs(static_cast<double>(host[i]));
        postMax = std::max(postMax, a);
        if (a > ringingFloor)
            postLast = i;
    }

    m.preRingingDb = (preMax > 0.0 && peak > 0.0) ? 20.0 * std::log10(preMax / peak) : -200.0;
    m.postRingingDb = (postMax > 0.0 && peak > 0.0) ? 20.0 * std::log10(postMax / peak) : -200.0;
    m.preRingingSpanMs = 1000.0 * static_cast<double>(leftEdge - preFirst) / hostRate;
    m.postRingingSpanMs = 1000.0 * static_cast<double>(postLast - rightEdge) / hostRate;

    // A unity-DC-gain converter preserves impulse area: sum(out) * native/host = 1.
    double sum = 0.0;
    for (float v : host)
        sum += static_cast<double>(v);
    m.dcGainImpulse = sum * (kNativeRate / hostRate);

    // Settled response to a constant input is the direct DC gain test.
    std::vector<float> constant(nativeNeeded, 1.0f);
    std::vector<float> constantHost;
    c.run(constant, constantHost, numHost, hostRate);
    double tail = 0.0;
    const std::size_t tailStart = constantHost.size() - 4096;
    for (std::size_t i = tailStart; i < constantHost.size(); ++i)
        tail += static_cast<double>(constantHost[i]);
    m.dcGainConstant = tail / 4096.0;

    return m;
}

void impulseReport(const std::vector<Candidate>& candidates,
                   const std::vector<double>& rates,
                   const std::string& csvPath)
{
    FILE* csv = std::fopen(csvPath.c_str(), "w");
    if (csv == nullptr)
        return;
    std::fprintf(csv, "candidate,host_rate,delay_host_samples,delay_native_samples,delay_ms,"
                      "peak,pre_ringing_db,post_ringing_db,pre_ringing_ms,post_ringing_ms,"
                      "dc_gain_impulse,dc_gain_constant\n");

    for (double rate : rates)
        for (const Candidate& c : candidates)
        {
            const ImpulseMetrics m = measureImpulse(c, rate);
            std::fprintf(csv, "%s,%.1f,%.4f,%.4f,%.5f,%.6f,%.2f,%.2f,%.4f,%.4f,%.8f,%.8f\n",
                         c.name.c_str(), rate,
                         m.delayHostSamples, m.delayHostSamples * kNativeRate / rate,
                         1000.0 * m.delayHostSamples / rate,
                         m.peakValue, m.preRingingDb, m.postRingingDb,
                         m.preRingingSpanMs, m.postRingingSpanMs,
                         m.dcGainImpulse, m.dcGainConstant);
        }
    std::fclose(csv);
    std::cout << "wrote " << csvPath << "\n";
}

void cpuReport(const std::vector<Candidate>& candidates,
               const std::vector<double>& rates,
               const std::string& csvPath)
{
    FILE* csv = std::fopen(csvPath.c_str(), "w");
    if (csv == nullptr)
        return;
    std::fprintf(csv, "candidate,taps,host_rate,ns_per_output_sample_median,"
                      "ns_per_output_sample_p95,ns_per_second_audio,"
                      "realtime_fraction_percent,coefficient_bytes\n");

    const int numHost = 1 << 16;
    constexpr int kRepeats = 21;

    for (double rate : rates)
    {
        const std::size_t nativeNeeded =
            static_cast<std::size_t>(numHost * (kNativeRate / rate)) + 4 * kNativeBlock + 64;
        const std::vector<float> native = srcq::nativeNoise(nativeNeeded);

        for (const Candidate& c : candidates)
        {
            const auto [median, p95] = c.benchmark(native, numHost, rate, kRepeats);
            const double nsPerSecond = median * rate;
            std::fprintf(csv, "%s,%d,%.1f,%.4f,%.4f,%.1f,%.5f,%zu\n",
                         c.name.c_str(), c.taps, rate, median, p95, nsPerSecond,
                         100.0 * nsPerSecond / 1.0e9, c.coefficientBytes);
        }
        std::cerr << "  benchmarked " << rate << " Hz\n";
    }
    std::fclose(csv);
    std::cout << "wrote " << csvPath << "\n";
}

void streamingReport(const std::vector<Candidate>& candidates,
                     const std::vector<double>& rates,
                     const std::string& csvPath)
{
    FILE* csv = std::fopen(csvPath.c_str(), "w");
    if (csv == nullptr)
        return;
    std::fprintf(csv, "candidate,host_rate,block_size,max_abs_difference,bit_exact\n");

    const std::vector<int> blocks { 63, 64, 65, 127, 128, 129, 257 };
    const int numHost = 1 << 15;

    for (double rate : rates)
    {
        const std::size_t nativeNeeded =
            static_cast<std::size_t>(numHost * (kNativeRate / rate)) + 4 * kNativeBlock + 64;
        const std::vector<float> native = srcq::nativeNoise(nativeNeeded, 0x1234567u);

        for (const Candidate& c : candidates)
        {
            std::vector<float> reference;
            c.run(native, reference, numHost, rate);

            for (int block : blocks)
            {
                std::vector<float> chunked;
                c.runChunked(native, chunked, numHost, rate, block);
                double worst = 0.0;
                for (std::size_t i = 0; i < reference.size(); ++i)
                    worst = std::max(worst, std::abs(static_cast<double>(reference[i]) - chunked[i]));
                std::fprintf(csv, "%s,%.1f,%d,%.3e,%d\n",
                             c.name.c_str(), rate, block, worst, worst == 0.0 ? 1 : 0);
            }
        }
    }
    std::fclose(csv);
    std::cout << "wrote " << csvPath << "\n";
}

void longRunReport(const std::vector<Candidate>& candidates,
                   const std::vector<double>& rates,
                   const std::string& csvPath)
{
    FILE* csv = std::fopen(csvPath.c_str(), "w");
    if (csv == nullptr)
        return;
    std::fprintf(csv, "candidate,host_rate,seconds,host_samples_out,native_consumed,"
                      "expected_native,native_error_samples,ratio_measured,ratio_exact,"
                      "ratio_error_ppm,start_freq_hz,end_freq_hz,freq_drift_hz,"
                      "silent_samples,nonfinite\n");

    constexpr double kSeconds = 60.0;
    constexpr double kProbeHz = 1000.0;

    for (double rate : rates)
    {
        const int numHost = static_cast<int>(std::lround(kSeconds * rate));
        const std::size_t nativeNeeded =
            static_cast<std::size_t>(numHost * (kNativeRate / rate)) + 8 * kNativeBlock + 1024;
        const std::vector<float> native = srcq::nativeSine(kProbeHz, kToneAmplitude, nativeNeeded);

        for (const Candidate& c : candidates)
        {
            std::vector<float> host;
            const std::size_t consumed = c.run(native, host, numHost, rate);

            std::size_t silent = 0;
            std::size_t nonFinite = 0;
            for (float v : host)
            {
                if (! std::isfinite(v))
                    ++nonFinite;
                else if (v == 0.0f)
                    ++silent;
            }

            const std::size_t window = 1 << 16;
            std::vector<float> head(host.begin() + kWarmupHost,
                                    host.begin() + kWarmupHost + static_cast<long>(window));
            std::vector<float> tail(host.end() - static_cast<long>(window), host.end());
            const srcq::ToneMetrics startMetrics = srcq::measureTone(head, rate, kProbeHz, kToneAmplitude);
            const srcq::ToneMetrics endMetrics = srcq::measureTone(tail, rate, kProbeHz, kToneAmplitude);

            // The queue pulls native samples in 40-sample blocks, so the running
            // total is expected to sit within one block plus the converter's
            // steady-state occupancy of the exact 20 MHz / 510 relationship.
            const double exactRatio = kNativeRate / rate;
            const double expectedNative = static_cast<double>(numHost) * exactRatio;
            const double measuredRatio = static_cast<double>(consumed) / static_cast<double>(numHost);

            std::fprintf(csv,
                         "%s,%.1f,%.1f,%d,%zu,%.1f,%.3f,%.9f,%.9f,%.4f,%.4f,%.4f,%.5f,%zu,%zu\n",
                         c.name.c_str(), rate, kSeconds, numHost,
                         consumed, expectedNative,
                         static_cast<double>(consumed) - expectedNative,
                         measuredRatio, exactRatio,
                         1.0e6 * (measuredRatio - exactRatio) / exactRatio,
                         startMetrics.measuredHz, endMetrics.measuredHz,
                         endMetrics.measuredHz - startMetrics.measuredHz,
                         silent, nonFinite);
        }
        std::cerr << "  long run " << rate << " Hz\n";
    }
    std::fclose(csv);
    std::cout << "wrote " << csvPath << "\n";
}

// Writes the native stream plus each converter's host output so the external
// high-quality reference comparison can be done offline.
void dumpForReference(const std::vector<Candidate>& candidates,
                      const std::vector<double>& rates,
                      const std::string& directory)
{
    const int numHost = 1 << 18;

    struct Source {
        std::string name;
        std::function<std::vector<float>(std::size_t)> make;
    };
    const std::vector<Source> sources {
        { "noise", [](std::size_t n) { return srcq::nativeNoise(n, 0xC0FFEEu, 0.35f); } },
        { "sine10k", [](std::size_t n) { return srcq::nativeSine(10000.0, 0.5, n); } },
        { "sine15k", [](std::size_t n) { return srcq::nativeSine(15000.0, 0.5, n); } },
        { "impulse", [](std::size_t n) { return srcq::nativeImpulse(n, 4096, 1.0f); } },
        { "step", [](std::size_t n) { return srcq::nativeStep(n, 4096, 0.5f); } },
    };

    for (const Source& source : sources)
    {
        for (double rate : rates)
        {
            const std::size_t nativeNeeded =
                static_cast<std::size_t>(numHost * (kNativeRate / rate)) + 8 * kNativeBlock + 1024;
            const std::vector<float> native = source.make(nativeNeeded);
            const std::string rateTag = std::to_string(static_cast<long>(std::lround(rate)));

            srcq::writeRawFloat32(directory + "/native_" + source.name + "_" + rateTag + ".f32", native);

            for (const Candidate& c : candidates)
            {
                std::vector<float> host;
                c.run(native, host, numHost, rate);
                srcq::writeRawFloat32(
                    directory + "/host_" + source.name + "_" + rateTag + "_" + c.name + ".f32", host);
            }
        }
    }
    std::cout << "wrote reference dumps to " << directory << "\n";
}

void kernelReport(const std::string& csvPath)
{
    FILE* csv = std::fopen(csvPath.c_str(), "w");
    if (csv == nullptr)
        return;
    std::fprintf(csv, "candidate,taps,phases,phase_interp,stopband_db,coefficient_bytes,"
                      "history_bytes,group_delay_native_samples,group_delay_ms\n");

    for (const Candidate& c : designSearchCandidates())
        std::fprintf(csv, "%s,%d,%d,%d,%.1f,%zu,%zu,%.3f,%.5f\n",
                     c.name.c_str(), c.taps, c.phases, c.phaseInterpolation ? 1 : 0,
                     c.stopbandDb, c.coefficientBytes,
                     sizeof(float) * 16384u,
                     c.groupDelayNative, 1000.0 * c.groupDelayNative / kNativeRate);
    std::fclose(csv);
    std::cout << "wrote " << csvPath << "\n";
}

std::vector<Candidate> filterByName(const std::vector<Candidate>& all,
                                    const std::vector<std::string>& names)
{
    std::vector<Candidate> out;
    for (const std::string& n : names)
        for (const Candidate& c : all)
            if (c.name == n)
                out.push_back(c);
    return out;
}

}  // namespace

int main(int argc, char** argv)
{
    const std::string mode = argc > 1 ? argv[1] : "all";
    const std::string root = artifactRoot();
    const std::string measurements = root + "/measurements";
    const std::string renders = root + "/renders";

    const std::vector<Candidate> all = designSearchCandidates();

    // Everything except the design search runs on the baseline plus the tap
    // sweep, which keeps the six-rate matrix affordable.
    const std::vector<Candidate> headline = filterByName(all, {
        "Hermite", "FIR32-P256-lin-A100", "FIR48-P256-lin-A100", "FIR64-P256-lin-A100",
        "FIR64-P256-lin-A80", "FIR80-P256-lin-A100", "FIR96-P256-lin-A100",
        "FIR128-P256-lin-A100" });

    if (mode == "kernels" || mode == "all")
        kernelReport(measurements + "/kernels.csv");

    if (mode == "search" || mode == "all")
    {
        std::cerr << "design search sweep (44.1k / 96k)\n";
        sweep(all, { 44100.0, 96000.0 }, measurements + "/design-search.csv");
    }

    if (mode == "sweep" || mode == "all")
    {
        std::cerr << "six-rate pure-tone sweep\n";
        sweep(headline, kHostRates, measurements + "/pure-tone.csv");
    }

    if (mode == "impulse" || mode == "all")
        impulseReport(all, kHostRates, measurements + "/impulse.csv");

    if (mode == "cpu" || mode == "all")
    {
        std::cerr << "cpu benchmark\n";
        cpuReport(all, kHostRates, measurements + "/cpu.csv");
    }

    if (mode == "stream" || mode == "all")
        streamingReport(headline, kHostRates, measurements + "/streaming.csv");

    if (mode == "longrun" || mode == "all")
    {
        std::cerr << "60 s long-run\n";
        longRunReport(headline, kHostRates, measurements + "/long-run.csv");
    }

    if (mode == "dump" || mode == "all")
        dumpForReference(filterByName(all, { "Hermite", "FIR64-P256-lin-A100",
                                             "FIR32-P256-lin-A100", "FIR128-P256-lin-A100" }),
                         { 44100.0, 96000.0 }, renders + "/native");

    return 0;
}
