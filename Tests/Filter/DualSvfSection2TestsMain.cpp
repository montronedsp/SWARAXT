// Copyright 2026 MontroneDSP.
// SPDX-License-Identifier: GPL-3.0-or-later
#include <JuceHeader.h>
#include "Plugin/PluginEditor.h"
#include "Engine/Filter/ShruthiSvfSection2Control.h"
#include "shruthi/resources.h"
#include "DualSvfSection2Goldens.h"
#include <array>
#include <atomic>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <thread>

#if defined(__linux__)
extern "C" void swaraxtBeginAllocationAudit() __attribute__((weak));
extern "C" unsigned long swaraxtEndAllocationAudit() __attribute__((weak));
#endif

namespace {
using namespace swaraxt;
using Selection = ExperimentalSvfSelection;
using Control = ShruthiSvfSection2Control;
int failures = 0;
void expect(bool ok, const char* message)
{
    if (! ok) { ++failures; std::printf("FAIL: %s\n", message); }
}
bool finite(const juce::AudioBuffer<float>& b)
{
    for (int ch = 0; ch < b.getNumChannels(); ++ch)
        for (int i = 0; i < b.getNumSamples(); ++i)
            if (! std::isfinite(b.getSample(ch, i))) return false;
    return true;
}
bool identical(double a, double b) { return std::memcmp(&a, &b, sizeof(a)) == 0; }
struct Sources {
    std::array<std::uint8_t, shruthi::kNumModulationSources> bytes {};
    std::uint16_t cutoff = 8192;
    std::uint8_t modulation_source(std::uint8_t i) const { return bytes[i]; }
    std::uint16_t filter_cutoff14() const { return cutoff; }
};

void referenceControls()
{
    // Cross-check the reconstruction against the existing compiled HD voice's
    // complete SVF-board transaction, not a second copy of the new helper.
    avrlib_hd::Random rng;
    avrlib_hd::HdVoice voice;
    voice.Init(&rng);
    avrlib_hd::EnvelopeTables envelopes;
    envelopes.portamento_increments = shruthi::lut_res_env_portamento_increments;
    envelopes.env_expo = shruthi::wav_res_env_expo;
    voice.set_envelope_tables(envelopes);
    avrlib_hd::HdVoiceSystemSettings system;
    system.expansion_filter_board = avrlib_hd::kFilterBoardSvf;
    avrlib_hd::HdVoicePatch hd;
    hd.filter_cutoff = 64;
    hd.filter_env = 31;
    hd.filter_lfo = -23;
    shruthi::Patch patch {};
    Sources sources;
    std::uint32_t random = 12345;
    auto byte = [&] { random = random * 1664525u + 1013904223u; return static_cast<std::uint8_t>(random >> 24); };
    int checked = 0;
    for (int iteration = 0; iteration < 18000; ++iteration)
    {
        for (auto& b : sources.bytes) b = byte();
        for (int i = 0; i < shruthi::kModulationMatrixSize; ++i)
        {
            auto& row = patch.modulation_matrix.modulation[i];
            row.source = static_cast<std::uint8_t>(byte() % shruthi::kNumModulationSources);
            row.destination = static_cast<std::uint8_t>((i & 1) != 0 ? shruthi::MOD_DST_CV_1 : shruthi::MOD_DST_CV_2);
            row.amount = static_cast<std::int8_t>(static_cast<int>(byte() % 127) - 63);
            hd.mod[i].source = row.source;
            hd.mod[i].destination = row.destination;
            hd.mod[i].amount = row.amount;
        }
        const auto cut = static_cast<std::uint8_t>(iteration % 128);
        const auto res = static_cast<std::uint8_t>((iteration / 128) % 64);
        const bool relative = (iteration & 1) != 0;
        hd.filter_cutoff_2 = cut;
        hd.filter_resonance_2 = res;
        hd.filter_1_mode = relative ? avrlib_hd::kFilterModeLpCoupled : 0;
        voice.import_modulation_sources(sources.bytes.data());
        voice.NoteOn(static_cast<std::uint16_t>((24 + iteration % 90) * 128), 100, 0, true);
        voice.ProcessControlBlock(hd, system);
        voice.export_modulation_sources(sources.bytes.data());
        sources.cutoff = voice.filter_cutoff14();
        const auto result = reconstructSvfSection2(patch, sources, cut, res, relative);
        std::array<std::int8_t, avrlib_hd::kNumModulationDestinations> destinations {};
        voice.export_modulation_destinations(destinations.data());
        expect(result.cutoff == static_cast<std::uint8_t>(destinations[shruthi::MOD_DST_CV_1])
            && result.resonance == static_cast<std::uint8_t>(destinations[shruthi::MOD_DST_CV_2]),
            "reconstruction agrees with compiled firmware SVF control transaction");
        ++checked;
    }
    patch = {};
    sources.bytes[shruthi::MOD_SRC_OFFSET] = 255;
    patch.modulation_matrix.modulation[0] = { shruthi::MOD_SRC_OFFSET, shruthi::MOD_DST_CV_1, 63 };
    patch.modulation_matrix.modulation[1] = { shruthi::MOD_SRC_OFFSET, shruthi::MOD_DST_CV_1, -63 };
    expect(reconstructSvfSection2(patch, sources, 127, 0, false).cutoff == 4,
        "positive saturation precedes negative modulation row");
    patch = {};
    sources.bytes[shruthi::MOD_SRC_WHEEL] = 1;
    patch.modulation_matrix.modulation[shruthi::kModulationMatrixSize - 1] =
        { shruthi::MOD_SRC_OFFSET, shruthi::MOD_DST_CV_1, -1 };
    expect(reconstructSvfSection2(patch, sources, 64, 0, false).cutoff == 124,
        "negative last-row wheel scaling uses arithmetic floor");
    patch = {};
    for (int cut = 0; cut <= 127; ++cut)
        for (int res = 0; res <= 63; ++res)
        {
            const auto c = reconstructSvfSection2(patch, sources, static_cast<std::uint8_t>(cut),
                static_cast<std::uint8_t>(res), false);
            expect(c.cutoff == cut * 2 && c.resonance == res * 4, "all unmodulated control codes exact");
        }
    for (int f1 : { 0, 1, 127, 4095, 8192, 8255, 12000, 16383 })
        for (int offset : { 0, 52, 64, 76, 127 })
        {
            sources.cutoff = static_cast<std::uint16_t>(f1);
            const auto c = reconstructSvfSection2(patch, sources, static_cast<std::uint8_t>(offset), 63, true);
            expect(c.cutoff == std::clamp(offset * 128 + f1 - 8192, 0, 16383) / 64
                && c.resonance == 252, "52/64/76 relative offsets, fractional cutoff and both clipping boundaries");
        }
    std::printf("Firmware references: %d HD SVF transactions, 8192 exact base pairs, 40 relative boundaries\n", checked);
}

void packedControls()
{
    expect(Selection::pack(Selection {}) == 0 && Selection::unpack(0).cutoff2 == 64,
        "legacy packed zero retains defaults");
    for (int mode = 0; mode < 3; ++mode)
        for (int cut = 0; cut < 128; ++cut)
            for (int res = 0; res < 64; ++res)
            {
                Selection s;
                s.control2 = static_cast<Control>(mode);
                s.cutoff2 = static_cast<std::uint8_t>(cut);
                s.resonance2 = static_cast<std::uint8_t>(res);
                const auto copy = Selection::unpack(Selection::pack(s));
                expect(copy.control2 == s.control2 && copy.cutoff2 == s.cutoff2 && copy.resonance2 == s.resonance2,
                    "all 24576 Section 2 packed domains roundtrip");
            }
    ExperimentalSvfControl publication;
    Selection a, b;
    a.enabled = true; a.control2 = Control::relative; a.cutoff2 = 127; a.resonance2 = 63;
    a.routing = ShruthiSvfDualRouting::serial; a.mode1 = ShruthiSvf2pMode::bandPass;
    a.mode2 = ShruthiSvf2pMode::highPass; a.calibration = ShruthiSvfCalibration::eagleHardware;
    b.control2 = Control::independent; b.cutoff2 = 52; b.resonance2 = 21;
    publication.request(a);
    std::atomic<int> bad { 0 };
    std::thread reader([&] {
        ShruthiSvfDual dual;
        dual.prepare(SwaraXtEngine::kInternalSampleRate);
        bool enabled = false;
        for (int i = 0; i < 40000; ++i)
        {
            const auto s = publication.applyOnAudioThread(dual, enabled);
            if (Selection::pack(s) != Selection::pack(a) && Selection::pack(s) != Selection::pack(b)) ++bad;
        }
    });
    for (int i = 0; i < 40000; ++i) publication.request((i & 1) != 0 ? a : b);
    reader.join();
    expect(bad.load() == 0, "all fields applied from one coherent snapshot");
    std::thread first([&] { for (int i = 0; i < 10000; ++i) publication.patch([](Selection& s) { s.cutoff2 = 76; s.resonance2 = 63; }); });
    std::thread second([&] { for (int i = 0; i < 10000; ++i) publication.patch([](Selection& s) { s.control2 = Control::relative; s.calibration = ShruthiSvfCalibration::eagleHardware; }); });
    first.join(); second.join();
    const auto s = publication.selection();
    expect(s.cutoff2 == 76 && s.resonance2 == 63 && s.control2 == Control::relative
        && s.calibration == ShruthiSvfCalibration::eagleHardware, "concurrent CAS writers lose no unrelated fields");
    std::printf("Publication: 24576 domains, 40000 coherent audio applications, two CAS writers\n");
}

struct Render {
    std::uint64_t hash = 1469598103934665603ull;
    std::uint64_t fxHash = 1469598103934665603ull;
    double f1 = 0, f2 = 0, q1 = 0;
    std::uint8_t fx1 = 0, fx2 = 0;
};
void parameter(SwaraXtAudioProcessor& p, const char* id, float value)
{
    auto* param = p.getApvts().getParameter(id);
    param->setValueNotifyingHost(param->convertTo0to1(value));
}
Render render(Control control, int cut, int res, int route, int calibration, int m1, int m2, bool fx = false)
{
    SwaraXtAudioProcessor p;
    p.setExperimentalSvfEnabled(true);
    p.setExperimentalSvfControl2(control);
    p.setExperimentalSvfCutoff2(cut);
    p.setExperimentalSvfResonance2(res);
    p.setExperimentalSvfRouting(static_cast<ShruthiSvfDualRouting>(route));
    p.setExperimentalSvfCalibration(static_cast<ShruthiSvfCalibration>(calibration));
    p.setExperimentalSvfMode1(static_cast<ShruthiSvf2pMode>(m1));
    p.setExperimentalSvfMode2(static_cast<ShruthiSvf2pMode>(m2));
    if (fx)
    {
        parameter(p, IDs::dspFxProgram, 1);
        parameter(p, "mod.row1.source", shruthi::MOD_SRC_LFO_1);
        parameter(p, "mod.row1.destination", shruthi::MOD_DST_CV_1);
        parameter(p, "mod.row1.amount", -63);
        parameter(p, "mod.row2.source", shruthi::MOD_SRC_ENV_1);
        parameter(p, "mod.row2.destination", shruthi::MOD_DST_CV_2);
        parameter(p, "mod.row2.amount", 63);
    }
    p.prepareToPlay(48000.0, 256);
    juce::AudioBuffer<float> buffer(2, 256);
    juce::MidiBuffer midi;
    Render result;
    for (int b = 0; b < 100; ++b)
    {
        midi.clear();
        if (b == 0) midi.addEvent(juce::MidiMessage::noteOn(1, 48, static_cast<juce::uint8>(110)), 0);
        if (b == 40) midi.addEvent(juce::MidiMessage::noteOn(1, 60, static_cast<juce::uint8>(96)), 0);
        p.processBlock(buffer, midi);
        expect(finite(buffer), "production render finite");
        const auto& voice = p.engineForTests().shruthiPart().voice();
        result.fxHash ^= static_cast<std::uint32_t>(voice.cv_1()) | (static_cast<std::uint32_t>(voice.cv_2()) << 8);
        result.fxHash *= 1099511628211ull;
        for (int i = 0; i < buffer.getNumSamples(); ++i)
        {
            const auto quantized = static_cast<std::int32_t>(std::llround(
                std::clamp(static_cast<double>(buffer.getSample(0, i)), -32.0, 32.0) * 1048576.0));
            result.hash ^= static_cast<std::uint32_t>(quantized);
            result.hash *= 1099511628211ull;
        }
    }
    const auto& dual = p.engineForTests().experimentalSvfDualForTests();
    result.f1 = dual.section1().cutoffHz(); result.q1 = dual.section1().q();
    result.f2 = dual.section2().cutoffHz();
    result.fx1 = p.engineForTests().shruthiPart().voice().cv_1();
    result.fx2 = p.engineForTests().shruthiPart().voice().cv_2();
    return result;
}

void audioControls()
{
    for (int cal = 0; cal < 2; ++cal)
        for (int route = 0; route < 3; ++route)
            for (int m1 = 0; m1 < 3; ++m1)
                for (int m2 = 0; m2 < 3; ++m2)
                {
                    const auto low = render(Control::independent, 30, 0, route, cal, m1, m2);
                    const auto high = render(Control::independent, 100, 63, route, cal, m1, m2);
                    expect(identical(low.f1, high.f1) && identical(low.q1, high.q1), "independent F2 cannot alter F1 coefficients");
                    expect(route == 0 ? low.hash == high.hash : low.hash != high.hash,
                        "Section 1 output invariant; serial/parallel independently audible");
                }
    for (int cal = 0; cal < 2; ++cal)
    {
        const auto low = render(Control::relative, 52, 0, 1, cal, 0, 0);
        const auto mid = render(Control::relative, 64, 0, 1, cal, 0, 0);
        const auto high = render(Control::relative, 76, 0, 1, cal, 0, 0);
        expect(low.hash == dualRelativeGoldens[cal][0] && mid.hash == dualRelativeGoldens[cal][1]
            && high.hash == dualRelativeGoldens[cal][2], "separate Relative audio references unchanged");
        expect(std::abs(mid.f2 / low.f2 - 2.0) < 0.02 && std::abs(high.f2 / mid.f2 - 2.0) < 0.02,
            "relative octave tracking in each frozen calibration");
        const auto fxA = render(Control::independent, 0, 0, 1, cal, 0, 1, true);
        const auto fxB = render(Control::relative, 127, 63, 1, cal, 0, 1, true);
        expect(fxA.fx1 == fxB.fx1 && fxA.fx2 == fxB.fx2 && fxA.fxHash == fxB.fxHash,
            "FX firmware CV bytes invariant for entire modulated render under Section 2 requests");
        const auto cutA = render(Control::independent, 40, 32, 1, cal, 0, 1);
        const auto cutB = render(Control::independent, 90, 32, 1, cal, 0, 1);
        const auto resB = render(Control::independent, 90, 63, 1, cal, 0, 1);
        expect(cutA.hash == dualIndependentGoldens[cal][0] && cutB.hash == dualIndependentGoldens[cal][1]
            && resB.hash == dualIndependentGoldens[cal][2], "separate Independent audio references unchanged");
        expect(cutA.hash != cutB.hash && cutB.hash != resB.hash && identical(cutA.f1, cutB.f1)
            && identical(cutB.f1, resB.f1), "cutoff and resonance independently audible with fixed F1");
        std::printf("DualIndependent_%s_Cut40_Res32 hash=%016llx\n", cal == 0 ? "Paper" : "Eagle", static_cast<unsigned long long>(cutA.hash));
        std::printf("DualIndependent_%s_Cut90_Res32 hash=%016llx\n", cal == 0 ? "Paper" : "Eagle", static_cast<unsigned long long>(cutB.hash));
        std::printf("DualIndependent_%s_Cut90_Res63 hash=%016llx\n", cal == 0 ? "Paper" : "Eagle", static_cast<unsigned long long>(resB.hash));
        std::printf("DualRelative_%s_Minus12 hash=%016llx\n", cal == 0 ? "Paper" : "Eagle", static_cast<unsigned long long>(low.hash));
        std::printf("DualRelative_%s_Center64 hash=%016llx\n", cal == 0 ? "Paper" : "Eagle", static_cast<unsigned long long>(mid.hash));
        std::printf("DualRelative_%s_Plus12 hash=%016llx\n", cal == 0 ? "Paper" : "Eagle", static_cast<unsigned long long>(high.hash));
    }
    std::printf("Production independence: 108 paired renders across routes, modes and calibrations\n");
}

void guiAndStress()
{
    SwaraXtAudioProcessor p;
    p.prepareToPlay(48000.0, 256);
    juce::MemoryBlock before;
    p.getStateInformation(before);
    p.setExperimentalSvfEnabled(true);
    std::atomic<int> invalid { 0 };
    std::thread audio([&] {
        juce::AudioBuffer<float> b(2, 256);
        juce::MidiBuffer midi;
        for (int block = 0; block < 1800; ++block)
        {
            midi.clear();
            if (block % 100 == 0) midi.addEvent(juce::MidiMessage::noteOn(1, 36 + block % 48, static_cast<juce::uint8>(110)), 0);
            p.processBlock(b, midi);
            if (! finite(b)) ++invalid;
        }
    });
    std::thread writer([&] {
        for (int i = 0; i < 12000; ++i)
        {
            p.setExperimentalSvfCutoff2(i % 128); p.setExperimentalSvfResonance2(i % 64);
            p.setExperimentalSvfCalibration(static_cast<ShruthiSvfCalibration>(i & 1));
            p.setExperimentalSvfRouting(static_cast<ShruthiSvfDualRouting>(i % 3));
        }
    });
    for (int cycle = 0; cycle < 12; ++cycle)
    {
        auto editor = std::make_unique<SwaraXtAudioProcessorEditor>(p);
        auto& panel = editor->mainPanelForTests().dualSvfPanelForTests();
        for (int mode = 0; mode < 3; ++mode)
        {
            panel.control2ForTests().onSelect(mode);
            panel.cutoff2ForTests().setValue(76, juce::sendNotificationSync);
            panel.resonance2ForTests().setValue(63, juce::sendNotificationSync);
            editor->refreshExperimentalSvfForTests();
            expect(panel.control2ForTests().selectedIndex() == mode, "visible control mode matches coherent request");
            expect(panel.cutoff2ForTests().isEnabled() == (mode != 0), "Follow disables independent knobs");
            if (mode == 2) expect(panel.cutoff2ForTests().getTextFromValue(76).contains("+12"), "relative offset explicit");
        }
        auto pending = editor->contextMenuCallbackForTests();
        panel.control2ForTests().buttonForTests(1).triggerClick();
        editor.reset();
        const auto s = Selection::pack(p.experimentalSvfSelection());
        pending(652);
        // The second writer owns other fields; only control2 must remain unchanged.
        expect((s & Selection::kControl2Mask) == (Selection::pack(p.experimentalSvfSelection()) & Selection::kControl2Mask),
            "pending destroyed-editor callback cannot publish a control mode");
        juce::MessageManager::getInstance()->runDispatchLoopUntil(3);
    }
    writer.join(); audio.join();
    expect(invalid.load() == 0, "1800 audio blocks with live GUI, rapid controls and teardown finite");
    juce::MemoryBlock after;
    p.getStateInformation(after);
    expect(before == after, "all new session controls absent from serialization");
    SwaraXtAudioProcessor fresh;
    fresh.setStateInformation(after.getData(), static_cast<int>(after.getSize()));
    expect(Selection::pack(fresh.experimentalSvfSelection()) == 0, "fresh restored session remains Classic/Follow/Paper");
    SwaraXtAudioProcessorEditor editor(p);
    editor.mainPanelForTests().setDualSvfView(true);
    const auto directory = juce::File::getCurrentWorkingDirectory().getChildFile("section2-screenshots");
    expect(directory.createDirectory().wasOk(), "Section 2 screenshot directory");
    int images = 0;
    for (auto skin : { ui::SkinId::pastel, ui::SkinId::midnightGold, ui::SkinId::neonCobalt, ui::SkinId::jungle, ui::SkinId::rossocorsa })
        for (auto size : { ui::GuiSize::small, ui::GuiSize::medium, ui::GuiSize::large })
            for (int mode : { 1, 2 })
            {
                editor.setSkinForTests(skin); editor.setGuiSizeForTests(size);
                p.setExperimentalSvfControl2(static_cast<Control>(mode));
                editor.refreshExperimentalSvfForTests(); editor.resized();
                const auto& panel = editor.mainPanelForTests().dualSvfPanelForTests();
                for (int i = 0; i < panel.getNumChildComponents(); ++i)
                    expect(panel.getLocalBounds().contains(panel.getChildComponent(i)->getBounds())
                        && ! panel.getChildComponent(i)->getBounds().isEmpty(), "all new panel controls within bounds");
                const auto image = editor.createComponentSnapshot(editor.getLocalBounds());
                juce::FileOutputStream stream(directory.getChildFile(juce::String(images++) + ".png"));
                expect(stream.openedOk() && juce::PNGImageFormat().writeImageToStream(image, stream), "actual Section 2 skin/size snapshot");
            }
    std::printf("Section 2 GUI/lifetime: 12 teardown cycles, 1800 audio blocks, 48000 concurrent requests\n");
    std::printf("Section 2 screenshots: %d across five skins, three sizes, both new modes\n", images);
}

void rateMatrix()
{
#if defined(__linux__)
    const bool auditLoaded = swaraxtBeginAllocationAudit != nullptr && swaraxtEndAllocationAudit != nullptr;
    if (auditLoaded)
    {
        swaraxtBeginAllocationAudit();
        auto* proof = static_cast<char*>(std::malloc(17));
        if (proof != nullptr) *static_cast<volatile char*>(proof) = 1;
        const auto count = swaraxtEndAllocationAudit();
        std::free(proof);
        expect(count > 0, "heap interposer detects deliberate allocation before zero-allocation checks");
    }
#endif
    for (double rate : { 8000.0, 22050.0, 44100.0, 48000.0, 88200.0, 96000.0, 176400.0, 192000.0, 384000.0 })
        for (int block : { 1, 64, 256, 1024 })
        {
            SwaraXtAudioProcessor p;
            p.prepareToPlay(rate, block);
            const int latency = p.getLatencySamples();
            p.setExperimentalSvfEnabled(true);
            juce::AudioBuffer<float> b(2, block);
            juce::MidiBuffer midi;
            for (int i = 0; i < 180; ++i)
            {
                p.setExperimentalSvfControl2(static_cast<Control>(i % 3));
                p.setExperimentalSvfCutoff2((i * 23) % 128);
                p.setExperimentalSvfResonance2(i % 2 == 0 ? 63 : 0);
                p.setExperimentalSvfRouting(static_cast<ShruthiSvfDualRouting>(i % 3));
                p.setExperimentalSvfCalibration(static_cast<ShruthiSvfCalibration>(i & 1));
                midi.clear();
                if (i == 0) midi.addEvent(juce::MidiMessage::noteOn(1, 48, static_cast<juce::uint8>(127)), 0);
                if (i == 90) midi.addEvent(juce::MidiMessage::allNotesOff(1), 0);
#if defined(__linux__)
                const bool audit = swaraxtBeginAllocationAudit != nullptr && swaraxtEndAllocationAudit != nullptr && i > 2;
                if (audit) swaraxtBeginAllocationAudit();
#endif
                p.processBlock(b, midi);
#if defined(__linux__)
                if (audit) expect(swaraxtEndAllocationAudit() == 0, "new controls allocate no audio heap memory");
#endif
                expect(finite(b) && p.getLatencySamples() == latency, "full supported rate/block stress finite with unchanged latency");
            }
        }
    std::printf("Rate/block stress: 36 cases, 6480 blocks, 8-384 kHz, blocks 1/64/256/1024\n");
#if defined(__linux__)
    std::printf("Audio heap audit: %s\n", auditLoaded ? "6372 switched processBlocks, allocations=0 if suite passes" : "external interposer not loaded");
#endif
}

void dormantTargets()
{
    SwaraXtAudioProcessor p;
    p.prepareToPlay(48000.0, 256);
    p.setExperimentalSvfEnabled(true);
    p.setExperimentalSvfControl2(Control::relative);
    juce::AudioBuffer<float> buffer(2, 256);
    juce::MidiBuffer midi;
    for (int i = 0; i < 100; ++i)
    {
        p.setExperimentalSvfCutoff2(i % 128);
        p.setExperimentalSvfResonance2(i % 64);
        p.processBlock(buffer, midi);
    }
    auto& engine = p.engineForTests();
    const auto selection = p.experimentalSvfSelection();
    const auto cv = reconstructSvfSection2(engine.shruthiPart().patch(), engine.shruthiPart().voice(),
        selection.cutoff2, selection.resonance2, true);
    ShruthiSvf2p reference;
    reference.prepare(SwaraXtEngine::kInternalSampleRate);
    reference.setFirmwareCutoff(cv.cutoff); reference.setFirmwareResonance(cv.resonance);
    reference.snapControlsForTests();
    // Quiescent readback: snap only exposes the already published control targets.
    auto& actual = engine.experimentalSvfDualForTests().section2();
    actual.snapControlsForTests();
    expect(identical(actual.cutoffHz(), reference.cutoffHz()) && identical(actual.q(), reference.q()),
        "idle control transactions update genuine F2 targets without a note");
    midi.addEvent(juce::MidiMessage::noteOn(1, 48, static_cast<juce::uint8>(100)), 0);
    p.processBlock(buffer, midi);
    expect(finite(buffer) && Selection::pack(selection) == Selection::pack(p.experimentalSvfSelection()),
        "wake preserves independent requests and finite output");
    std::printf("Idle native control target readback and note wake: PASS\n");
}
} // namespace

int main()
{
    juce::ScopedJuceInitialiser_GUI init;
    referenceControls(); packedControls(); audioControls(); guiAndStress(); rateMatrix(); dormantTargets();
    std::printf("Dual SVF Section 2: %s (%d failures)\n", failures == 0 ? "PASS" : "FAIL", failures);
    return failures == 0 ? 0 : 1;
}
