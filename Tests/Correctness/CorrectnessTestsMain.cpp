// Copyright 2026 MontroneDSP.
// SPDX-License-Identifier: GPL-3.0-or-later
// Range safety, PWM restoration, and LFO visualizer regressions.

#include <JuceHeader.h>

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <set>
#include <string>
#include <vector>

#include "Engine/SwaraXtEngine.h"
#include "Plugin/PluginProcessor.h"
#include "Plugin/SwaraXtParameterLayout.h"
#include "Ui/SwaraXtLfoVisualizer.h"
#include "Ui/SwaraXtModulationNames.h"
#include "Ui/SwaraXtPanels.h"

#include "avrlib/base.h"
#include "avrlib/random.h"
#include "avr/pgmspace.h"
#include "shruthi/audio_out.h"
#include "shruthi/lfo.h"
#include "shruthi/midi_dispatcher.h"
#include "shruthi/note_stack.h"
#include "shruthi/oscillator.h"
#include "shruthi/parameter.h"
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

void testNoteStackInvariants()
{
    constexpr uint8_t kFreeSlot = 0xff;
    shruthi::NoteStack stack;
    stack.Init();
    expect(stack.size() == 0, "note stack initializes empty");
    expect(stack.dummy().note == kFreeSlot, "empty note stack exposes its free dummy node");

    stack.NoteOff(60);
    expect(stack.size() == 0, "note-off on an empty stack is harmless");
    stack.NoteOn(60, 90);
    expect(stack.size() == 1 && stack.most_recent_note().note == 60,
           "first note occupies a valid stack slot");
    stack.NoteOn(60, 111);
    expect(stack.size() == 1 && stack.most_recent_note().velocity == 111,
           "duplicate note-on refreshes velocity without duplicating the note");

    stack.Clear();
    constexpr std::array<uint8_t, kNoteStackSize> notes { 67, 60, 64, 62, 69, 61, 65, 63 };
    constexpr std::array<uint8_t, kNoteStackSize> sorted { 60, 61, 62, 63, 64, 65, 67, 69 };
    for (const auto note : notes)
        stack.NoteOn(note, static_cast<uint8_t>(note + 20));

    expect(stack.size() == stack.max_size(), "eight distinct notes fill the stack exactly");
    expect(stack.least_recent_note().note == notes.front(),
           "least-recent selection follows insertion age, not pitch");
    expect(stack.most_recent_note().note == notes.back(),
           "most-recent selection follows the linked-list root");
    for (uint8_t i = 0; i < stack.size(); ++i)
        expect(stack.sorted_note(i).note == sorted[i],
               "pitch-sorted note access remains ordered at full capacity");

    stack.NoteOn(70, 100);
    expect(stack.size() == stack.max_size(), "saturation keeps the fixed stack capacity");
    expect(stack.least_recent_note().note == 60,
           "saturation evicts exactly the least-recent note");
    bool evictedNoteRemains = false;
    for (uint8_t i = 0; i < stack.size(); ++i)
        evictedNoteRemains = evictedNoteRemains || stack.sorted_note(i).note == notes.front();
    expect(! evictedNoteRemains, "voice stealing removes the evicted note from sorted access");

    stack.NoteOff(64);
    expect(stack.size() == stack.max_size() - 1, "note-off frees exactly one pool slot");
    stack.NoteOn(71, 101);
    expect(stack.size() == stack.max_size() && stack.most_recent_note().note == 71,
           "a freed pool slot is reused without selecting the dummy slot");

    stack.Clear();
    for (int event = 0; event < 4096; ++event)
    {
        const uint8_t note = static_cast<uint8_t>(36 + event % 24);
        stack.NoteOn(note, static_cast<uint8_t>(1 + event % 127));
        if (event % 3 == 0)
            stack.NoteOff(static_cast<uint8_t>(36 + (event + 7) % 24));

        expect(stack.size() <= stack.max_size(), "rapid note storm never exceeds pool capacity");
        std::set<uint8_t> activeNotes;
        for (uint8_t slot = 1; slot <= stack.max_size(); ++slot)
        {
            const auto active = stack.note(slot).note;
            if (active != kFreeSlot)
                activeNotes.insert(active);
        }
        expect(activeNotes.size() == stack.size(),
               "rapid note storm preserves unique occupied pool slots");
        for (uint8_t i = 1; i < stack.size(); ++i)
            expect(stack.sorted_note(i - 1).note < stack.sorted_note(i).note,
                   "rapid note storm preserves strict pitch ordering");
    }
}

