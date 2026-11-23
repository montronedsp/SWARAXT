// Copyright 2026 MontroneDSP.
// SPDX-License-Identifier: GPL-3.0-or-later
// Cutoff/resonance/master/quality-transition measurements and regressions.
// TestDirectFilterQualityClick was removed with the legacy processSample()
// cascade; the remaining tests exercise the production processInstrumentSample()
// path through the real plugin.

#include <JuceHeader.h>

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <functional>
#include <vector>

#include "Engine/Filter/SwaraXtFilter.h"
#include "Engine/SwaraXtEngine.h"
#include "Plugin/PluginProcessor.h"
#include "Plugin/SwaraXtParameterLayout.h"
#include "shruthi/patch.h"

namespace {

int gFailures = 0;

void expect(bool ok, const char* message)
{
    if (! ok)
    {
        std::fprintf(stderr, "FAIL: %s\n", message);
        ++gFailures;
    }
}

void setFloat(SwaraXtAudioProcessor& proc, const char* id, float value)
{
    auto* parameter = proc.getApvts().getParameter(id);
    expect(parameter != nullptr, id);
    if (parameter != nullptr)
        parameter->setValueNotifyingHost(parameter->convertTo0to1(value));
}

void setInt(SwaraXtAudioProcessor& proc, const char* id, int value)
{
    auto* parameter = proc.getApvts().getParameter(id);
    expect(parameter != nullptr, id);
    if (parameter != nullptr)
        parameter->setValueNotifyingHost(parameter->convertTo0to1(static_cast<float>(value)));
}

void configureNeutralSaw(SwaraXtAudioProcessor& proc)
{
    setInt(proc, swaraxt::IDs::osc1Shape, 1);
    setInt(proc, swaraxt::IDs::osc2Shape, 0);
    setInt(proc, swaraxt::IDs::mixBalance, 0);
    setInt(proc, swaraxt::IDs::mixSub, 0);
    setInt(proc, swaraxt::IDs::mixNoise, 0);
    setInt(proc, swaraxt::IDs::env2Attack, 0);
    setInt(proc, swaraxt::IDs::env2Decay, 0);
    setInt(proc, swaraxt::IDs::env2Sustain, 127);
    setInt(proc, swaraxt::IDs::env2Release, 40);
    setFloat(proc, swaraxt::IDs::filterEnvDepth, 32);
    setFloat(proc, swaraxt::IDs::filterLfoDepth, 0);
    setFloat(proc, swaraxt::IDs::filterKeyTracking, 0.0f);
    setFloat(proc, swaraxt::IDs::master, 0.85f);
    for (int row = 1; row <= 12; ++row)
        setInt(proc, ("mod.row" + juce::String(row) + ".amount").toRawUTF8(), 0);
}

struct RenderStats
{
    float peak = 0.0f;
    float maxDelta = 0.0f;
    int invalid = 0;
    int firstChange = -1;
};

RenderStats analyze(const std::vector<float>& samples, int start = 1)
{
    RenderStats stats;
    for (int i = 0; i < static_cast<int>(samples.size()); ++i)
    {
        const float y = samples[static_cast<size_t>(i)];
        if (! std::isfinite(y))
            ++stats.invalid;
        stats.peak = std::max(stats.peak, std::abs(y));
        if (i >= start)
        {
            const float dy = std::abs(y - samples[static_cast<size_t>(i - 1)]);
            if (dy > stats.maxDelta)
            {
                stats.maxDelta = dy;
                stats.firstChange = i;
            }
        }
    }
    return stats;
}

std::vector<float> renderHeldNote(SwaraXtAudioProcessor& proc,
                                  int warmupBlocks,
                                  int captureBlocks,
                                  int blockSize,
                                  const std::function<void(int)>& onBlock)
{
    std::vector<float> out;
    out.reserve(static_cast<size_t>((warmupBlocks + captureBlocks) * blockSize));
    for (int block = 0; block < warmupBlocks + captureBlocks; ++block)
    {
        juce::AudioBuffer<float> buffer(2, blockSize);
        juce::MidiBuffer midi;
        if (block == 0)
            midi.addEvent(juce::MidiMessage::noteOn(1, 60, static_cast<juce::uint8>(110)), 0);
        if (onBlock)
            onBlock(block);
        proc.processBlock(buffer, midi);
        if (block >= warmupBlocks)
        {
            const float* samples = buffer.getReadPointer(0);
            out.insert(out.end(), samples, samples + blockSize);
        }
    }
    return out;
}

void testEngineQualityDeclick()
{
    const int block = 64;
    const auto runSwitch = [&](swaraxt::FilterQuality from,
                               swaraxt::FilterQuality to,
                               float resonance,
                               const char* label) {
        SwaraXtAudioProcessor proc;
        proc.prepareToPlay(48000.0, block);
        configureNeutralSaw(proc);
        setFloat(proc, swaraxt::IDs::filterCutoff, 900.0f);
        setFloat(proc, swaraxt::IDs::filterResonance, resonance);
        proc.setFilterQuality(from);
        const int warmup = 40;
        const int capture = 40;
        const auto samples = renderHeldNote(proc, warmup, capture, block,
            [&](int current) {
                if (current == warmup)
                    proc.setFilterQuality(to);
            });
        const auto stats = analyze(samples);
        std::printf("quality %s res=%.2f peak=%.4f max|dy|=%.6f invalid=%d applied=%d\n",
                    label,
                    static_cast<double>(resonance),
                    static_cast<double>(stats.peak),
                    static_cast<double>(stats.maxDelta),
                    stats.invalid,
                    static_cast<int>(proc.engineForTests().filter().quality()));
        expect(stats.invalid == 0, "quality switch output is finite");
        expect(stats.peak > 0.001f, "quality switch is not stuck muted");
        expect(proc.engineForTests().filter().quality() == to, "target quality is reached");
        // Compare against both unswitched renders of the same patch. An absolute
        // derivative bound confuses restored filter modulation / bandwidth with
        // a mode-switch click, even when that edge is in the ordinary waveform.
        float ordinaryDelta = 0, ordinaryPeak = 0;
        for (auto quality : {from, to}) {
            auto control = std::make_unique<SwaraXtAudioProcessor>();
            control->prepareToPlay(48000.0, block);
            configureNeutralSaw(*control);
            setFloat(*control, swaraxt::IDs::filterCutoff, 900.0f);
            setFloat(*control, swaraxt::IDs::filterResonance, resonance);
            control->setFilterQuality(quality);
            const auto normal = analyze(renderHeldNote(*control, warmup, capture, block, {}));
            ordinaryDelta = std::max(ordinaryDelta, normal.maxDelta);
            ordinaryPeak = std::max(ordinaryPeak, normal.peak);
        }
        std::printf("quality %s ordinary max|dy|=%.6f peak=%.6f\n", label, static_cast<double>(ordinaryDelta), static_cast<double>(ordinaryPeak));
        expect(stats.maxDelta < ordinaryDelta * 1.1f + .02f, "quality transition adds no disproportionate discontinuity");
        expect(stats.peak < ordinaryPeak * 1.1f + .05f, "quality transition adds no level explosion");
        return stats.maxDelta;
    };

    runSwitch(swaraxt::FilterQuality::high, swaraxt::FilterQuality::normal, 0.2f, "High->Normal");
    runSwitch(swaraxt::FilterQuality::normal, swaraxt::FilterQuality::high, 0.2f, "Normal->High");
    runSwitch(swaraxt::FilterQuality::high, swaraxt::FilterQuality::eco, 0.2f, "High->Eco");
    runSwitch(swaraxt::FilterQuality::eco, swaraxt::FilterQuality::high, 0.2f, "Eco->High");
    runSwitch(swaraxt::FilterQuality::normal, swaraxt::FilterQuality::eco, 0.2f, "Normal->Eco");
    runSwitch(swaraxt::FilterQuality::eco, swaraxt::FilterQuality::normal, 0.2f, "Eco->Normal");
    runSwitch(swaraxt::FilterQuality::high, swaraxt::FilterQuality::eco, 0.95f, "High->Eco high-res");
    runSwitch(swaraxt::FilterQuality::eco, swaraxt::FilterQuality::high, 0.95f, "Eco->High high-res");

    SwaraXtAudioProcessor rapid;
    rapid.prepareToPlay(48000.0, block);
    configureNeutralSaw(rapid);
    setFloat(rapid, swaraxt::IDs::filterCutoff, 700.0f);
    setFloat(rapid, swaraxt::IDs::filterResonance, 0.9f);
    rapid.setFilterQuality(swaraxt::FilterQuality::high);
    const auto rapidSamples = renderHeldNote(rapid, 20, 60, block, [&](int current) {
        if (current == 20)
            rapid.setFilterQuality(swaraxt::FilterQuality::eco);
        if (current == 28)
            rapid.setFilterQuality(swaraxt::FilterQuality::normal);
        if (current == 36)
            rapid.setFilterQuality(swaraxt::FilterQuality::high);
    });
    const auto rapidStats = analyze(rapidSamples);
    expect(rapidStats.invalid == 0, "rapid quality switching stays finite");
    expect(rapid.engineForTests().filter().quality() == swaraxt::FilterQuality::high,
           "rapid switching ends on the latest requested quality");
    expect(rapidStats.peak > 0.001f, "rapid switching does not leave the filter muted");
}

struct IsolatedClick
{
    float baselineDy = 0.0f;
    float boundaryDy = 0.0f;
    float followDy = 0.0f;
    float nextDy = 0.0f;
    bool click = false;
};

IsolatedClick measureIsolatedClick(const std::vector<float>& y, int jumpAt, int window = 256)
{
    IsolatedClick r;
    const int n = static_cast<int>(y.size());
    if (jumpAt <= 1 || jumpAt + 2 >= n)
        return r;
    const int pre0 = std::max(1, jumpAt - window);
    for (int i = pre0; i < jumpAt; ++i)
        r.baselineDy = std::max(r.baselineDy, std::abs(y[static_cast<size_t>(i)] - y[static_cast<size_t>(i - 1)]));
    r.boundaryDy = std::abs(y[static_cast<size_t>(jumpAt)] - y[static_cast<size_t>(jumpAt - 1)]);
    r.nextDy = std::abs(y[static_cast<size_t>(jumpAt + 1)] - y[static_cast<size_t>(jumpAt)]);
    const int post1 = std::min(n, jumpAt + window);
    for (int i = jumpAt + 2; i < post1; ++i)
        r.followDy = std::max(r.followDy, std::abs(y[static_cast<size_t>(i)] - y[static_cast<size_t>(i - 1)]));
    const float neighbor = std::max(r.baselineDy, r.followDy);
    r.click = r.boundaryDy > 3.0f * std::max(neighbor, 1.0e-6f)
        && r.nextDy < 0.45f * r.boundaryDy;
    return r;
}

struct ChangeCapture
{
    std::vector<float> samples;
    int changeIndex = 0;
};

ChangeCapture renderAroundChange(SwaraXtAudioProcessor& proc,
                                 int prerollBlocks,
                                 int changeBlock,
                                 int captureBlocks,
                                 int blockSize,
                                 const std::function<void(int)>& onBlock)
{
    ChangeCapture cap;
    const int total = changeBlock + captureBlocks;
    cap.samples.reserve(static_cast<size_t>((prerollBlocks + captureBlocks) * blockSize));
    cap.changeIndex = prerollBlocks * blockSize;
    for (int block = 0; block < total; ++block)
    {
        juce::AudioBuffer<float> buffer(2, blockSize);
        juce::MidiBuffer midi;
        if (block == 0)
            midi.addEvent(juce::MidiMessage::noteOn(1, 60, static_cast<juce::uint8>(110)), 0);
        if (onBlock)
            onBlock(block);
        proc.processBlock(buffer, midi);
        if (block >= changeBlock - prerollBlocks)
        {
            const float* samples = buffer.getReadPointer(0);
            cap.samples.insert(cap.samples.end(), samples, samples + blockSize);
        }
    }
    return cap;
}

void testMasterRamp()
{
    SwaraXtAudioProcessor proc;
    proc.prepareToPlay(48000.0, 64);
    configureNeutralSaw(proc);
    setFloat(proc, swaraxt::IDs::filterCutoff, 8000.0f);
    setFloat(proc, swaraxt::IDs::filterResonance, 0.0f);
    setFloat(proc, swaraxt::IDs::master, 0.85f);

    const int changeBlock = 30;
    auto cap = renderAroundChange(proc, 2, changeBlock, 40, 64, [&](int block) {
        if (block == changeBlock)
            setFloat(proc, swaraxt::IDs::master, 0.0f);
    });
    const auto stats = analyze(cap.samples);
    const auto click = measureIsolatedClick(cap.samples, cap.changeIndex);
    const int quietStart = cap.changeIndex + static_cast<int>(0.020 * 48000.0);
    float postDy = 0.0f;
    int postN = 0;
    for (int i = quietStart + 1; i < std::min(static_cast<int>(cap.samples.size()), quietStart + 512); ++i)
    {
        postDy = std::max(postDy, std::abs(cap.samples[static_cast<size_t>(i)]
                                           - cap.samples[static_cast<size_t>(i - 1)]));
        ++postN;
    }
    std::printf("master 0.85->0 peak=%.4f max|dy|=%.6f boundDy=%.5f followDy=%.5f post20ms|dy|=%.6f click=%s\n",
                static_cast<double>(stats.peak),
                static_cast<double>(stats.maxDelta),
                static_cast<double>(click.boundaryDy),
                static_cast<double>(click.followDy),
                static_cast<double>(postDy),
                click.click ? "YES" : "no");
    expect(stats.invalid == 0, "master ramp stays finite");
    expect(! click.click, "master 0.85->0 has no isolated click");
    expect(postN > 100, "master post-ramp window exists");
    expect(postDy < 0.01f, "master reaches silence except DC-blocker decay");

    setFloat(proc, swaraxt::IDs::master, 0.85f);
    juce::AudioBuffer<float> recover(2, 64);
    juce::MidiBuffer midi;
    for (int i = 0; i < 20; ++i)
        proc.processBlock(recover, midi);
    float recoveredPeak = 0.0f;
    for (int i = 0; i < recover.getNumSamples(); ++i)
        recoveredPeak = std::max(recoveredPeak, std::abs(recover.getSample(0, i)));
    expect(recoveredPeak > 0.01f, "master ramp recovers from 0 to nominal");
}

void testCutoffAndResonancePluginJumps()
{
    const auto runJump = [](const char* id, float from, float to, const char* label,
                            double sampleRate, int blockSize) {
        SwaraXtAudioProcessor proc;
        proc.prepareToPlay(sampleRate, blockSize);
        configureNeutralSaw(proc);
        setFloat(proc, swaraxt::IDs::filterCutoff, 1000.0f);
        setFloat(proc, swaraxt::IDs::filterResonance, 0.50f);
        setFloat(proc, id, from);
        const int changeBlock = 24;
        auto cap = renderAroundChange(proc, 2, changeBlock, 16, blockSize, [&](int block) {
            if (block == changeBlock)
                setFloat(proc, id, to);
        });
        const auto stats = analyze(cap.samples);
        const auto click = measureIsolatedClick(cap.samples, cap.changeIndex);
        std::printf("%s sr=%.1fk blk=%d peak=%.4f max|dy|=%.5f bound=%.5f follow=%.5f click=%s\n",
                    label,
                    sampleRate / 1000.0,
                    blockSize,
                    static_cast<double>(stats.peak),
                    static_cast<double>(stats.maxDelta),
                    static_cast<double>(click.boundaryDy),
                    static_cast<double>(click.followDy),
                    click.click ? "YES" : "no");
        expect(stats.invalid == 0, "base-parameter jump stays finite");
        expect(! click.click, "base-parameter jump has no isolated click");
        return click;
    };

    runJump(swaraxt::IDs::filterCutoff, 20.0f, 20000.0f, "cutoff 20->20k", 48000.0, 64);
    runJump(swaraxt::IDs::filterCutoff, 20000.0f, 20.0f, "cutoff 20k->20", 48000.0, 64);
    runJump(swaraxt::IDs::filterCutoff, 100.0f, 1000.0f, "cutoff 100->1k", 48000.0, 64);
    runJump(swaraxt::IDs::filterCutoff, 1000.0f, 8000.0f, "cutoff 1k->8k", 48000.0, 64);
    runJump(swaraxt::IDs::filterCutoff, 8000.0f, 500.0f, "cutoff 8k->500", 48000.0, 64);
    runJump(swaraxt::IDs::filterResonance, 0.00f, 0.25f, "res 0->0.25", 48000.0, 64);
    runJump(swaraxt::IDs::filterResonance, 0.25f, 0.50f, "res 0.25->0.5", 48000.0, 64);
    runJump(swaraxt::IDs::filterResonance, 0.50f, 0.75f, "res 0.5->0.75", 48000.0, 64);
    runJump(swaraxt::IDs::filterResonance, 0.75f, 0.95f, "res 0.75->0.95", 48000.0, 64);
    runJump(swaraxt::IDs::filterResonance, 0.95f, 0.00f, "res 0.95->0", 48000.0, 64);

    const int blocks[] = { 64, 256, 1024 };
    const double rates[] = { 44100.0, 48000.0, 96000.0 };
    int hostClicks = 0;
    int hostCases = 0;
    for (double rate : rates)
    {
        for (int block : blocks)
        {
            ++hostCases;
            const auto c = runJump(swaraxt::IDs::filterCutoff, 100.0f, 8000.0f,
                                   "cutoff host 100->8k", rate, block);
            if (c.click)
                ++hostClicks;
            const auto r = runJump(swaraxt::IDs::filterResonance, 0.0f, 0.95f,
                                   "res host 0->0.95", rate, block);
            ++hostCases;
            if (r.click)
                ++hostClicks;
        }
    }
    std::printf("plugin host-buffer isolated clicks: %d / %d\n", hostClicks, hostCases);
    expect(hostClicks == 0, "host-buffer size does not create isolated cutoff/resonance clicks");
}

void testCombinedAndQualityInteraction()
{
    SwaraXtAudioProcessor proc;
    proc.prepareToPlay(48000.0, 64);
    configureNeutralSaw(proc);
    setFloat(proc, swaraxt::IDs::filterCutoff, 200.0f);
    setFloat(proc, swaraxt::IDs::filterResonance, 0.10f);
    proc.setFilterQuality(swaraxt::FilterQuality::high);
    const int changeBlock = 24;
    auto cap = renderAroundChange(proc, 2, changeBlock, 24, 64, [&](int block) {
        if (block == changeBlock)
        {
            setFloat(proc, swaraxt::IDs::filterCutoff, 8000.0f);
            setFloat(proc, swaraxt::IDs::filterResonance, 0.90f);
        }
        if (block == changeBlock + 4)
            proc.setFilterQuality(swaraxt::FilterQuality::eco);
        if (block == changeBlock + 12)
            proc.setFilterQuality(swaraxt::FilterQuality::high);
    });
    const auto stats = analyze(cap.samples);
    const auto click = measureIsolatedClick(cap.samples, cap.changeIndex);
    std::printf("combined+quality peak=%.4f max|dy|=%.5f bound=%.5f click=%s applied=%d\n",
                static_cast<double>(stats.peak),
                static_cast<double>(stats.maxDelta),
                static_cast<double>(click.boundaryDy),
                click.click ? "YES" : "no",
                static_cast<int>(proc.engineForTests().filter().quality()));
    expect(stats.invalid == 0, "combined cutoff/resonance/quality stays finite");
    expect(! click.click, "combined cutoff/resonance jump has no isolated click");
    expect(proc.engineForTests().filter().quality() == swaraxt::FilterQuality::high,
           "quality returns to the latest requested mode");
    expect(stats.peak > 0.001f, "combined move does not mute the filter");
}

void testMatrixAmountAndLfoRateStayNative()
{
    SwaraXtAudioProcessor proc;
    proc.prepareToPlay(48000.0, 64);
    configureNeutralSaw(proc);
    setFloat(proc, swaraxt::IDs::filterCutoff, 1200.0f);
    setFloat(proc, swaraxt::IDs::filterResonance, 0.4f);
    setInt(proc, "mod.row1.source", shruthi::MOD_SRC_LFO_1);
    setInt(proc, "mod.row1.destination", shruthi::MOD_DST_FILTER_CUTOFF);
    setInt(proc, "mod.row1.amount", 0);
    setInt(proc, swaraxt::IDs::lfo1Wave, 1);
    setInt(proc, swaraxt::IDs::lfo1Rate, 80);

    const auto amountJump = renderHeldNote(proc, 24, 16, 64, [&](int block) {
        if (block == 24)
            setInt(proc, "mod.row1.amount", 63);
    });
    const auto amountStats = analyze(amountJump);
    std::printf("matrix amount 0->+63 max|dy|=%.6f invalid=%d\n",
                static_cast<double>(amountStats.maxDelta), amountStats.invalid);
    expect(amountStats.invalid == 0, "matrix amount jump stays finite");

    setInt(proc, swaraxt::IDs::lfo1Rate, 20);
    juce::AudioBuffer<float> buffer(2, 64);
    juce::MidiBuffer midi;
    proc.processBlock(buffer, midi);
    bool finite = true;
    for (int i = 0; i < buffer.getNumSamples(); ++i)
        finite = finite && std::isfinite(buffer.getSample(0, i));
    expect(finite, "LFO rate jump stays finite");
}


// RMS of the second difference: a level-independent proxy for high-band energy of a
// low-passed sawtooth. It rises monotonically with filter cutoff at zero resonance.
double highBandRms(const std::vector<float>& y)
{
    double sum = 0.0;
    int n = 0;
    for (size_t i = 2; i < y.size(); ++i)
    {
        const double d = static_cast<double>(y[i]) - 2.0 * y[i - 1] + y[i - 2];
        sum += d * d;
        ++n;
    }
    return n > 0 ? std::sqrt(sum / n) : 0.0;
}

// The cutoff parameter is continuous (10 Hz-20 kHz) but the firmware patch stores a
// 7-bit semitone code. Moving the knob inside one semitone cell must still move the
// Classic filter: nine values 10 cents apart (all inside the cell of code 90) must
// give nine distinct, strictly brightening renders at every host block size.
void testCutoffKnobSubSemitoneResolution()
{
    const double centerHz = 20000.0 * std::pow(2.0, (90.0 - 127.0) / 12.0);
    for (int block : { 64, 1024 })
    {
        std::vector<double> energy;
        std::vector<std::vector<float>> renders;
        for (int k = -4; k <= 4; ++k)
        {
            SwaraXtAudioProcessor proc;
            proc.prepareToPlay(48000.0, block);
            configureNeutralSaw(proc);
            setFloat(proc, swaraxt::IDs::filterCutoff,
                     static_cast<float>(centerHz * std::pow(2.0, 10.0 * k / 1200.0)));
            setFloat(proc, swaraxt::IDs::filterResonance, 0.0f);
            // Long warm-up so the filter envelope has finished its note-on movement.
            auto samples = renderHeldNote(proc, 24000 / block, 9600 / block, block, {});
            energy.push_back(highBandRms(samples));
            renders.push_back(std::move(samples));
        }
        int distinct = 0;
        for (size_t i = 1; i < renders.size(); ++i)
            distinct += renders[i] != renders[i - 1] ? 1 : 0;
        std::printf("cutoff knob 10-cent steps blk=%d: distinct adjacent renders=%d/8 highBandRms %.6f..%.6f\n",
                    block, distinct, energy.front(), energy.back());
        expect(distinct == 8, "knob moves inside one semitone cell change the render");
        bool increasing = true;
        for (size_t i = 1; i < energy.size(); ++i)
            increasing = increasing && energy[i] > energy[i - 1] * (1.0 + 1.0e-6);
        expect(increasing, "sub-semitone cutoff steps brighten the filter monotonically");
    }
}


void applyOneBlock(SwaraXtAudioProcessor& proc)
{
    juce::AudioBuffer<float> buffer(2, 64);
    juce::MidiBuffer midi;
    proc.processBlock(buffer, midi);
}

// The fine path must be invisible wherever the cutoff is a firmware code: every one of the
// 128 codes, and every imported Shruthi factory patch, keeps exactly zero knob trim. Off-grid
// values stay inside +-1 CV byte (half a semitone) and are monotonic across a cell.
void testKnobTrimGridAndRange()
{
    SwaraXtAudioProcessor proc;
    proc.prepareToPlay(48000.0, 64);
    int gridViolations = 0;
    for (int code = 0; code <= 127; ++code)
    {
        const float hz = static_cast<float>(20000.0 * std::pow(2.0, (code - 127.0) / 12.0));
        setFloat(proc, swaraxt::IDs::filterCutoff, juce::jlimit(10.0f, 20000.0f, hz));
        applyOneBlock(proc);
        if (proc.engineForTests().knobCutoffTrimBytesForTests() != 0.0f && hz >= 10.0f)
            ++gridViolations;
    }
    std::printf("knob trim on 128 grid codes: violations=%d\n", gridViolations);
    expect(gridViolations == 0, "cutoff exactly on a semitone code has zero knob trim");

    float maxAbs = 0.0f, previous = -2.0f;
    bool monotonic = true;
    const double centerHz = 20000.0 * std::pow(2.0, (90.0 - 127.0) / 12.0);
    for (int cents = -49; cents <= 49; ++cents)
    {
        setFloat(proc, swaraxt::IDs::filterCutoff, static_cast<float>(centerHz * std::pow(2.0, cents / 1200.0)));
        applyOneBlock(proc);
        const float trim = proc.engineForTests().knobCutoffTrimBytesForTests();
        maxAbs = std::max(maxAbs, std::abs(trim));
        monotonic = monotonic && trim >= previous;
        previous = trim;
    }
    std::printf("knob trim across one cell: max |trim|=%.4f bytes monotonic=%d\n", static_cast<double>(maxAbs), monotonic ? 1 : 0);
    expect(maxAbs <= 1.0f, "knob trim stays within one CV byte");
    expect(monotonic, "knob trim rises monotonically across a semitone cell");

    int nonZeroImported = 0;
    for (int index = 0; index < static_cast<int>(swaraxt::kShruthiFactoryPresetCount); ++index)
    {
        SwaraXtAudioProcessor factory;
        factory.prepareToPlay(48000.0, 64);
        factory.setCurrentProgram(swaraxt::kShruthiFactoryPresetStart + index);
        applyOneBlock(factory);
        if (factory.engineForTests().knobCutoffTrimBytesForTests() != 0.0f)
            ++nonZeroImported;
    }
    std::printf("imported Shruthi factory patches with non-zero knob trim: %d / %d\n", nonZeroImported,
                static_cast<int>(swaraxt::kShruthiFactoryPresetCount));
    expect(nonZeroImported == 0, "imported Shruthi factory patches render with zero knob trim");
}

}  // namespace

int main()
{
    juce::ScopedJuceInitialiser_GUI juce;
    testEngineQualityDeclick();
    testMasterRamp();
    testCutoffAndResonancePluginJumps();
    testCombinedAndQualityInteraction();
    testMatrixAmountAndLfoRateStayNative();
    testCutoffKnobSubSemitoneResolution();
    testKnobTrimGridAndRange();
    std::printf(gFailures == 0 ? "Swara XT smoothing audit tests: PASSED\n"
                               : "Swara XT smoothing audit tests: FAILED (%d)\n",
                gFailures);
    return gFailures == 0 ? 0 : 1;
}
