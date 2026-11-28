// Copyright 2026 MontroneDSP. SPDX-License-Identifier: GPL-3.0-or-later
// Offline FX-Off host DC-blocker redundancy audit. Measures pre-blocker SRC
// output via independent replay of native post-FX taps. Production DSP is
// unmodified; this target only observes and analyses.
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
#include <iomanip>
#include <limits>
#include <string>
#include <vector>

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

struct Metrics
{
    double mean = 0.0;
    double absMean = 0.0;
    double dcDbfs = -std::numeric_limits<double>::infinity();
    double peak = 0.0;
    double rms = 0.0;
    double dcToRms = 0.0;
    double maxJump = 0.0;
    int nanInf = 0;
    size_t samples = 0;
};

Metrics analyse(const std::vector<float>& x, size_t start, size_t count)
{
    Metrics m;
    if (count == 0 || start >= x.size())
        return m;
    const size_t end = std::min(x.size(), start + count);
    m.samples = end - start;
    if (m.samples == 0)
        return m;

    long double sum = 0.0L;
    long double sumSq = 0.0L;
    float prev = x[start];
    for (size_t i = start; i < end; ++i)
    {
        const float y = x[i];
        if (!std::isfinite(y))
        {
            ++m.nanInf;
            continue;
        }
        const double yd = static_cast<double>(y);
        sum += yd;
        sumSq += static_cast<long double>(yd) * yd;
        m.peak = std::max(m.peak, std::abs(yd));
        m.maxJump = std::max(m.maxJump, static_cast<double>(std::abs(y - prev)));
        prev = y;
    }
    const auto n = static_cast<long double>(m.samples);
    m.mean = static_cast<double>(sum / n);
    m.absMean = std::abs(m.mean);
    m.rms = std::sqrt(static_cast<double>(sumSq / n));
    m.dcToRms = m.rms > 0.0 ? m.absMean / m.rms : 0.0;
    m.dcDbfs = m.absMean > 0.0 ? 20.0 * std::log10(m.absMean) : -std::numeric_limits<double>::infinity();
    return m;
}

// Align analysis to an integer number of MIDI-note periods when possible.
Metrics analyseIntegerPeriods(const std::vector<float>& x,
                              size_t preferEnd,
                              double hostRate,
                              int midiNote,
                              double windowSeconds)
{
    if (x.empty() || hostRate <= 0.0)
        return {};
    const double f0 = 440.0 * std::pow(2.0, (static_cast<double>(midiNote) - 69.0) / 12.0);
    const double samplesPerPeriod = hostRate / f0;
    const size_t periods = std::max<size_t>(8, static_cast<size_t>(windowSeconds * f0));
    const size_t count = static_cast<size_t>(std::llround(periods * samplesPerPeriod));
    if (count == 0 || count > x.size())
        return analyse(x, x.size() > count ? x.size() - count : 0, std::min(count, x.size()));
    size_t end = std::min(preferEnd, x.size());
    if (end < count)
        end = std::min(x.size(), count);
    return analyse(x, end - count, count);
}

Metrics analyseQuietResidual(const std::vector<float>& x, size_t from, double hostRate)
{
    if (from >= x.size())
        return {};
    // Skip 250 ms after note-off for envelope/circuit tails, then require a
    // quiet window (|y| < 1e-3) of at least 0.5 s.
    const size_t skip = static_cast<size_t>(hostRate * 0.25);
    const size_t need = static_cast<size_t>(hostRate * 0.5);
    size_t start = std::min(x.size(), from + skip);
    std::vector<float> quiet;
    quiet.reserve(need);
    for (size_t i = start; i < x.size(); ++i)
    {
        if (std::abs(x[i]) < 1.0e-3f)
            quiet.push_back(x[i]);
        else if (!quiet.empty() && quiet.size() < need)
            quiet.clear();
        if (quiet.size() >= need)
            break;
    }
    return analyse(quiet, 0, quiet.size());
}

