// Copyright 2026 MontroneDSP. SPDX-License-Identifier: GPL-3.0-or-later
// Proves the Classic audio-parity delta is the host 0.7 Hz blocker transfer.
#include <JuceHeader.h>

#include "Engine/SampleRate/HostResampler.h"
#include "Engine/SwaraXtEngine.h"
#include "Plugin/PluginProcessor.h"
#include "Plugin/SwaraXtParameterLayout.h"
#include "../Parity/CurrentAudioGoldens.h"
#include "../Support/CurrentRenderContract.h"

#include "shruthi/patch.h"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <limits>
#include <string>
#include <vector>

#ifndef SWARAXT_FORCE_HOST_DC_BLOCKER
#define SWARAXT_FORCE_HOST_DC_BLOCKER 0
#endif

#if !SWARAXT_FORCE_HOST_DC_BLOCKER
#if !SWARAXT_ENABLE_DC_AUDIT_TAPS
#error The bypass differential requires the host DC audit tap.
#endif
#endif

namespace {
constexpr double kHostRate = 44100.0;
constexpr int kBlock = 128;
constexpr int kSamples = 44100;
int failures = 0;

void expect(bool ok, const char* message)
{
    if (!ok)
    {
        std::fprintf(stderr, "FAIL: %s\n", message);
        ++failures;
    }
}

uint64_t hashSamples(const std::vector<float>& samples)
{
    uint64_t h = 1469598103934665603ull;
    for (float sample : samples)
    {
        const float finite = std::isfinite(sample) ? sample : 0.0f;
        const auto q = static_cast<int32_t>(std::lround(
            std::clamp(finite, -8.0f, 8.0f) * 8388608.0f));
        uint8_t bytes[sizeof(q)];
        std::memcpy(bytes, &q, sizeof(q));
        for (uint8_t b : bytes)
        {
            h ^= b;
            h *= 1099511628211ull;
        }
    }
    return h;
}

struct ReplayBlocker {
    float pole = 0.0f;
    float x1 = 0.0f;
    float y1 = 0.0f;
    void prepare(double rate) noexcept
    {
        pole = static_cast<float>(std::exp(-6.28318530717958647692 * 0.7 / rate));
        x1 = y1 = 0.0f;
    }
    void reset() noexcept { x1 = y1 = 0.0f; }
    float process(float sample) noexcept
    {
        const float y = sample - x1 + pole * y1;
        x1 = sample;
        y1 = y;
        return y;
    }
};

int protectionGuard(double rate)
{
    const double internal = swaraxt::SwaraXtEngine::kInternalSampleRate;
    const double native = static_cast<double>(
        swaraxt::SwaraXtEngine::kOutputLatencyNativeSamples
        + swaraxt::SwaraXtEngine::kAudioBlockSize + 256);
    return static_cast<int>(std::ceil(native * rate / internal))
        + static_cast<int>(std::ceil(rate * 0.25));
}

void setInt(SwaraXtAudioProcessor& proc, const char* id, int value)
{
    if (auto* param = dynamic_cast<juce::AudioParameterInt*>(proc.getApvts().getParameter(id)))
        *param = value;
    else
        expect(false, id);
}

void setFloat(SwaraXtAudioProcessor& proc, const char* id, float value)
{
    if (auto* param = dynamic_cast<juce::AudioParameterFloat*>(proc.getApvts().getParameter(id)))
        *param = value;
    else
        expect(false, id);
}

void configure(SwaraXtAudioProcessor& proc, int osc1, int osc2, int balance, int noise,
               float cutoff, float resonance)
{
    setFloat(proc, swaraxt::IDs::master, 0.85f);
    setInt(proc, swaraxt::IDs::osc1Shape, osc1);
    setInt(proc, swaraxt::IDs::osc1Param, osc1 == shruthi::WAVEFORM_SQUARE ? 64 : 0);
    setInt(proc, swaraxt::IDs::osc1Range, 0);
    setInt(proc, swaraxt::IDs::osc1Option, 0);
    setInt(proc, swaraxt::IDs::osc2Shape, osc2);
    setInt(proc, swaraxt::IDs::osc2Param, 16);
    setInt(proc, swaraxt::IDs::osc2Range, -12);
    setInt(proc, swaraxt::IDs::osc2Option, 12);
    setInt(proc, swaraxt::IDs::mixBalance, balance);
    setInt(proc, swaraxt::IDs::mixSub, 0);
    setInt(proc, swaraxt::IDs::mixNoise, noise);
    setFloat(proc, swaraxt::IDs::filterCutoff, cutoff);
    setFloat(proc, swaraxt::IDs::filterResonance, resonance);
    setInt(proc, swaraxt::IDs::filterEnvDepth, 32);
    setInt(proc, swaraxt::IDs::filterLfoDepth, 0);
    setFloat(proc, swaraxt::IDs::filterKeyTracking, 0.0f);
    setInt(proc, swaraxt::IDs::env2Attack, 0);
    setInt(proc, swaraxt::IDs::env2Decay, 30);
    setInt(proc, swaraxt::IDs::env2Sustain, 110);
    setInt(proc, swaraxt::IDs::env2Release, 25);
    setInt(proc, swaraxt::IDs::midiChannel, 0);
    const juce::String row9 = "mod.row9.";
    const juce::String row10 = "mod.row10.";
    setInt(proc, (row9 + "source").toRawUTF8(), shruthi::MOD_SRC_ENV_2);
    setInt(proc, (row9 + "destination").toRawUTF8(), shruthi::MOD_DST_VCA);
    setInt(proc, (row9 + "amount").toRawUTF8(), 63);
    setInt(proc, (row10 + "source").toRawUTF8(), shruthi::MOD_SRC_VELOCITY);
    setInt(proc, (row10 + "destination").toRawUTF8(), shruthi::MOD_DST_VCA);
    setInt(proc, (row10 + "amount").toRawUTF8(), 16);
}

struct Sample {
    float pre = 0.0f;
    float post = 0.0f;
    bool dormant = false;
    bool drain = false;
    bool emergency = false;
    bool applied = false;
};

#if SWARAXT_FORCE_HOST_DC_BLOCKER
struct Capture { std::vector<Sample> rows; };
#else
struct Capture {
    std::vector<Sample> rows;
    static void sink(void* context, const swaraxt::SwaraXtEngine::HostDcAuditSample& sample)
    {
        auto* self = static_cast<Capture*>(context);
        Sample row;
        row.pre = sample.preBlocker;
        row.post = sample.postBlocker;
        row.dormant = sample.dormant;
        row.drain = sample.dcDrain;
        row.emergency = sample.emergency;
        row.applied = sample.blockerApplied;
        self->rows.push_back(row);
    }
};
#endif

std::vector<float> render(SwaraXtAudioProcessor& proc, int note, bool release, Capture* capture)
{
    proc.prepareToPlay(kHostRate, kBlock);
#if !SWARAXT_FORCE_HOST_DC_BLOCKER
    if (capture != nullptr)
    {
        capture->rows.clear();
        proc.engineForTests().setHostDcAuditSink(capture, Capture::sink);
    }
#else
    juce::ignoreUnused(capture);
#endif
    swaraxt::test::currentRenderInput(proc, kHostRate, kBlock, kSamples, note, release, expect);
    std::vector<float> output;
    output.reserve(kSamples);
    juce::AudioBuffer<float> buffer(2, kBlock);
    int rendered = 0;
    while (rendered < kSamples)
    {
        const int n = std::min(kBlock, kSamples - rendered);
        buffer.setSize(2, n, false, false, true);
        buffer.clear();
        juce::MidiBuffer midi;
        if (rendered == 0 && note >= 0)
            midi.addEvent(juce::MidiMessage::noteOn(1, note, (juce::uint8) 100), 0);
        if (release && note >= 0 && rendered <= kSamples * 3 / 5
            && rendered + n > kSamples * 3 / 5)
            midi.addEvent(juce::MidiMessage::noteOff(1, note), kSamples * 3 / 5 - rendered);
        proc.processBlock(buffer, midi);
        for (int i = 0; i < n; ++i)
            output.push_back(buffer.getSample(0, i));
        rendered += n;
    }
#if !SWARAXT_FORCE_HOST_DC_BLOCKER
    proc.engineForTests().setHostDcAuditSink(nullptr, nullptr);
#endif
    proc.releaseResources();
    return output;
}

struct Scenario {
    const char* name;
    int osc1;
    int osc2;
    int balance;
    int noise;
    float cutoff;
    float resonance;
    int note;
    bool release;
};

const swaraxt_golden::Entry* blockedGolden(const char* name)
{
    return swaraxt_current_golden::find(swaraxt_current_golden::kClassicHostDcBlocked,
                                         swaraxt_current_golden::kClassicHostDcBlockedCount,
                                         name);
}
}

