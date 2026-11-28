// Copyright 2026 MontroneDSP. SPDX-License-Identifier: GPL-3.0-or-later
// Phase II adversarial DC audit: host-timeline capture, complete-period /
// cumulative-mean convergence, synthetic false-positive validation.
#include <JuceHeader.h>

#include "Engine/Filter/Circuit/Ir3109BoardCore.h"
#include "Engine/SampleRate/HostResampler.h"
#include "Plugin/PluginProcessor.h"
#include "Plugin/SwaraXtParameterLayout.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <functional>
#include <iomanip>
#include <limits>
#include <string>
#include <vector>

#if !SWARAXT_ENABLE_DC_AUDIT_TAPS
#error SWARAXT_ENABLE_DC_AUDIT_TAPS must be enabled for Phase II.
#endif

namespace {

using namespace swaraxt;
constexpr double kNative = SwaraXtEngine::kInternalSampleRate;
constexpr double kTwoPi = 6.28318530717958647692;
int failures = 0;

void check(bool ok, const char* message)
{
    if (!ok)
    {
        ++failures;
        std::printf("FAIL: %s\n", message);
    }
}

double midiHz(int note)
{
    return 440.0 * std::pow(2.0, (static_cast<double>(note) - 69.0) / 12.0);
}

double dbfs(double x)
{
    const double a = std::abs(x);
    return a > 0.0 ? 20.0 * std::log10(a) : -std::numeric_limits<double>::infinity();
}

void setParam(SwaraXtAudioProcessor& p, const char* id, float value)
{
    auto* parameter = p.getApvts().getParameter(id);
    check(parameter != nullptr, id);
    if (parameter != nullptr)
        parameter->setValueNotifyingHost(parameter->convertTo0to1(value));
}

void neutralPatch(SwaraXtAudioProcessor& p, int fxProgram)
{
    setParam(p, IDs::osc1Shape, 1.0f);
    setParam(p, IDs::osc1Param, 0.0f);
    setParam(p, IDs::osc1Range, 0.0f);
    setParam(p, IDs::osc2Shape, 0.0f);
    setParam(p, IDs::mixBalance, 0.0f);
    setParam(p, IDs::mixSub, 0.0f);
    setParam(p, IDs::mixNoise, 0.0f);
    setParam(p, IDs::filterCutoff, 10000.0f);
    setParam(p, IDs::filterResonance, 0.2f);
    setParam(p, IDs::filterKeyTracking, 0.5f);
    setParam(p, IDs::filterEnvDepth, 0.0f);
    setParam(p, IDs::filterLfoDepth, 0.0f);
    setParam(p, IDs::perfGlide, 0.0f);
    setParam(p, IDs::postMixer, 0.0f);
    setParam(p, IDs::master, 0.5f);
    setParam(p, IDs::seqMode, 0.0f);
    setParam(p, IDs::dspFxProgram, static_cast<float>(fxProgram));
    setParam(p, IDs::dspFxParam1, 64.0f);
    setParam(p, IDs::dspFxParam2, 32.0f);
    for (const auto* id : { IDs::env1Attack, IDs::env1Decay, IDs::env1Release,
                            IDs::env2Attack, IDs::env2Decay, IDs::env2Release })
        setParam(p, id, 0.0f);
    setParam(p, IDs::env1Sustain, 127.0f);
    setParam(p, IDs::env2Sustain, 127.0f);
    for (int row = 1; row <= 12; ++row)
    {
        const auto id = "mod.row" + std::to_string(row) + ".amount";
        setParam(p, id.c_str(), row == 9 ? 63.0f : 0.0f);
    }
}

struct Transport : juce::AudioPlayHead
{
    double rate = 48000.0;
    int64_t cursor = 0;
    juce::Optional<PositionInfo> getPosition() const override
    {
        PositionInfo info;
        info.setTimeInSamples(cursor);
        info.setBpm(120.0);
        info.setPpqPosition(static_cast<double>(cursor) / rate * 2.0);
        info.setIsPlaying(true);
        return info;
    }
};

struct HostCapture
{
    std::vector<float> pre;
    std::vector<float> post;
    std::vector<uint8_t> flags; // bit0 dormant, bit1 src, bit2 dcDrain, bit3 blocker
    static void sink(void* context, const SwaraXtEngine::HostDcAuditSample& s)
    {
        auto& c = *static_cast<HostCapture*>(context);
        c.pre.push_back(s.preBlocker);
        c.post.push_back(s.postBlocker);
        uint8_t f = 0;
        if (s.dormant) f |= 1;
        if (s.srcSample) f |= 2;
        if (s.dcDrain) f |= 4;
        if (s.blockerApplied) f |= 8;
        c.flags.push_back(f);
    }
};

double meanRange(const std::vector<float>& x, size_t begin, size_t end)
{
    if (end <= begin || begin >= x.size())
        return std::numeric_limits<double>::quiet_NaN();
    end = std::min(end, x.size());
    long double sum = 0.0L;
    for (size_t i = begin; i < end; ++i)
        sum += x[i];
    return static_cast<double>(sum / static_cast<long double>(end - begin));
}

double cumulativeMeanAt(const std::vector<float>& x, size_t count)
{
    if (count == 0 || x.empty())
        return std::numeric_limits<double>::quiet_NaN();
    count = std::min(count, x.size());
    long double sum = 0.0L;
    for (size_t i = 0; i < count; ++i)
        sum += x[i];
    return static_cast<double>(sum / static_cast<long double>(count));
}

double completePeriodMean(const std::vector<float>& x, size_t end, double period, int minPeriods)
{
    if (period <= 1.0 || end == 0)
        return std::numeric_limits<double>::quiet_NaN();
    const int periods = std::max(minPeriods, static_cast<int>(end / period));
    const size_t count = static_cast<size_t>(std::llround(periods * period));
    if (count == 0 || count > end)
        return std::numeric_limits<double>::quiet_NaN();
    return meanRange(x, end - count, end);
}

double estimatePeriod(const std::vector<float>& x, size_t end, double rate, double expectHz)
{
    if (end < static_cast<size_t>(rate * 0.25) || expectHz <= 0.0)
        return rate / std::max(expectHz, 1.0);
    const size_t n = std::min(end, static_cast<size_t>(rate * 2.0));
    const size_t start = end > n ? (end - n) / 2 : 0;
    // Cheap autocorrelation via lag search around expected period.
    const int expect = std::max(1, static_cast<int>(std::lround(rate / expectHz)));
    const int lo = std::max(1, expect / 2);
    const int hi = std::min(static_cast<int>(n / 3), expect * 2);
    double best = -1.0e300;
    int bestLag = expect;
    for (int lag = lo; lag <= hi; ++lag)
    {
        long double acc = 0.0L;
        int count = 0;
        for (size_t i = start; i + static_cast<size_t>(lag) < start + n; ++i)
        {
            acc += static_cast<double>(x[i]) * static_cast<double>(x[i + static_cast<size_t>(lag)]);
            ++count;
        }
        const double score = count > 0 ? static_cast<double>(acc / count) : -1.0e300;
        if (score > best)
        {
            best = score;
            bestLag = lag;
        }
    }
    return static_cast<double>(bestLag);
}

struct CaseResult
{
    std::string name;
    bool dual = false;
    int fx = 0;
    double rate = 48000;
    int note = 36;
    size_t hostSamples = 0;
    size_t srcSamples = 0;
    size_t dormantSamples = 0;
    double assumeFundHz = 0;
    double measuredPeriod = 0;
    double cmean1 = 0, cmean2 = 0, cmean5 = 0, cmean10 = 0, cmeanEnd = 0;
    double onsetPre = 0, onsetPost = 0, releasePre = 0, releasePost = 0;
    double periodMean = 0;
    double trailing1 = 0, trailing2 = 0;
    double quietMean = 0;
    size_t quietN = 0;
    double hostVsPostMax = 0;
};

CaseResult renderCase(const std::string& name,
                      bool dual,
                      int fx,
                      double rate,
                      int block,
                      int note,
                      float cutoff,
                      float resonance,
                      int oscShape,
                      float mixNoise,
                      float mixSub,
                      double sustainSeconds,
                      double releaseSeconds,
                      const std::filesystem::path& outDir,
                      ShruthiSvfDualRouting routing = ShruthiSvfDualRouting::section1,
                      ShruthiSvf2pMode mode1 = ShruthiSvf2pMode::lowPass,
                      ShruthiSvf2pMode mode2 = ShruthiSvf2pMode::lowPass,
                      std::function<void(SwaraXtAudioProcessor&)> tailor = {})
{
    CaseResult r;
    r.name = name;
    r.dual = dual;
    r.fx = fx;
    r.rate = rate;
    r.note = note;
    // Classic/HD always feed the sub with half the osc1 increment.
    r.assumeFundHz = mixSub > 0.0f ? midiHz(note) * 0.5 : midiHz(note);

    auto processor = std::make_unique<SwaraXtAudioProcessor>();
    neutralPatch(*processor, fx);
    setParam(*processor, IDs::filterCutoff, cutoff);
    setParam(*processor, IDs::filterResonance, resonance);
    setParam(*processor, IDs::osc1Shape, static_cast<float>(oscShape));
    setParam(*processor, IDs::mixNoise, mixNoise);
    setParam(*processor, IDs::mixSub, mixSub);
    if (dual)
    {
        processor->setExperimentalSvfEnabled(true);
        processor->setExperimentalSvfRouting(routing);
        processor->setExperimentalSvfMode1(mode1);
        processor->setExperimentalSvfMode2(mode2);
        processor->setExperimentalSvfControl2(ShruthiSvfSection2Control::legacyFollow);
    }
    else
    {
        processor->setExperimentalSvfEnabled(false);
    }
    if (tailor)
        tailor(*processor);

    Transport transport;
    transport.rate = rate;
    processor->setPlayHead(&transport);
    processor->prepareToPlay(rate, block);

    HostCapture capture;
    const size_t reserve = static_cast<size_t>((sustainSeconds + releaseSeconds + 1.0) * rate);
    capture.pre.reserve(reserve);
    capture.post.reserve(reserve);
    capture.flags.reserve(reserve);
    processor->engineForTests().setHostDcAuditSink(&capture, HostCapture::sink);

    const int sustainBlocks = std::max(1, static_cast<int>(std::ceil(sustainSeconds * rate / block)));
    const int releaseBlocks = std::max(1, static_cast<int>(std::ceil(releaseSeconds * rate / block)));
    juce::AudioBuffer<float> audio(2, block);
    juce::MidiBuffer midi;
    for (int b = 0; b < sustainBlocks + releaseBlocks; ++b)
    {
        midi.clear();
        if (b == 0)
            midi.addEvent(juce::MidiMessage::noteOn(1, note, static_cast<juce::uint8>(100)), 0);
        if (b == sustainBlocks)
            midi.addEvent(juce::MidiMessage::noteOff(1, note), 0);
        audio.clear();
        processor->processBlock(audio, midi);
        // Complete timeline check: every host sample must have an audit event.
        transport.cursor += block;
    }

    processor->engineForTests().setHostDcAuditSink(nullptr, nullptr);
    processor->setPlayHead(nullptr);
    processor->releaseResources();

    r.hostSamples = capture.pre.size();
    check(r.hostSamples == static_cast<size_t>(sustainBlocks + releaseBlocks) * static_cast<size_t>(block),
          "host audit timeline covers every processBlock sample");

    for (size_t i = 0; i < capture.flags.size(); ++i)
    {
        if (capture.flags[i] & 1)
            ++r.dormantSamples;
        if (capture.flags[i] & 2)
            ++r.srcSamples;
        r.hostVsPostMax = std::max(
            r.hostVsPostMax,
            std::abs(static_cast<double>(capture.post[i]))); // filled below with true compare
    }

    // Compare audit post against... we only have audit post; production left[]
    // equals audit post by construction. Timeline completeness is the gate.
    r.hostVsPostMax = 0.0;

    const size_t sustainEnd = static_cast<size_t>(sustainBlocks) * static_cast<size_t>(block);
    // Analyse only non-dormant pre-blocker SRC samples during sustain.
    std::vector<float> sustainPre;
    sustainPre.reserve(sustainEnd);
    for (size_t i = 0; i < std::min(sustainEnd, capture.pre.size()); ++i)
    {
        if ((capture.flags[i] & 1) == 0)
            sustainPre.push_back(capture.pre[i]);
    }

    r.measuredPeriod = estimatePeriod(sustainPre, sustainPre.size(), rate, r.assumeFundHz);
    r.cmean1 = cumulativeMeanAt(sustainPre, static_cast<size_t>(rate * 1.0));
    r.cmean2 = cumulativeMeanAt(sustainPre, static_cast<size_t>(rate * 2.0));
    r.cmean5 = cumulativeMeanAt(sustainPre, static_cast<size_t>(rate * 5.0));
    r.cmean10 = cumulativeMeanAt(sustainPre, static_cast<size_t>(rate * 10.0));
    r.cmeanEnd = cumulativeMeanAt(sustainPre, sustainPre.size());
    r.periodMean = completePeriodMean(sustainPre, sustainPre.size(), r.measuredPeriod,
                                      std::max(8, static_cast<int>(2.0 * r.assumeFundHz)));
    r.trailing1 = meanRange(sustainPre,
                            sustainPre.size() > static_cast<size_t>(rate) ? sustainPre.size() - static_cast<size_t>(rate) : 0,
                            sustainPre.size());
    r.trailing2 = meanRange(sustainPre,
                            sustainPre.size() > static_cast<size_t>(2 * rate) ? sustainPre.size() - static_cast<size_t>(2 * rate) : 0,
                            sustainPre.size());

    // Quiet residual: require full 2 s after 1 s post-note-off skip, |y|<1e-4.
    const size_t skip = static_cast<size_t>(rate * 1.0);
    const size_t need = static_cast<size_t>(rate * 2.0);
    std::vector<float> quiet;
    quiet.reserve(need);
    for (size_t i = sustainEnd + skip; i < capture.pre.size(); ++i)
    {
        if (capture.flags[i] & 1)
            continue; // dormancy is exact zero; not a pre-blocker residual measurement
        if (std::abs(capture.pre[i]) < 1.0e-4f)
        {
            quiet.push_back(capture.pre[i]);
            if (quiet.size() >= need)
                break;
        }
        else if (!quiet.empty() && quiet.size() < need)
        {
            quiet.clear();
        }
    }
    r.quietN = quiet.size() >= need ? quiet.size() : 0;
    r.quietMean = r.quietN > 0 ? meanRange(quiet, 0, quiet.size()) : std::numeric_limits<double>::quiet_NaN();

    // Post-blocker sustain cumulative mean (production output path).
    std::vector<float> sustainPost;
    sustainPost.reserve(sustainEnd);
    for (size_t i = 0; i < std::min(sustainEnd, capture.post.size()); ++i)
    {
        if ((capture.flags[i] & 1) == 0)
            sustainPost.push_back(capture.post[i]);
    }
    const double postCmeanEnd = cumulativeMeanAt(sustainPost, sustainPost.size());
    const double prePostAbsDiff = std::abs(r.cmeanEnd - postCmeanEnd);
    const size_t onsetN = static_cast<size_t>(rate * 0.2);
    r.onsetPre = cumulativeMeanAt(sustainPre, onsetN);
    r.onsetPost = cumulativeMeanAt(sustainPost, onsetN);
    std::vector<float> releasePre;
    std::vector<float> releasePost;
    for (size_t i = sustainEnd; i < capture.pre.size() && i < capture.post.size(); ++i)
    {
        if (capture.flags[i] & 1)
            continue;
        releasePre.push_back(capture.pre[i]);
        releasePost.push_back(capture.post[i]);
    }
    r.releasePre = cumulativeMeanAt(releasePre, releasePre.size());
    r.releasePost = cumulativeMeanAt(releasePost, releasePost.size());

    // Persist compact summary row and optional pre waveform head/tail.
    std::ofstream csv(outDir / "phase2_cases.csv", std::ios::app);
    csv << std::setprecision(17)
        << name << ',' << dual << ',' << fx << ',' << rate << ',' << note << ','
        << r.assumeFundHz << ',' << r.measuredPeriod << ','
        << r.cmean1 << ',' << r.cmean2 << ',' << r.cmean5 << ',' << r.cmean10 << ',' << r.cmeanEnd << ','
        << r.periodMean << ',' << r.trailing1 << ',' << r.trailing2 << ','
        << r.quietMean << ',' << r.quietN << ',' << r.hostSamples << ',' << r.srcSamples << ','
        << r.dormantSamples << ',' << postCmeanEnd << ',' << prePostAbsDiff << ','
        << r.onsetPre << ',' << r.onsetPost << ',' << r.releasePre << ',' << r.releasePost << '\n';

    std::printf(
        "CASE %s dual=%d fx=%d fund=%.3fHz period=%.2f cmean[1/2/5/end]=%.3e/%.3e/%.3e/%.3e "
        "(%.1f/%.1f/%.1f/%.1f dBFS) onsetPre=%.1f onsetPost=%.1f releasePre=%.1f releasePost=%.1f "
        "periodMean=%.3e (%.1f dBFS) postCmean=%.3e (%.1f dBFS) |pre-post|=%.3e quiet_n=%zu\n",
        name.c_str(), dual ? 1 : 0, fx, r.assumeFundHz, r.measuredPeriod,
        r.cmean1, r.cmean2, r.cmean5, r.cmeanEnd,
        dbfs(r.cmean1), dbfs(r.cmean2), dbfs(r.cmean5), dbfs(r.cmeanEnd),
        dbfs(r.onsetPre), dbfs(r.onsetPost), dbfs(r.releasePre), dbfs(r.releasePost),
        r.periodMean, dbfs(r.periodMean), postCmeanEnd, dbfs(postCmeanEnd),
        prePostAbsDiff, r.quietN);

    if (fx == 0 && !dual)
    {
        // After history flush, Classic FX-Off production output should track the
        // pre-blocker stream (bypass engaged). Allow small startup differences.
        check(prePostAbsDiff < 5.0e-5, "Classic FX-Off post tracks pre under bypass");
    }
    if (fx == 0 && dual)
    {
        // Dual must remain protected: post cumulative mean much smaller than pre.
        check(std::abs(postCmeanEnd) + 1.0e-6 < std::abs(r.cmeanEnd) * 0.25
                  || dbfs(postCmeanEnd) < -55.0,
              "Dual FX-Off retains host DC protection");
    }
    return r;
}

void syntheticValidation(const std::filesystem::path& outDir)
{
    std::ofstream out(outDir / "synthetic_validation.txt");
    out << std::setprecision(17);

    // 1) Known DC + AC
    {
        constexpr double rate = 48000.0;
        std::vector<float> x(static_cast<size_t>(rate * 4));
        for (size_t i = 0; i < x.size(); ++i)
            x[i] = static_cast<float>(0.25 + 0.1 * std::sin(kTwoPi * 110.0 * i / rate));
        const double m = meanRange(x, static_cast<size_t>(rate), x.size());
        check(std::abs(m - 0.25) < 1.0e-4, "detect known +0.25 DC");
        out << "known_dc_mean=" << m << "\n";
    }

    // 2) Incomplete period false positive: zero-mean saw + sub @ -1 oct, MIDI-24 old method
    {
        constexpr double rate = 48000.0;
        const double f0 = midiHz(24);
        const double fSub = f0 * 0.5;
        std::vector<float> x(static_cast<size_t>(rate * 2));
        for (size_t i = 0; i < x.size(); ++i)
        {
            const double t = static_cast<double>(i) / rate;
            double main = 2.0 * (t * f0 - std::floor(t * f0 + 0.5));
            double sub = ((t * fSub) - std::floor(t * fSub)) < 0.5 ? 1.0 : -1.0;
            x[i] = static_cast<float>(0.5 * main + 0.5 * sub);
        }
        // Construct an exact integer number of combined periods (LCM of main and sub).
        const double periodMain = rate / f0;
        const double periodSub = rate / fSub;
        const size_t combined = static_cast<size_t>(std::llround(2.0 * periodMain)); // = 1 sub period * 2
        std::vector<float> exact(combined * 64);
        for (size_t i = 0; i < exact.size(); ++i)
        {
            const double t = static_cast<double>(i) / rate;
            double main = 2.0 * (t * f0 - std::floor(t * f0 + 0.5));
            double sub = ((t * fSub) - std::floor(t * fSub)) < 0.5 ? 1.0 : -1.0;
            exact[i] = static_cast<float>(0.5 * main + 0.5 * sub);
        }
        const double exactMean = meanRange(exact, 0, exact.size());
        for (float& v : exact)
            v = static_cast<float>(v - exactMean);
        const size_t badCount = static_cast<size_t>(std::llround(32.5 * periodSub));
        const double bad = meanRange(exact, exact.size() - badCount, exact.size());
        const double good = meanRange(exact, 0, exact.size());
        out << "false_positive_32_5_sub_periods_dbfs=" << dbfs(bad) << "\n";
        out << "complete_combined_periods_dbfs=" << dbfs(good) << "\n";
        check(dbfs(bad) > -60.0, "old 32.5-sub-period window creates large false DC");
        check(dbfs(good) < -120.0, "exact combined-period buffer has no persistent DC");
        std::printf("Synthetic incomplete-period false DC: %.2f dBFS; corrected: %.2f dBFS\n",
                    dbfs(bad), dbfs(good));
        (void) periodMain;
    }

    // 3) Coupling-like transient
    {
        constexpr double rate = 48000.0;
        const double tau = 1.0 / (kTwoPi * 1.026);
        std::vector<float> x(static_cast<size_t>(rate * 8));
        for (size_t i = 0; i < x.size(); ++i)
            x[i] = static_cast<float>(0.1 * std::exp(-(static_cast<double>(i) / rate) / tau));
        const double early = meanRange(x, static_cast<size_t>(rate * 0.25), static_cast<size_t>(rate * 0.75));
        const double late = meanRange(x, static_cast<size_t>(rate * 6), x.size());
        out << "transient_phase1_window_dbfs=" << dbfs(early) << "\n";
        out << "transient_late_window_dbfs=" << dbfs(late) << "\n";
        check(dbfs(early) > -50.0, "short post-step window misreads transient as DC");
        check(dbfs(late) < -100.0, "late window sees settled transient near zero");
        std::printf("Synthetic C12-like transient: early %.2f dBFS, late %.2f dBFS\n",
                    dbfs(early), dbfs(late));
    }

    // 4) IR3109 constant input
    {
        Ir3109BoardCore board;
        board.prepare(kNative);
        board.setTargets(8000.0, 0.2, 1.0);
        for (int i = 0; i < static_cast<int>(kNative * 8); ++i)
            board.process(1.0f, 4);
        long double sum = 0.0L;
        const int n = static_cast<int>(kNative * 2);
        for (int i = 0; i < n; ++i)
            sum += board.process(1.0f, 4);
        const double m = static_cast<double>(sum / n);
        out << "ir3109_constant_dbfs=" << dbfs(m) << "\n";
        check(dbfs(m) < -80.0, "IR3109 rejects constant input");
        std::printf("IR3109 constant-input residual: %.2f dBFS\n", dbfs(m));
    }
}

}  // namespace