struct CaseResult
{
    std::string name;
    bool dual = false;
    int fx = 0;
    double rate = 48000.0;
    int block = 64;
    int note = 36;
    Metrics hostWithBlocker;
    Metrics preBlocker;
    Metrics preInteger;
    Metrics quietPre;
    Metrics postBlockerReplay;
    double hostVsReplayMax = 0.0;
};

struct Capture
{
    std::vector<float> nativePostFx;
    std::vector<float> host;
    static void tap(void* context, const SwaraXtEngine::DebugBlockCapture& block)
    {
        auto& c = *static_cast<Capture*>(context);
        c.nativePostFx.insert(c.nativePostFx.end(), block.postVca, block.postVca + block.samples);
    }
};

void replaySrc(const std::vector<float>& native,
               double hostRate,
               std::vector<float>& preDc,
               std::vector<float>& postDc)
{
    HostRateConverter src;
    src.setStep(kNative, hostRate);
    src.reset();
    DcBlocker dc;
    dc.prepare(hostRate);
    size_t at = 0;
    preDc.clear();
    postDc.clear();
    preDc.reserve(static_cast<size_t>(native.size() * hostRate / kNative) + 8);
    postDc.reserve(preDc.capacity());
    while (at < native.size() || src.size() >= src.minimumReadableSize())
    {
        while (src.size() < src.queueTargetSize() && at < native.size())
            src.push(native[at++]);
        if (src.size() < src.minimumReadableSize())
            break;
        const float pre = src.readInterpolated();
        preDc.push_back(pre);
        postDc.push_back(dc.process(pre));
    }
}

CaseResult renderCase(const std::string& name,
                      bool dual,
                      int fx,
                      double rate,
                      int block,
                      int note,
                      int velocity,
                      float cutoff,
                      float resonance,
                      int oscShape,
                      float mixNoise,
                      float mixSub,
                      int sustainBlocks,
                      int releaseBlocks,
                      const std::filesystem::path& artifactDir,
                      ShruthiSvfDualRouting routing = ShruthiSvfDualRouting::section1)
{
    CaseResult result;
    result.name = name;
    result.dual = dual;
    result.fx = fx;
    result.rate = rate;
    result.block = block;
    result.note = note;

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
        processor->setExperimentalSvfMode1(ShruthiSvf2pMode::lowPass);
        processor->setExperimentalSvfMode2(ShruthiSvf2pMode::lowPass);
        processor->setExperimentalSvfControl2(ShruthiSvfSection2Control::legacyFollow);
    }
    else
    {
        processor->setExperimentalSvfEnabled(false);
    }

    Transport transport;
    transport.rate = rate;
    processor->setPlayHead(&transport);
    processor->prepareToPlay(rate, block);

    Capture capture;
    capture.nativePostFx.reserve(static_cast<size_t>(sustainBlocks + releaseBlocks + 8) * 40);
    capture.host.reserve(static_cast<size_t>((sustainBlocks + releaseBlocks + 8) * block));
    processor->engineForTests().setDebugTapSink(&capture, Capture::tap);

    juce::AudioBuffer<float> audio(2, block);
    juce::MidiBuffer midi;
    const int totalBlocks = sustainBlocks + releaseBlocks;
    for (int b = 0; b < totalBlocks; ++b)
    {
        midi.clear();
        if (b == 0)
            midi.addEvent(juce::MidiMessage::noteOn(1, note, static_cast<juce::uint8>(velocity)), 0);
        if (b == sustainBlocks)
            midi.addEvent(juce::MidiMessage::noteOff(1, note), 0);
        audio.clear();
        processor->processBlock(audio, midi);
        for (int i = 0; i < block; ++i)
            capture.host.push_back(audio.getSample(0, i));
        transport.cursor += block;
    }

    processor->engineForTests().setDebugTapSink(nullptr, nullptr);
    processor->setPlayHead(nullptr);
    processor->releaseResources();

    std::vector<float> preDc, postDc;
    replaySrc(capture.nativePostFx, rate, preDc, postDc);

    const size_t sustainSamples = static_cast<size_t>(sustainBlocks) * static_cast<size_t>(block);
    const size_t window = std::max<size_t>(static_cast<size_t>(rate * 1.0), 4096);
    const size_t hostEnd = std::min(capture.host.size(), sustainSamples);
    const size_t preEnd = std::min(preDc.size(), sustainSamples);

    result.hostWithBlocker = analyse(capture.host,
                                     hostEnd > window ? hostEnd - window : hostEnd / 2,
                                     window);
    result.preBlocker = analyse(preDc, preEnd > window ? preEnd - window : preEnd / 2, window);
    result.preInteger = analyseIntegerPeriods(preDc, preEnd, rate, note, 2.0);
    result.quietPre = analyseQuietResidual(preDc, preEnd, rate);
    result.postBlockerReplay = analyse(postDc, preEnd > window ? preEnd - window : preEnd / 2, window);

    const size_t compare = std::min(capture.host.size(), postDc.size());
    for (size_t i = 0; i < compare; ++i)
        result.hostVsReplayMax = std::max(
            result.hostVsReplayMax,
            std::abs(static_cast<double>(capture.host[i]) - static_cast<double>(postDc[i])));

    if (!artifactDir.empty())
    {
        const auto stem = artifactDir / (name + ".preDc.f32");
        std::ofstream out(stem, std::ios::binary);
        out.write(reinterpret_cast<const char*>(preDc.data()),
                  static_cast<std::streamsize>(preDc.size() * sizeof(float)));
    }
    return result;
}