int main()
{
    juce::ScopedJuceInitialiser_GUI init;
    const Scenario scenarios[] = {
        { "Test A basic saw", shruthi::WAVEFORM_SAW, shruthi::WAVEFORM_NONE, 0, 0, 20000.0f, 0.0f, 60, true },
        { "Test B basic square", shruthi::WAVEFORM_SQUARE, shruthi::WAVEFORM_NONE, 0, 0, 20000.0f, 0.0f, 60, true },
        { "Test C two oscillators", shruthi::WAVEFORM_SAW, shruthi::WAVEFORM_SQUARE, 64, 0, 20000.0f, 0.0f, 60, true },
        { "Test F filter low resonance", shruthi::WAVEFORM_SAW, shruthi::WAVEFORM_NONE, 0, 0, 1200.0f, 0.25f, 48, true },
        { "Test G filter high resonance", shruthi::WAVEFORM_SAW, shruthi::WAVEFORM_NONE, 0, 0, 900.0f, 0.82f, 48, true },
        { "Test I normal envelope", shruthi::WAVEFORM_SAW, shruthi::WAVEFORM_SAW, 32, 0, 650.0f, 0.35f, 36, true },
        { "Test D noise only", shruthi::WAVEFORM_NONE, shruthi::WAVEFORM_NONE, 0, 127, 5000.0f, 0.2f, 60, true },
        { "Test J silence", shruthi::WAVEFORM_SAW, shruthi::WAVEFORM_NONE, 0, 0, 20000.0f, 0.0f, -1, false },
    };

#if SWARAXT_FORCE_HOST_DC_BLOCKER
    std::printf("Classic host-DC baseline (unconditional 0.7 Hz blocker)\n");
    for (const auto& scenario : scenarios)
    {
        SwaraXtAudioProcessor proc;
        configure(proc, scenario.osc1, scenario.osc2, scenario.balance, scenario.noise,
                  scenario.cutoff, scenario.resonance);
        const auto audio = render(proc, scenario.note, scenario.release, nullptr);
        const auto hash = hashSamples(audio);
        const auto* expected = blockedGolden(scenario.name);
        expect(expected != nullptr, "blocked golden exists");
        const bool match = expected != nullptr && hash == expected->hash && audio.size() == expected->samples;
        std::printf("%s hash=%016llx match=%d\n", scenario.name,
                    static_cast<unsigned long long>(hash), match ? 1 : 0);
        expect(match, scenario.name);
    }
#else
    std::printf("Classic host-DC bypass differential\n");
    const int guard = protectionGuard(kHostRate);
    const int releaseAt = kSamples * 3 / 5;
    std::printf("guard=%d releaseAt=%d\n", guard, releaseAt);
    for (const auto& scenario : scenarios)
    {
        SwaraXtAudioProcessor proc;
        configure(proc, scenario.osc1, scenario.osc2, scenario.balance, scenario.noise,
                  scenario.cutoff, scenario.resonance);
        Capture capture;
        const auto audio = render(proc, scenario.note, scenario.release, &capture);
        expect(capture.rows.size() == audio.size(), "audit timeline covers the render");

        ReplayBlocker historical;
        ReplayBlocker predicted;
        historical.prepare(kHostRate);
        predicted.prepare(kHostRate);
        int remaining = guard;
        bool engaged = true;
        int bitMismatches = 0;
        int appliedMismatches = 0;
        double maxAbsBypass = 0.0;
        double maxAbsHistorical = 0.0;
        double regionBypass[3] {};
        double regionHistorical[3] {};
        int regionCount[3] {};
        std::vector<float> reconstructed(audio.size(), 0.0f);
        std::vector<float> predictedOut(audio.size(), 0.0f);

        for (size_t i = 0; i < capture.rows.size(); ++i)
        {
            const auto& row = capture.rows[i];
            const bool samePre = row.pre == audio[i] || row.dormant || row.drain || row.applied;
            juce::ignoreUnused(samePre);

            float hist = 0.0f;
            if (row.dormant)
                hist = 0.0f;
            else if (row.emergency)
            {
                historical.reset();
                hist = row.pre;
            }
            else if (row.drain)
                hist = historical.process(0.0f);
            else
                hist = historical.process(row.pre);
            if (!std::isfinite(hist))
                hist = 0.0f;
            reconstructed[i] = hist;

            if (!row.dormant)
            {
                if (remaining > 0)
                    --remaining;
            }
            const bool require = remaining > 0 || row.emergency || row.drain;
            const bool expectApplied = !row.dormant && !row.emergency && !row.drain && require;
            if (row.applied != expectApplied)
                ++appliedMismatches;

            float pred = 0.0f;
            if (row.dormant)
                pred = 0.0f;
            else if (row.emergency)
            {
                predicted.reset();
                engaged = true;
                pred = row.pre;
            }
            else if (row.drain)
            {
                engaged = true;
                pred = predicted.process(0.0f);
            }
            else if (require)
            {
                if (!engaged)
                {
                    predicted.reset();
                    engaged = true;
                }
                pred = predicted.process(row.pre);
            }
            else
            {
                if (engaged)
                {
                    predicted.reset();
                    engaged = false;
                }
                pred = row.pre;
            }
            if (!std::isfinite(pred))
                pred = 0.0f;
            predictedOut[i] = pred;
            if (pred != row.post || pred != audio[i])
                ++bitMismatches;
            const double bypassError = std::abs(static_cast<double>(audio[i]) - static_cast<double>(hist));
            maxAbsHistorical = std::max(maxAbsHistorical, bypassError);
            const int region = scenario.note < 0 ? 2 : (static_cast<int>(i) < guard ? 0 : static_cast<int>(i) < releaseAt ? 1 : 2);
            regionHistorical[region] = std::max(regionHistorical[region], bypassError);
            const double passError = std::abs(static_cast<double>(audio[i]) - static_cast<double>(row.pre));
            regionBypass[region] = std::max(regionBypass[region], row.dormant ? 0.0 : passError);
            ++regionCount[region];
            maxAbsBypass = std::max(maxAbsBypass, row.dormant ? 0.0 : passError);
        }

        const auto blocked = blockedGolden(scenario.name);
        const auto* current = swaraxt_current_golden::find(
            swaraxt_current_golden::kClassic, swaraxt_current_golden::kClassicCount, scenario.name);
        const auto blockedHash = hashSamples(reconstructed);
        const auto liveHash = hashSamples(audio);
        const bool blockedMatch = blocked != nullptr && blockedHash == blocked->hash;
        const bool liveMatch = current != nullptr && liveHash == current->hash;
        std::printf("%s live=%016llx blockedReplay=%016llx blockedMatch=%d bitMismatches=%d appliedMismatches=%d\n",
                    scenario.name,
                    static_cast<unsigned long long>(liveHash),
                    static_cast<unsigned long long>(blockedHash),
                    blockedMatch ? 1 : 0,
                    bitMismatches,
                    appliedMismatches);
        std::printf("  max|live-blocked|=%.3e regions(guard,sustain,release)=%.3e %.3e %.3e counts=%d %d %d\n",
                    maxAbsHistorical, regionHistorical[0], regionHistorical[1], regionHistorical[2],
                    regionCount[0], regionCount[1], regionCount[2]);
        std::printf("  max|live-pre|=%.3e by region %.3e %.3e %.3e\n",
                    maxAbsBypass, regionBypass[0], regionBypass[1], regionBypass[2]);
        expect(capture.rows.size() == static_cast<size_t>(kSamples), scenario.name);
        expect(bitMismatches == 0, "predicted bypass matches rendered samples");
        expect(appliedMismatches == 0, "protection guard matches blocker application");
        expect(blockedMatch, "continuous blocker replay matches the historical golden");
        expect(liveMatch, "bypass render matches the current classic reference");
        if (scenario.note < 0)
            expect(liveHash == blockedHash, "silence is unchanged by the bypass");
    }
#endif
    std::printf("%s\n", failures == 0 ? "Classic DC differential: PASSED" : "Classic DC differential: FAILED");
    return failures == 0 ? 0 : 1;
}