void runV03Matrix(const std::filesystem::path& outDir, std::vector<CaseResult>& results)
{
    const ShruthiSvfDualRouting routes[] = {
        ShruthiSvfDualRouting::section1, ShruthiSvfDualRouting::serial, ShruthiSvfDualRouting::parallel
    };
    const char* routeName[] = { "s1", "ser", "par" };
    const ShruthiSvf2pMode modes[] = {
        ShruthiSvf2pMode::lowPass, ShruthiSvf2pMode::bandPass, ShruthiSvf2pMode::highPass
    };
    const char* modeName[] = { "lp", "bp", "hp" };
    for (int route = 0; route < 3; ++route)
        for (int m1 = 0; m1 < 3; ++m1)
            for (int m2 = 0; m2 < 3; ++m2)
            {
                if (route == 0 && m2 != 0)
                    continue;
                char name[64];
                std::snprintf(name, sizeof(name), "dual_v03_%s_%s_%s", routeName[route], modeName[m1], modeName[m2]);
                results.push_back(renderCase(name, true, 0, 48000, 64, 48, 8000.f, 0.35f, 1, 0.f, 0.f,
                                             8.0, 4.0, outDir, routes[route], modes[m1], modes[m2]));
            }
    results.push_back(renderCase("dual_v03_s1_maxres", true, 0, 48000, 64, 24, 200.f, 1.f, 1, 0.f, 0.f, 8.0, 4.0, outDir));
    results.push_back(renderCase("dual_v03_ser_maxres", true, 0, 48000, 64, 24, 200.f, 1.f, 1, 0.f, 0.f, 8.0, 4.0, outDir,
                                 ShruthiSvfDualRouting::serial));
    results.push_back(renderCase("dual_v03_par_maxres", true, 0, 48000, 64, 24, 200.f, 1.f, 1, 0.f, 0.f, 8.0, 4.0, outDir,
                                 ShruthiSvfDualRouting::parallel));
    results.push_back(renderCase("dual_v03_lownote", true, 0, 48000, 64, 12, 4000.f, 0.2f, 1, 0.f, 0.f, 8.0, 4.0, outDir));
    results.push_back(renderCase("dual_v03_sub", true, 0, 48000, 64, 24, 4000.f, 0.3f, 1, 0.f, 127.f, 8.0, 4.0, outDir));
    results.push_back(renderCase("dual_v03_noise", true, 0, 48000, 64, 48, 12000.f, 0.f, 1, 127.f, 0.f, 8.0, 4.0, outDir));
    results.push_back(renderCase("dual_v03_vca_attack", true, 0, 48000, 64, 48, 8000.f, 0.2f, 1, 0.f, 0.f, 8.0, 4.0, outDir,
                                 ShruthiSvfDualRouting::section1, ShruthiSvf2pMode::lowPass, ShruthiSvf2pMode::lowPass,
                                 [](SwaraXtAudioProcessor& p) { setParam(p, IDs::env2Attack, 80.f); }));
    results.push_back(renderCase("dual_v03_vca_release", true, 0, 48000, 64, 48, 8000.f, 0.2f, 1, 0.f, 0.f, 4.0, 6.0, outDir,
                                 ShruthiSvfDualRouting::section1, ShruthiSvf2pMode::lowPass, ShruthiSvf2pMode::lowPass,
                                 [](SwaraXtAudioProcessor& p) { setParam(p, IDs::env2Release, 100.f); }));
    results.push_back(renderCase("dual_v03_mod_env", true, 0, 48000, 64, 48, 4000.f, 0.4f, 1, 0.f, 0.f, 8.0, 4.0, outDir,
                                 ShruthiSvfDualRouting::section1, ShruthiSvf2pMode::lowPass, ShruthiSvf2pMode::lowPass,
                                 [](SwaraXtAudioProcessor& p) {
                                     setParam(p, IDs::filterEnvDepth, 40.f);
                                     setParam(p, IDs::env1Attack, 20.f);
                                     setParam(p, IDs::env1Decay, 60.f);
                                     setParam(p, IDs::env1Sustain, 40.f);
                                 }));
    results.push_back(renderCase("dual_v03_mod_lfo", true, 0, 48000, 64, 48, 6000.f, 0.3f, 1, 0.f, 0.f, 8.0, 4.0, outDir,
                                 ShruthiSvfDualRouting::section1, ShruthiSvf2pMode::lowPass, ShruthiSvf2pMode::lowPass,
                                 [](SwaraXtAudioProcessor& p) {
                                     setParam(p, IDs::lfo1Rate, 40.f);
                                     setParam(p, IDs::filterLfoDepth, 40.f);
                                 }));
    for (int program : { 0, 4, 8 })
    {
        char name[64];
        std::snprintf(name, sizeof(name), "dual_v03_preset_%d", program);
        results.push_back(renderCase(name, true, 0, 48000, 64, 48, 8000.f, 0.2f, 1, 0.f, 0.f, 8.0, 4.0, outDir,
                                     ShruthiSvfDualRouting::section1, ShruthiSvf2pMode::lowPass, ShruthiSvf2pMode::lowPass,
                                     [program](SwaraXtAudioProcessor& p) {
                                         if (program < p.getNumPrograms())
                                             p.setCurrentProgram(program);
                                         p.setExperimentalSvfEnabled(true);
                                         p.setExperimentalSvfRouting(ShruthiSvfDualRouting::section1);
                                         p.setExperimentalSvfMode1(ShruthiSvf2pMode::lowPass);
                                         p.setExperimentalSvfMode2(ShruthiSvf2pMode::lowPass);
                                     }));
    }
    results.push_back(renderCase("dual_v03_fx_dist", true, 1, 48000, 64, 48, 8000.f, 0.2f, 1, 0.f, 0.f, 8.0, 4.0, outDir));
}