void validateHarness(const std::filesystem::path& outDir)
{
    constexpr double rate = 48000.0;
    DcBlocker dc;
    dc.prepare(rate);
    std::vector<float> withDc, blocked;
    const int n = static_cast<int>(rate * 8.0);
    withDc.reserve(static_cast<size_t>(n));
    blocked.reserve(static_cast<size_t>(n));
    for (int i = 0; i < n; ++i)
    {
        const float x = 0.25f + 0.1f * static_cast<float>(std::sin(kTwoPi * 110.0 * i / rate));
        withDc.push_back(x);
        blocked.push_back(dc.process(x));
    }
    const auto pre = analyse(withDc, static_cast<size_t>(rate * 4.0), static_cast<size_t>(rate * 2.0));
    const auto post = analyse(blocked, static_cast<size_t>(rate * 4.0), static_cast<size_t>(rate * 2.0));
    check(pre.absMean > 0.2 && pre.absMean < 0.3, "harness detects known +0.25 DC");
    check(post.dcDbfs < -80.0, "harness sees blocker reject constant offset below -80 dBFS");

    std::vector<float> lf;
    const int period = static_cast<int>(std::lround(rate / 0.5));
    lf.resize(static_cast<size_t>(8 * period));
    for (size_t i = 0; i < lf.size(); ++i)
        lf[i] = static_cast<float>(0.5 * std::sin(kTwoPi * 0.5 * static_cast<double>(i) / rate));
    const auto lfMetrics = analyse(lf, 0, lf.size());
    check(lfMetrics.dcDbfs < -100.0, "integer-period LF sine is not mistaken for DC");

    // Schematic IR3109 AC path must reject a constant mixer unit.
    Ir3109BoardCore board;
    board.prepare(kNative);
    board.setTargets(8000.0, 0.2, 1.0);
    for (int i = 0; i < static_cast<int>(kNative * 8.0); ++i)
        board.process(1.0f, 4);
    long double sum = 0.0L;
    const int measure = static_cast<int>(kNative * 2.0);
    for (int i = 0; i < measure; ++i)
        sum += board.process(1.0f, 4);
    const double mean = static_cast<double>(sum / measure);
    const double dbfs = std::abs(mean) > 0.0 ? 20.0 * std::log10(std::abs(mean)) : -240.0;
    check(dbfs < -80.0, "IR3109 board AC coupling rejects constant input below -80 dBFS");

    std::ofstream note(outDir / "harness-validation.txt");
    note << std::setprecision(17);
    note << "known_dc_pre_dbfs=" << pre.dcDbfs << "\n";
    note << "known_dc_post_dbfs=" << post.dcDbfs << "\n";
    note << "lf_sine_dc_dbfs=" << lfMetrics.dcDbfs << "\n";
    note << "ir3109_constant_input_dc_dbfs=" << dbfs << "\n";
    std::printf("IR3109 constant-input residual: %.2f dBFS\n", dbfs);
}