void testShruthiSignedParameterMetadata()
{
    struct ExpectedRange {
        uint8_t parameterIndex;
        int8_t minimum;
        int8_t maximum;
    };
    constexpr std::array expectedRanges {
        ExpectedRange { 2, -24, 24 },
        ExpectedRange { 6, -24, 24 },
        ExpectedRange { 35, -63, 63 },
        ExpectedRange { 44, -2, 2 },
        ExpectedRange { 48, -127, 127 },
    };

    for (const auto expected : expectedRanges)
    {
        const auto& parameter = shruthi::ParameterManager::parameter(expected.parameterIndex);
        expect(parameter.unit == shruthi::UNIT_INT8,
               "signed Shruthi metadata keeps the UNIT_INT8 interpretation");
        expect(static_cast<int8_t>(parameter.min_value) == expected.minimum,
               "signed Shruthi metadata preserves the minimum byte pattern");
        expect(static_cast<int8_t>(parameter.max_value) == expected.maximum,
               "signed Shruthi metadata preserves the maximum byte pattern");
        if (expected.minimum > -128)
            expect(parameter.Clamp(static_cast<uint8_t>(expected.minimum - 1))
                       == parameter.min_value,
                   "signed Shruthi metadata clamps below its minimum");
        if (expected.maximum < 127)
            expect(parameter.Clamp(static_cast<uint8_t>(expected.maximum + 1))
                       == parameter.max_value,
                   "signed Shruthi metadata clamps above its maximum");
        expect(parameter.Increment(parameter.min_value, -1) == parameter.min_value,
               "signed Shruthi metadata rejects decrement below minimum");
        expect(parameter.Increment(parameter.max_value, 1) == parameter.max_value,
               "signed Shruthi metadata rejects increment above maximum");
    }
}

