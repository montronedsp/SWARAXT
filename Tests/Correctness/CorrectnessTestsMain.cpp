// Copyright 2026 MontroneDSP.
// SPDX-License-Identifier: GPL-3.0-or-later
// Range safety, PWM restoration, and LFO visualizer regressions.

#include <JuceHeader.h>

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

#include "Engine/SwaraXtEngine.h"
#include "Plugin/PluginProcessor.h"
#include "Plugin/SwaraXtParameterLayout.h"
#include "Ui/SwaraXtLfoVisualizer.h"
#include "Ui/SwaraXtModulationNames.h"

#include "avrlib/base.h"
#include "avrlib/random.h"
#include "shruthi/audio_out.h"
#include "shruthi/midi_dispatcher.h"
#include "shruthi/oscillator.h"
#include "shruthi/part.h"
#include "shruthi/patch.h"
#include "shruthi/storage.h"

#if !SWARAXT_ENABLE_SHRUTHI_DEBUG_TAPS
#error SWARAXT_ENABLE_SHRUTHI_DEBUG_TAPS must be enabled for these tests.
#endif

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

struct ShruthiRuntime {
    shruthi::HostAudioRing ring;
    avrlib::Random random;
    shruthi::MidiDispatcher midi;
    shruthi::Storage storage;
    shruthi::Part part;

    void init()
    {
        random.Seed(0x21);
        ring.Init();
        part.Init(ring, random, midi, storage);
        part.ProcessBlock();
        ring.Init();
    }
};

void zeroPartMatrix(shruthi::Part& part)
{
    auto* patch = part.mutable_patch();
    for (int row = 0; row < shruthi::kModulationMatrixSize; ++row)
    {
        patch->modulation_matrix.modulation[row].source = shruthi::MOD_SRC_OFFSET;
        patch->modulation_matrix.modulation[row].destination = shruthi::MOD_DST_VCA;
        patch->modulation_matrix.modulation[row].amount = 0;
    }
}

void configureSquarePart(shruthi::Part& part, uint8_t timbre)
{
    auto* patch = part.mutable_patch();
    patch->osc[0].shape = shruthi::WAVEFORM_SQUARE;
    patch->osc[0].parameter = timbre;
    patch->osc[0].range = 0;
    patch->osc[0].option = 0;
    patch->osc[1].shape = shruthi::WAVEFORM_NONE;
    patch->osc[1].parameter = 0;
    patch->osc[1].range = 0;
    patch->osc[1].option = 0;
    patch->mix_balance = 0;
    patch->mix_sub_osc = 0;
    patch->mix_noise = 0;
    patch->env[0].attack = 0;
    patch->env[0].decay = 0;
    patch->env[0].sustain = 127;
    patch->env[0].release = 0;
    patch->env[1].attack = 0;
    patch->env[1].decay = 0;
    patch->env[1].sustain = 127;
    patch->env[1].release = 0;
    part.mutable_voice()->RefreshEnvelopeRatesFromPatch();
    zeroPartMatrix(part);
    part.Touch(false);
}

struct PwmStats {
    double duty = 0.0;
    double rms = 0.0;
    double peak = 0.0;
    double fundHz = 0.0;
    int invalid = 0;
};

double estimatePitchHz(const std::vector<uint8_t>& samples, double rate)
{
    if (samples.size() < 8)
        return 0.0;
    int crossings = 0;
    for (size_t i = 1; i < samples.size(); ++i)
    {
        const bool prev = samples[i - 1] >= 128;
        const bool cur = samples[i] >= 128;
        if (prev != cur)
            ++crossings;
    }
    return (static_cast<double>(crossings) * rate) / (2.0 * static_cast<double>(samples.size()));
}

PwmStats measurePwm(const std::vector<uint8_t>& samples, double rate)
{
    PwmStats s;
    if (samples.empty())
        return s;
    double sumSq = 0.0;
    int high = 0;
    for (uint8_t sample : samples)
    {
        if (sample != sample) // unreachable for uint8, keeps the pattern obvious
            ++s.invalid;
        const double centered = (static_cast<double>(sample) - 128.0) / 128.0;
        sumSq += centered * centered;
        s.peak = std::max(s.peak, std::abs(centered));
        if (sample > 128)
            ++high;
    }
    s.duty = static_cast<double>(high) / static_cast<double>(samples.size());
    s.rms = std::sqrt(sumSq / static_cast<double>(samples.size()));
    s.fundHz = estimatePitchHz(samples, rate);
    return s;
}