const char* grade(double dbfs)
{
    if (!std::isfinite(dbfs) || dbfs < -100.0)
        return "excellent";
    if (dbfs < -80.0)
        return "acceptable";
    if (dbfs < -60.0)
        return "investigate";
    return "blocks";
}

double decisionMetric(const CaseResult& r)
{
    // Prefer integer-period sustain and post-release quiet residuals. A raw
    // finite window mean is reported separately because non-integer periods
    // of musical waveforms are not persistent DC.
    bool have = false;
    double worst = -std::numeric_limits<double>::infinity();
    if (r.preInteger.samples > 0)
    {
        worst = std::max(worst, r.preInteger.dcDbfs);
        have = true;
    }
    if (r.quietPre.samples > 0)
    {
        worst = std::max(worst, r.quietPre.dcDbfs);
        have = true;
    }
    return have ? worst : r.preBlocker.dcDbfs;
}

}  // namespace

int main(int argc, char** argv)
{
    std::printf("SWARA XT FX-Off DC blocker audit\n");
    const std::filesystem::path outDir =
        argc > 1 ? std::filesystem::path(argv[1])
                 : std::filesystem::path("Z:/SWARAXT-DC-AUDIT-builds/dc-artifacts");
    std::filesystem::create_directories(outDir);
    validateHarness(outDir);

    std::vector<CaseResult> results;
    struct Spec
    {
        const char* name;
        bool dual;
        int fx;
        double rate;
        int block;
        int note;
        int velocity;
        float cutoff;
        float resonance;
        int oscShape;
        float mixNoise;
        float mixSub;
        int sustainBlocks;
        int releaseBlocks;
    };

    // ~4 s sustain + ~2 s release at each rate (scaled by block size).
    const Spec specs[] = {
        { "classic_fxoff_saw_48k", false, 0, 48000, 64, 36, 100, 8000.f, 0.2f, 1, 0, 0, 3000, 1500 },
        { "classic_fxoff_saw_44k1", false, 0, 44100, 64, 36, 100, 8000.f, 0.2f, 1, 0, 0, 2800, 1400 },
        { "classic_fxoff_saw_96k", false, 0, 96000, 64, 36, 100, 8000.f, 0.2f, 1, 0, 0, 6000, 3000 },
        { "classic_fxoff_saw_192k", false, 0, 192000, 128, 36, 100, 8000.f, 0.2f, 1, 0, 0, 6000, 3000 },
        { "classic_fxoff_maxres", false, 0, 48000, 64, 24, 127, 200.f, 1.0f, 1, 0, 0, 3600, 1800 },
        { "classic_fxoff_mincut", false, 0, 48000, 64, 24, 127, 20.f, 0.0f, 1, 0, 0, 3600, 1800 },
        { "classic_fxoff_noise", false, 0, 48000, 64, 48, 100, 12000.f, 0.0f, 1, 127, 0, 3000, 1500 },
        { "classic_fxoff_sub", false, 0, 48000, 64, 24, 100, 4000.f, 0.3f, 1, 0, 127, 3000, 1500 },
        { "classic_fxoff_square_pw", false, 0, 48000, 64, 36, 100, 6000.f, 0.4f, 2, 0, 0, 3000, 1500 },
        { "classic_fxoff_block16", false, 0, 48000, 16, 36, 100, 8000.f, 0.2f, 1, 0, 0, 12000, 6000 },
        { "classic_fxoff_block512", false, 0, 48000, 512, 36, 100, 8000.f, 0.2f, 1, 0, 0, 400, 200 },
        { "dual_fxoff_saw_48k", true, 0, 48000, 64, 36, 100, 8000.f, 0.2f, 1, 0, 0, 3000, 1500 },
        { "dual_fxoff_saw_96k", true, 0, 96000, 64, 36, 100, 8000.f, 0.2f, 1, 0, 0, 6000, 3000 },
        { "dual_fxoff_maxres", true, 0, 48000, 64, 24, 127, 200.f, 1.0f, 1, 0, 0, 3600, 1800 },
        { "dual_fxoff_noise", true, 0, 48000, 64, 48, 100, 12000.f, 0.0f, 1, 127, 0, 3000, 1500 },
        { "dual_fxoff_mincut", true, 0, 48000, 64, 24, 127, 20.f, 0.0f, 1, 0, 0, 3600, 1800 },
        { "classic_fxon_dist", false, 1, 48000, 64, 36, 100, 8000.f, 0.2f, 1, 0, 0, 3000, 1500 },
        { "dual_fxon_dist", true, 1, 48000, 64, 36, 100, 8000.f, 0.2f, 1, 0, 0, 3000, 1500 },
        { "classic_fxon_crush", false, 2, 48000, 64, 36, 100, 8000.f, 0.2f, 1, 0, 0, 3000, 1500 },
    };

    std::ofstream csv(outDir / "fx_off_dc_matrix.csv");
    csv << "name,dual,fx,rate,block,host_dc_dbfs,pre_dc_dbfs,pre_integer_dbfs,quiet_pre_dbfs,"
           "pre_abs_mean,pre_peak,pre_rms,decision_dbfs,decision_grade,host_vs_replay_max,nan_inf,"
           "quiet_samples\n";
    csv << std::setprecision(17);

    for (const auto& spec : specs)
    {
        std::printf("CASE %s ...\n", spec.name);
        const auto caseDir = outDir / "captures";
        std::filesystem::create_directories(caseDir);
        auto r = renderCase(spec.name, spec.dual, spec.fx, spec.rate, spec.block, spec.note,
                            spec.velocity, spec.cutoff, spec.resonance, spec.oscShape,
                            spec.mixNoise, spec.mixSub, spec.sustainBlocks, spec.releaseBlocks,
                            caseDir);

        const double decision = decisionMetric(r);
        std::printf("  host=%.1f  pre=%.1f  int=%.1f  quiet=%.1f (%s)  replay_err=%.3g  quiet_n=%zu\n",
                    r.hostWithBlocker.dcDbfs, r.preBlocker.dcDbfs, r.preInteger.dcDbfs,
                    r.quietPre.dcDbfs, grade(decision), r.hostVsReplayMax, r.quietPre.samples);
        csv << r.name << ',' << (r.dual ? 1 : 0) << ',' << r.fx << ',' << r.rate << ',' << r.block
            << ',' << r.hostWithBlocker.dcDbfs << ',' << r.preBlocker.dcDbfs << ','
            << r.preInteger.dcDbfs << ',' << r.quietPre.dcDbfs << ',' << r.preBlocker.absMean << ','
            << r.preBlocker.peak << ',' << r.preBlocker.rms << ',' << decision << ','
            << grade(decision) << ',' << r.hostVsReplayMax << ',' << r.preBlocker.nanInf << ','
            << r.quietPre.samples << '\n';
        results.push_back(std::move(r));
    }

    // Dual serial routing FX-Off corner.
    {
        std::printf("CASE dual_fxoff_serial ...\n");
        auto r = renderCase("dual_fxoff_serial", true, 0, 48000, 64, 36, 100, 8000.f, 0.3f, 1, 0, 0,
                            3000, 1500, outDir / "captures", ShruthiSvfDualRouting::serial);
        const double decision = decisionMetric(r);
        std::printf("  host=%.1f  pre=%.1f  int=%.1f  quiet=%.1f (%s)\n",
                    r.hostWithBlocker.dcDbfs, r.preBlocker.dcDbfs, r.preInteger.dcDbfs,
                    r.quietPre.dcDbfs, grade(decision));
        csv << r.name << ',' << 1 << ',' << 0 << ',' << 48000 << ',' << 64 << ','
            << r.hostWithBlocker.dcDbfs << ',' << r.preBlocker.dcDbfs << ',' << r.preInteger.dcDbfs
            << ',' << r.quietPre.dcDbfs << ',' << r.preBlocker.absMean << ',' << r.preBlocker.peak
            << ',' << r.preBlocker.rms << ',' << decision << ',' << grade(decision) << ','
            << r.hostVsReplayMax << ',' << r.preBlocker.nanInf << ',' << r.quietPre.samples << '\n';
        results.push_back(std::move(r));
    }

    double worstClassic = -std::numeric_limits<double>::infinity();
    double worstDual = -std::numeric_limits<double>::infinity();
    double worstFxOn = -std::numeric_limits<double>::infinity();
    std::string worstClassicName, worstDualName, worstFxOnName;
    for (const auto& r : results)
    {
        const double d = decisionMetric(r);
        if (r.fx == 0 && !r.dual && d > worstClassic)
        {
            worstClassic = d;
            worstClassicName = r.name;
        }
        if (r.fx == 0 && r.dual && d > worstDual)
        {
            worstDual = d;
            worstDualName = r.name;
        }
        if (r.fx != 0 && d > worstFxOn)
        {
            worstFxOn = d;
            worstFxOnName = r.name;
        }
        check(r.preBlocker.nanInf == 0, "no NaN/Inf in pre-blocker window");
        check(r.hostVsReplayMax < 1.0e-4, "host matches SRC+DC replay within tolerance");
    }

    const bool classicOk = worstClassic < -80.0;
    const bool dualOk = worstDual < -80.0;

    std::ofstream summary(outDir / "summary.json");
    summary << std::setprecision(17);
    summary << "{\n";
    summary << "  \"worst_classic_fxoff_decision_dbfs\": " << worstClassic << ",\n";
    summary << "  \"worst_classic_case\": \"" << worstClassicName << "\",\n";
    summary << "  \"worst_dual_fxoff_decision_dbfs\": " << worstDual << ",\n";
    summary << "  \"worst_dual_case\": \"" << worstDualName << "\",\n";
    summary << "  \"worst_fxon_decision_dbfs\": " << worstFxOn << ",\n";
    summary << "  \"worst_fxon_case\": \"" << worstFxOnName << "\",\n";
    summary << "  \"classic_fxoff_qualifies\": " << (classicOk ? "true" : "false") << ",\n";
    summary << "  \"dual_fxoff_qualifies\": " << (dualOk ? "true" : "false") << ",\n";
    summary << "  \"failures\": " << failures << "\n";
    summary << "}\n";

    std::printf("\nWORST Classic FX-Off decision: %.2f dBFS (%s) -> %s\n",
                worstClassic, worstClassicName.c_str(), grade(worstClassic));
    std::printf("WORST Dual FX-Off decision: %.2f dBFS (%s) -> %s\n",
                worstDual, worstDualName.c_str(), grade(worstDual));
    std::printf("WORST FX-On decision: %.2f dBFS (%s)\n", worstFxOn, worstFxOnName.c_str());
    std::printf("Decision: Classic bypass %s; Dual bypass %s\n",
                classicOk ? "QUALIFIED" : "NOT QUALIFIED",
                dualOk ? "QUALIFIED" : "NOT QUALIFIED");
    std::printf("Audit failures=%d\n", failures);
    return failures == 0 ? 0 : 1;
}
