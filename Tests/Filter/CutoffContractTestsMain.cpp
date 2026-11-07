// Copyright 2026 MontroneDSP.
// SPDX-License-Identifier: GPL-3.0-or-later
//
// Cutoff contracts for the panel mapping and the Classic board's audio path.

#include "Engine/Filter/SwaraXtFilter.h"
#include "Engine/SwaraXtEngine.h"
#include "Plugin/PluginProcessor.h"
#include "Plugin/SwaraXtParameterLayout.h"

#include <cmath>
#include <cstdio>
#include <limits>
#include <string>

namespace {

int gFailures = 0;

void expect(bool cond, const char* message)
{
    if (! cond)
    {
        std::printf("FAIL: %s\n", message);
        ++gFailures;
    }
}

void testCutoffRoundtrip()
{
    SwaraXtAudioProcessor proc;
    auto* parameter = proc.getApvts().getParameter(swaraxt::IDs::filterCutoff);
    expect(parameter != nullptr, "cutoff parameter exists");
    if (! parameter)
        return;

    const double important[] = { 20.0, 25.0, 30.0, 40.0, 50.0, 70.0, 100.0,
                                 150.0, 200.0, 500.0, 1000.0, 2000.0 };
    for (double hz : important)
    {
        float normalized = parameter->convertTo0to1(static_cast<float>(hz));
        double recovered = parameter->convertFrom0to1(normalized);
        const double relErr = std::fabs(recovered - hz) / hz;
        expect(relErr < 0.01, "cutoff normalized roundtrip");
    }
}

void testCutoffMonotonicity()
{
    SwaraXtAudioProcessor proc;
    auto* parameter = proc.getApvts().getParameter(swaraxt::IDs::filterCutoff);
    expect(parameter != nullptr, "cutoff parameter exists");
    if (! parameter)
        return;

    double previousNormalized = -1.0;
    double previousCoefficient = -1.0;
    double minimumCoefficientStep = std::numeric_limits<double>::max();
    int currentPlateau = 0;
    int longestPlateau = 0;

    constexpr int points = 512;
    swaraxt::CutoffMapper mapper;
    for (int i = 0; i < points; ++i)
    {
        const double position = static_cast<double>(i) / static_cast<double>(points - 1);
        const double requested = 10.0 * std::pow(2000.0, position);
        float normalized = parameter->convertTo0to1(static_cast<float>(requested));
        double physical = parameter->convertFrom0to1(normalized);
        double mapped = mapper.mapCutoffHz(physical, 0.0, 69.0, 0.0);
        double coefficient = swaraxt::CutoffMapper::fourPoleMinus3DbRatio()
            * mapped;
        if (i > 0)
        {
            expect(normalized > previousNormalized, "cutoff low-end monotonicity");
            expect(coefficient > previousCoefficient, "cutoff coefficient monotonicity");
            const double step = coefficient - previousCoefficient;
            minimumCoefficientStep = std::min(minimumCoefficientStep, step);
            currentPlateau = step <= 1.0e-12 ? currentPlateau + 1 : 0;
            longestPlateau = std::max(longestPlateau, currentPlateau);
        }
        previousNormalized = normalized;
        previousCoefficient = coefficient;
    }

    expect(longestPlateau == 0, "cutoff no low-end plateau");
    expect(minimumCoefficientStep > 1.0e-8, "cutoff dense sweep retains coefficient separation");
}

constexpr double nativeRate = swaraxt::SwaraXtEngine::kInternalSampleRate;
// A byte's voltage is one 51st of a volt. This tolerance is much smaller
// than one byte, but comfortably above the settled double rounding error.
constexpr double cvTolerance = 1.0e-9;

void testExplicitBoardCvAuthority()
{
    swaraxt::SwaraXtFilter filter;
    filter.prepare(nativeRate);
    swaraxt::SwaraXtFilterParams params;
    const auto settle = [&filter]
    {
        bool finite = true;
        // 512 native samples cover the 128-sample input-control delay and
        // over 50 time constants of the 184.8 us cutoff CV smoother.
        for (int i = 0; i < 512; ++i)
            finite &= std::isfinite(filter.processInstrumentSample(i % 16 < 8 ? 0.25f : -0.25f, 1.0f));
        expect(finite && filter.instrumentVcaControl() > 0.9f, "explicit CV fixture renders with the VCA open");
    };

    params.cutoffHz = 20000.0f;
    params.boardCutoffCvVolts = 0.75;
    filter.setParams(params);
    settle();
    expect(std::abs(filter.boardCutoffCvForTests() - 0.75) < cvTolerance,
           "explicit low CV overrides the high diagnostic Hz input");

    params.cutoffHz = 10.0f;
    params.boardCutoffCvVolts = 4.25;
    filter.setParams(params);
    settle();
    expect(std::abs(filter.boardCutoffCvForTests() - 4.25) < cvTolerance,
           "explicit high CV overrides the low diagnostic Hz input after a live edit");
}

void setParameter(SwaraXtAudioProcessor& processor, const char* id, float value)
{
    auto* parameter = processor.getApvts().getParameter(id);
    expect(parameter != nullptr, id);
    if (parameter)
        parameter->setValueNotifyingHost(parameter->convertTo0to1(value));
}

struct CutoffCase
{
    int firmwareByte;
    int note;
    int panelCode;
    float keyTracking;
    int expectedBoardCode;
};

void testFirmwareCutoff(const CutoffCase& test)
{
    using namespace swaraxt;
    SwaraXtAudioProcessor processor;
    setParameter(processor, IDs::osc1Option, 0.0f);
    setParameter(processor, IDs::seqMode, 0.0f);
    setParameter(processor, IDs::midiChannel, 0.0f);
    setParameter(processor, IDs::midiInputTuning, 0.0f);
    setParameter(processor, IDs::perfGlide, 0.0f);
    setParameter(processor, IDs::filterEnvDepth, 0.0f);
    setParameter(processor, IDs::filterLfoDepth, 0.0f);
    setParameter(processor, IDs::filterResonance, 0.0f);
    setParameter(processor, IDs::env1Attack, 0.0f);
    setParameter(processor, IDs::env1Decay, 0.0f);
    setParameter(processor, IDs::env1Sustain, 127.0f);
    for (int row = 1; row <= 12; ++row)
    {
        const auto id = "mod.row" + std::to_string(row) + ".amount";
        setParameter(processor, id.c_str(), row == 9 ? 63.0f : 0.0f);
    }

    setParameter(processor, IDs::filterCutoff, 1000.0f);
    setParameter(processor, IDs::filterKeyTracking, test.keyTracking);
    processor.prepareToPlay(48000, 128);
    juce::AudioBuffer<float> audio(2, 128);
    juce::MidiBuffer midi;
    midi.addEvent(juce::MidiMessage::noteOn(1, test.note, juce::uint8(110)), 0);
    const auto render = [&]
    {
        bool finite = true;
        // About 1670 native samples through the active voice and Classic path.
        for (int block = 0; block < 16; ++block)
        {
            processor.processBlock(audio, midi);
            midi.clear();
            for (int i = 0; i < audio.getNumSamples(); ++i)
                finite &= std::isfinite(audio.getSample(0, i));
        }
        return finite;
    };
    const bool warmupFinite = render();

    auto& engine = processor.engineForTests();
    const auto& filter = engine.filter();
    const double initialCv = filter.boardCutoffCvForTests();
    // The panel uses semitone codes. Below its representable range, the
    // panel's 10 Hz minimum selects code zero. Note tracking takes place
    // in the firmware before the byte reaches the Classic filter.
    const auto panelHz = test.panelCode == 0 ? 10.0f
        : static_cast<float>(20000.0 * std::exp2((test.panelCode - 127) / 12.0));
    setParameter(processor, IDs::filterCutoff, panelHz);
    bool activeClassicOutput = false;
    engine.setDebugTapSink(&activeClassicOutput, [](void* context, const SwaraXtEngine::DebugBlockCapture& block)
    {
        auto& active = *static_cast<bool*>(context);
        for (int i = 0; i < block.samples; ++i)
            active |= block.vcaGain[i] > 0 && std::isfinite(block.filterOutput[i]) && block.filterOutput[i] != 0;
    });
    const bool finite = render();
    engine.setDebugTapSink(nullptr, nullptr);
    const int actualByte = engine.shruthiPart().voice().cutoff();
    const double expectedCv = test.expectedBoardCode / 51.0;
    const double targetCv = filter.paramsForTests().boardCutoffCvVolts;
    const double actualCv = filter.boardCutoffCvForTests();
    const double expectedHz = 20000.0 * std::exp2((test.expectedBoardCode - 254) / 24.0);
    const double actualHz = Ir3109BoardCore::cutoffHzFromCv(actualCv);
    const double relativeError = std::abs(actualHz / expectedHz - 1.0);

    std::printf("Firmware cutoff byte=%d note=%d trim=%.1f board code=%d actual byte=%d"
                " target=%.10g V settled=%.10g V frequency=%.10g Hz relative error=%.6f%%\n",
                test.firmwareByte, test.note, static_cast<double>(test.keyTracking), test.expectedBoardCode,
                actualByte, targetCv, actualCv, actualHz, 100.0 * relativeError);
    expect(warmupFinite && finite && activeClassicOutput,
           "firmware cutoff fixture renders finite audio with the Classic VCA open");
    expect(actualByte == test.firmwareByte, "held note and panel setting produce the expected firmware cutoff byte");
    expect(std::abs(initialCv - expectedCv) > cvTolerance, "cutoff edit changes the initial board CV");
    expect(std::abs(targetCv - expectedCv) < cvTolerance, "engine publishes the expected explicit firmware board CV");
    expect(std::abs(actualCv - expectedCv) < cvTolerance, "processed Classic board settles to the explicit firmware CV");
    // Compare the modeled board cutoff law with the firmware exponential.
    // 1.5% is about 26 cents, well below the 50-cent step of one cutoff byte.
    expect(relativeError <= 0.015, "settled CV frequency agrees with the firmware exponential");
}

void testFirmwareByteTransfer()
{
    for (int code : {0, 32, 64, 96, 128, 160, 192, 224})
        testFirmwareCutoff({code, 64, code / 2, 0.5f, code});
    testFirmwareCutoff({255, 76, 127, 0.5f, 255});
}

void testKeyTrackingTrim()
{
    // At full trim, notes an octave either side of 64 add +/-24 byte codes
    // after the native tracking already included in the firmware byte.
    testFirmwareCutoff({128, 76, 52, 1.0f, 152});
    testFirmwareCutoff({128, 52, 76, 1.0f, 104});
    testFirmwareCutoff({255, 76, 127, 1.0f, 255});
    testFirmwareCutoff({0, 52, 0, 1.0f, 0});
}

}  // namespace

int main()
{
    juce::ScopedJuceInitialiser_GUI init;
    testCutoffRoundtrip();
    testCutoffMonotonicity();
    testExplicitBoardCvAuthority();
    testFirmwareByteTransfer();
    testKeyTrackingTrim();
    std::printf(gFailures == 0 ? "Cutoff contract tests: PASSED\n"
                               : "Cutoff contract tests: FAILED (%d)\n",
                gFailures);
    return gFailures == 0 ? 0 : 1;
}
