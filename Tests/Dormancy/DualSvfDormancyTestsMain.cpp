// Copyright 2026 MontroneDSP. SPDX-License-Identifier: GPL-3.0-or-later
#include <JuceHeader.h>
#include "Plugin/PluginProcessor.h"
#include "Plugin/SwaraXtParameterLayout.h"
#include "Engine/Filter/ShruthiSvfSection2Control.h"
#include <algorithm>
#include <array>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <vector>
#if defined(_MSC_VER) && defined(_DEBUG)
#include <crtdbg.h>
#endif

namespace swaraxt {
struct ManualSwitchTestAccess {
    static auto& delay(SwaraXtEngine& e) { return e.dualOutputDelay_; }
    static bool tail(const SwaraXtEngine& e) { return e.selectedFilterTailActive(); }
    static bool source(const SwaraXtEngine& e) { return e.patchRequiresAudioSource(); }
    static bool board(const SwaraXtEngine& e) { return e.boardRequiresAudio(); }
    static int phase(const SwaraXtEngine& e) { return static_cast<int>(e.manualSwitchPhase_); }
    static float tick(SwaraXtEngine& e) { return e.processSelectedFilterSample(0, 0); }
    static float drive(SwaraXtEngine& e, float vca) { return e.processSelectedFilterSample(.2f, vca); }
    static void render(SwaraXtEngine& e) { e.renderInternalBlock(); }
    static void request(SwaraXtEngine& e, ExperimentalSvfSelection s) { e.experimentalSvfControl_.request(s); }
    static auto installed(const SwaraXtEngine& e) { return e.appliedExperimentalSvfSelection_; }
    static void queuedDrain(SwaraXtEngine& e) {
        e.dormant_ = false; e.drainingToDormant_ = true; e.dcDrainingToDormant_ = false;
        e.dcBlocker_.reset(); e.dcBlocker_.process(1); e.internalQueue_.reset();
        for (int n = 0; n < 280; ++n) e.internalQueue_.push(n < 24 ? static_cast<float>(n + 1) * 1.e-6f : 0);
    }
};
}
namespace {
using namespace swaraxt;
using Access = ManualSwitchTestAccess;
using Selection = ExperimentalSvfSelection;
constexpr double nativeRate = SwaraXtEngine::kInternalSampleRate;
constexpr float dcThreshold = 1.e-7f;
int failures = 0;
thread_local bool inAudio = false;
unsigned heapOperations = 0;
std::filesystem::path artifacts;
#if defined(_MSC_VER) && defined(_DEBUG)
int __cdecl heapHook(int kind, void*, size_t, int, long, const unsigned char*, int) {
    if (inAudio && (kind == _HOOK_ALLOC || kind == _HOOK_REALLOC || kind == _HOOK_FREE)) ++heapOperations;
    return 1;
}
#endif
void check(bool ok, const char* message) {
    if (!ok) { if (failures < 32) std::printf("FAIL: %s\n", message); ++failures; }
}
bool bits(float a, float b) { return std::memcmp(&a, &b, sizeof(float)) == 0; }
void messages() {
    check(juce::MessageManager::getInstance()->isThisTheMessageThread(), "fixture services notifications on message thread");
    juce::MessageManager::getInstance()->runDispatchLoopUntil(1);
}
void set(SwaraXtAudioProcessor& p, const char* id, float value) {
    auto* a = p.getApvts().getParameter(id); check(a != nullptr, id);
    if (a) a->setValueNotifyingHost(a->convertTo0to1(value));
}
void setup(SwaraXtAudioProcessor& p, int route, int calibration, int fx = 0) {
    set(p, IDs::osc1Shape, 1); set(p, IDs::osc1Param, 0); set(p, IDs::osc1Range, 0); set(p, IDs::osc2Shape, 0);
    set(p, IDs::mixBalance, 0); set(p, IDs::mixSub, 0); set(p, IDs::mixNoise, 0);
    set(p, IDs::filterCutoff, 8000); set(p, IDs::filterResonance, 1); set(p, IDs::filterKeyTracking, .5f);
    set(p, IDs::filterEnvDepth, 0); set(p, IDs::filterLfoDepth, 0); set(p, IDs::perfGlide, 0);
    set(p, IDs::postMixer, 0); set(p, IDs::master, .5f); set(p, IDs::seqMode, 0);
    set(p, IDs::dspFxProgram, static_cast<float>(fx)); set(p, IDs::dspFxParam1, 64); set(p, IDs::dspFxParam2, 32);
    for (const auto* id : {IDs::env1Attack, IDs::env1Decay, IDs::env1Release, IDs::env2Attack, IDs::env2Decay, IDs::env2Release}) set(p, id, 0);
    set(p, IDs::env1Sustain, 127); set(p, IDs::env2Sustain, 127);
    for (int row = 1; row <= 12; ++row) { const auto id = "mod.row" + std::to_string(row) + ".amount"; set(p, id.c_str(), row == 9 ? 63.f : 0.f); }
    Selection s; s.enabled = true; s.routing = static_cast<ShruthiSvfDualRouting>(route);
    s.calibration = static_cast<ShruthiSvfCalibration>(calibration);
    s.control2 = ShruthiSvfSection2Control::independent; s.cutoff2 = 86; s.resonance2 = 63;
    Access::request(p.engineForTests(), s);
}
struct Trace {
    SwaraXtEngine* engine = nullptr;
    std::array<float, 256> fifo {};
    size_t cursor = 0;
    uint32_t pending = 0;
    int64_t lastInput = -1, lastOutput = -1, exactZero = -1, firstDrain = -1;
    bool resonatorBehindClosedVca = false, closedWithPending = false;
    uint64_t blocksAfterEmpty = 0;
    float drainInput = 0;
    float lastPostFx = 0;
    void clearFifo() { fifo.fill(0); cursor = 0; pending = 0; }
    static void tap(void* context, const SwaraXtEngine::DebugBlockCapture& b) {
        auto& t = *static_cast<Trace*>(context); auto& e = *t.engine;
        t.lastPostFx = b.filterOutput[b.samples - 1];
        for (int i = 0; i < b.samples; ++i) {
            const auto n = static_cast<int64_t>(b.nativeBlockIndex) * 40 + i;
            const float u = b.dualUndelayed[i], y = t.fifo[t.cursor];
            t.fifo[t.cursor] = u; t.cursor = (t.cursor + 1) % 256;
            if (u != 0) { t.pending = 256; t.lastInput = n; } else if (t.pending) --t.pending;
            check(bits(y, b.manualSwitch[i].raw), "real processor FIFO matches an independently indexed stream");
            if (y != 0) t.lastOutput = n;
            if (b.vcaTarget[i] == 0 && b.vcaGain[i] == 0 && e.experimentalSvfDualForTests().tailActive()) {
                if (t.exactZero < 0) t.exactZero = n;
                t.resonatorBehindClosedVca = true; t.closedWithPending |= t.pending != 0;
                check(u == 0, "exact-zero selected VCA produces exact-zero FIFO input despite active resonator");
            }
        }
        const auto& actual = Access::delay(e);
        check(actual.samples == t.fifo && actual.cursor == t.cursor && actual.pending == t.pending, "FIFO ownership and pending state remain exact");
        if (t.exactZero >= 0 && !t.pending) {
            ++t.blocksAfterEmpty;
            check(t.lastOutput == t.lastInput + 256, "final nonzero FIFO sample is delivered exactly 256 native ticks after insertion");
        }
        if (e.drainingForTests()) {
            if (t.firstDrain < 0) { t.firstDrain = static_cast<int64_t>(b.nativeBlockIndex + 1) * 40; t.drainInput = b.postVca[b.samples - 1]; }
            check(e.experimentalSvfDualForTests().vcaControl() == 0 && t.pending == 0,
                  "native drainage never starts at merely low VCA or before FIFO exhaustion");
            check(!Access::board(e) && !Access::source(e) && Access::phase(e) == 0,
                  "drainage respects shared FX, firmware audio source and manual-switch ownership");
            check(!(e.shruthiPart().running() && e.shruthiPart().sequencer_settings().seq_tempo >= 40),
                  "native drain cannot freeze an internal firmware clock that is still running");
        }
    }
};
void process(SwaraXtAudioProcessor& p, juce::AudioBuffer<float>& b, juce::MidiBuffer& m, Trace* t = nullptr) {
    if (t && p.engineForTests().dormantForTests()) t->clearFifo();
    inAudio = true; p.processBlock(b, m); inAudio = false; m.clear();
}
struct ReleaseResult { bool slept = false; int sleepHost = -1; Trace trace; uint64_t blocks = 0, dcSamples = 0; double peak = 0; };
ReleaseResult release(int route, int cal, int fx, double rate, int size, int offCode, bool requireSleep, bool compare) {
    auto p = std::make_unique<SwaraXtAudioProcessor>(); setup(*p, route, cal, fx); p->prepareToPlay(rate, size);
    std::unique_ptr<SwaraXtAudioProcessor> control;
    if (compare) { control = std::make_unique<SwaraXtAudioProcessor>(); setup(*control, route, cal, fx); control->prepareToPlay(rate, size); control->engineForTests().setDormancyEnabledForTests(false); }
    ReleaseResult result; result.trace.engine = &p->engineForTests(); p->engineForTests().setDebugTapSink(&result.trace, Trace::tap);
    juce::AudioBuffer<float> b(2, size), c(2, size); juce::MidiBuffer m, cm; m.ensureSize(4096); cm.ensureSize(4096);
    const int off = (4096 / size + 1) * size;
    const int end = static_cast<int>(rate * 6) / size * size;
    uint64_t mismatches = 0; double maximumDifference = 0; int firstMismatch = -1;
    std::vector<float> measured, unchanged;
    if (!artifacts.empty() && fx == 6) { measured.reserve(static_cast<size_t>(end)); unchanged.reserve(static_cast<size_t>(end)); }
    for (int pos = 0; pos < end; pos += size) {
        if (pos == 0) { m.addEvent(juce::MidiMessage::noteOn(1, 48, static_cast<juce::uint8>(110)), 0); if (control) cm = m; }
        if (pos == off) { m.addEvent(offCode ? juce::MidiMessage::controllerEvent(1, offCode, 0) : juce::MidiMessage::noteOff(1, 48), 0); if (control) cm = m; }
        process(*p, b, m, &result.trace); if (control) process(*control, c, cm);
        const bool asleep = p->engineForTests().dormantForTests();
        if (asleep && result.sleepHost < 0) result.sleepHost = pos + size;
        for (int i = 0; i < size; ++i) {
            const float y = b.getSample(0, i); check(std::isfinite(y), "release/FX/DC output remains finite"); result.peak = std::max(result.peak, std::abs(static_cast<double>(y)));
            if (control && !bits(y, c.getSample(0, i))) {
                maximumDifference = std::max(maximumDifference, std::abs(static_cast<double>(y) - c.getSample(0, i)));
                if (!(asleep && y == 0 && std::abs(c.getSample(0, i)) <= dcThreshold)) {
                    if (firstMismatch < 0) { firstMismatch = pos + i; std::printf("First mismatch host=%d output=%.9g continuous=%.9g nativeDrain=%lld drainInput=%.9g\n", firstMismatch, y, c.getSample(0, i), static_cast<long long>(result.trace.firstDrain), result.trace.drainInput); }
                    ++mismatches;
                }
            }
            if (!artifacts.empty() && fx == 6) { measured.push_back(y); unchanged.push_back(control ? c.getSample(0, i) : 0); }
        }
        if (asleep && pos > off) { result.slept = true; break; }
    }
    auto& e = p->engineForTests(); result.blocks = e.cpuProfileForTests().nativeBlocksRendered; result.dcSamples = e.cpuProfileForTests().dcDrainHostSamples;
    check(mismatches == 0, "all pending sound and DC output match continuous rendering until established DC settlement");
    if (mismatches) std::printf("Mismatch samples=%llu maximumDifference=%.9g (%.3f dBFS)\n", static_cast<unsigned long long>(mismatches), maximumDifference, 20 * std::log10(maximumDifference));
    if (!artifacts.empty() && fx == 6) for (bool reference : {false, true}) { const auto& data = reference ? unchanged : measured; std::ofstream f(artifacts / (reference ? "continuous.f32" : "draining.f32"), std::ios::binary); f.write(reinterpret_cast<const char*>(data.data()), static_cast<std::streamsize>(data.size() * sizeof(float))); }
    check(e.shruthiPart().voice().amplitude_envelope_dead(), "completed release has genuinely dead amplitude envelope");
    check(result.trace.resonatorBehindClosedVca && result.trace.closedWithPending, "fixture observes active resonator, exact closed VCA and still-pending audio");
    if (requireSleep) check(result.slept && result.dcSamples > 0, "FX-Off Dual reaches genuine dormancy after complete SRC/DC drainage");
    if (result.slept) {
        const auto blocks = e.cpuProfileForTests().nativeBlocksRendered;
        for (int n = 0; n < 64; ++n) { process(*p, b, m, &result.trace); for (int i = 0; i < size; ++i) check(b.getSample(0, i) == 0, "settled dormant output is exact zero"); }
        check(blocks == e.cpuProfileForTests().nativeBlocksRendered, "dormant Dual executes no extra native DSP transactions");
    } else if (fx && !requireSleep) check(Access::board(e) || result.trace.lastPostFx != 0,
        "remaining FX work or nonzero terminal FX level prevents an artificial drain transient");
    std::printf("Release route=%d cal=%d fx=%d rate=%.0f block=%d off=%d zero=%lld finalFIFO=%lld nativeDrain=%lld nativeBlocks=%llu extraAfterEmpty=%llu dcHost=%llu sleepHost=%d peak=%.9g\n",
                route, cal, fx, rate, size, offCode, static_cast<long long>(result.trace.exactZero), static_cast<long long>(result.trace.lastOutput),
                static_cast<long long>(result.trace.firstDrain), static_cast<unsigned long long>(result.blocks), static_cast<unsigned long long>(result.trace.blocksAfterEmpty),
                static_cast<unsigned long long>(result.dcSamples), result.sleepHost, result.peak);
    e.setDebugTapSink(nullptr, nullptr); result.trace.engine = nullptr; p.reset(); control.reset(); messages(); return result;
}
void fifoAndFirmwareGuard() {
    for (int fill : {1, 37, 256}) {
        auto p = std::make_unique<SwaraXtAudioProcessor>(); setup(*p, 0, 0); p->prepareToPlay(48000, 64); auto& e = p->engineForTests();
        std::array<float, 256> expected {};
        for (int i = 0; i < fill; ++i) { const float x = static_cast<float>(i + 1) / 1024; Access::delay(e).process(x); expected[static_cast<size_t>(i)] = x; }
        for (int n = 0; n < 256; ++n) { check(Access::tail(e), "FIFO pending samples protect eligibility until final delivery"); const float y = Access::tick(e); check(bits(y, expected[static_cast<size_t>((fill + n) % 256)]), "tagged FIFO independently indexes full and partial histories"); }
        check(!Access::delay(e).tailActive(), "FIFO reports empty only after final queued sample");
    }
    auto p = std::make_unique<SwaraXtAudioProcessor>(); setup(*p, 1, 0);
    set(*p, "mod.row1.source", static_cast<float>(shruthi::MOD_SRC_AUDIO)); set(*p, "mod.row1.destination", static_cast<float>(shruthi::MOD_DST_PWM_1)); set(*p, "mod.row1.amount", 32);
    p->prepareToPlay(48000, 64); juce::AudioBuffer<float> audio(2, 64); juce::MidiBuffer midi; midi.ensureSize(4096);
    midi.addEvent(juce::MidiMessage::noteOn(1, 48, static_cast<juce::uint8>(110)), 0);
    for (int n = 0; n < 192; ++n) { if (n == 64) midi.addEvent(juce::MidiMessage::noteOff(1, 48), 0); process(*p, audio, midi); }
    auto& e = p->engineForTests();
    check(e.experimentalSvfDualForTests().tailActive() && e.experimentalSvfDualForTests().vcaControl() == 0,
          "firmware guard fixture has live pre-VCA state behind exact-zero VCA");
    Access::render(e);
    check(Access::source(e) && !e.drainingForTests(), "audio-source firmware contract cannot be replaced with inserted drain zeros");
}
void longReleaseAndSwitches() {
    auto p = std::make_unique<SwaraXtAudioProcessor>(); setup(*p, 2, 1); set(*p, IDs::env2Release, 127); p->prepareToPlay(48000, 64);
    juce::AudioBuffer<float> b(2, 64); juce::MidiBuffer m; m.ensureSize(4096); m.addEvent(juce::MidiMessage::noteOn(1, 48, static_cast<juce::uint8>(110)), 0);
    double peak = 0;
    for (int n = 0; n < 1600; ++n) { if (n == 64) m.addEvent(juce::MidiMessage::noteOff(1, 48), 0); process(*p, b, m); for (int i = 0; i < 64; ++i) peak = std::max(peak, std::abs(static_cast<double>(b.getSample(0, i)))); check(!p->engineForTests().dormantForTests(), "maximum-length release cannot sleep while voice/VCA is active"); }
    check(peak > .01 && !p->engineForTests().shruthiPart().voice().amplitude_envelope_dead(), "long release remains musically active at test endpoint"); p.reset(); messages();
    for (bool initial : {false, true}) for (int phase : {1, 2, 3}) {
        auto a = std::make_unique<SwaraXtAudioProcessor>(); setup(*a, 1, 0); a->setExperimentalSvfEnabled(initial); a->prepareToPlay(48000, 32);
        juce::AudioBuffer<float> x(2, 32); juce::MidiBuffer midi; midi.ensureSize(4096); midi.addEvent(juce::MidiMessage::noteOn(1, 48, static_cast<juce::uint8>(110)), 0);
        bool sent = false, prepared = false;
        for (int n = 0; n < 600; ++n) { if (n == 64) a->setExperimentalSvfEnabled(!initial); if (!sent && Access::phase(a->engineForTests()) == phase) { midi.addEvent(juce::MidiMessage::noteOff(1, 48), 0); sent = true; }
            if (n == 75 && phase == 2) midi.addEvent(juce::MidiMessage::noteOn(1, 60, static_cast<juce::uint8>(90)), 17);
            process(*a, x, midi); prepared |= Access::phase(a->engineForTests()) == 2;
            if (Access::phase(a->engineForTests()) != 0) check(!a->engineForTests().dormantForTests() && Access::tail(a->engineForTests()), "every active manual-switch phase prevents premature dormancy");
        }
        check(sent && prepared && Access::phase(a->engineForTests()) == 0, "NoteOff/NoteOn handoff completes without a stranded phase"); a.reset(); messages();
    }
}
void exactVcaGate() {
    for (int route = 0; route < 3; ++route) {
        auto p = std::make_unique<SwaraXtAudioProcessor>(); setup(*p, route, 0); p->prepareToPlay(48000, 64); auto& e = p->engineForTests();
        for (int i = 0; i < 40; ++i) Access::drive(e, 1);
        bool tiny = false;
        for (int i = 0; i < 512; ++i) {
            Access::drive(e, 0); const float vca = e.experimentalSvfDualForTests().vcaControl();
            if (vca > 0 && vca < 1.e-8f) {
                // Isolate the VCA predicate; full processor FIFO drainage is
                // checked independently by Trace and the indexed tag tests.
                Access::delay(e).reset(); tiny = true; check(Access::tail(e), "positive sub-threshold VCA alone still prohibits dormancy");
            }
        }
        check(tiny && e.experimentalSvfDualForTests().vcaControl() == 0, "each routing reaches exact zero after a representable positive VCA interval");
    }
}
struct WakeReplay {
    SwaraXtEngine* engine = nullptr;
    ShruthiSvfDual fresh;
    std::array<float, 256> fifo {};
    size_t cursor = 0;
    std::vector<float> native;
    WakeReplay() { native.reserve(8192); }
    void configure(SwaraXtEngine& e) {
        engine = &e; const auto s = Access::installed(e); const auto& v = e.shruthiPart().voice();
        fresh.prepare(nativeRate); fresh.setRouting(s.routing); fresh.setMode1(s.mode1); fresh.setMode2(s.mode2); fresh.setCalibration(s.calibration);
        fresh.setSection2FollowsSection1(s.control2 == ShruthiSvfSection2Control::legacyFollow);
        fresh.setFirmwareCutoff(v.cutoff()); fresh.setFirmwareResonance(v.resonance());
        if (s.control2 != ShruthiSvfSection2Control::legacyFollow) { const auto cv = reconstructSvfSection2(e.shruthiPart().patch(), v, s.cutoff2, s.resonance2, s.control2 == ShruthiSvfSection2Control::relative); fresh.setFirmwareCutoff2(cv.cutoff); fresh.setFirmwareResonance2(cv.resonance); }
        fresh.reset();
    }
    static void tap(void* context, const SwaraXtEngine::DebugBlockCapture& b) {
        auto& t = *static_cast<WakeReplay*>(context); const auto& e = *t.engine; const auto& v = e.shruthiPart().voice(); const auto s = Access::installed(e);
        t.fresh.setFirmwareCutoff(v.cutoff()); t.fresh.setFirmwareResonance(v.resonance());
        if (s.control2 != ShruthiSvfSection2Control::legacyFollow) { const auto cv = reconstructSvfSection2(e.shruthiPart().patch(), v, s.cutoff2, s.resonance2, s.control2 == ShruthiSvfSection2Control::relative); t.fresh.setFirmwareCutoff2(cv.cutoff); t.fresh.setFirmwareResonance2(cv.resonance); }
        for (int i = 0; i < b.samples; ++i) {
            const float y = t.fifo[t.cursor]; t.fifo[t.cursor] = t.fresh.process(b.postShruthiMixer[i], b.vcaTarget[i]); t.cursor = (t.cursor + 1) % 256;
            check(bits(y, b.filterOutput[i]), "wake uses independently constructed fresh SVF and empty FIFO, with no stale state");
            const float output = y * .5f;
            check(bits(output, b.postVca[i]), "wake applies established master/quality gains exactly once"); t.native.push_back(output);
        }
    }
};
void wakeContracts() {
    int cases = 0;
    for (double rate : {44100., 48000., 96000., 192000.}) for (int size : {64, 128, 256}) for (int offset : {0, 17, size - 1}) {
        auto p = std::make_unique<SwaraXtAudioProcessor>(); setup(*p, cases % 3, cases % 2); p->prepareToPlay(rate, size);
        juce::AudioBuffer<float> b(2, size); juce::MidiBuffer m; m.ensureSize(4096); m.addEvent(juce::MidiMessage::noteOn(1, 48, static_cast<juce::uint8>(110)), 0);
        for (int i = 0; i < 32; ++i) process(*p, b, m);
        if (cases % 3 == 0) m.addEvent(juce::MidiMessage::noteOff(1, 48), 0);
        else m.addEvent(juce::MidiMessage::controllerEvent(1, cases % 3 == 1 ? 120 : 123, 0), 0);
        for (int i = 0; i < static_cast<int>(rate * 6) / size && !p->engineForTests().dormantForTests(); ++i) process(*p, b, m);
        check(p->engineForTests().dormantForTests(), "wake fixture first confirms genuine dormancy");
        if (cases % 4 == 0) { juce::MemoryBlock state; p->getStateInformation(state); p->setStateInformation(state.getData(), static_cast<int>(state.getSize())); process(*p, b, m); }
        else if (cases % 4 == 1) { p->releaseResources(); p->prepareToPlay(rate, size); process(*p, b, m); }
        auto s = p->experimentalSvfSelection(); s.routing = static_cast<ShruthiSvfDualRouting>((cases + 1) % 3); s.mode1 = static_cast<ShruthiSvf2pMode>(cases % 3); s.mode2 = static_cast<ShruthiSvf2pMode>((cases + 1) % 3); s.control2 = static_cast<ShruthiSvfSection2Control>(cases % 3);
        Access::request(p->engineForTests(), s); for (int i = 0; i < 4; ++i) process(*p, b, m);
        check(p->engineForTests().dormantForTests() && Selection::pack(Access::installed(p->engineForTests())) == Selection::pack(s), "dormant routing/mode/control changes remain coherent without waking");
        WakeReplay replay; replay.configure(p->engineForTests()); p->engineForTests().setDebugTapSink(&replay, WakeReplay::tap);
        std::vector<float> host; host.reserve(4096); m.addEvent(juce::MidiMessage::noteOn(1, 60, static_cast<juce::uint8>(100)), offset);
        for (int i = 0; i < 4096 / size; ++i) { process(*p, b, m); host.insert(host.end(), b.getReadPointer(0), b.getReadPointer(0) + size); }
        p->engineForTests().setDebugTapSink(nullptr, nullptr);
        HostRateConverter src; src.reset(); src.setStep(nativeRate, rate); DcBlocker dc; dc.prepare(rate); size_t next = 0; int first = -1;
        for (size_t i = 0; i < host.size(); ++i) {
            float expected = 0;
            if (i >= static_cast<size_t>(offset)) {
                while (src.size() < src.queueTargetSize()) { check(next + 40 <= replay.native.size(), "independent wake SRC has a complete native transaction"); if (next + 40 > replay.native.size()) break; for (int j = 0; j < 40; ++j) src.push(replay.native[next++]); }
                expected = dc.process(src.readInterpolated());
            }
            check(bits(host[i], expected), "host-offset wake matches independent complete fresh-filter/FIFO/SRC/DC replay bit-for-bit");
            if (first < 0 && std::abs(host[i]) > 1.e-6f) first = static_cast<int>(i);
        }
        check(first >= offset && first < static_cast<int>(host.size()), "no pre-event output or lost wake attack");
        check(p->getLatencySamples() == static_cast<int>(std::ceil(384 * rate / nativeRate)), "wake preserves declared fixed PDC");
        std::printf("Wake rate=%.0f block=%d offset=%d firstAudible=%d\n", rate, size, offset, first); ++cases; p.reset(); messages();
    }
}
std::vector<float> partition(double rate, int size) {
    auto p = std::make_unique<SwaraXtAudioProcessor>(); setup(*p, 2, 0); p->prepareToPlay(rate, size);
    const int wake = (static_cast<int>(rate * 4) / 256 + 1) * 256 + 17;
    const int end = (wake + 8192 + 255) / 256 * 256;
    std::vector<float> output; output.reserve(static_cast<size_t>(end)); juce::AudioBuffer<float> b(2, size); juce::MidiBuffer m; m.ensureSize(4096);
    for (int pos = 0; pos < end; pos += size) {
        if (pos == 0) m.addEvent(juce::MidiMessage::noteOn(1, 48, static_cast<juce::uint8>(110)), 0);
        if (pos <= 4096 && pos + size > 4096) m.addEvent(juce::MidiMessage::noteOff(1, 48), 4096 - pos);
        if (pos <= wake && pos + size > wake) { check(p->engineForTests().dormantForTests(), "partition fixture reaches dormancy before new note"); m.addEvent(juce::MidiMessage::noteOn(1, 60, static_cast<juce::uint8>(100)), wake - pos); }
        process(*p, b, m); output.insert(output.end(), b.getReadPointer(0), b.getReadPointer(0) + size);
    }
    return output;
}
void partitionContracts() {
    for (double rate : {44100., 48000., 96000., 192000.}) { const auto a = partition(rate, 1); messages(); for (int size : {16, 64, 256}) { const auto b = partition(rate, size); check(a.size() == b.size() && std::memcmp(a.data(), b.data(), a.size() * sizeof(float)) == 0, "release, exact drain, dormant interval and wake are host-block-partition independent"); messages(); } }
    for (int size : {1, 16, 32, 64}) { auto p = std::make_unique<SwaraXtAudioProcessor>(); setup(*p, 1, 1); p->prepareToPlay(384000, size); juce::AudioBuffer<float> b(2, size); juce::MidiBuffer m; m.ensureSize(4096); for (int pos = 0; pos < 8192; pos += size) { if (pos == 0) m.addEvent(juce::MidiMessage::noteOn(1, 48, static_cast<juce::uint8>(110)), 0); if (pos == 1024) m.addEvent(juce::MidiMessage::noteOff(1, 48), 0); process(*p, b, m); for (int i = 0; i < size; ++i) check(std::isfinite(b.getSample(0, i)), "384 kHz tiny-call wake/release remains finite and bounded"); } p.reset(); messages(); }
}
void cancellationAndFxPrograms() {
    for (int route = 0; route < 3; ++route) {
        auto p = std::make_unique<SwaraXtAudioProcessor>(); auto control = std::make_unique<SwaraXtAudioProcessor>();
        for (auto* a : {p.get(), control.get()}) { setup(*a, route, 0); a->prepareToPlay(48000, 64); Access::queuedDrain(a->engineForTests()); }
        juce::AudioBuffer<float> first(2, 1), referenceFirst(2, 1), x(2, 64), y(2, 64); juce::MidiBuffer mx, my; mx.ensureSize(4096); my.ensureSize(4096);
        const auto before = p->engineForTests().debugInternalBlockCount(); p->setExperimentalSvfEnabled(false);
        process(*p, first, mx); process(*control, referenceFirst, my); p->setExperimentalSvfEnabled(true);
        check(bits(first.getSample(0, 0), referenceFirst.getSample(0, 0)) && before == p->engineForTests().debugInternalBlockCount(), "cancelled Dual-to-Classic request cannot advance firmware while queued output is sufficient");
        bool identical = true;
        for (int n = 0; n < 64; ++n) {
            if (n == 32) { mx.addEvent(juce::MidiMessage::noteOn(1, 60, static_cast<juce::uint8>(100)), 17); my = mx; }
            process(*p, x, mx); process(*control, y, my); identical &= std::memcmp(x.getReadPointer(0), y.getReadPointer(0), 64 * sizeof(float)) == 0;
        }
        check(identical, "cancelled Dual drain request preserves queued output, DC history and the next attack bit-for-bit"); p.reset(); control.reset(); messages();
    }
    auto p = std::make_unique<SwaraXtAudioProcessor>(); auto control = std::make_unique<SwaraXtAudioProcessor>();
    for (auto* a : {p.get(), control.get()}) { setup(*a, 2, 0, 7); a->prepareToPlay(48000, 64); }
    control->engineForTests().setDormancyEnabledForTests(false);
    juce::AudioBuffer<float> x(2, 64), y(2, 64); juce::MidiBuffer mx, my; mx.ensureSize(4096); my.ensureSize(4096); bool identical = true;
    for (int pos = 0; pos < 240000; pos += 64) {
        if (pos == 0) { mx.addEvent(juce::MidiMessage::noteOn(1, 48, static_cast<juce::uint8>(110)), 0); my = mx; }
        if (pos == 4096) { mx.addEvent(juce::MidiMessage::noteOff(1, 48), 0); my = mx; }
        if (pos == 8192 || pos == 16384) for (auto* a : {p.get(), control.get()}) set(*a, IDs::dspFxProgram, pos == 8192 ? 6.f : 0.f);
        process(*p, x, mx); process(*control, y, my);
        for (int i = 0; i < 64; ++i) if (!bits(x.getSample(0, i), y.getSample(0, i))) identical &= p->engineForTests().dormantForTests() && x.getSample(0, i) == 0 && std::abs(y.getSample(0, i)) <= dcThreshold;
    }
    check(identical && p->engineForTests().dormantForTests(), "existing FX-program fades preserve audio and eventually permit dormancy after Off");
}
void internalClockContracts() {
    for (int mode : {0, 1, 2}) {
        auto p = std::make_unique<SwaraXtAudioProcessor>(); setup(*p, 1, 0); set(*p, IDs::seqMode, static_cast<float>(mode)); set(*p, IDs::seqTempo, 40); set(*p, IDs::seqGate, 5); p->prepareToPlay(48000, 64);
        juce::AudioBuffer<float> b(2, 64); juce::MidiBuffer m; m.ensureSize(4096); m.addEvent(juce::MidiMessage::noteOn(1, 48, static_cast<juce::uint8>(110)), 0); bool sawClosed = false;
        for (int n = 0; n < 1500; ++n) {
            if (n == 128 && mode == 0) m.addEvent(juce::MidiMessage::noteOff(1, 48), 0);
            process(*p, b, m); const auto& e = p->engineForTests();
            if (e.shruthiPart().running() && e.experimentalSvfDualForTests().vcaControl() == 0) {
                sawClosed = true; check(!e.drainingForTests() && !e.dormantForTests(), "running internal firmware clock survives silent gates and its normal blank-note stop sequence");
            }
        }
        check(sawClosed, "clock fixture exercises a genuinely silent interval while firmware playback is running"); p.reset(); messages();
    }
}
void autonomousVcaContract() {
    auto p = std::make_unique<SwaraXtAudioProcessor>(); setup(*p, 0, 0);
    set(*p, IDs::lfo1Wave, static_cast<float>(shruthi::LFO_WAVEFORM_SQUARE)); set(*p, IDs::lfo1Rate, 80);
    set(*p, IDs::lfo1Attack, 0); set(*p, IDs::lfo1Retrig, 0);
    set(*p, "mod.row1.source", static_cast<float>(shruthi::MOD_SRC_LFO_1)); set(*p, "mod.row1.destination", static_cast<float>(shruthi::MOD_DST_VCA)); set(*p, "mod.row1.amount", 63);
    set(*p, "mod.row9.amount", 0);
    p->prepareToPlay(48000, 64); juce::AudioBuffer<float> b(2, 64); juce::MidiBuffer m; m.ensureSize(4096); bool opened = false, closed = false;
    m.addEvent(juce::MidiMessage::noteOn(1, 48, static_cast<juce::uint8>(110)), 0);
    for (int n = 0; n < 2250; ++n) {
        if (n == 64) m.addEvent(juce::MidiMessage::noteOff(1, 48), 0);
        process(*p, b, m); const auto& e = p->engineForTests();
        if (!e.shruthiPart().running() && e.shruthiPart().voice().amplitude_envelope_dead()) {
            if (e.experimentalSvfDualForTests().vcaControl() > 0) opened = true;
            else { closed = true; check(!e.drainingForTests() && !e.dormantForTests(), "autonomous VCA modulation cannot have its next opening delayed by frozen drain controls"); }
        }
    }
    std::printf("Autonomous VCA opened=%d closed=%d running=%d\n", opened ? 1 : 0, closed ? 1 : 0, p->engineForTests().shruthiPart().running() ? 1 : 0);
    check(opened && closed && !p->engineForTests().shruthiPart().running(), "autonomous VCA fixture cycles after note release and internal-clock stop");
}
void drainingControlEditContract() {
    auto p = std::make_unique<SwaraXtAudioProcessor>(); setup(*p, 0, 0); p->prepareToPlay(48000, 64);
    juce::AudioBuffer<float> b(2, 64); juce::MidiBuffer m; m.ensureSize(4096);
    m.addEvent(juce::MidiMessage::noteOn(1, 48, static_cast<juce::uint8>(110)), 0);
    for (int n = 0; n < 1000; ++n) { if (n == 64) m.addEvent(juce::MidiMessage::noteOff(1, 48), 0); process(*p, b, m); if (p->engineForTests().drainingForTests()) break; }
    check(p->engineForTests().drainingForTests() && !p->engineForTests().dormantForTests(), "control-edit fixture enters genuine SRC/DC drainage");
    const auto before = p->engineForTests().debugInternalBlockCount();
    set(*p, "mod.row9.amount", 0);
    float peak = 0;
    for (int n = 0; n < 32; ++n) { process(*p, b, m); for (int i = 0; i < 64; ++i) peak = std::max(peak, std::abs(b.getSample(0, i))); }
    check(peak > .01f, "control edit produces actual audio rather than only a bookkeeping wake");
    check(p->engineForTests().debugInternalBlockCount() > before && p->engineForTests().experimentalSvfDualForTests().vcaControl() > 0,
          "removing a closed-envelope gate during drainage resumes its newly audible firmware VCA");
}
void envelopeGateContracts() {
    auto p = std::make_unique<SwaraXtAudioProcessor>(); setup(*p, 0, 0);
    set(*p, IDs::lfo1Wave, static_cast<float>(shruthi::LFO_WAVEFORM_SQUARE)); set(*p, IDs::lfo1Rate, 80); set(*p, IDs::lfo1Attack, 0); set(*p, IDs::lfo1Retrig, 0);
    set(*p, "mod.row1.source", static_cast<float>(shruthi::MOD_SRC_LFO_1)); set(*p, "mod.row1.destination", static_cast<float>(shruthi::MOD_DST_VCA)); set(*p, "mod.row1.amount", 63);
    p->prepareToPlay(48000, 64); juce::AudioBuffer<float> b(2, 64); juce::MidiBuffer m; m.ensureSize(4096); m.addEvent(juce::MidiMessage::noteOn(1, 48, static_cast<juce::uint8>(110)), 0);
    for (int n = 0; n < 4500 && (n < 65 || !p->engineForTests().dormantForTests()); ++n) { if (n == 64) m.addEvent(juce::MidiMessage::noteOff(1, 48), 0); process(*p, b, m); }
    check(p->engineForTests().dormantForTests(), "ordinary hard ENV2 gating still permits dormancy with other multiplicative VCA factors");
    for (int variant = 0; variant < 3; ++variant) {
        auto a = std::make_unique<SwaraXtAudioProcessor>(); setup(*a, 0, 0);
        if (variant == 0) { set(*a, "mod.row9.amount", 0); set(*a, "mod.row12.source", static_cast<float>(shruthi::MOD_SRC_ENV_2)); set(*a, "mod.row12.destination", static_cast<float>(shruthi::MOD_DST_VCA)); set(*a, "mod.row12.amount", 63); }
        if (variant == 1) set(*a, IDs::lfo2Retrig, static_cast<float>(shruthi::LFO_MODE_MASTER));
        if (variant == 2) { set(*a, "mod.row1.source", static_cast<float>(shruthi::MOD_SRC_LFO_1)); set(*a, "mod.row1.destination", static_cast<float>(shruthi::MOD_DST_TRIGGER_ENV_2)); set(*a, "mod.row1.amount", 63); }
        a->prepareToPlay(48000, 64); a->engineForTests().applyParameters();
        check(Access::tail(a->engineForTests()), "wheel-scaled gates and autonomous envelope retriggers cannot falsely prove silence");
    }
}
}
int main(int argc, char** argv) {
    juce::ScopedJuceInitialiser_GUI init; std::setvbuf(stdout, nullptr, _IONBF, 0);
#if defined(_MSC_VER) && defined(_DEBUG)
    const auto previous = _CrtSetAllocHook(heapHook);
#endif
    const std::string mode = argc > 1 ? argv[1] : "contracts";
    if (argc > 2) { artifacts = argv[2]; std::filesystem::create_directories(artifacts); }
    if (mode != "guard" && mode != "fx" && mode != "fx-export" && mode != "clock" && mode != "modulation" && mode != "edit") for (int route = 0; route < 3; ++route) for (int cal = 0; cal < 2; ++cal) release(route, cal, 0, 48000, 64, 0, true, true);
    if (mode == "contracts") {
        fifoAndFirmwareGuard(); exactVcaGate(); longReleaseAndSwitches(); wakeContracts(); partitionContracts(); cancellationAndFxPrograms(); internalClockContracts(); autonomousVcaContract(); envelopeGateContracts(); drainingControlEditContract();
        for (int fx : {3, 6, 7, 8}) release(2, 0, fx, 48000, 64, 0, false, true);
        for (int code : {120, 123}) release(1, 1, 0, 48000, 64, code, true, true);
    } else if (mode == "guard") fifoAndFirmwareGuard();
    else if (mode == "fx") release(2, 0, 6, 48000, 64, 0, false, true);
    else if (mode == "fx-export") release(2, 0, 6, 48000, 64, 0, false, false);
    else if (mode == "clock") internalClockContracts();
    else if (mode == "modulation") autonomousVcaContract();
    else if (mode == "edit") drainingControlEditContract();
    else check(mode == "repro", "known fixture mode");
    messages(); check(heapOperations == 0, "no callback allocation, reallocation or free");
#if defined(_MSC_VER) && defined(_DEBUG)
    _CrtSetAllocHook(previous);
#endif
    std::printf("Dual dormancy: %d failures; callback heap operations %u.\n", failures, heapOperations); return failures ? 1 : 0;
}