std::vector<uint8_t> renderOsc1(ShruthiRuntime& runtime, int blocks, int note)
{
    runtime.ring.Init();
    runtime.part.NoteOn(0, static_cast<uint8_t>(note), 100);
    std::vector<uint8_t> out;
    out.reserve(static_cast<size_t>(blocks * shruthi::kAudioBlockSize));
    for (int block = 0; block < blocks; ++block)
    {
        runtime.part.ProcessBlock();
        const auto* raw = runtime.part.voice().debug_osc1_buffer();
        out.insert(out.end(), raw, raw + shruthi::kAudioBlockSize);
    }
    runtime.part.NoteOff(0, static_cast<uint8_t>(note));
    return out;
}

void testGuiEngineRanges()
{
    expect(swaraxt::ui::oscillatorNames().size() == static_cast<int>(shruthi::WAVEFORM_LAST),
           "oscillator GUI names cover 0 .. WAVEFORM_LAST-1");
    expect(swaraxt::ui::oscillatorNames().indexOf("Reserved") < 0,
           "Reserved oscillator model is not listed");
    expect(swaraxt::ui::modulationDestinationNames().size()
               == static_cast<int>(shruthi::kNumModulationDestinations),
           "destination GUI names match engine destination count");
    expect(swaraxt::ui::modulationSourceNames().size()
               == static_cast<int>(shruthi::kNumModulationSources),
           "source GUI names match engine source count");
    expect(swaraxt::ui::modulationDestinationNames().indexOf("Dest 27") < 0,
           "placeholder Dest 27 is not listed");

    SwaraXtAudioProcessor proc;
    const auto* osc1 = dynamic_cast<juce::AudioParameterInt*>(
        proc.getApvts().getParameter(swaraxt::IDs::osc1Shape));
    const auto* osc2 = dynamic_cast<juce::AudioParameterInt*>(
        proc.getApvts().getParameter(swaraxt::IDs::osc2Shape));
    const auto* dest = dynamic_cast<juce::AudioParameterInt*>(
        proc.getApvts().getParameter("mod.row1.destination"));
    const auto* source = dynamic_cast<juce::AudioParameterInt*>(
        proc.getApvts().getParameter("mod.row1.source"));
    expect(osc1 != nullptr && std::lround(osc1->convertFrom0to1(1.0f))
               == static_cast<int>(shruthi::WAVEFORM_LAST) - 1,
           "osc1.shape APVTS max is WAVEFORM_LAST-1");
    expect(osc2 != nullptr && std::lround(osc2->convertFrom0to1(1.0f))
               == static_cast<int>(shruthi::WAVEFORM_LAST) - 1,
           "osc2.shape APVTS max is WAVEFORM_LAST-1");
    expect(dest != nullptr && std::lround(dest->convertFrom0to1(1.0f))
               == static_cast<int>(shruthi::kNumModulationDestinations) - 1,
           "mod destination APVTS max is kNumModulationDestinations-1");
    expect(source != nullptr && std::lround(source->convertFrom0to1(1.0f))
               == static_cast<int>(shruthi::kNumModulationSources) - 1,
           "mod source APVTS max is kNumModulationSources-1");
}

void testInvalidRestoredState()
{
    SwaraXtAudioProcessor proc;
    proc.prepareToPlay(44100.0, 128);

    auto forceInt = [&](const char* id, float unit) {
        if (auto* p = proc.getApvts().getParameter(id))
            p->setValueNotifyingHost(unit);
    };

    forceInt(swaraxt::IDs::osc1Shape, 1.0f);
    forceInt(swaraxt::IDs::osc2Shape, 1.0f);
    forceInt("mod.row1.destination", 1.0f);
    forceInt("mod.row1.source", 1.0f);

    juce::AudioBuffer<float> buffer(2, 128);
    juce::MidiBuffer midi;
    buffer.clear();
    proc.processBlock(buffer, midi);

    const auto& patch = proc.engineForTests().shruthiPart().patch();
    expect(patch.osc[0].shape < shruthi::WAVEFORM_LAST, "restored osc1 shape is a valid waveform");
    expect(patch.osc[1].shape < shruthi::WAVEFORM_LAST, "restored osc2 shape is a valid waveform");
    expect(patch.modulation_matrix.modulation[0].destination
               < shruthi::kNumModulationDestinations,
           "restored destination is in range");
    expect(patch.modulation_matrix.modulation[0].source < shruthi::kNumModulationSources,
           "restored source is in range");

    for (int i = 0; i < buffer.getNumSamples(); ++i)
        expect(std::isfinite(buffer.getSample(0, i)), "malformed-state output is finite");
}

