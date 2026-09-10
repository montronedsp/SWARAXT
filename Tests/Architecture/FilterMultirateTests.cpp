// Copyright 2026 MontroneDSP. SPDX-License-Identifier: GPL-3.0-or-later
#include "Engine/Filter/SwaraXtFilter.h"
#include <algorithm>
#include <array>
#include <chrono>
#include <complex>
#include <cstdio>
#include <fstream>
#include <vector>

namespace {
constexpr double fs = 20000000.0 / 510.0, pi = 3.14159265358979323846;
int failures = 0;
void check(bool ok, const char* message) { if (!ok) { ++failures; std::printf("FAIL: %s\n", message); } }
double db(double value) { return 20 * std::log10(std::max(value, 1.e-15)); }
double fold(double frequency) { return std::abs(std::remainder(frequency, fs)); }
std::complex<double> project(const std::vector<float>& signal, double frequency) {
    std::complex<double> sum{}; double weight = 0;
    for (size_t i = 0; i < signal.size(); ++i) {
        const double window = .5 - .5 * std::cos(2 * pi * static_cast<double>(i) / static_cast<double>(signal.size() - 1));
        sum += static_cast<double>(signal[i]) * window * std::polar(1., -2 * pi * frequency * static_cast<double>(i) / fs);
        weight += window;
    }
    return sum * (2. / weight);
}
void matrix(std::ostream& csv) {
    csv << "factor,frequency_hz,amplitude,resonance,fundamental_dbfs,gain_db,third_fold_hz,third_dbfs,third_dbc,harmonics2to9_dbc,phase_rad,rms,dc,peak,max_delta,ns_per_native_sample\n";
    for (int factor : {2, 4, 8}) for (double frequency : {1000., 5000., 10000., 12000., 15000., 18000., 19000.})
    for (float amplitude : {.001f, .5f, 1.f}) for (float resonance : {0.f, .5f, .95f}) {
        swaraxt::SwaraXtFilter filter; filter.prepare(fs); filter.setOversampleFactorForTests(factor);
        swaraxt::SwaraXtFilterParams params; params.cutoffHz = 20000; params.resonance = resonance; filter.setParams(params);
        constexpr int warm = 8192, length = 16384;
        std::vector<float> signal; signal.reserve(length);
        double squares = 0, mean = 0, peak = 0, delta = 0; float previous = 0;
        const auto start = std::chrono::steady_clock::now();
        for (int i = 0; i < warm + length; ++i) {
            const auto output = filter.processSample(static_cast<float>(amplitude * std::sin(2 * pi * frequency * i / fs)));
            check(std::isfinite(output) && std::abs(output) <= 8, "finite bounded nonlinear filter");
            if (i >= warm) { signal.push_back(output); squares += output * output; mean += output; peak = std::max(peak, std::abs(static_cast<double>(output))); delta = std::max(delta, std::abs(static_cast<double>(output - previous))); }
            previous = output;
        }
        const double ns = std::chrono::duration<double, std::nano>(std::chrono::steady_clock::now() - start).count() / (warm + length);
        const auto fundamental = project(signal, frequency);
        const double magnitude = std::abs(fundamental), third = std::abs(project(signal, fold(3 * frequency)));
        double harmonicPower = 0;
        for (int harmonic = 2; harmonic <= 9; ++harmonic) harmonicPower += std::norm(project(signal, fold(harmonic * frequency)));
        csv << factor << ',' << frequency << ',' << amplitude << ',' << resonance << ',' << db(magnitude) << ',' << db(magnitude / amplitude)
            << ',' << fold(3 * frequency) << ',' << db(third) << ',' << db(third / magnitude) << ',' << db(std::sqrt(harmonicPower) / magnitude)
            << ',' << std::arg(fundamental) << ',' << std::sqrt(squares / length) << ',' << mean / length << ',' << peak << ',' << delta << ',' << ns << '\n';
        if (frequency == 12000 && amplitude == 1 && resonance == 0) {
            std::printf("factor=%d 12k gain=%.3f third=%.3f dBFS / %.3f dBc CPU=%.1f ns/sample\n", factor, db(magnitude / amplitude), db(third), db(third / magnitude), ns);
#ifndef SWARAXT_MEASURE_LEGACY_FILTER
            const double oldThirdDbc = factor == 2 ? -33.851 : factor == 4 ? -42.004 : -45.545;
            check(db(third / magnitude) < oldThirdDbc - 20, "12 kHz folded product reduced by at least 20 dB");
#endif
        }
    }
}
#ifndef SWARAXT_MEASURE_LEGACY_FILTER
void converter(const char* path) {
    std::ofstream response(std::string(path) + ".fir.csv");
    response << "factor,frequency_hz,one_way_gain_db,round_trip_gain_db,phase_rad\n";
    for (int factor : {2, 4, 8}) {
        swaraxt::FilterRateConverter converter; converter.prepare(factor);
        for (int hz = 0; hz <= 39000; hz += 25) {
            std::complex<double> h{};
            for (int tap = 0; tap <= factor * converter.kSpan; ++tap)
                h += converter.coefficientForTests(tap) * std::polar(1., -2 * pi * hz * tap / (fs * factor));
            response << factor << ',' << hz << ',' << db(std::abs(h)) << ',' << 2 * db(std::abs(h)) << ',' << std::arg(h) << '\n';
            if (hz <= 19000) check(std::abs(2 * db(std::abs(h))) < .01, "FIR round-trip passband through 19 kHz within 0.01 dB");
            if (hz >= 20500) check(db(std::abs(h)) < -95, "FIR stopband from 20.5 kHz below -95 dB");
        }
        int peakIndex = 0; float peak = 0;
        for (int i = 0; i < 1024; ++i) {
            const float output = converter.process(i == 0 ? 1.f : 0.f, [](float x) { return x; });
            if (std::abs(output) > peak) { peak = std::abs(output); peakIndex = i; }
            if (i > 512) check(output == 0, "FIR impulse tail drains exactly");
        }
        check(peakIndex == converter.latency(), "reported converter delay matches impulse peak");
        converter.reset();
        check(converter.process(0, [](float x) { return x; }) == 0, "reset clears FIR state");
    }
}
#endif
}
int main(int argc, char** argv) {
    std::ofstream csv(argc > 1 ? argv[1] : "filter-multirate.csv");
    check(csv.good(), "measurement file opens");
#ifndef SWARAXT_MEASURE_LEGACY_FILTER
    converter(argc > 1 ? argv[1] : "filter-multirate.csv");
#endif
    matrix(csv); std::printf("Multirate: %d failures\n", failures); return failures ? 1 : 0;
}