void testPgmspaceStrncpyCompatibility()
{
    std::array<char, 8> destination;
    destination.fill('?');
    char* const original = destination.data();
    expect(strncpy_P(destination.data(), "ignored", 0) == original,
           "strncpy_P returns the original destination for zero length");
    expect(destination[0] == '?', "strncpy_P leaves the destination untouched for zero length");

    destination.fill('?');
    expect(strncpy_P(destination.data(), "abcdef", 4) == original,
           "strncpy_P returns the original destination when truncating");
    expect(std::memcmp(destination.data(), "abcd", 4) == 0 && destination[4] == '?',
           "strncpy_P truncates without adding a terminator when the source is too long");

    destination.fill('?');
    strncpy_P(destination.data(), "xy", 5);
    const std::array<char, 5> padded { 'x', 'y', '\0', '\0', '\0' };
    expect(std::memcmp(destination.data(), padded.data(), padded.size()) == 0,
           "strncpy_P null-pads the complete requested count");
    expect(destination[5] == '?', "strncpy_P does not write beyond the requested count");

    destination.fill('?');
    strncpy_P(destination.data(), "xy", 3);
    expect(destination[0] == 'x' && destination[1] == 'y' && destination[2] == '\0',
           "strncpy_P copies an exact-fit source terminator");
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

struct MixerTrace {
    std::vector<uint8_t> audio;
    std::vector<uint8_t> osc1;
    std::vector<uint8_t> osc2;
};

MixerTrace renderMainMixerOperator(int op, int mix, bool twoNotes = false,
                                   int sequenceMask = -1)
{
    ShruthiRuntime runtime;
    runtime.init();
    configureSquarePart(runtime.part, 48);
    auto* patch = runtime.part.mutable_patch();
    patch->osc[0].shape = shruthi::WAVEFORM_SAW;
    patch->osc[0].parameter = 24;
    patch->osc[0].option = static_cast<uint8_t>(op);
    patch->osc[1].shape = shruthi::WAVEFORM_SQUARE;
    patch->osc[1].parameter = 72;
    patch->osc[1].range = 7;
    patch->osc[1].option = 19;
    patch->mix_balance = static_cast<uint8_t>(mix);
    if (sequenceMask >= 0)
    {
        patch->mix_sub_osc = 63;
        patch->mix_noise = 63;
        for (int step = 0; step < shruthi::kNumSteps; ++step)
            runtime.part.mutable_sequencer_settings()->steps[step]
                .set_controller(static_cast<uint8_t>(sequenceMask));
    }
    runtime.part.Touch(false);

    runtime.ring.Init();
    runtime.part.NoteOn(0, 60, 100);
    if (twoNotes)
        runtime.part.NoteOn(0, 67, 100);

    MixerTrace trace;
    for (int block = 0; block < 16; ++block)
    {
        runtime.part.ProcessBlock();
        const auto* osc1 = runtime.part.voice().debug_osc1_buffer();
        const auto* osc2 = runtime.part.voice().debug_osc2_buffer();
        trace.osc1.insert(trace.osc1.end(), osc1, osc1 + shruthi::kAudioBlockSize);
        trace.osc2.insert(trace.osc2.end(), osc2, osc2 + shruthi::kAudioBlockSize);
        while (runtime.ring.readable())
            trace.audio.push_back(runtime.ring.ImmediateRead());
    }
    return trace;
}

bool hasExactHolds(const std::vector<uint8_t>& samples, size_t holdLength)
{
    if (samples.empty() || samples.size() % holdLength != 0)
        return false;
    for (size_t start = 0; start < samples.size(); start += holdLength)
        for (size_t offset = 1; offset < holdLength; ++offset)
            if (samples[start] != samples[start + offset])
                return false;
    return true;
}

std::vector<uint8_t> renderLfoShape(int shape)
{
    avrlib::Random random;
    random.Seed(0x21);
    shruthi::SequencerSettings sequence {};
    sequence.pattern_size = 16;
    for (int step = 0; step < shruthi::kNumSteps; ++step)
        sequence.steps[step].set_controller(static_cast<uint8_t>(step));

    shruthi::Lfo lfo;
    lfo.Init(&random);
    lfo.Update(static_cast<uint8_t>(shape), 1024, 0, shruthi::LFO_MODE_FREE);
    lfo.Reset();
    std::vector<uint8_t> trace;
    trace.reserve(128);
    for (int sample = 0; sample < 132; ++sample)
    {
        const auto value = lfo.Render(sequence);
        if (sample >= 4)
            trace.push_back(value);
    }
    return trace;
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
    expect(swaraxt::ui::modulationDestinationNames()[shruthi::MOD_DST_PWM_1] == "Timbre 1",
           "oscillator-parameter destination 1 is presented as Timbre 1");
    expect(swaraxt::ui::modulationDestinationNames()[shruthi::MOD_DST_PWM_2] == "Timbre 2",
           "oscillator-parameter destination 2 is presented as Timbre 2");
    expect(swaraxt::ui::modulationDestinationNames().indexOf("PWM 1") < 0
               && swaraxt::ui::modulationDestinationNames().indexOf("PWM 2") < 0,
           "PWM 1/PWM 2 labels are not used in the modulation matrix");
    expect(swaraxt::ui::isHardwareOnlyModulationSource(shruthi::MOD_SRC_CV_1)
               && swaraxt::ui::isHardwareOnlyModulationSource(shruthi::MOD_SRC_CV_4),
           "physical CV inputs are classified hardware-only");
    expect(swaraxt::ui::isHardwareOnlyModulationDestination(shruthi::MOD_DST_CV_1)
               && swaraxt::ui::isHardwareOnlyModulationDestination(shruthi::MOD_DST_CV_2),
           "physical CV outputs are classified hardware-only");
    expect(swaraxt::ui::pluginVisibleModulationSourceCount()
               == static_cast<int>(shruthi::kNumModulationSources) - 4,
           "four hardware CV sources are hidden from the plugin menu");
    expect(swaraxt::ui::pluginVisibleModulationDestinationCount()
               == static_cast<int>(shruthi::kNumModulationDestinations) - 2,
           "two hardware CV destinations are hidden from the plugin menu");

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

void testJuce9ParameterMetadataCompatibility()
{
    SwaraXtAudioProcessor proc;
    const auto& parameters = proc.getParameters();
    expect(parameters.size() == 88, "JUCE 9 preserves the host-visible parameter count");

    std::set<std::string> parameterIds;
    for (const auto* parameter : parameters)
    {
        expect(parameter != nullptr, "parameter list contains no null entries");
        if (parameter != nullptr)
        {
            expect(parameter->getVersionHint() == 1,
                   "all existing parameters preserve version hint 1");
            const auto* withId = dynamic_cast<const juce::AudioProcessorParameterWithID*>(parameter);
            expect(withId != nullptr, "all existing parameters retain stable string IDs");
            if (withId != nullptr)
                parameterIds.insert(withId->paramID.toStdString());
        }
    }
    expect(parameterIds.size() == parameters.size(), "parameter IDs remain unique");

    auto checkInt = [&](const char* id,
                        const char* name,
                        int minimum,
                        int maximum,
                        int defaultValue) {
        const auto* parameter = dynamic_cast<const juce::AudioParameterInt*>(
            proc.getApvts().getParameter(id));
        expect(parameter != nullptr, "expected integer parameter type is preserved");
        if (parameter == nullptr)
            return;
        expect(parameter->getParameterID() == id, "integer parameter ID is preserved");
        expect(parameter->getName(128) == name, "integer parameter name is preserved");
        expect(std::lround(parameter->convertFrom0to1(0.0f)) == minimum,
               "integer parameter minimum is preserved");
        expect(std::lround(parameter->convertFrom0to1(1.0f)) == maximum,
               "integer parameter maximum is preserved");
        const auto* base = static_cast<const juce::AudioProcessorParameter*>(parameter);
        expect(std::lround(parameter->convertFrom0to1(base->getDefaultValue()))
                   == defaultValue,
               "integer parameter default is preserved");
        expect(base->getNumSteps() == maximum - minimum + 1,
               "integer parameter step count is preserved");
        expect(parameter->isAutomatable(), "integer parameter remains automatable");
        expect(! parameter->isDiscrete(),
               "integer parameter preserves the JUCE 7 discrete flag contract");
        expect(! parameter->isBoolean(), "integer parameter does not become boolean");
    };

    checkInt(swaraxt::IDs::osc1Range, "Osc 1 Range", -48, 48, 0);
    checkInt(swaraxt::IDs::osc2Range, "Osc 2 Range", -48, 48, -12);
    checkInt(swaraxt::IDs::osc1Option, "Mixer Operator", 0, 13, 0);
    checkInt(swaraxt::IDs::lfo1Wave, "LFO1 Wave", 0, 20, 1);
    checkInt(swaraxt::IDs::lfo2Wave, "LFO2 Wave", 0, 20, 1);
    checkInt("mod.row1.source", "Mod Source 1", 0, 31, 0);
    checkInt("mod.row1.destination", "Mod Dest 1", 0, 26, 4);
    checkInt("mod.row1.amount", "Mod Amount 1", -63, 63, 0);

    const auto* cutoff = dynamic_cast<const juce::AudioParameterFloat*>(
        proc.getApvts().getParameter(swaraxt::IDs::filterCutoff));
    expect(cutoff != nullptr, "cutoff remains an AudioParameterFloat");
    if (cutoff != nullptr)
    {
        const auto* base = static_cast<const juce::AudioProcessorParameter*>(cutoff);
        expect(cutoff->getParameterID() == swaraxt::IDs::filterCutoff,
               "cutoff parameter ID is preserved");
        expect(std::abs(cutoff->convertFrom0to1(base->getDefaultValue()) - 8000.0f) < 0.01f,
               "cutoff physical default is preserved");
        expect(std::abs(cutoff->convertFrom0to1(base->getValueForText("1 kHz")) - 1000.0f)
                   < 0.01f,
               "cutoff text parser is preserved");
    }
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

void testShruthiFilterControlDomain()
{
    auto runRoute = [](uint8_t source, uint8_t destination, int8_t amount,
                       uint8_t sourceValue) {
        ShruthiRuntime runtime;
        runtime.init();
        zeroPartMatrix(runtime.part);
        auto* patch = runtime.part.mutable_patch();
        patch->filter_cutoff = 80;
        patch->filter_resonance = 32;
        patch->modulation_matrix.modulation[0].source = source;
        patch->modulation_matrix.modulation[0].destination = destination;
        patch->modulation_matrix.modulation[0].amount = amount;
        runtime.part.mutable_voice()->set_modulation_source(source, sourceValue);
        runtime.part.mutable_voice()->ProcessControlBlock();
        return std::pair<int, int> {
            runtime.part.voice().cutoff_matrix_delta(),
            runtime.part.voice().resonance_matrix_delta()
        };
    };

    const auto lfoLow = runRoute(shruthi::MOD_SRC_LFO_1,
                                 shruthi::MOD_DST_FILTER_CUTOFF, 30, 0);
    const auto lfoCentre = runRoute(shruthi::MOD_SRC_LFO_1,
                                    shruthi::MOD_DST_FILTER_CUTOFF, 30, 128);
    const auto lfoHigh = runRoute(shruthi::MOD_SRC_LFO_1,
                                  shruthi::MOD_DST_FILTER_CUTOFF, 30, 255);
    const auto lfoInvertedLow = runRoute(shruthi::MOD_SRC_LFO_1,
                                         shruthi::MOD_DST_FILTER_CUTOFF, -30, 0);
    const auto offsetPositive = runRoute(shruthi::MOD_SRC_OFFSET,
                                         shruthi::MOD_DST_FILTER_CUTOFF, 20, 255);
    const auto offsetNegative = runRoute(shruthi::MOD_SRC_OFFSET,
                                         shruthi::MOD_DST_FILTER_CUTOFF, -20, 255);
    const auto resonanceNegative = runRoute(shruthi::MOD_SRC_OFFSET,
                                            shruthi::MOD_DST_FILTER_RESONANCE, -20, 255);

    expect(lfoLow.first == -3840, "centered LFO +30 reaches -30 cutoff display units");
    expect(lfoCentre.first == 0, "centered LFO is neutral at 128");
    expect(lfoHigh.first == 3810, "centered LFO +30 reaches the positive endpoint");
    expect(lfoInvertedLow.first == 3840, "negative centered amount inverts polarity");
    expect(offsetPositive.first == 5100, "unipolar +20 reaches about +40 cutoff display units");
    expect(offsetNegative.first == -5100, "negative unipolar amount moves below the base");
    expect(resonanceNegative.second == -5100, "negative resonance modulation remains signed");
}

void testSwaraFilterRouteSeparation()
{
    SwaraXtAudioProcessor proc;
    auto setPhysical = [&](const juce::String& id, float value) {
        auto* parameter = proc.getApvts().getParameter(id);
        expect(parameter != nullptr, "filter-route test parameter exists");
        if (parameter != nullptr)
            parameter->setValueNotifyingHost(parameter->convertTo0to1(value));
    };

    setPhysical(swaraxt::IDs::filterCutoff, 1000.0f);
    setPhysical(swaraxt::IDs::filterResonance, 0.5f);
    setPhysical(swaraxt::IDs::filterEnvAmount, 0.0f);
    setPhysical(swaraxt::IDs::filterKeyTracking, 0.0f);
    setPhysical(swaraxt::IDs::filterModAmount, 1.0f);
    for (int row = 1; row <= shruthi::kModulationMatrixSize; ++row)
        setPhysical("mod.row" + juce::String(row) + ".amount", 0.0f);

    setPhysical("mod.row1.source", static_cast<float>(shruthi::MOD_SRC_OFFSET));
    setPhysical("mod.row1.destination", static_cast<float>(shruthi::MOD_DST_FILTER_CUTOFF));
    setPhysical("mod.row1.amount", 20.0f);

    proc.prepareToPlay(44100.0, 256);
    juce::AudioBuffer<float> buffer(2, 256);
    juce::MidiBuffer midi;
    midi.addEvent(juce::MidiMessage::noteOn(1, 69, static_cast<juce::uint8>(100)), 0);
    buffer.clear();
    proc.processBlock(buffer, midi);

    const auto& cutoffParams = proc.engineForTests().filter().paramsForTests();
    expect(std::abs(cutoffParams.matrixCutoffOctaves - (5100.0f / 1536.0f)) < 1.0e-6f,
           "matrix cutoff uses Shruthi control units directly");
    const int lfo2 = proc.engineForTests().shruthiPart().voice().modulation_source(shruthi::MOD_SRC_LFO_2);
    expect(std::abs(cutoffParams.modValue - static_cast<float>(lfo2 - 128) / 128.0f) < 1.0e-6f,
           "Filter MOD is the dedicated centered LFO2 route");

    setPhysical("mod.row1.destination", static_cast<float>(shruthi::MOD_DST_FILTER_RESONANCE));
    setPhysical("mod.row1.amount", -20.0f);
    midi.clear();
    buffer.clear();
    proc.processBlock(buffer, midi);
    const auto& resonanceParams = proc.engineForTests().filter().paramsForTests();
    expect(std::abs(resonanceParams.matrixCutoffOctaves) < 1.0e-6f,
           "resonance routes do not leak into cutoff");
    expect(std::abs(resonanceParams.resonance - (0.5f - 5100.0f / 16320.0f)) < 1.0e-6f,
           "negative matrix resonance lowers the panel value");
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

    for (int waveform = shruthi::LFO_WAVEFORM_WAVE_1;
         waveform < shruthi::LFO_WAVEFORM_LAST;
         ++waveform)
    {
        int shapeOffset = waveform - shruthi::LFO_WAVEFORM_WAVE_1;
        shapeOffset = shapeOffset == 0 ? 3 : shapeOffset + 16;
        for (const int phaseIndex : { 0, 17, 63, 101, 127 })
        {
            const auto expected = shruthi::ResourcesManager::Lookup<uint8_t, uint8_t>(
                shruthi::wav_res_waves + shapeOffset * 129,
                static_cast<uint8_t>(phaseIndex));
            const float phase = static_cast<float>(phaseIndex) / 128.0f;
            expect(std::abs(swaraxt::ui::lfoVisualizerY01(waveform, phase)
                            - static_cast<float>(expected) / 255.0f) < 1.0e-6f,
                   "wavetable LFO visualizer matches the Shruthi resource waveform");
        }
    }
}

void testRenderedLfoShapes()
{
    std::vector<std::vector<uint8_t>> traces;
    traces.reserve(shruthi::LFO_WAVEFORM_LAST);
    for (int shape = 0; shape < shruthi::LFO_WAVEFORM_LAST; ++shape)
    {
        traces.push_back(renderLfoShape(shape));
        expect(traces.back().size() == 128,
               "every exposed LFO waveform renders a complete control-rate trace");
    }

    const std::set<uint8_t> triangle(traces[shruthi::LFO_WAVEFORM_TRIANGLE].begin(),
                                     traces[shruthi::LFO_WAVEFORM_TRIANGLE].end());
    const std::set<uint8_t> square(traces[shruthi::LFO_WAVEFORM_SQUARE].begin(),
                                   traces[shruthi::LFO_WAVEFORM_SQUARE].end());
    const std::set<uint8_t> sampleHold(traces[shruthi::LFO_WAVEFORM_S_H].begin(),
                                       traces[shruthi::LFO_WAVEFORM_S_H].end());
    const std::set<uint8_t> ramp(traces[shruthi::LFO_WAVEFORM_RAMP].begin(),
                                 traces[shruthi::LFO_WAVEFORM_RAMP].end());
    const std::set<uint8_t> step(traces[shruthi::LFO_WAVEFORM_STEP_SEQUENCER].begin(),
                                 traces[shruthi::LFO_WAVEFORM_STEP_SEQUENCER].end());
    expect(triangle.size() > 32, "triangle LFO produces a continuous rising/falling trace");
    expect(square.size() == 2, "square LFO produces exactly two control levels");
    expect(sampleHold.size() <= 3,
           "sample-and-hold LFO holds one random value for each complete cycle");
    expect(ramp.size() > 32 && traces[shruthi::LFO_WAVEFORM_RAMP]
                                  != traces[shruthi::LFO_WAVEFORM_TRIANGLE],
           "ramp LFO produces a distinct continuous trace");
    expect(step.size() == 16,
           "step-sequencer LFO renders all sixteen configured controller values");

    for (int shape = shruthi::LFO_WAVEFORM_WAVE_1;
         shape < shruthi::LFO_WAVEFORM_LAST;
         ++shape)
    {
        const std::set<uint8_t> values(traces[shape].begin(), traces[shape].end());
        expect(values.size() > 2, "each additional Shruthi LFO wave produces modulation");
        expect(traces[shape] != traces[shruthi::LFO_WAVEFORM_TRIANGLE],
               "additional Shruthi LFO waves do not fall back to triangle output");
    }
}

void testMainMixerOperatorDsp()
{
    for (int op = 0; op < shruthi::OP_LAST; ++op)
    {
        const auto center = renderMainMixerOperator(op, 32);
        expect(center.audio.size() == 16 * shruthi::kAudioBlockSize,
               "all fourteen main mixer modes render the native audio block count");

        const auto low = renderMainMixerOperator(op, 0);
        const auto high = renderMainMixerOperator(op, 63);
        if (op < shruthi::OP_PING_PONG_2)
            expect(low.audio != high.audio,
                   "MIX changes the native behavior of every non-sequenced mixer mode");
        else
            expect(low.audio == high.audio,
                   "sequenced mixer modes use their source mask instead of normal balance");
    }

    const auto sum = renderMainMixerOperator(shruthi::OP_SUM, 32);
    const auto sync = renderMainMixerOperator(shruthi::OP_SYNC, 32);
    expect(sum.osc1 == sync.osc1,
           "sync leaves oscillator 1 as the master waveform");
    expect(sum.osc2 != sync.osc2,
           "sync routes oscillator 1 resets into oscillator 2");

    expect(hasExactHolds(renderMainMixerOperator(shruthi::OP_CRUSH_4, 32).audio, 4),
           ">>4 holds each mixed sample for four native samples");
    expect(hasExactHolds(renderMainMixerOperator(shruthi::OP_CRUSH_8, 32).audio, 8),
           ">>8 holds each mixed sample for eight native samples");

    const auto duoOneNote = renderMainMixerOperator(shruthi::OP_DUO, 32, false);
    const auto duoTwoNotes = renderMainMixerOperator(shruthi::OP_DUO, 32, true);
    expect(duoOneNote.audio != duoTwoNotes.audio && duoOneNote.osc2 != duoTwoNotes.osc2,
           "Duo assigns the second oscillator to the additional held note");

    const auto seqNone = renderMainMixerOperator(shruthi::OP_PING_PONG_SEQ, 32, false, 0);
    const auto seqOsc2 = renderMainMixerOperator(shruthi::OP_PING_PONG_SEQ, 32, false, 1);
    const auto seqOsc1 = renderMainMixerOperator(shruthi::OP_PING_PONG_SEQ, 32, false, 2);
    const auto seqSub = renderMainMixerOperator(shruthi::OP_PING_PONG_SEQ, 32, false, 4);
    const auto seqNoise = renderMainMixerOperator(shruthi::OP_PING_PONG_SEQ, 32, false, 8);
    expect(seqNone.audio != seqOsc1.audio && seqNone.audio != seqOsc2.audio,
           "seqmix bits 0 and 1 enable oscillator 2 and oscillator 1 independently");
    expect(seqOsc1.audio != seqOsc2.audio,
           "seqmix oscillator source bits are not swapped or collapsed");
    expect(seqNone.audio != seqSub.audio,
           "seqmix bit 2 enables the sub oscillator independently");
    expect(seqNone.audio != seqNoise.audio,
           "seqmix bit 3 enables noise independently");
}

std::vector<uint8_t> renderFm(int range, int timbre, int note)
{
    ShruthiRuntime runtime;
    runtime.init();
    configureSquarePart(runtime.part, static_cast<uint8_t>(timbre));
    auto* patch = runtime.part.mutable_patch();
    patch->osc[0].shape = shruthi::WAVEFORM_FM;
    patch->osc[0].range = static_cast<int8_t>(range);
    runtime.part.Touch(false);
    return renderOsc1(runtime, 16, note);
}

void testLfoMatrixAndMixerMappings()
{
    SwaraXtAudioProcessor proc;
    proc.prepareToPlay(48000.0, 128);
    auto setPhysical = [&](const char* id, int value) {
        auto* parameter = proc.getApvts().getParameter(id);
        expect(parameter != nullptr, id);
        if (parameter != nullptr)
            parameter->setValueNotifyingHost(parameter->convertTo0to1(static_cast<float>(value)));
    };
    auto process = [&] {
        juce::AudioBuffer<float> buffer(2, 128);
        juce::MidiBuffer midi;
        proc.processBlock(buffer, midi);
    };

    for (int waveform = 0; waveform < shruthi::LFO_WAVEFORM_LAST; ++waveform)
    {
        setPhysical(swaraxt::IDs::lfo1Wave, waveform);
        setPhysical(swaraxt::IDs::lfo2Wave,
                    static_cast<int>(shruthi::LFO_WAVEFORM_LAST) - 1 - waveform);
        process();
        const auto& part = proc.engineForTests().shruthiPart();
        expect(part.patch().lfo[0].waveform == waveform,
               "LFO1 APVTS code reaches the Shruthi patch unchanged");
        expect(part.patch().lfo[1].waveform
                   == static_cast<int>(shruthi::LFO_WAVEFORM_LAST) - 1 - waveform,
               "LFO2 APVTS code reaches the Shruthi patch unchanged");
        expect(part.lfo_shape_for_tests(0) == waveform,
               "LFO1 patch changes refresh the live LFO object");
        expect(part.lfo_shape_for_tests(1)
                   == static_cast<int>(shruthi::LFO_WAVEFORM_LAST) - 1 - waveform,
               "LFO2 patch changes refresh the live LFO object");
    }

    for (int op = 0; op < 14; ++op)
    {
        setPhysical(swaraxt::IDs::osc1Option, op);
        process();
        expect(proc.engineForTests().shruthiPart().patch().osc[0].option == op,
               "all fourteen APVTS mixer operators map to the same Shruthi code");
    }

    for (int row = 0; row < shruthi::kModulationMatrixSize; ++row)
    {
        const auto sourceId = "mod.row" + juce::String(row + 1) + ".source";
        const auto destinationId = "mod.row" + juce::String(row + 1) + ".destination";
        const auto amountId = "mod.row" + juce::String(row + 1) + ".amount";
        const int source = (row * 7) % shruthi::kNumModulationSources;
        const int destination = (row * 5) % shruthi::kNumModulationDestinations;
        const int amounts[] { -63, -1, 0, 1, 63 };
        setPhysical(sourceId.toRawUTF8(), source);
        setPhysical(destinationId.toRawUTF8(), destination);
        setPhysical(amountId.toRawUTF8(), amounts[row % 5]);
    }
    process();
    for (int row = 0; row < shruthi::kModulationMatrixSize; ++row)
    {
        const auto& route = proc.engineForTests().shruthiPart().patch()
                                .modulation_matrix.modulation[row];
        const int amounts[] { -63, -1, 0, 1, 63 };
        expect(route.source == (row * 7) % shruthi::kNumModulationSources,
               "matrix source APVTS code reaches the same Shruthi enum");
        expect(route.destination == (row * 5) % shruthi::kNumModulationDestinations,
               "matrix destination APVTS code reaches the same Shruthi enum");
        expect(route.amount == amounts[row % 5],
               "matrix amount remains signed at engine ingress");
    }

    for (int source = 0; source < shruthi::kNumModulationSources; ++source)
    {
        setPhysical("mod.row1.source", source);
        process();
        expect(proc.engineForTests().shruthiPart().patch()
                   .modulation_matrix.modulation[0].source == source,
               "every matrix source preserves its complete Shruthi enum value");
    }
    for (int destination = 0;
         destination < shruthi::kNumModulationDestinations;
         ++destination)
    {
        setPhysical("mod.row1.destination", destination);
        process();
        expect(proc.engineForTests().shruthiPart().patch()
                   .modulation_matrix.modulation[0].destination == destination,
               "every matrix destination preserves its complete Shruthi enum value");
    }

    for (const auto [apvts, shruthiValue] :
         { std::pair<int, int> { 0, 0 }, { 1, 0 }, { 63, 31 },
           { 64, 32 }, { 126, 63 }, { 127, 63 } })
    {
        setPhysical(swaraxt::IDs::mixBalance, apvts);
        process();
        expect(proc.engineForTests().shruthiPart().patch().mix_balance == shruthiValue,
               "0..127 Mix Balance maps monotonically to Shruthi 0..63");
    }
}

void testFmRangeSemantics()
{
    SwaraXtAudioProcessor proc;
    proc.prepareToPlay(48000.0, 128);
    auto setPhysical = [&](const char* id, int value) {
        auto* parameter = proc.getApvts().getParameter(id);
        expect(parameter != nullptr, id);
        if (parameter != nullptr)
            parameter->setValueNotifyingHost(parameter->convertTo0to1(static_cast<float>(value)));
    };
    for (int row = 0; row < shruthi::kModulationMatrixSize; ++row)
    {
        const auto amountId = "mod.row" + juce::String(row + 1) + ".amount";
        setPhysical(amountId.toRawUTF8(), 0);
    }
    setPhysical(swaraxt::IDs::osc1Shape, shruthi::WAVEFORM_FM);
    setPhysical(swaraxt::IDs::osc2Shape, shruthi::WAVEFORM_FM);
    setPhysical(swaraxt::IDs::osc2Option, 0);

    juce::MidiBuffer note;
    note.addEvent(juce::MidiMessage::noteOn(1, 60, static_cast<juce::uint8>(100)), 0);
    juce::AudioBuffer<float> buffer(2, 128);
    proc.processBlock(buffer, note);
    const auto& voice = proc.engineForTests().shruthiPart().voice();
    uint16_t fmCarrierIncrement[2] {};
    for (const int range : { -24, -18, -13, -12, -11, -6, 0, 6, 11, 12, 13, 18, 24 })
    {
        setPhysical(swaraxt::IDs::osc1Range, range);
        setPhysical(swaraxt::IDs::osc2Range, range);
        juce::MidiBuffer empty;
        buffer.clear();
        proc.processBlock(buffer, empty);
        const auto& patch = proc.engineForTests().shruthiPart().patch();
        expect(patch.osc[0].range == range && patch.osc[1].range == range,
               "both FM oscillators receive the complete Shruthi -24..24 range");
        expect(voice.debug_oscillator_secondary_parameter(0) == range + 24
                   && voice.debug_oscillator_secondary_parameter(1) == range + 24,
               "FM range reaches the native secondary ratio parameter");
        for (int oscillator = 0; oscillator < 2; ++oscillator)
        {
            const auto increment = voice.debug_oscillator_increment(oscillator);
            if (range == -24)
                fmCarrierIncrement[oscillator] = increment;
            else
                expect(increment == fmCarrierIncrement[oscillator],
                       "FM range changes modulator ratio without transposing the carrier");
        }
    }

    const auto fmMinus24 = renderFm(-24, 64, 60);
    const auto fmMinus13 = renderFm(-13, 64, 60);
    const auto fmMinus12 = renderFm(-12, 64, 60);
    const auto fmMinus11 = renderFm(-11, 64, 60);
    const auto fmPlus11 = renderFm(11, 64, 60);
    const auto fmPlus12 = renderFm(12, 64, 60);
    const auto fmPlus13 = renderFm(13, 64, 60);
    const auto fmPlus24 = renderFm(24, 64, 60);
    expect(fmMinus24 == fmMinus13 && fmMinus13 == fmMinus12,
           "native FM ratio saturates at the -12 boundary");
    expect(fmMinus11 != fmMinus12, "native FM ratio changes immediately inside -12");
    expect(fmPlus11 != fmPlus12, "native FM ratio changes immediately inside +12");
    expect(fmPlus12 == fmPlus13 && fmPlus13 == fmPlus24,
           "native FM ratio saturates at the +12 boundary");
    expect(renderFm(0, 0, 60) != renderFm(0, 127, 60),
           "FM TIMBRE controls modulation depth");
    expect(renderFm(0, 64, 36) != renderFm(0, 64, 48)
               && renderFm(0, 64, 48) != renderFm(0, 64, 60)
               && renderFm(0, 64, 60) != renderFm(0, 64, 72),
           "FM carrier and modulator track C2 through C5");

    setPhysical(swaraxt::IDs::osc1Shape, shruthi::WAVEFORM_SAW);
    uint16_t normalIncrement[3] {};
    int index = 0;
    for (const int range : { -24, 0, 24 })
    {
        setPhysical(swaraxt::IDs::osc1Range, range);
        juce::MidiBuffer empty;
        proc.processBlock(buffer, empty);
        normalIncrement[index++] = voice.debug_oscillator_increment(0);
    }
    expect(normalIncrement[0] < normalIncrement[1]
               && normalIncrement[1] < normalIncrement[2],
           "normal oscillator range transposes across the full -24..24 span");
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

void testModulationMenuAndLegacyHardwareIds()
{
    SwaraXtAudioProcessor proc;
    proc.prepareToPlay(44100.0, 64);
    swaraxt::ui::ModPanel panel;
    panel.attach(proc);

    auto* destParam = proc.getApvts().getParameter("mod.row1.destination");
    auto* sourceParam = proc.getApvts().getParameter("mod.row1.source");
    expect(destParam != nullptr && sourceParam != nullptr, "mod row 1 parameters exist");

    auto& destCombo = panel.destinationComboForTests(0);
    destCombo.setSelectedId(shruthi::MOD_DST_PWM_1 + 1, juce::sendNotificationSync);
    destCombo.setSelectedId(shruthi::MOD_DST_PWM_2 + 1, juce::sendNotificationSync);
    expect(std::lround(destParam->convertFrom0to1(destParam->getValue())) == shruthi::MOD_DST_PWM_2,
           "selecting Timbre 2 writes the original PWM 2 destination ID");
    destCombo.setSelectedId(shruthi::MOD_DST_PWM_1 + 1, juce::sendNotificationSync);
    expect(std::lround(destParam->convertFrom0to1(destParam->getValue())) == shruthi::MOD_DST_PWM_1,
           "selecting Timbre 1 writes the original PWM 1 destination ID");
    expect(destCombo.indexOfItemId(shruthi::MOD_DST_CV_1 + 1) < 0,
           "hardware CV destinations cannot be selected from the visible menu");
    expect(panel.sourceComboForTests(0).indexOfItemId(shruthi::MOD_SRC_CV_1 + 1) < 0,
           "hardware CV sources cannot be selected from the visible menu");

    sourceParam->setValueNotifyingHost(
        sourceParam->convertTo0to1(static_cast<float>(shruthi::MOD_SRC_CV_1)));
    destParam->setValueNotifyingHost(
        destParam->convertTo0to1(static_cast<float>(shruthi::MOD_DST_CV_1)));

    juce::AudioBuffer<float> buffer(2, 64);
    juce::MidiBuffer midi;
    midi.addEvent(juce::MidiMessage::noteOn(1, 60, static_cast<juce::uint8>(100)), 0);
    proc.processBlock(buffer, midi);

    const auto& route = proc.engineForTests().shruthiPart().patch().modulation_matrix.modulation[0];
    expect(route.source == shruthi::MOD_SRC_CV_1, "legacy hardware CV source ID is preserved");
    expect(route.destination == shruthi::MOD_DST_CV_1, "legacy hardware CV destination ID is preserved");
    expect(proc.engineForTests().shruthiPart().voice().modulation_source(shruthi::MOD_SRC_CV_1) == 0,
           "unsupported physical CV input contributes deterministic zero");
    for (int i = 0; i < buffer.getNumSamples(); ++i)
        expect(std::isfinite(buffer.getSample(0, i)), "legacy hardware-only route remains finite");
}

}  // namespace

int main()
{
    juce::ScopedJuceInitialiser_GUI juce;
    testNoteStackInvariants();
    testShruthiSignedParameterMetadata();
    testPgmspaceStrncpyCompatibility();
    testGuiEngineRanges();
    testInvalidRestoredState();
    testJuce9ParameterMetadataCompatibility();
    testPwmDutyAndParity();
    testNonSquareUnchangedPath();
    testLfoTimbreRoute();
    testShruthiFilterControlDomain();
    testSwaraFilterRouteSeparation();
    testLfoVisualizer();
    testRenderedLfoShapes();
    testLfoMatrixAndMixerMappings();
    testMainMixerOperatorDsp();
    testFmRangeSemantics();
    testFactoryPulseLeadFinite();
    testKeyTrackParameter();
    testModulationMenuAndLegacyHardwareIds();
    std::printf(gFailures == 0 ? "Swara XT correctness tests: PASSED\n"
                               : "Swara XT correctness tests: FAILED (%d)\n",
                gFailures);
    return gFailures == 0 ? 0 : 1;
}