void testPwmDutyAndParity()
{
    constexpr double kRate = swaraxt::SwaraXtEngine::kInternalSampleRate;
    constexpr int kNote = 60;
    const int kTimbre[] = { 0, 16, 32, 64, 96, 127 };
    double previousDuty = -1.0;
    bool changed = false;

    for (int timbre : kTimbre)
    {
        ShruthiRuntime runtime;
        runtime.init();
        configureSquarePart(runtime.part, static_cast<uint8_t>(timbre));
        const auto samples = renderOsc1(runtime, 200, kNote);
        const auto stats = measurePwm(samples, kRate);
        std::printf("PWM timbre=%3d duty=%.4f rms=%.4f peak=%.4f pitch=%.1f\n",
                    timbre, stats.duty, stats.rms, stats.peak, stats.fundHz);
        expect(stats.invalid == 0, "PWM samples are valid");
        expect(stats.peak > 0.05 || timbre == 127, "PWM output is not silent");
        if (timbre < 127)
            expect(stats.fundHz > 200.0 && stats.fundHz < 320.0, "PWM pitch remains near C4");
        if (previousDuty >= 0.0 && std::abs(stats.duty - previousDuty) > 0.02)
            changed = true;
        previousDuty = stats.duty;
    }
    expect(changed, "increasing Timbre changes PWM duty cycle");

    constexpr uint8_t kGuard = 0xA5;
    shruthi::Oscillator oscillator;
    oscillator.Reset();
    uint8_t syncIn[shruthi::kAudioBlockSize + 8];
    uint8_t syncOut[shruthi::kAudioBlockSize + 8];
    uint8_t buffer[shruthi::kAudioBlockSize + 8];
    std::memset(syncIn, kGuard, sizeof(syncIn));
    std::memset(syncOut, kGuard, sizeof(syncOut));
    std::memset(buffer, kGuard, sizeof(buffer));
    uint24_t increment {};
    increment.integral = 0x0400;
    increment.fractional = 0;
    oscillator.set_parameter(64);
    oscillator.Render(shruthi::WAVEFORM_SQUARE,
                      60,
                      increment,
                      syncIn + 4,
                      syncOut + 4,
                      buffer + 4);
    expect(syncIn[0] == kGuard && syncIn[1] == kGuard && syncIn[2] == kGuard && syncIn[3] == kGuard,
           "PWM does not read before sync_input");
    expect(syncIn[shruthi::kAudioBlockSize + 4] == kGuard, "PWM does not read past sync_input");
    expect(syncOut[0] == kGuard && syncOut[3] == kGuard, "PWM does not write before sync_output");
    expect(syncOut[shruthi::kAudioBlockSize + 4] == kGuard, "PWM does not write past sync_output");
    expect(buffer[0] == kGuard && buffer[3] == kGuard, "PWM does not write before audio buffer");
    expect(buffer[shruthi::kAudioBlockSize + 4] == kGuard, "PWM does not write past audio buffer");
}

void testNonSquareUnchangedPath()
{
    const uint8_t shapes[] = {
        shruthi::WAVEFORM_SAW,
        shruthi::WAVEFORM_TRIANGLE,
        shruthi::WAVEFORM_WAVETABLE_1,
        shruthi::WAVEFORM_FM
    };
    for (uint8_t shape : shapes)
    {
        ShruthiRuntime runtime;
        runtime.init();
        auto* patch = runtime.part.mutable_patch();
        patch->osc[0].shape = shape;
        patch->osc[0].parameter = 64;
        patch->osc[1].shape = shruthi::WAVEFORM_NONE;
        patch->mix_balance = 0;
        patch->mix_sub_osc = 0;
        patch->mix_noise = 0;
        zeroPartMatrix(runtime.part);
        runtime.part.Touch(false);
        const auto samples = renderOsc1(runtime, 80, 60);
        expect(! samples.empty(), "non-square model produced samples");
        bool energy = false;
        for (uint8_t sample : samples)
        {
            if (sample != 128)
                energy = true;
        }
        expect(energy, "non-square model is not silent");
    }
}

