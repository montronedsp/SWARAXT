// Copyright 2026 MontroneDSP. SPDX-License-Identifier: GPL-3.0-or-later
#include "Engine/Filter/SwaraXtFilter.h"
#include <complex>
#include <cstdio>
#include <fstream>
#include <vector>

namespace {
constexpr double native = 20000000.0 / 510.0;
int failures = 0;
void check(bool ok, const char* what) { if (!ok) { ++failures; std::printf("FAIL: %s\n", what); } }
double db(double x) { return 20 * std::log10(std::max(std::abs(x), 1.e-15)); }
void cvSteps(std::ostream& csv) {
    csv << "factor,seconds,cutoff_cv,resonance_cv,vca_cv\n";
    using Core = swaraxt::Ir3109BoardCore;
    for (int factor : {2, 4, 8}) {
        Core core; const double rate = native * factor; core.prepare(rate);
        core.setTargets(20000, 1, 1);
        int firstTen = -1, firstNinety = -1;
        double maxError = 0;
        for (int i = 0; i < static_cast<int>(rate * .003); ++i) {
            core.process(0, 8);
            const double seconds = (i + 1) / rate;
            const double cutoffTarget = 5 * 254.0 / 255;
            maxError = std::max(maxError, std::abs(core.cutoffCv() / cutoffTarget - (1 - std::exp(-seconds / Core::kCutoffCvTau))));
            maxError = std::max(maxError, std::abs(core.resonanceCv() / 5 - (1 - std::exp(-seconds / Core::kResonanceCvTau))));
            maxError = std::max(maxError, std::abs(core.vcaControl() - (1 - std::exp(-seconds / Core::kVcaCvTau))));
            if (firstTen < 0 && core.vcaControl() >= .1) firstTen = i;
            if (firstNinety < 0 && core.vcaControl() >= .9) firstNinety = i;
            csv << factor << ',' << seconds << ',' << core.cutoffCv() << ',' << core.resonanceCv() << ',' << core.vcaControl() * 5 << '\n';
        }
        const double rise = (firstNinety - firstTen) / rate;
        std::printf("IR3109 %dx CV max error %.3g VCA10-90 %.3f us\n", factor, maxError, rise * 1.e6);
        check(maxError < 1.e-12, "all three CV responses match independently calculated component time constants");
        check(std::abs(rise - Core::kVcaCvTau * std::log(9.0)) <= 1 / rate, "VCA rise time follows 10k/33n, not old 1250 Hz");
        Core idle; idle.prepare(rate); idle.setTargets(20000, 1, 1); idle.advanceControls(.003);
        check(std::abs(idle.vcaControl() - (1 - std::exp(-.003 / Core::kVcaCvTau))) < 1.e-12, "dormant controls retain physical time units");
    }
}
std::complex<double> project(const std::vector<float>& x, double frequency) {
    std::complex<double> sum{}; double weight = 0;
    for (size_t i = 0; i < x.size(); ++i) {
        const double w = .5 - .5 * std::cos(2 * swaraxt::kPi * static_cast<double>(i) / static_cast<double>(x.size() - 1));
        sum += static_cast<double>(x[i]) * w * std::polar(1., -2 * swaraxt::kPi * frequency * static_cast<double>(i) / native);
        weight += w;
    }
    return sum * (2 / weight);
}
void audioMatrix(std::ostream& csv) {
    csv << "factor,frequency_hz,amplitude,resonance,gain_db,fundamental_dbfs,third_fold_dbfs,third_fold_dbc,harmonics2to9_dbc,windowed_dc,peak\n";
    for (int factor : {2, 4, 8}) for (double hz : {1000., 5000., 10000., 12000., 15000., 18000., 19000.})
    for (float amplitude : {.001f, .5f, 1.f}) for (float resonance : {0.f, .5f, .95f}) {
        swaraxt::SwaraXtFilter filter; filter.prepare(native); filter.setOversampleFactorForTests(factor);
        swaraxt::SwaraXtFilterParams params; params.cutoffHz = 20000; params.resonance = resonance; filter.setParams(params);
        std::vector<float> audio; audio.reserve(16384); double peak = 0;
        for (int i = 0; i < 32768; ++i) {
            const float input = static_cast<float>(amplitude * std::sin(2 * swaraxt::kPi * hz * i / native));
            const float y = filter.processInstrumentSample(input, 1, 1);
            check(std::isfinite(y) && std::abs(y) <= 8, "complete IR3109 signal finite and bounded");
            if (i >= 16384) { audio.push_back(y); peak = std::max(peak, std::abs(static_cast<double>(y))); }
        }
        const auto fund = std::abs(project(audio, hz));
        const auto third = std::abs(project(audio, std::abs(std::remainder(3 * hz, native))));
        double harmonics = 0;
        for (int h = 2; h <= 9; ++h) harmonics += std::norm(project(audio, std::abs(std::remainder(h * hz, native))));
        const double dc = std::abs(project(audio, 0)) * .5;
        csv << factor << ',' << hz << ',' << amplitude << ',' << resonance << ',' << db(fund / amplitude) << ',' << db(fund)
            << ',' << db(third) << ',' << db(third / fund) << ',' << db(std::sqrt(harmonics) / fund) << ',' << dc << ',' << peak << '\n';
        check(dc < .001, "IR3109 weighted DC remains bounded under resonance");
        if (hz == 12000 && amplitude == 1 && resonance == 0) {
            std::printf("IR3109 %dx 12k gain %.3f third %.3f dBc harmonic sum %.3f dBc\n", factor, db(fund), db(third / fund), db(std::sqrt(harmonics) / fund));
            check(db(third / fund) < -70, "board path retains suppression of cited 12 kHz third-order fold");
        }
    }
}
void lowLevelAndTail() {
    using Core = swaraxt::Ir3109BoardCore;
    swaraxt::SwaraXtFilter filter; filter.prepare(native);
    swaraxt::SwaraXtFilterParams p; p.cutoffHz = 20000; filter.setParams(p);
    std::vector<float> audio;
    for (int i = 0; i < 32768; ++i) {
        const float output = filter.processInstrumentSample(static_cast<float>(1.e-4 * std::sin(2 * swaraxt::kPi * 1000 * i / native)), 1, 1);
        if (i >= 16384) audio.push_back(output);
    }
    const double measured = std::abs(project(audio, 1000)) / 1.e-4;
    const double dcGain = .68 * (5.0 / 33000) * 10000 * Core::kVcaDivider / (2 * swaraxt::kOtaThermalVoltageVolts) * (68000.0 / 33000);
    const double fc = Core::cutoffHzFromCv(5 * 254.0 / 255);
    const double expected = dcGain / std::pow(1 + std::pow(1000 / fc, 2), 2)
        / (1 + std::pow(2 * swaraxt::kPi * 1000 * Core::kInputPoleTau, 2));
    std::printf("IR3109 1k small-signal measured %.6f schematic estimate %.6f\n", measured, expected);
    check(std::abs(measured / expected - 1) < .015, "small-signal gain agrees with independently assembled resistor/OTA transfer");
    int drain = 0;
    while (filter.instrumentTailActive() && drain < static_cast<int>(native * 5)) {
        const auto y = filter.processInstrumentSample(0, 0, 1);
        check(std::isfinite(y), "release output is finite"); ++drain;
    }
    check(drain > swaraxt::FilterRateConverter::kSpan, "release drains analog and FIR tails");
    check(drain < native * 5, "release reaches dormant threshold within five seconds");
    filter.reset(); check(filter.processInstrumentSample(0, 0, 1) == 0, "hard reset is silent");
}
void resetAfterDifferentPatch() {
    swaraxt::SwaraXtFilter filter; filter.prepare(native);
    const auto render = [&filter](float cutoff, float resonance) {
        filter.reset();
        swaraxt::SwaraXtFilterParams params;
        params.cutoffHz = cutoff; params.resonance = resonance;
        filter.setParams(params);
        std::vector<float> audio(2048);
        for (size_t i = 0; i < audio.size(); ++i)
            audio[i] = filter.processInstrumentSample(i == 0 ? 1.f : .1f, 1, 1);
        return audio;
    };
    const auto first = render(700, .55f);
    render(17000, .95f);
    const auto repeated = render(700, .55f);
    check(first == repeated, "reset before committing patch clears prior audio and CV prehistory exactly");
}
void explicitByteCv() {
    swaraxt::SwaraXtFilter filter; filter.prepare(native);
    for (int byte = 0; byte <= 255; ++byte) {
        filter.reset();
        swaraxt::SwaraXtFilterParams params;
        params.cutoffHz = 10; // Deliberately different RAW/diagnostic mapping.
        params.boardCutoffCvVolts = byte * (5.0 / 255.0);
        filter.setParams(params);
        filter.processInstrumentSample(0, 0, 1);
        check(std::abs(filter.boardCoreForTests().cutoffCv() - params.boardCutoffCvVolts) < 1.e-12,
            "all 256 firmware CV codes reach hardware domain independently of RAW Hz limits");
    }
}
}
int main(int argc, char** argv) {
    const std::string prefix = argc > 1 ? argv[1] : "ir3109-board";
    std::ofstream cv(prefix + "-cv.csv"), audio(prefix + "-audio.csv");
    check(cv.good() && audio.good(), "measurement outputs open");
    cvSteps(cv); lowLevelAndTail(); resetAfterDifferentPatch(); explicitByteCv(); audioMatrix(audio);
    std::printf("IR3109 board: %d failures\n", failures); return failures ? 1 : 0;
}
