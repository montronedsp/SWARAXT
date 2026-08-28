// Copyright 2026 MontroneDSP.
// SPDX-License-Identifier: GPL-3.0-or-later
// Cutoff/resonance/master/quality-transition measurements and regressions.

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
    setFloat(proc, swaraxt::IDs::filterEnvAmount, 0.0f);
    setFloat(proc, swaraxt::IDs::filterModAmount, 0.0f);
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

void testDirectFilterQualityClick()
{
    swaraxt::SwaraXtFilter filter;
    filter.prepare(swaraxt::SwaraXtEngine::kInternalSampleRate);
    swaraxt::SwaraXtFilterParams params;
    params.cutoffHz = 800.0f;
    params.resonance = 0.92f;
    filter.setParams(params);
    filter.setQuality(swaraxt::FilterQuality::high);

    float previous = 0.0f;
    float maxDelta = 0.0f;
    for (int i = 0; i < 4000; ++i)
    {
        const float x = std::sin(2.0f * 3.14159265f * 220.0f
            * static_cast<float>(i) / static_cast<float>(swaraxt::SwaraXtEngine::kInternalSampleRate));
        if (i == 2000)
            filter.setQuality(swaraxt::FilterQuality::eco);
        const float y = filter.processSample(x);
        expect(std::isfinite(y), "direct quality switch stays finite");
        if (i > 0)
            maxDelta = std::max(maxDelta, std::abs(y - previous));
        previous = y;
    }
    std::printf("quality direct-reset max|dy|=%.6f\n", static_cast<double>(maxDelta));
    expect(maxDelta > 0.05f, "live quality reset is the measured click mechanism");
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
        expect(stats.maxDelta < 0.25f, "quality de-click bounds the live transition");
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
        std::printf("%s sr=%.1fk blk=%d peak=%.4f max|dy|=%.5f bound=%.5f follow=%.5f click=%s invalid=%d\n",
                    label,
                    sampleRate / 1000.0,
                    blockSize,
                    static_cast<double>(stats.peak),
                    static_cast<double>(stats.maxDelta),
                    static_cast<double>(click.boundaryDy),
                    static_cast<double>(click.followDy),
                    click.click ? "YES" : "no",
                    stats.invalid);
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

}  // namespace

int main()
{
    juce::ScopedJuceInitialiser_GUI juce;
    testDirectFilterQualityClick();
    testEngineQualityDeclick();
    testMasterRamp();
    testCutoffAndResonancePluginJumps();
    testCombinedAndQualityInteraction();
    testMatrixAmountAndLfoRateStayNative();
    std::printf(gFailures == 0 ? "Swara XT smoothing audit tests: PASSED\n"
                               : "Swara XT smoothing audit tests: FAILED (%d)\n",
                gFailures);
    return gFailures == 0 ? 0 : 1;
}
