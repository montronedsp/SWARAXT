// Copyright 2026 MontroneDSP. SPDX-License-Identifier: GPL-3.0-or-later
#include "Engine/SampleRate/PolyphaseFirResampler.h"
#include <complex>
#include <cstdio>
#include <fstream>
#include <vector>
#include <chrono>

namespace {
constexpr double fs = 20000000.0 / 510.0, pi = 3.14159265358979323846;
using Converter = swaraxt::PolyphaseFirResampler<256, 256, true>;
int failures = 0;
void check(bool ok, const char* message) { if (!ok) { ++failures; std::printf("FAIL: %s\n", message); } }
double db(double x) { return 20 * std::log10(std::max(std::abs(x), 1.e-15)); }
double fold(double f, double rate) { return std::abs(std::remainder(f, rate)); }
std::complex<double> project(const std::vector<float>& x, double frequency, double rate) {
    std::complex<double> sum{}; double weight = 0;
    for (size_t i = 0; i < x.size(); ++i) {
        const double w = .5 - .5 * std::cos(2 * pi * static_cast<double>(i) / static_cast<double>(x.size() - 1));
        sum += static_cast<double>(x[i]) * w * std::polar(1., -2 * pi * frequency * static_cast<double>(i) / rate);
        weight += w;
    }
    return sum * (2 / weight);
}
void coefficients(std::ostream& csv) {
    Converter converter;
    csv << "host_hz,cutoff_hz,pass_edge_hz,pass_error_db,stop_peak_db\n";
    for (double rate : {8000., 11025., 16000., 22050., 32000., 44100., 48000., 96000., 192000., 384000.}) {
        converter.setStep(fs, rate);
        const double passEdge = rate >= fs ? 19000. : rate * .5 - fs * 8 / Converter::kTaps;
        double passError = 0, stopPeak = 0;
        for (int phase : {0, 64, 128, 192, 256}) {
            const auto* row = converter.coefficientRow(phase);
            for (double frequency = 0; frequency <= fs * .5; frequency += 25) {
                std::complex<double> sum{};
                for (int tap = 0; tap < Converter::kTaps; ++tap)
                    sum += static_cast<double>(row[tap]) * std::polar(1., -2 * pi * frequency * tap / fs);
                if (frequency <= passEdge) passError = std::max(passError, std::abs(db(std::abs(sum))));
                if (rate < fs && frequency >= rate * .5) stopPeak = std::max(stopPeak, std::abs(sum));
            }
        }
        csv << rate << ',' << converter.cutoffNyquist() * fs * .5 << ',' << passEdge << ',' << passError << ','
            << (rate < fs ? std::to_string(db(stopPeak)) : "NA") << '\n';
        check(passError < .001, "actual polyphase coefficients meet specified passband");
        if (rate < fs) check(db(stopPeak) < -95, "downsampling stopband begins by host Nyquist");
        const auto designs = converter.kernelDesignCount();
        for (int i = 0; i < 100; ++i) { converter.reset(); converter.setStep(fs, rate); }
        check(converter.kernelDesignCount() == designs, "reset/dormancy at unchanged rate never redesigns coefficients");
    }
}
void tones(std::ostream& csv) {
    csv << "host_hz,input_hz,output_fold_hz,fundamental_dbfs,first_image_dbfs,max_image_dbfs,phase_rad,ns_per_output\n";
    Converter converter;
    for (double rate : {8000., 16000., 32000., 44100., 48000., 96000., 192000., 384000.})
    for (double frequency : {1000., 5000., 12000., 15000., 17000., 18000., 19000.}) {
        converter.setStep(fs, rate); converter.reset();
        int inputIndex = 0;
        std::vector<float> audio;
        const int warm = static_cast<int>(rate * .05), count = static_cast<int>(rate * .25);
        audio.reserve(static_cast<size_t>(count));
        const auto start = std::chrono::steady_clock::now();
        for (int i = 0; i < warm + count; ++i) {
            while (converter.size() < converter.queueTargetSize())
                converter.push(static_cast<float>(std::sin(2 * pi * frequency * inputIndex++ / fs)));
            const auto y = converter.readInterpolated();
            check(std::isfinite(y) && std::abs(y) < 1.3, "SRC signal finite and bounded");
            if (i >= warm) audio.push_back(y);
        }
        const double ns = std::chrono::duration<double, std::nano>(std::chrono::steady_clock::now() - start).count() / (warm + count);
        const auto fundamental = project(audio, fold(frequency, rate), rate);
        const double firstImage = std::abs(project(audio, fold(fs - frequency, rate), rate));
        double maxImage = 0;
        for (int order = 1; order <= 3; ++order) for (double sign : {-1., 1.}) {
            const double image = fold(order * fs + sign * frequency, rate);
            if (std::abs(image - fold(frequency, rate)) > 20)
                maxImage = std::max(maxImage, std::abs(project(audio, image, rate)));
        }
        csv << rate << ',' << frequency << ',' << fold(frequency, rate) << ',' << db(std::abs(fundamental))
            << ',' << db(firstImage) << ',' << db(maxImage) << ',' << std::arg(fundamental) << ',' << ns << '\n';
        const double passEdge = rate >= fs ? 19000. : rate * .5 - fs * 8 / Converter::kTaps;
        if (frequency <= passEdge) {
            check(std::abs(db(std::abs(fundamental))) < .002, "streamed passband includes coefficient and phase interpolation error");
            check(db(maxImage) < -85, "streamed first three image pairs below specified rejection bound");
        }
        if (frequency >= rate * .5 && rate < fs)
            check(db(std::abs(fundamental)) < -85, "out-of-band input is removed before downsampling, including 32 kHz host");
    }
}
void latencyAndTail() {
    Converter converter;
    for (double rate : {fs, 8000., 32000., 44100., 48000., 96000., 192000., 384000.}) {
        converter.setStep(fs, rate); converter.reset();
        int input = 0, peakIndex = 0; float peak = 0;
        const int length = static_cast<int>(std::ceil(1024 * rate / fs));
        for (int i = 0; i < length; ++i) {
            while (converter.size() < converter.queueTargetSize()) converter.push(input++ == 0 ? 1.f : 0.f);
            const float y = converter.readInterpolated();
            if (std::abs(y) > peak) { peak = std::abs(y); peakIndex = i; }
        }
        check(std::abs(peakIndex - Converter::groupDelayNativeSamples() * rate / fs) <= 1,
            "impulse peak matches declared causal SRC latency at every host rate");
        converter.reset();
        for (int i = 0; i < 40; ++i) converter.push(i == 0 ? .7f : 0.f);
        for (int i = 0; i < Converter::kTaps; ++i) converter.push(0);
        int drained = 0; float last = 1;
        while (converter.size() >= converter.minimumReadableSize() && drained < 20000) {
            last = converter.readInterpolated(); ++drained;
        }
        check(drained > 0 && drained < 20000 && last == 0, "finite tail drains completely without dropping the impulse or spinning");
        converter.reset();
        for (int i = 0; i < Converter::kTaps; ++i) converter.push(0);
        check(converter.readInterpolated() == 0, "reset removes previous audio exactly");
    }
}
}
int main(int argc, char** argv) {
    const std::string prefix = argc > 1 ? argv[1] : "host-src";
    std::ofstream response(prefix + "-response.csv"), spectrum(prefix + "-tones.csv");
    check(response.good() && spectrum.good(), "measurement outputs open");
    coefficients(response); latencyAndTail(); tones(spectrum);
    std::printf("Host SRC: %d failures\n", failures); return failures ? 1 : 0;
}