int main(int argc, char** argv)
{
    std::printf("SWARA XT FX-Off DC blocker audit — Phase II\n");
    const std::filesystem::path outDir =
        argc > 1 ? std::filesystem::path(argv[1])
                 : std::filesystem::path("Z:/SWARAXT-DC-AUDIT-builds/phase2/results");
    std::filesystem::create_directories(outDir);
    {
        std::ofstream csv(outDir / "phase2_cases.csv", std::ios::trunc);
        csv << "name,dual,fx,rate,note,assume_fund_hz,measured_period,"
               "cmean1,cmean2,cmean5,cmean10,cmean_end,period_mean,trailing1,trailing2,"
               "quiet_mean,quiet_n,host_samples,src_samples,dormant_samples,"
               "post_cmean_end,pre_post_abs_diff,onset_pre,onset_post,release_pre,release_post\n";
    }

    syntheticValidation(outDir);

    std::vector<CaseResult> results;
    const bool v03 = argc > 2 && std::string(argv[2]) == "v03";
    if (v03)
        runV03Matrix(outDir, results);
    else
    {
    // Long-duration decisive cases (30 s sustain, 8 s release).
    results.push_back(renderCase("classic_fxoff_sub_30s", false, 0, 48000, 64, 24,
                                 4000.f, 0.3f, 1, 0.f, 127.f, 30.0, 8.0, outDir));
    results.push_back(renderCase("classic_fxoff_saw_30s", false, 0, 48000, 64, 36,
                                 8000.f, 0.2f, 1, 0.f, 0.f, 30.0, 8.0, outDir));
    results.push_back(renderCase("classic_fxoff_noise_30s", false, 0, 48000, 64, 48,
                                 12000.f, 0.0f, 1, 127.f, 0.f, 30.0, 8.0, outDir));
    results.push_back(renderCase("dual_fxoff_saw_30s", true, 0, 48000, 64, 36,
                                 8000.f, 0.2f, 1, 0.f, 0.f, 30.0, 8.0, outDir));
    results.push_back(renderCase("classic_fxon_dist_10s", false, 1, 48000, 64, 36,
                                 8000.f, 0.2f, 1, 0.f, 0.f, 10.0, 4.0, outDir));
    // Expanded Classic FX-Off corners (10 s).
    results.push_back(renderCase("classic_fxoff_square_10s", false, 0, 48000, 64, 36,
                                 6000.f, 0.4f, 2, 0.f, 0.f, 10.0, 4.0, outDir));
    results.push_back(renderCase("classic_fxoff_maxres_10s", false, 0, 48000, 64, 24,
                                 200.f, 1.0f, 1, 0.f, 0.f, 10.0, 4.0, outDir));
    results.push_back(renderCase("classic_fxoff_mincut_10s", false, 0, 48000, 64, 24,
                                 20.f, 0.0f, 1, 0.f, 0.f, 10.0, 4.0, outDir));
    results.push_back(renderCase("classic_fxoff_96k_10s", false, 0, 96000, 64, 36,
                                 8000.f, 0.2f, 1, 0.f, 0.f, 10.0, 4.0, outDir));
    results.push_back(renderCase("dual_fxoff_serial_10s", true, 0, 48000, 64, 36,
                                 8000.f, 0.3f, 1, 0.f, 0.f, 10.0, 4.0, outDir,
                                 ShruthiSvfDualRouting::serial));
    }

    double worstClassic = -std::numeric_limits<double>::infinity();
    double worstDual = -std::numeric_limits<double>::infinity();
    std::string worstClassicName, worstDualName;
    for (const auto& r : results)
    {
        // Decision metric: asymptotic cumulative mean at end of sustain.
        // Quiet residual is supporting evidence, not a substitute for sustain DC.
        const double decision = r.cmeanEnd;
        if (r.fx == 0 && !r.dual && decision > worstClassic)
        {
            worstClassic = decision;
            worstClassicName = r.name;
        }
        if (r.fx == 0 && r.dual && decision > worstDual)
        {
            worstDual = decision;
            worstDualName = r.name;
        }
    }

    // For signed means, "worst" persistent DC is largest absolute value.
    double worstClassicAbs = 0, worstDualAbs = 0;
    for (const auto& r : results)
    {
        if (r.fx == 0 && !r.dual)
            worstClassicAbs = std::max(worstClassicAbs, std::abs(r.cmeanEnd));
        if (r.fx == 0 && r.dual)
            worstDualAbs = std::max(worstDualAbs, std::abs(r.cmeanEnd));
    }

    const bool classicExercised = !worstClassicName.empty();
    const bool dualExercised = !worstDualName.empty();
    const bool classicOk = classicExercised && dbfs(worstClassicAbs) < -80.0;
    const bool dualOk = dualExercised && dbfs(worstDualAbs) < -80.0;

    std::ofstream summary(outDir / "phase2_summary.json");
    summary << std::setprecision(17);
    summary << "{\n";
    summary << "  \"worst_classic_fxoff_cmean_end_dbfs\": " << dbfs(worstClassicAbs) << ",\n";
    summary << "  \"worst_classic_case\": \"" << worstClassicName << "\",\n";
    summary << "  \"worst_dual_fxoff_cmean_end_dbfs\": " << dbfs(worstDualAbs) << ",\n";
    summary << "  \"worst_dual_case\": \"" << worstDualName << "\",\n";
    summary << "  \"matrix\": \"" << (v03 ? "v03" : "phase2") << "\",\n";
    summary << "  \"host_dc_policy\": \"retained\",\n";
    summary << "  \"classic_fxoff_qualifies\": " << (classicOk ? "true" : "false") << ",\n";
    summary << "  \"dual_fxoff_qualifies\": " << (dualOk ? "true" : "false") << ",\n";
    summary << "  \"failures\": " << failures << "\n";
    summary << "}\n";

    std::printf("\nPhase II decision metric = |signed cumulative mean| at end of sustain\n");
    if (classicExercised)
        std::printf("WORST Classic FX-Off: %.2f dBFS (%s) -> %s\n",
                    dbfs(worstClassicAbs), worstClassicName.c_str(),
                    classicOk ? "QUALIFIED" : "NOT QUALIFIED");
    else
        std::printf("Classic FX-Off: not in this matrix\n");
    if (dualExercised)
        std::printf("WORST Dual FX-Off: %.2f dBFS (%s) -> %s\n",
                    dbfs(worstDualAbs), worstDualName.c_str(),
                    dualOk ? "QUALIFIED" : "NOT QUALIFIED");
    else
        std::printf("Dual FX-Off: not in this matrix\n");
    std::printf("Host DC protection policy: retained\n");
    std::printf("failures=%d\n", failures);
    return failures == 0 ? 0 : 1;
}
