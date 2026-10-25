// Copyright 2026 MontroneDSP. SPDX-License-Identifier: GPL-3.0-or-later
#include "Ir3109SolverReference.h"
#include <chrono>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <iomanip>
#include <numeric>
#include <string>
#include <vector>

namespace {
using Access = swaraxt::Ir3109SolverAccess;
using Core = swaraxt::Ir3109BoardCore;
constexpr double native = 20000000.0 / 510.0;
int failures = 0;
#if SWARAXT_IR3109_SOLVER_TRACE
void check(bool ok, const char* message) {
    if (!ok) { if (failures < 12) std::printf("FAIL: %s\n", message); ++failures; }
}
bool exact(double a, double b) noexcept { return std::memcmp(&a, &b, sizeof(a)) == 0; }
#endif
double quantile(std::vector<double> values, double fraction) {
    if (values.empty()) return 0;
    const auto index = static_cast<size_t>(fraction * static_cast<double>(values.size() - 1));
    std::nth_element(values.begin(), values.begin() + static_cast<std::ptrdiff_t>(index), values.end());
    return values[index];
}
#if SWARAXT_IR3109_SOLVER_TRACE
enum class Wave { impulse, step, sine, saw, square, noise, twoTone, release, silence };
const char* waveName(Wave wave) {
    const char* names[] {"impulse", "step", "sine", "saw", "square", "noise", "two-tone", "release", "silence"};
    return names[static_cast<int>(wave)];
}
float signal(Wave wave, int sample, int length, double rate, double amplitude, uint32_t& seed) noexcept {
    seed ^= seed << 13; seed ^= seed >> 17; seed ^= seed << 5;
    const double t = sample / rate, phase = std::fmod(t * 997, 1.0);
    const double noise = static_cast<double>(seed) / 2147483648.0 - 1;
    double x = 0;
    switch (wave) {
        case Wave::impulse: x = sample == 0 ? 1 : 0; break;
        case Wave::step: x = sample < length / 2 ? 1 : -1; break;
        case Wave::sine: x = std::sin(2 * swaraxt::kPi * 997 * t); break;
        case Wave::saw: x = 2 * phase - 1; break;
        case Wave::square: x = phase < .5 ? 1 : -1; break;
        case Wave::noise: x = noise; break;
        case Wave::twoTone: x = .5 * (std::sin(2 * swaraxt::kPi * 1177 * t) + std::sin(2 * swaraxt::kPi * 7313 * t)); break;
        case Wave::release: x = sample < length / 4 ? noise : 0; break;
        case Wave::silence: break;
    }
    return static_cast<float>(amplitude * x);
}
double rms(const std::vector<double>& x) {
    return x.empty() ? 0 : std::sqrt(std::inner_product(x.begin(), x.end(), x.begin(), 0.0) / static_cast<double>(x.size()));
}

struct Stats {
    std::vector<double> initial, residual, rootError, outputError;
    uint64_t samples = 0, active = 0, stopped = 0, cap = 0, exhausted = 0, iterations = 0;
    uint64_t finalWithinTolerance = 0;
    std::array<uint64_t, 9> iterationHistogram{};
    double worstInternal = 0, maxReferenceResidual = 0, maxState = 0, peak = 0, dcSum = 0, stateError = 0;
    double worstResidual = -1, worstOutput = -1;
    std::string residualCase, outputCase;
};
void matchedStep(Core& board, float input, int budget, Stats& stats, const std::string& key, bool independent, bool referenceHistory = false) {
    auto equation = Access::prepare(board, input);
    const auto before = Access::state(equation.board);
    const auto solution = Access::root(equation);
    check(solution.converged && std::abs(solution.residual) <= 1.e-12, "reference residual is independently bounded");
    stats.maxReferenceResidual = std::max(stats.maxReferenceResidual, std::abs(solution.residual));
    const float output = board.process(input, budget);
    const auto& trace = board.solverTraceForTests();
    check(exact(trace.mixer, equation.mixer) && exact(trace.compensation, equation.compensation)
          && exact(trace.bias, equation.bias), "replayed prefix matches the actual production prefix exactly");
    auto replay = equation.board;
    double raw = 0;
    const float replayOutput = Access::commit(replay, equation, trace.estimate, &raw);
    check(exact(output, replayOutput) && Access::state(replay) == Access::state(board)
          && exact(raw, trace.rawOutput), "preview/commit replay matches actual production states and output exactly");
    check(exact(trace.filtered, equation.evaluate(trace.estimate)), "committed cascade is exactly the frozen-state preview");
    check(Access::state(equation.board) == before, "preview and root finding never mutate their frozen board");
    check(trace.used <= budget && trace.used >= 0, "actual iteration count respects its budget");
    check(std::isfinite(trace.initial) && std::isfinite(trace.estimate) && std::isfinite(trace.lastResidual),
          "actual solver iterates and pre-update residual remain finite");
    for (double state : Access::state(board)) {
        check(std::isfinite(state), "actual committed states are finite before external output clamps");
        stats.maxState = std::max(stats.maxState, std::abs(state));
    }
    check(std::isfinite(trace.rawOutput), "raw board output is finite before clampFinite");
    check(Access::rawStagesFinite(equation, trace.estimate), "raw pole calculations are finite before integrator/output clamps");
    auto reference = equation.board;
    const double referenceOutput = Access::commit(reference, equation, solution.x);
    const auto actualStates = Access::state(board), referenceStates = Access::state(reference);
    for (size_t i = 0; i < 8; ++i) stats.stateError = std::max(stats.stateError, std::abs(actualStates[i] - referenceStates[i]));
    const double jackError = std::abs(static_cast<double>(output) - referenceOutput);
    const double internalError = std::abs(trace.filtered - equation.evaluate(solution.x));
    const double finalResidual = std::abs(equation.evaluate(trace.estimate) - trace.estimate);
    const double distance = std::abs(trace.estimate - solution.x);
    check(distance <= finalResidual + 3.e-12, "unique-root error obeys the monotonic-equation bound");
    check(internalError <= equation.slopeBound() * distance + 3.e-12, "committed-output error obeys the feedback Lipschitz bound");
    stats.worstInternal = std::max(stats.worstInternal, internalError);
    stats.peak = std::max(stats.peak, std::abs(static_cast<double>(output))); stats.dcSum += output;
    ++stats.samples;
    if (trace.bias > 0) {
        ++stats.active; stats.stopped += trace.stopped ? 1u : 0u;
        stats.cap += trace.used == budget ? 1u : 0u;
        stats.exhausted += trace.used == budget && !trace.stopped ? 1u : 0u;
        stats.iterations += static_cast<uint64_t>(trace.used);
        ++stats.iterationHistogram[static_cast<size_t>(trace.used)];
        stats.finalWithinTolerance += finalResidual <= 1.e-9 ? 1u : 0u;
        stats.initial.push_back(std::abs(equation.evaluate(trace.initial) - trace.initial));
        stats.residual.push_back(finalResidual); stats.rootError.push_back(distance); stats.outputError.push_back(jackError);
        if (finalResidual > stats.worstResidual) { stats.worstResidual = finalResidual; stats.residualCase = key; }
        if (jackError > stats.worstOutput) { stats.worstOutput = jackError; stats.outputCase = key; }
    }
    if (independent) {
        check(exact(Access::fixed(equation, budget), trace.estimate), "isolated fixed-point loop reproduces the actual production estimate");
        const auto bisected = Access::root(equation, true);
        check(bisected.converged && std::abs(bisected.residual) <= 1.e-12
              && std::abs(solution.x - bisected.x) <= 2.e-12, "safeguarded Newton agrees with independent bracketing");
        double derivative = 0;
        equation.evaluate(solution.x, &derivative);
        const double numeric = (equation.evaluate(solution.x + 1.e-6) - equation.evaluate(solution.x - 1.e-6)) / 2.e-6;
        check(std::abs(numeric - derivative) <= 2.e-5, "analytic root derivative agrees with central differences");
    }
    // For the four/eight comparison, both budgets start from the same
    // converged trajectory. Separately evolving shipping paths are tested below.
    if (referenceHistory) board = reference;
}
void matrix(int length, const std::string& prefix, bool referenceHistory = true) {
    std::ofstream csv, summary;
    if (!prefix.empty()) {
        csv.open(prefix + "-points.csv"); summary.open(prefix + "-summary.csv");
        check(csv.good() && summary.good(), "measurement streams open outside the source tree");
        csv << std::setprecision(17) << "factor,budget,cv,resonance,amplitude,wave,initial_rms,residual_rms,residual_p99,residual_max,root_error_max,jack_error_rms,jack_error_max,internal_error_max,stopped,cap,exhausted,active,peak,dc,pole_state_error_max\n";
        summary << std::setprecision(17) << "factor,budget,samples,active,stopped,cap,exhausted,mean_iterations,initial_p99,residual_rms,residual_p50,residual_p95,residual_p99,residual_max,root_error_max,jack_error_rms,jack_error_p99,jack_error_max,internal_error_max,reference_residual_max,state_max,peak,pole_state_error_max,final_within_tolerance,iter1,iter2,iter3,iter4,iter5,iter6,iter7,iter8\n";
    }
    for (int factor : {1, 2, 4, 8}) for (int budget : {4, 8}) {
        Stats total;
        for (double cv : {0., 1.25, 2.5, 3.75, 5.}) for (double resonance : {0., .25, .5, .9, 1.})
        for (double amplitude : {1.e-5, .1, 1., 4.}) for (auto wave : {Wave::impulse, Wave::step, Wave::sine, Wave::saw, Wave::square, Wave::noise}) {
            Core board; board.prepare(native * factor); board.setTargets(1000, resonance, 1, cv); board.advanceControls(.1);
            Stats local;
            const std::string key = "cv=" + std::to_string(cv) + " res=" + std::to_string(resonance)
                + " amp=" + std::to_string(amplitude) + " " + waveName(wave);
            uint32_t seed = 0x56b91e27u;
            for (int i = 0; i < length; ++i)
                matchedStep(board, signal(wave, i, length, native * factor, amplitude, seed), budget, local, key + " n=" + std::to_string(i), i == length - 1, referenceHistory);
            total.samples += local.samples; total.active += local.active; total.stopped += local.stopped;
            total.cap += local.cap; total.exhausted += local.exhausted; total.iterations += local.iterations;
            total.finalWithinTolerance += local.finalWithinTolerance;
            for (size_t i = 0; i < total.iterationHistogram.size(); ++i) total.iterationHistogram[i] += local.iterationHistogram[i];
            total.worstInternal = std::max(total.worstInternal, local.worstInternal);
            total.stateError = std::max(total.stateError, local.stateError);
            total.maxReferenceResidual = std::max(total.maxReferenceResidual, local.maxReferenceResidual);
            total.maxState = std::max(total.maxState, local.maxState); total.peak = std::max(total.peak, local.peak);
            if (local.worstResidual > total.worstResidual) { total.worstResidual = local.worstResidual; total.residualCase = local.residualCase; }
            if (local.worstOutput > total.worstOutput) { total.worstOutput = local.worstOutput; total.outputCase = local.outputCase; }
            if (csv.is_open()) csv << factor << ',' << budget << ',' << cv << ',' << resonance << ',' << amplitude << ',' << waveName(wave)
                << ',' << rms(local.initial) << ',' << rms(local.residual) << ',' << quantile(local.residual, .99) << ',' << std::max(0., local.worstResidual)
                << ',' << quantile(local.rootError, 1) << ',' << rms(local.outputError) << ',' << std::max(0., local.worstOutput) << ',' << local.worstInternal
                << ',' << local.stopped << ',' << local.cap << ',' << local.exhausted << ',' << local.active << ',' << local.peak << ',' << local.dcSum / length << ',' << local.stateError << '\n';
            for (auto pair : {std::pair{&total.initial, &local.initial}, std::pair{&total.residual, &local.residual},
                              std::pair{&total.rootError, &local.rootError}, std::pair{&total.outputError, &local.outputError}})
                pair.first->insert(pair.first->end(), pair.second->begin(), pair.second->end());
        }
        std::printf("MATRIX L=%d budget=%d N=%llu stopped=%.3f%% residual RMS=%.6g p99=%.6g max=%.6g jack RMS=%.6g max=%.6g\n",
            factor, budget, static_cast<unsigned long long>(total.samples), 100.0 * static_cast<double>(total.stopped) / static_cast<double>(total.active),
            rms(total.residual), quantile(total.residual, .99), total.worstResidual, rms(total.outputError), total.worstOutput);
        std::printf("  residual worst: %s\n  jack worst: %s\n", total.residualCase.c_str(), total.outputCase.c_str());
        if (summary.is_open()) {
            summary << factor << ',' << budget << ',' << total.samples << ',' << total.active << ',' << total.stopped << ',' << total.cap
            << ',' << total.exhausted << ',' << static_cast<double>(total.iterations) / static_cast<double>(total.active) << ',' << quantile(total.initial, .99)
            << ',' << rms(total.residual) << ',' << quantile(total.residual, .5) << ',' << quantile(total.residual, .95) << ',' << quantile(total.residual, .99)
            << ',' << total.worstResidual << ',' << quantile(total.rootError, 1) << ',' << rms(total.outputError) << ',' << quantile(total.outputError, .99)
            << ',' << total.worstOutput << ',' << total.worstInternal << ',' << total.maxReferenceResidual << ',' << total.maxState << ',' << total.peak << ',' << total.stateError << ',' << total.finalWithinTolerance;
            for (size_t i = 1; i < total.iterationHistogram.size(); ++i) summary << ',' << total.iterationHistogram[i];
            summary << '\n';
        }
    }
}
void transientsAndReset() {
    for (int factor : {1, 2, 4, 8}) for (int budget : {4, 8}) {
        Core maximum; maximum.prepare(native * factor); maximum.setTargets(1000, 1, 1, 5); maximum.advanceControls(.1);
        const auto maximumEquation = Access::prepare(maximum, 1.e-5f);
        const auto maximumRoot = Access::root(maximumEquation);
        const double longIteration = Access::fixed(maximumEquation, 2048, 1.e-13);
        const double slope = maximumEquation.slopeBound();
        std::printf("CONTRACTION L=%d slope=%.9g dampedBound=%.9g rootResidual=%.3g fixed2048Residual=%.6g\n",
            factor, slope, std::max(.28, std::abs(.28 - .72 * slope)), maximumRoot.residual, maximumEquation.evaluate(longIteration) - longIteration);
        if (factor >= 2) check(std::max(.28, std::abs(.28 - .72 * slope)) < 1, "all supported production controls admit a global contraction bound");
        Core board; board.prepare(native * factor);
        Stats stats; uint32_t seed = 123456789;
        for (int i = 0; i < 2048; ++i) {
            if (i % 40 == 0) board.setTargets(1000, (i / 40) % 2 ? 1 : 0, (i / 40) % 3 * .5, (i / 80) % 2 ? 5 : 0);
            matchedStep(board, signal(Wave::twoTone, i, 2048, native * factor, 4, seed), budget, stats, "abrupt CV/resonance/VCA", i % 257 == 0);
        }
        board.setTargets(1000, .95, .25, 5); board.reset();
        const Core initial = board;
        std::vector<float> first;
        for (int i = 0; i < 512; ++i) first.push_back(board.process(i == 0 ? 1.f : 0.f, budget));
        board.reset();
        check(Access::state(board) == Access::state(initial), "actual reset clears board histories and controls deterministically");
        for (int i = 0; i < 512; ++i) check(exact(first[static_cast<size_t>(i)], board.process(i == 0 ? 1.f : 0.f, budget)), "board reset/reinitialization is deterministic");
        board.prepare(native * factor);
        for (int i = 0; i < 512; ++i) check(exact(first[static_cast<size_t>(i)], board.process(i == 0 ? 1.f : 0.f, budget)), "preparation reproduces the same reset sequence");
        // A reference above eight iterations is checked rather than assumed.
        auto equation = Access::prepare(board, .1f);
        const auto solution = Access::root(equation);
        const double iterated = Access::fixed(equation, 2048, 1.e-13);
        std::printf("REFERENCE L=%d budget=%d rootResidual=%.3g fixed2048Residual=%.3g slopeBound=%.6g\n", factor, budget,
            solution.residual, equation.evaluate(iterated) - iterated, equation.slopeBound());
        if (factor >= 2) check(std::max(.28, std::abs(.28 - .72 * equation.slopeBound())) < 1, "production-rate damping has a global contraction bound");
        for (double hz : {12000., 19000.}) {
            Core highFrequency; highFrequency.prepare(native * factor);
            highFrequency.setTargets(1000, 1, 1, 5); highFrequency.advanceControls(.1);
            Stats high;
            for (int i = 0; i < 1024; ++i)
                matchedStep(highFrequency, static_cast<float>(std::sin(2 * swaraxt::kPi * hz * i / (native * factor))),
                    budget, high, "high-frequency sine", i == 1023, true);
            std::printf("HIGH_FREQUENCY L=%d budget=%d hz=%.0f residualRMS=%.6g jackRMS=%.6g jackMax=%.6g\n",
                factor, budget, hz, rms(high.residual), rms(high.outputError), high.worstOutput);
        }
    }
}
void trajectories(int length, const std::string& prefix, int onlyScene = -1) {
    std::ofstream csv;
    if (!prefix.empty()) {
        csv.open(prefix + "-trajectories.csv");
        csv << std::setprecision(17) << "factor,budget,scene,output_rms,reference_rms,error_rms,error_peak,peak,dc,reference_residual_max,tail_rms,reference_comparison_rms\n";
    }
    for (int factor : {1, 2, 4, 8}) for (int budget : {4, 8}) for (int scene = 0; scene < 8; ++scene) {
        if (onlyScene >= 0 && scene != onlyScene) continue;
        swaraxt::SwaraXtFilter actual; actual.prepare(native); actual.setOversampleFactorForTests(factor); actual.setSolverIterationLimitForTests(budget);
        swaraxt::Ir3109ReferenceFilter replay, reference, secondReference;
        replay.prepare(factor, budget, false); reference.prepare(factor, budget, true);
        secondReference.prepare(factor, budget, true, true, 1.e-14);
        const bool compareReferences = (factor == 2 || factor == 4) && scene <= 4;
        std::vector<double> outputs, references, errors, referenceDifferences;
        std::ofstream audio;
        if (!prefix.empty() && (factor == 2 || factor == 4) && budget == 4 && (scene == 0 || scene == 3))
            audio.open(prefix + "-audio-L" + std::to_string(factor) + "-S" + std::to_string(scene) + ".f32", std::ios::binary);
        uint32_t seed = 0x98765431u;
        for (int i = 0; i < length; ++i) {
            const double cv = scene == 1 ? ((i / 40) % 2 ? 5 : 0) : scene == 3 || scene == 6 ? 3.75 : 5;
            const float resonance = scene == 2 ? ((i / 40) % 2 ? 1.f : 0.f) : scene == 5 || scene == 6 ? .5f : 1.f;
            const float vca = i < length / 8 ? 0.f : i < length / 4 ? .25f : 1.f;
            float input = signal(scene == 0 ? Wave::saw : scene == 3 ? Wave::release : scene == 4 || scene == 7 ? Wave::silence : scene == 5 ? Wave::sine : scene == 6 ? Wave::noise : Wave::twoTone, i, length, native, 1, seed);
            if (scene == 4 && i == 0) input = 1.e-4f;
            swaraxt::SwaraXtFilterParams params; params.boardCutoffCvVolts = cv; params.resonance = resonance; actual.setParams(params);
            const double y = actual.processInstrumentSample(input, vca);
            const double same = replay.process(input, cv, resonance, vca);
            const double converged = reference.process(input, cv, resonance, vca);
            if (compareReferences) referenceDifferences.push_back(converged - secondReference.process(input, cv, resonance, vca));
            check(exact(y, same), "offline wrapper preserves production FIR, priming and control-delay behavior exactly");
            check(std::isfinite(y) && std::isfinite(converged), "production and reference trajectories remain finite");
            for (double state : Access::state(actual.boardCoreForTests())) check(std::isfinite(state), "full-filter committed states remain finite");
            outputs.push_back(y); references.push_back(converged); errors.push_back(y - converged);
            if (audio.is_open()) { const float stereo[] {static_cast<float>(y), static_cast<float>(converged)}; audio.write(reinterpret_cast<const char*>(stereo), sizeof(stereo)); }
        }
        check(reference.referenceFailures() == 0 && reference.maximumResidual() <= 1.e-12, "every evolving reference sample meets residual tolerance");
        check(secondReference.referenceFailures() == 0, "independent long-run reference meets its own stricter residual tolerance");
        if (scene == 7) check(rms(outputs) == 0 && rms(references) == 0, "unexcited silence stays exactly silent at maximum resonance");
        const double peak = std::max(std::abs(*std::min_element(outputs.begin(), outputs.end())), std::abs(*std::max_element(outputs.begin(), outputs.end())));
        const double peakError = std::max(std::abs(*std::min_element(errors.begin(), errors.end())), std::abs(*std::max_element(errors.begin(), errors.end())));
        const double dc = std::accumulate(outputs.begin(), outputs.end(), 0.0) / length;
        const std::vector<double> tail(outputs.begin() + length * 3 / 4, outputs.end());
        std::printf("TRAJECTORY L=%d budget=%d scene=%d rms=%.6g error=%.6g peakError=%.6g tail=%.6g\n", factor, budget, scene, rms(outputs), rms(errors), peakError, rms(tail));
        if (csv.is_open()) {
            csv << factor << ',' << budget << ',' << scene << ',' << rms(outputs) << ',' << rms(references) << ',' << rms(errors)
                << ',' << peakError << ',' << peak << ',' << dc << ',' << reference.maximumResidual() << ',' << rms(tail) << ',';
            if (compareReferences) csv << rms(referenceDifferences); else csv << "NA";
            csv << '\n';
        }
    }
}
#else
volatile double benchmarkSink = 0;
template<class Function> double timeLoop(Function function, int count) {
    std::vector<double> timings;
    for (int repeat = 0; repeat < 7; ++repeat) {
        double sum = 0;
        const auto start = std::chrono::steady_clock::now();
        for (int i = 0; i < count; ++i) sum += function(i);
        const double elapsed = std::chrono::duration<double, std::nano>(std::chrono::steady_clock::now() - start).count();
        benchmarkSink = sum;
        timings.push_back(elapsed / count);
    }
    return quantile(timings, .5);
}
void benchmark() {
    std::vector<float> input(16384);
    for (size_t i = 0; i < input.size(); ++i) input[i] = static_cast<float>(std::sin(static_cast<double>(i) * .3));
    std::printf("factor,budget,cv,resonance,modulated,solver_ns_per_board_tick,board_ns_per_tick,fir_ns_per_native,filter_ns_per_native\n");
    for (int factor : {1, 2, 4, 8}) for (int budget : {4, 8}) for (double cv : {2.5, 5.}) for (double resonance : {0., .5, 1.}) for (int modulation : {0, 1}) {
        if (modulation != 0 && factor != 2 && factor != 4) continue;
        std::vector<float> boardInput(input.size());
        for (size_t i = 0; i < boardInput.size(); ++i)
            boardInput[i] = static_cast<float>(std::sin(static_cast<double>(i) * .3 / factor));
        Core core; core.prepare(native * factor); core.setTargets(1000, resonance, 1, cv); core.advanceControls(.1);
        std::vector<Access::Equation> equations;
        for (int i = 0; i < 256; ++i) {
            if (modulation != 0 && i % (40 * factor) == 0) core.setTargets(1000, resonance, 1, (i / (40 * factor)) % 2 ? 5 : 2.5);
            const float x = boardInput[static_cast<size_t>(i)]; equations.push_back(Access::prepare(core, x)); core.process(x, budget);
        }
        const double solver = timeLoop([&](int i) { return Access::fixed(equations[static_cast<size_t>(i % 256)], budget); }, 16384);
        const double board = timeLoop([&](int i) {
            if (modulation != 0 && i % (40 * factor) == 0) core.setTargets(1000, resonance, 1, (i / (40 * factor)) % 2 ? 5 : 2.5);
            return core.process(boardInput[static_cast<size_t>(i)], budget);
        }, 16384);
        swaraxt::FilterRateConverter converter; converter.prepare(factor);
        const double fir = timeLoop([&](int i) { return converter.process(input[static_cast<size_t>(i)], [](float x) { return x; }); }, 16384);
        swaraxt::SwaraXtFilter filter; filter.prepare(native); filter.setOversampleFactorForTests(factor); filter.setSolverIterationLimitForTests(budget);
        swaraxt::SwaraXtFilterParams params; params.boardCutoffCvVolts = cv; params.resonance = static_cast<float>(resonance); filter.setParams(params);
        const double full = timeLoop([&](int i) {
            if (modulation != 0 && i % 40 == 0) { params.boardCutoffCvVolts = (i / 40) % 2 ? 5 : 2.5; filter.setParams(params); }
            return filter.processInstrumentSample(input[static_cast<size_t>(i)], 1);
        }, 16384);
        std::printf("%d,%d,%.1f,%.1f,%d,%.3f,%.3f,%.3f,%.3f\n", factor, budget, cv, resonance, modulation, solver, board, fir, full);
    }
}
#endif
}
int main(int argc, char** argv) {
#if SWARAXT_IR3109_SOLVER_TRACE
    const bool measure = argc > 1;
    const std::string prefix = measure ? argv[1] : "";
    const std::string mode = argc > 2 ? argv[2] : "";
    if (mode != "--trajectories-only") { matrix(measure ? 1024 : 32, prefix, mode != "--shipping-history"); transientsAndReset(); }
    if (mode != "--matrix-only" && mode != "--shipping-history")
        trajectories(measure ? 65536 : 2048, prefix, argc > 3 ? std::stoi(argv[3]) : -1);
    std::printf("Production IR3109 solver verification: %d failures\n", failures);
    return failures ? 1 : 0;
#else
    static_cast<void>(argc); static_cast<void>(argv);
    benchmark();
    return failures ? 1 : 0;
#endif
}