void testLfoTimbreRoute()
{
    auto runRoute = [](uint8_t source, int8_t amount) {
        ShruthiRuntime runtime;
        runtime.init();
        configureSquarePart(runtime.part, 64);
        auto* patch = runtime.part.mutable_patch();
        patch->lfo[0].waveform = shruthi::LFO_WAVEFORM_TRIANGLE;
        patch->lfo[0].rate = 80;
        patch->modulation_matrix.modulation[0].source = source;
        patch->modulation_matrix.modulation[0].destination = shruthi::MOD_DST_PWM_1;
        patch->modulation_matrix.modulation[0].amount = amount;
        runtime.part.Touch(false);
        runtime.ring.Init();
        runtime.part.NoteOn(0, 60, 100);

        int srcMin = 255, srcMax = 0;
        int dstMin = 32767, dstMax = -32768;
        int paramMin = 255, paramMax = 0;
        for (int block = 0; block < 400; ++block)
        {
            runtime.part.ProcessBlock();
            const auto& voice = runtime.part.voice();
            const int src = voice.modulation_source(source);
            const int dst = voice.debug_destination14(shruthi::MOD_DST_PWM_1);
            const int param = dst >> 7;
            srcMin = std::min(srcMin, src);
            srcMax = std::max(srcMax, src);
            dstMin = std::min(dstMin, dst);
            dstMax = std::max(dstMax, dst);
            paramMin = std::min(paramMin, param);
            paramMax = std::max(paramMax, param);
        }
        std::printf("route src=%u amount=%+d src=%d..%d dst14=%d..%d param=%d..%d\n",
                    static_cast<unsigned>(source), static_cast<int>(amount),
                    srcMin, srcMax, dstMin, dstMax, paramMin, paramMax);
        return std::pair<int, int> { paramMin, paramMax };
    };

    const auto posLfo = runRoute(shruthi::MOD_SRC_LFO_1, 63);
    const auto negLfo = runRoute(shruthi::MOD_SRC_LFO_1, -63);
    const auto posEnv = runRoute(shruthi::MOD_SRC_ENV_1, 63);
    const auto negEnv = runRoute(shruthi::MOD_SRC_ENV_1, -63);
    expect(posLfo.second > posLfo.first, "positive LFO depth swings Timbre");
    expect(negLfo.second > negLfo.first, "negative LFO depth swings Timbre");
    expect(posEnv.second >= 64, "positive ENV depth raises Timbre from the unipolar source");
    expect(negEnv.first <= 64, "negative ENV depth lowers Timbre from the unipolar source");
}

void testLfoVisualizer()
{
    expect(swaraxt::ui::lfoVisualizerY01(0, 0.0f) > 0.9f,
           "triangle visualizer starts at the top");
    expect(swaraxt::ui::lfoVisualizerY01(0, 0.5f) < 0.1f,
           "triangle visualizer is low at mid-cycle");
    expect(swaraxt::ui::lfoVisualizerY01(3, 0.0f) < 0.1f,
           "ramp visualizer starts at the bottom");
    expect(swaraxt::ui::lfoVisualizerY01(3, 1.0f) > 0.9f
               || swaraxt::ui::lfoVisualizerY01(3, 0.99f) > 0.9f,
           "ramp visualizer rises through the cycle");
    expect(swaraxt::ui::lfoVisualizerY01(1, 0.25f) < swaraxt::ui::lfoVisualizerY01(1, 0.75f),
           "square visualizer remains low then high");
}

void testFactoryPulseLeadFinite()
{
    SwaraXtAudioProcessor proc;
    proc.setCurrentProgram(1);
    proc.prepareToPlay(44100.0, 128);
    juce::MidiBuffer midi;
    midi.addEvent(juce::MidiMessage::noteOn(1, 60, (juce::uint8) 100), 0);
    juce::AudioBuffer<float> buffer(2, 2048);
    buffer.clear();
    proc.processBlock(buffer, midi);
    float peak = 0.0f;
    for (int i = 0; i < buffer.getNumSamples(); ++i)
    {
        const float s = buffer.getSample(0, i);
        expect(std::isfinite(s), "Pulse Lead output is finite");
        peak = std::max(peak, std::abs(s));
    }
    expect(peak > 0.001f, "Pulse Lead produces audio");
}

void testKeyTrackParameter()
{
    SwaraXtAudioProcessor proc;
    auto* p = proc.getApvts().getParameter(swaraxt::IDs::filterKeyTracking);
    expect(p != nullptr, "filter_key_tracking exists");
    expect(std::abs(p->getDefaultValue() - p->convertTo0to1(0.5f)) < 1.0e-6f,
           "Key Track default is 0.5");
}

}  // namespace

int main()
{
    juce::ScopedJuceInitialiser_GUI juce;
    testGuiEngineRanges();
    testInvalidRestoredState();
    testPwmDutyAndParity();
    testNonSquareUnchangedPath();
    testLfoTimbreRoute();
    testLfoVisualizer();
    testFactoryPulseLeadFinite();
    testKeyTrackParameter();
    std::printf(gFailures == 0 ? "Swara XT correctness tests: PASSED\n"
                               : "Swara XT correctness tests: FAILED (%d)\n",
                gFailures);
    return gFailures == 0 ? 0 : 1;
}
