// Copyright 2026 MontroneDSP. SPDX-License-Identifier: GPL-3.0-or-later
#include <JuceHeader.h>
#include "Plugin/PluginProcessor.h"
#include "Plugin/PluginEditor.h"
#include "Plugin/SwaraXtParameterLayout.h"
#include "DualSvfSection2Goldens.h"
#include <algorithm>
#include <cmath>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <numeric>
#include <vector>

namespace swaraxt {
struct DualSvfLatencyTestAccess {
    using Delay = SwaraXtEngine::DualOutputDelay;
    static Delay& delay(SwaraXtEngine& e) { return e.dualOutputDelay_; }
    static void select(SwaraXtEngine& e) { e.applyExperimentalSvfSelection(); }
    static bool tail(const SwaraXtEngine& e) { return e.selectedFilterTailActive(); }
    static float process(SwaraXtEngine& e, float x) { return e.processSelectedFilterSample(x, 0); }
    static void sleep(SwaraXtEngine& e) { e.enterDormant(); }
    static void injectTail(SwaraXtEngine& e) {
        e.dormant_=false; e.snapPostMixerGain(0); e.dualOutputDelay_.process(.75f);
    }
    static void render(SwaraXtEngine& e) { e.renderInternalBlock(); }
};
}

namespace {
using namespace swaraxt;
using Sel = ExperimentalSvfSelection;
using Route = ShruthiSvfDualRouting;
using Mode = ShruthiSvf2pMode;
using Control = ShruthiSvfSection2Control;
constexpr double native = SwaraXtEngine::kInternalSampleRate;
int failures = 0;
double worstFixedNativeError=0, worstHostReplayError=0;
void check(bool ok, const char* message) { if (!ok) { ++failures; std::printf("FAIL: %s\n", message); } }
void finish(std::ofstream& stream, const char* message) { stream.flush(); check(stream.good(), message); }
// The unity probes are process-wide; audit renders are serialized.
struct LatencyProbeScope {
    const bool oldClassic = Ir3109BoardCore::identityForLatencyTests;
    const bool oldDual = ShruthiSvfDual::identityForLatencyTests;
    explicit LatencyProbeScope(bool enabled) {
        Ir3109BoardCore::identityForLatencyTests = enabled; ShruthiSvfDual::identityForLatencyTests = enabled;
    }
    ~LatencyProbeScope() {
        Ir3109BoardCore::identityForLatencyTests = oldClassic; ShruthiSvfDual::identityForLatencyTests = oldDual;
    }
    LatencyProbeScope(const LatencyProbeScope&) = delete;
    LatencyProbeScope& operator=(const LatencyProbeScope&) = delete;
};
void set(SwaraXtAudioProcessor& p, const char* id, float v) {
    auto* parameter = p.getApvts().getParameter(id);
    check(parameter != nullptr, id);
    if (parameter) parameter->setValueNotifyingHost(parameter->convertTo0to1(v));
}
void neutral(SwaraXtAudioProcessor& p, bool noise = false, float res = .2f, int fx = 0) {
    set(p, IDs::osc1Shape, 1); set(p, IDs::osc1Param, 0); set(p, IDs::osc1Range, 0);
    set(p, IDs::osc2Shape, 0); set(p, IDs::mixBalance, 0); set(p, IDs::mixSub, 0);
    set(p, IDs::mixNoise, noise ? 127.f : 0.f); set(p, IDs::filterCutoff, 10000);
    set(p, IDs::filterResonance, res); set(p, IDs::filterKeyTracking, .5f);
    set(p, IDs::filterEnvDepth, 0); set(p, IDs::filterLfoDepth, 0); set(p, IDs::perfGlide, 0);
    set(p, IDs::postMixer, 0); set(p, IDs::master, .5f); set(p, IDs::seqMode, 0);
    set(p, IDs::dspFxProgram, static_cast<float>(fx)); set(p, IDs::dspFxParam1, 64); set(p, IDs::dspFxParam2, 32);
    for (const auto* id : {IDs::env1Attack, IDs::env1Decay, IDs::env1Release, IDs::env2Attack, IDs::env2Decay, IDs::env2Release}) set(p, id, 0);
    set(p, IDs::env1Sustain, 127); set(p, IDs::env2Sustain, 127);
    for (int row = 1; row <= 12; ++row) {
        const auto id = "mod.row" + std::to_string(row) + ".amount";
        set(p, id.c_str(), row == 9 ? 63.f : 0.f);
    }
}
void select(SwaraXtAudioProcessor& p, const Sel& s) {
    p.setExperimentalSvfEnabled(s.enabled); p.setExperimentalSvfRouting(s.routing);
    p.setExperimentalSvfMode1(s.mode1); p.setExperimentalSvfMode2(s.mode2);
    p.setExperimentalSvfCalibration(s.calibration); p.setExperimentalSvfControl2(s.control2);
    p.setExperimentalSvfCutoff2(s.cutoff2); p.setExperimentalSvfResonance2(s.resonance2);
}
struct Transport : juce::AudioPlayHead {
    double rate = 48000; int64_t cursor = 0;
    juce::Optional<PositionInfo> getPosition() const override {
        PositionInfo p; p.setTimeInSamples(cursor); p.setBpm(120); p.setPpqPosition(static_cast<double>(cursor) / rate * 2);
        p.setIsPlaying(true); return p;
    }
};
struct Trace {
    struct Controls { double cut1=0,cut2=0,q1=0,q2=0,classicCv=0; };
    const SwaraXtEngine* engine=nullptr;
    std::vector<Controls> controlFrames;
    std::vector<board::BoardControl> boardFrames;
    std::vector<float> host, mixer, filtered, post, vca, target, undelayed;
    int latency = 0, requestNative = -1, eventAt = 0;
    float masterGain = .5f;
    void reserve(int hostCount, double rate) {
        host.reserve(static_cast<size_t>(hostCount));
        const auto n = static_cast<size_t>(hostCount * native / rate + 1024);
        controlFrames.reserve(n/40+16);
        boardFrames.reserve(n/40+16);
        for (auto* v : {&mixer, &filtered, &post, &vca, &target, &undelayed}) v->reserve(n);
    }
    static void capture(void* context, const SwaraXtEngine::DebugBlockCapture& b) {
        auto& t = *static_cast<Trace*>(context);
        const auto& dual=t.engine->experimentalSvfDualForTests();
        t.controlFrames.push_back({dual.section1().cutoffHz(),dual.section2().cutoffHz(),dual.section1().q(),dual.section2().q(),t.engine->filter().boardCoreForTests().cutoffCv()});
        t.undelayed.insert(t.undelayed.end(),b.dualUndelayed,b.dualUndelayed+b.samples);
        t.boardFrames.push_back(t.engine->boardControlsForTests());
        const float* sources[] {b.postShruthiMixer, b.filterOutput, b.postVca, b.vcaGain, b.vcaTarget};
        std::vector<float>* destinations[] {&t.mixer, &t.filtered, &t.post, &t.vca, &t.target};
        for (size_t i = 0; i < 5; ++i) destinations[i]->insert(destinations[i]->end(), sources[i], sources[i] + b.samples);
    }
};
double rms(const std::vector<float>& v, size_t first = 0) {
    double sum = 0; for (size_t i = first; i < v.size(); ++i) sum += static_cast<double>(v[i]) * v[i];
    return std::sqrt(sum / static_cast<double>(std::max<size_t>(1, v.size() - first)));
}
double peak(const std::vector<float>& v) { double r = 0; for (float x : v) r = std::max(r, std::abs(static_cast<double>(x))); return r; }
int onset(const std::vector<float>& v, double relative = 1.e-4) {
    const double threshold = std::max(1.e-12, peak(v) * relative);
    for (size_t i = 0; i < v.size(); ++i) if (std::abs(v[i]) > threshold) return static_cast<int>(i);
    return -1;
}
void binary(const std::filesystem::path& f, const std::vector<float>& x) {
    std::ofstream stream(f, std::ios::binary); stream.write(reinterpret_cast<const char*>(x.data()), static_cast<std::streamsize>(x.size() * sizeof(float)));
    finish(stream, "external audio output written");
}
Trace render(double rate, int block, const Sel& initial, FilterQuality quality, bool identity,
             int count, int noteAt, int offAt = -1, int changeAt = -1, const Sel* next = nullptr,
             bool qualityChange = false, float res = .2f, int fx = 0, int offCode = 0) {
    Transport transport; transport.rate = rate;
    auto p = std::make_unique<SwaraXtAudioProcessor>(); neutral(*p, identity, res, fx); select(*p, initial);
    p->setFilterQuality(quality); p->setPlayHead(&transport); p->prepareToPlay(rate, block);
    const LatencyProbeScope probe(identity);
    Trace trace; trace.reserve(count, rate); trace.latency = p->getLatencySamples();
    trace.eventAt=noteAt;
    trace.engine=&p->engineForTests();
    p->engineForTests().setDebugTapSink(&trace, Trace::capture);
    for (int pos = 0; pos < count; pos += block) {
        const int n = std::min(block, count - pos); transport.cursor = pos;
        if (pos == changeAt) {
            trace.requestNative = static_cast<int>(trace.filtered.size());
            if (next) select(*p, *next);
            if (qualityChange) p->setFilterQuality(FilterQuality::eco);
        }
        juce::AudioBuffer<float> audio(2, n); juce::MidiBuffer midi;
        if (noteAt >= pos && noteAt < pos + n) midi.addEvent(juce::MidiMessage::noteOn(1, 48, static_cast<juce::uint8>(110)), noteAt - pos);
        if (offAt >= pos && offAt < pos + n) midi.addEvent(offCode ? juce::MidiMessage::controllerEvent(1, offCode, 0) : juce::MidiMessage::noteOff(1, 48), offAt - pos);
        p->processBlock(audio, midi);
        check(p->getLatencySamples() == trace.latency, "latency declaration remains unchanged through selections");
        for (int i = 0; i < n; ++i) { const float y = audio.getSample(0, i); check(std::isfinite(y), "actual processor output finite"); trace.host.push_back(y); }
    }
    p->engineForTests().setDebugTapSink(nullptr, nullptr); p->setPlayHead(nullptr);
    trace.engine=nullptr;
    return trace;
}
void delayContracts() {
    using Access = DualSvfLatencyTestAccess;
    Access::Delay d;
    check(sizeof(d) == 256 * sizeof(float) + 2 * sizeof(uint32_t), "fixed delay storage: 1032 bytes");
    for (int i=0;i<=512;++i)
        check(d.process(i==0 ? 1.f : 0.f)==(i==256 ? 1.f : 0.f), "impulse emerges only at native index 256");
    check(!d.tailActive(), "zero input drains pending samples");
    d.reset();
    for(int i=0;i<1024;++i)
        check(d.process(static_cast<float>(i+1))==(i>=256 ? static_cast<float>(i-255) : 0.f), "continuous input retains exact delay");
    for(int i=0;i<256;++i) {
        check(d.process(0)==static_cast<float>(769+i), "continuous tail delivers every queued sample");
        check(d.tailActive()==(i<255), "continuous tail countdown ends after last delivery");
    }
    d.process(1); d.reset();
    for(int i=0;i<257;++i) check(d.process(0)==0,"reset discards an immediately preceding impulse");
    for(float x:{-0.0f,0.0f,-1.0f,1.0f,std::numeric_limits<float>::max(),std::numeric_limits<float>::lowest()}) {
        d.reset(); d.process(x);
        for(int i=1;i<256;++i) check(d.process(0)==0,"finite boundary cold fill remains zero");
        const float y=d.process(0);
        check(std::memcmp(&x,&y,sizeof(x))==0,"finite values and signed zero retain their bits");
    }
    d.reset();
    std::array<float,256> oracle{};
    for (int i=0;i<100003;++i) {
        const auto slot=static_cast<size_t>(i)%oracle.size();
        const float x=static_cast<float>((i*73)%509-254)/256.f;
        check(d.process(x)==oracle[slot], "sample values preserved across repeated wraparound"); oracle[slot]=x;
    }
    d.reset();
    check(d.process(std::numeric_limits<float>::infinity())==0 && !d.tailActive(), "nonfinite input does not poison delay");
    check(d.process(std::numeric_limits<float>::quiet_NaN())==0 && !d.tailActive(), "NaN input becomes silence");
    auto e=std::make_unique<SwaraXtEngine>();
    check(!Access::delay(*e).tailActive(), "constructor initializes empty delay");
    e->prepare(48000,16); e->reset(); e->setExperimentalSvfEnabledForTests(true); Access::select(*e);
    const LatencyProbeScope probe(true);
    check(Access::process(*e,1)==0, "selected Dual begins with zero fill");
    // A routing reset can empty the current core while older output is queued.
    e->setExperimentalSvfRoutingForTests(Route::serial); Access::select(*e);
    check(!e->experimentalSvfDualForTests().tailActive() && Access::tail(*e), "pending delay protects tail independently of reset core");
    for(int i=1;i<=256;++i) {
        check(Access::process(*e,0)==(i==256 ? 1.f : 0.f), "last pending sample is delivered");
        check(Access::tail(*e)==(i<256), "pending protection expires exactly after delivery");
    }
    Access::sleep(*e); check(e->dormantForTests(), "empty delay does not itself prevent dormancy");
    for(int operation=0;operation<6;++operation) {
        Access::delay(*e).process(.75f);
        if(operation==0) e->reset();
        if(operation==1) e->resetResampler();
        if(operation==2) e->prepare(96000,1);
        if(operation==3) Access::sleep(*e);
        if(operation==4) {
            // Host MIDI wakes the sleeping engine through its normal dispatch.
            Access::sleep(*e); juce::MidiBuffer midi;
            midi.addEvent(juce::MidiMessage::noteOn(1,48,static_cast<juce::uint8>(100)),0);
            juce::AudioBuffer<float> audio(2,1); e->process(midi,audio,0,1);
            // Examine the actual wake reset separately from newly rendered audio.
            check(!e->dormantForTests(), "MIDI wakes dormant engine");
            e->reset();
        }
        if(operation==5) {
            e->setExperimentalSvfEnabledForTests(false); Access::select(*e);
            e->setExperimentalSvfEnabledForTests(true); Access::select(*e);
        }
        check(!Access::delay(*e).tailActive(), "hard reset or bank activation clears pending samples");
        for(int i=0;i<256;++i) check(Access::delay(*e).process(0)==0, "old Dual audio cannot reappear after reset");
    }
    for(int i=0;i<100;++i) {
        Access::delay(*e).process(1);
        e->setExperimentalSvfEnabledForTests(false); Access::select(*e);
        check(!Access::delay(*e).tailActive(), "deactivation discards old session");
        e->setExperimentalSvfEnabledForTests(true); Access::select(*e);
        check(Access::process(*e,0)==0, "rapid reactivation has no stale sample");
    }
    for(int i=0;i<256;++i) Access::process(*e,static_cast<float>(i+1));
    check(Access::delay(*e).pending==256,"bank change can encounter 256 queued native samples");
    e->setExperimentalSvfEnabledForTests(false); Access::select(*e);
    e->setExperimentalSvfEnabledForTests(true); Access::select(*e);
    for(int i=0;i<257;++i) check(Access::process(*e,0)==0,"discarded bank tail never reappears on return");
    {
        // Isolate the engine's real tail-drain decision from the known pre-VCA
        // activity of a running oscillator. Only the delayed sample is audible.
        const LatencyProbeScope realCore(false);
        auto p=std::make_unique<SwaraXtAudioProcessor>(); neutral(*p);
        set(*p,IDs::osc1Shape,0); set(*p,IDs::osc2Shape,0);
        p->setExperimentalSvfEnabled(true); p->prepareToPlay(48000,16);
        auto& engine=p->engineForTests();
        Trace t; t.reserve(1024,48000); t.engine=&engine;
        engine.setDebugTapSink(&t,Trace::capture); Access::injectTail(engine);
        for(int block=0;block<6;++block) {
            Access::render(engine);
            check(!engine.drainingForTests(), "production drain cannot begin while delayed output is pending");
        }
        Access::render(engine); engine.setDebugTapSink(nullptr,nullptr);
        check(t.filtered.size()==280 && t.filtered[255]==.75f, "production renderer delivers final pending impulse");
        check(engine.drainingForTests(), "empty delay allows production SRC-tail drainage");
        juce::AudioBuffer<float> audio(2,64); juce::MidiBuffer midi;
        for(int i=0;i<4096 && !engine.dormantForTests();++i) engine.process(midi,audio,0,64);
        check(engine.dormantForTests(), "pending-only tail eventually drains through SRC/DC into dormancy");
    }
    std::printf("Delay: %zu bytes; engine: %zu bytes. Exact indexing, 100003 samples, pending tail, reset/prepare/wake and rapid bank boundaries checked.\n",sizeof(d),sizeof(SwaraXtEngine));
}

// Replay the complete fixed pipeline without using an onset or a fitted lag.
// The unity core remains INSIDE Classic's interpolation/decimation FIRs.
void fixedPipeline(const Trace& t, double rate, bool dual, int fx, int factor=4, bool nonlinearDual=false) {
    std::vector<float> expected; expected.reserve(t.mixer.size());
    FilterRateConverter converter; converter.prepare(factor);
    for(size_t i=0;i<t.mixer.size();++i) {
        float y;
        const auto& source=nonlinearDual ? t.undelayed : t.mixer;
        // An indexed offline shift, independent of the engine's circular delay.
        if(dual) y=i>=256 ? source[i-256] : 0.0f;
        else y=converter.process(source[i],[](float v){return v;});
        expected.push_back(y);
    }
    board::BoardProcessor effect; board::BoardPostDacFilter postDac;
    effect.reset(static_cast<board::Effect>(fx));
    double maxNativeError=0;
    if(fx!=0) for(size_t at=0;at+40<=expected.size();at+=40) {
        board::FloatBlock b{}; std::copy_n(expected.begin()+static_cast<std::ptrdiff_t>(at),40,b.begin());
        effect.processClassicFx(b,t.boardFrames[at/40]); postDac.process(b,40);
        std::copy(b.begin(),b.end(),expected.begin()+static_cast<std::ptrdiff_t>(at));
    }
    for(size_t i=0;i<expected.size();++i) {
        check(std::isfinite(expected[i]),"independent native reference is finite");
        maxNativeError=std::max(maxNativeError,std::abs(static_cast<double>(expected[i])-t.filtered[i]));
    }
    check(maxNativeError==0, "processor native output matches exact FIR/delay then unchanged FX reference");
    worstFixedNativeError=std::max(worstFixedNativeError,maxNativeError);
    HostRateConverter src; src.setStep(native,rate); src.reset(); DcBlocker dc; dc.prepare(rate);
    size_t at=0; double maxHostError=0;
    for(size_t i=0;i<t.host.size();++i) {
        const float actual=t.host[i];
        if(fx==0 && static_cast<int>(i)<t.eventAt) { check(actual==0,"dormant pre-event output is zero"); continue; }
        while(src.size()<src.queueTargetSize() && at<expected.size()) src.push(expected[at++]*t.masterGain);
        const float y=dc.process(src.readInterpolated());
        maxHostError=std::max(maxHostError,std::abs(static_cast<double>(y)-actual));
    }
    check(maxHostError==0, "actual host output matches independent sample-at-a-time SRC/DC replay");
    worstHostReplayError=std::max(worstHostReplayError,maxHostError);
    check(t.latency==static_cast<int>(std::ceil(384*rate/native)), "reported latency retains 384 native sample contract");
}

uint64_t q20Hash(const std::vector<float>& signal) {
    uint64_t hash=1469598103934665603ull;
    for(float y:signal) {
        check(std::isfinite(y),"reference fingerprint input is finite");
        const auto q=static_cast<int32_t>(std::llround(std::clamp(static_cast<double>(y),-32.0,32.0)*1048576.0));
        hash^=static_cast<uint32_t>(q); hash*=1099511628211ull;
    }
    return hash;
}

Trace renderSection2Reference(const Sel& selection,int fx) {
    // Exact Section 2 reference stimulus: factory 0, no transport or neutral
    // patch substitution, 48 kHz/256, note48 at block0 and note60 at block40.
    auto p=std::make_unique<SwaraXtAudioProcessor>(); select(*p,selection);
    if(fx!=0) set(*p,IDs::dspFxProgram,static_cast<float>(fx));
    p->prepareToPlay(48000,256);
    const LatencyProbeScope realCore(false);
    Trace t; t.reserve(25600,48000); t.engine=&p->engineForTests(); t.latency=p->getLatencySamples();
    t.masterGain=p->getApvts().getRawParameterValue(IDs::master)->load();
    p->engineForTests().setDebugTapSink(&t,Trace::capture);
    juce::AudioBuffer<float> audio(2,256); juce::MidiBuffer midi;
    for(int block=0;block<100;++block) {
        midi.clear();
        if(block==0) midi.addEvent(juce::MidiMessage::noteOn(1,48,static_cast<juce::uint8>(110)),0);
        if(block==40) midi.addEvent(juce::MidiMessage::noteOn(1,60,static_cast<juce::uint8>(96)),0);
        p->processBlock(audio,midi);
        t.host.insert(t.host.end(),audio.getReadPointer(0),audio.getReadPointer(0)+256);
    }
    auto& e=p->engineForTests(); e.setDebugTapSink(nullptr,nullptr);
    const auto& d=DualSvfLatencyTestAccess::delay(e);
    check(t.undelayed.size()==20960,"reference native extent includes unchanged SRC lookahead");
    uint32_t expectedPending=0;
    for(uint32_t i=0;i<256;++i) {
        const float expected=t.undelayed[t.undelayed.size()-256+i];
        const float actual=d.samples[(d.cursor+i)%256];
        check(std::memcmp(&actual,&expected,sizeof(actual))==0,"end-of-render queue retains the final 256 source samples");
        if(expected!=0) expectedPending=i+1;
    }
    check(d.pending==expectedPending,"pending countdown describes the final nonzero source sample");
    t.engine=nullptr;
    return t;
}

void section2TimingContracts() {
    const auto verify=[](const Sel& s,int fx,uint64_t nativeGolden,uint64_t hostGolden=0) {
        const auto t=renderSection2Reference(s,fx);
        // The frozen pre-F2.1 native fingerprint prevents candidate synthesis
        // from supplying its own unverified reference to the downstream replay.
        check(q20Hash(t.undelayed)==nativeGolden,"pre-delay synthesis matches the published baseline fingerprint");
        fixedPipeline(t,48000,true,fx,4,true);
        if(hostGolden!=0) check(q20Hash(t.host)==hostGolden,"migrated Section 2 host reference matches exact timing contract");
    };
    for(int cal=0;cal<2;++cal) {
        Sel s; s.enabled=true; s.calibration=static_cast<ShruthiSvfCalibration>(cal); s.routing=Route::serial;
        for(int i=0;i<3;++i) {
            s.control2=Control::relative; s.cutoff2=static_cast<uint8_t>(52+12*i); s.resonance2=0; s.mode2=Mode::lowPass;
            verify(s,0,dualRelativeNativeGoldens[cal][i],dualRelativeGoldens[cal][i]);
            s.control2=Control::independent; s.cutoff2=static_cast<uint8_t>(i==0 ? 40 : 90);
            s.resonance2=static_cast<uint8_t>(i==2 ? 63 : 32); s.mode2=Mode::bandPass;
            verify(s,0,dualIndependentNativeGoldens[cal][i],dualIndependentGoldens[cal][i]);
        }
        for(int route=0;route<3;++route) for(int control=0;control<3;++control) for(int fx:{0,8}) {
            s.routing=static_cast<Route>(route); s.control2=static_cast<Control>(control);
            s.cutoff2=static_cast<uint8_t>(control==2 ? 76 : 40); s.resonance2=32; s.mode2=Mode::bandPass;
            verify(s,fx,dualTimingNativeGoldens[cal][route][control]);
        }
    }
    std::puts("Section 2 timing: 12 migrated references + 36 route/calibration/control/FX cases; frozen baseline synthesis and independent indexed delay/FX/gain/SRC/DC replay.");
}
void latency(const std::filesystem::path& out) {
    const auto centre = [](const std::vector<float>& x) {
        double weighted=0, energy=0;
        for(size_t i=0;i<x.size();++i) { const double e=static_cast<double>(x[i])*x[i]; energy+=e; weighted+=static_cast<double>(i)*e; }
        return weighted/energy;
    };
    std::ofstream impulse(out/"delay-calibration.csv"); impulse << std::setprecision(15) << "component,rate,factor,energy_centre,peak_index\n";
    for(int factor:{2,4}) {
        FilterRateConverter converter; converter.prepare(factor); std::vector<float> y;
        for(int i=0;i<1024;++i) y.push_back(converter.process(i==0 ? 1.f : 0.f,[](float x){return x;}));
        const auto max=std::max_element(y.begin(),y.end());
        check(std::abs(centre(y)-256)<1.e-9,"actual Classic converter impulse has exactly 256 native samples of fixed delay");
        impulse << "filter_converter," << native << ',' << factor << ',' << centre(y) << ',' << (max-y.begin()) << '\n';
    }
    for(double rate:{44100.,48000.,96000.,192000.}) {
        HostRateConverter converter; converter.setStep(native,rate); converter.reset(); int input=0; std::vector<float> y;
        for(int i=0;i<2048;++i) { while(converter.size()<converter.queueTargetSize()) converter.push(input++==0 ? 1.f : 0.f); y.push_back(converter.readInterpolated()); }
        const auto max=std::max_element(y.begin(),y.end());
        impulse << "host_src," << rate << ",1," << centre(y) << ',' << (max-y.begin()) << '\n';
    }
    std::ofstream csv(out / "latency.csv"); csv << "rate,block,quality,path,probe,fx,reported,event,onset_host,onset_native,onset_mixer,onset_vca,peak,rms,file\n";
    for (double rate : {44100., 48000., 96000., 192000.}) for (int block : {1, 16, 32, 64})
    for (auto quality : {FilterQuality::eco, FilterQuality::normal, FilterQuality::high}) for (int path = -1; path < 3; ++path) for (bool probe : {false, true}) for(int fx:{0,8}) {
        Sel s; s.enabled = path >= 0; s.routing = static_cast<Route>(std::max(0, path));
        const auto t = render(rate, block, s, quality, probe, static_cast<int>(rate * .08), 113, -1, -1, nullptr, false, 0, fx);
        if(probe) fixedPipeline(t,rate,path>=0,fx,quality==FilterQuality::eco ? 2 : 4);
        else if(path>=0) fixedPipeline(t,rate,true,fx,4,true);
        const std::string name = "lat-" + std::to_string(static_cast<int>(rate)) + "-" + std::to_string(block) + "-" + std::to_string(static_cast<int>(quality)) + "-" + std::to_string(path) + "-" + std::to_string(probe) + "-" + std::to_string(fx) + ".f32";
        binary(out / name, t.host);
        if(fx==0) binary(out/(name+".native"),t.filtered);
        csv << rate << ',' << block << ',' << static_cast<int>(quality) << ',' << path << ',' << probe << ',' << fx << ',' << t.latency << ",113," << onset(t.host) << ',' << onset(t.filtered) << ',' << onset(t.mixer) << ',' << onset(t.vca) << ',' << peak(t.host) << ',' << rms(t.host) << ',' << name << '\n';
    }
    finish(impulse,"delay calibration CSV written"); finish(csv,"latency CSV written");
    std::puts("Latency: 768 processor renders with FX Off/Delay; exact fixed-pipeline and host replay checks are separate from nonlinear phase.");
    std::printf("Worst native fixed-pipeline error %.17g; host SRC/DC replay error %.17g.\n",worstFixedNativeError,worstHostReplayError);
}
struct Change { const char* name; Sel before, after; };
std::vector<Change> changes() {
    Sel c, a; a.enabled = true;
    Sel serial = a; serial.routing = Route::serial;
    Sel parallel = a; parallel.routing = Route::parallel;
    Sel bp = a; bp.mode1 = Mode::bandPass;
    Sel hp = a; hp.mode1 = Mode::highPass;
    Sel hw = a; hw.calibration = ShruthiSvfCalibration::eagleHardware;
    Sel independent = serial; independent.control2 = Control::independent; independent.cutoff2 = 40; independent.resonance2 = 48;
    Sel relative = independent; relative.control2 = Control::relative; relative.cutoff2 = 76;
    return {{"classic_dual",c,a},{"dual_classic",a,c},{"section1_serial",a,serial},
        {"serial_parallel",serial,parallel},{"parallel_section1",parallel,a},{"lp_bp",a,bp},
        {"bp_hp",bp,hp},{"hp_lp",hp,a},{"paper_eagle",a,hw},{"eagle_paper",hw,a},
        {"follow_independent",serial,independent},{"independent_relative",independent,relative},
        {"relative_follow",relative,serial}};
}
struct Difference { int first = -1; double boundary = 0, peakStep = 0, excessDerivativeRms = 0, rmsBefore = 0, rmsAfter = 0; };
Difference difference(const std::vector<float>& a, const std::vector<float>& b, size_t request, size_t window) {
    Difference d;
    const size_t n = std::min(a.size(), b.size());
    for (size_t i = request; i < n; ++i) if (std::abs(a[i] - b[i]) > 1.e-9f) { d.first = static_cast<int>(i); break; }
    if (d.first < 1) return d;
    const auto start = static_cast<size_t>(d.first), end = std::min(n, start + window);
    double squares = 0, pre = 0, post = 0;
    for (size_t i = start; i < end; ++i) {
        const double da = static_cast<double>(a[i]) - a[i - 1], db = static_cast<double>(b[i]) - b[i - 1];
        if (i == start) d.boundary = std::abs(da);
        d.peakStep = std::max(d.peakStep, std::abs(da)); squares += (da - db) * (da - db); post += static_cast<double>(a[i]) * a[i];
    }
    const size_t preStart = start > window ? start - window : 0;
    for (size_t i = preStart; i < start; ++i) pre += static_cast<double>(a[i]) * a[i];
    d.excessDerivativeRms = std::sqrt(squares / static_cast<double>(end - start));
    d.rmsBefore = std::sqrt(pre / static_cast<double>(start - preStart)); d.rmsAfter = std::sqrt(post / static_cast<double>(end - start));
    return d;
}
void transitions(const std::filesystem::path& out) {
    std::ofstream csv(out / "transitions.csv"); csv << std::setprecision(12)
        << "change,context,quality_change,request_native,first_native,first_host,host_delay,reported,native_boundary,native_peak_step,native_excess_derivative_rms,host_peak_step,host_excess_derivative_rms,rms_before,rms_after,vca_before,vca_after,peak,dual1_before_hz,dual1_after_block_hz,dual2_before_hz,dual2_after_block_hz,q1_before,q1_after_block,classic_cv_before,classic_cv_after_block\n";
    for (const auto& change : changes()) for (int context = 0; context < 4; ++context) for (bool quality : {false,true}) {
        const int note = context == 3 ? -1 : 0, off = context == 1 ? 3584 : -1;
        const float resonance = context == 2 ? 1.f : .2f;
        const auto base = render(48000, 32, change.before, FilterQuality::normal, false, 8192, note, off, 4096, nullptr, quality, resonance);
        const auto changed = render(48000, 32, change.before, FilterQuality::normal, false, 8192, note, off, 4096, &change.after, quality, resonance);
        const size_t common = std::min(base.mixer.size(), changed.mixer.size());
        check(std::equal(base.mixer.begin(), base.mixer.begin() + static_cast<std::ptrdiff_t>(common), changed.mixer.begin()), "selection changes preserve common firmware mixer history");
        check(std::equal(base.target.begin(), base.target.begin() + static_cast<std::ptrdiff_t>(common), changed.target.begin()), "selection changes preserve common firmware VCA targets");
        const auto nd = difference(changed.filtered, base.filtered, static_cast<size_t>(changed.requestNative), 80);
        // Include the complete shared-SRC transient; its first tiny precursor
        // precedes the main transition by approximately 128 native samples.
        const auto hd = difference(changed.host, base.host, 4096, 960);
        const size_t at = nd.first > 0 ? static_cast<size_t>(nd.first) : 1;
        const size_t frame=at/40;
        const auto before=frame>0 && frame-1<changed.controlFrames.size() ? changed.controlFrames[frame-1] : Trace::Controls{};
        const auto after=frame<changed.controlFrames.size() ? changed.controlFrames[frame] : Trace::Controls{};
        csv << change.name << ',' << context << ',' << quality << ',' << changed.requestNative << ',' << nd.first << ',' << hd.first << ',' << (hd.first < 0 ? -1 : hd.first - 4096) << ',' << changed.latency << ','
            << nd.boundary << ',' << nd.peakStep << ',' << nd.excessDerivativeRms << ',' << hd.peakStep << ',' << hd.excessDerivativeRms << ',' << nd.rmsBefore << ',' << nd.rmsAfter << ','
            << (at < changed.vca.size() ? changed.vca[at - 1] : 0) << ',' << (at < changed.vca.size() ? changed.vca[at] : 0) << ',' << peak(changed.host)
            << ',' << before.cut1 << ',' << after.cut1 << ',' << before.cut2 << ',' << after.cut2 << ',' << before.q1 << ',' << after.q1 << ',' << before.classicCv << ',' << after.classicCv << '\n';
        if (context == 2 && !quality && (std::string(change.name) == "classic_dual" || std::string(change.name) == "dual_classic" || std::string(change.name) == "serial_parallel")) binary(out / (std::string(change.name) + "-switch.f32"), changed.host);
    }
    Sel dual; dual.enabled = true;
    const auto sameQuality = render(48000,32,dual,FilterQuality::normal,false,8192,0,-1,4096);
    const auto newQuality = render(48000,32,dual,FilterQuality::normal,false,8192,0,-1,4096,nullptr,true);
    check(sameQuality.filtered == newQuality.filtered, "quality-only request leaves Dual core output identical");
    const auto qd = difference(newQuality.post,sameQuality.post,static_cast<size_t>(newQuality.requestNative),160);
    double minimumGain = 1;
    for (size_t i=static_cast<size_t>(newQuality.requestNative);i<newQuality.post.size();++i)
        if(std::abs(newQuality.filtered[i])>.01f) minimumGain=std::min(minimumGain,std::abs(static_cast<double>(newQuality.post[i])/newQuality.filtered[i]));
    std::ofstream qcsv(out/"quality-only.csv"); qcsv << "first_post_change,peak_step,minimum_post_to_filter_gain,reported\n"
        << qd.first << ',' << qd.peakStep << ',' << minimumGain << ',' << newQuality.latency << '\n';
    finish(csv,"transition CSV written"); finish(qcsv,"quality CSV written");
    std::puts("Transitions: 104 counterfactual pairs; derivative error includes intended response changes, not just clicks.");
}
void frozenClassic(const std::filesystem::path& out) {
    auto p=std::make_unique<SwaraXtAudioProcessor>(); neutral(*p); p->prepareToPlay(48000,64);
    juce::AudioBuffer<float> audio(2,64); juce::MidiBuffer midi;
    const auto process=[&](int blocks) { for(int i=0;i<blocks;++i) { p->processBlock(audio,midi); midi.clear(); } };
    midi.addEvent(juce::MidiMessage::noteOn(1,48,static_cast<juce::uint8>(110)),0); process(64);
    const double before=p->engineForTests().filter().boardCoreForTests().vcaControl();
    p->setExperimentalSvfEnabled(true); midi.addEvent(juce::MidiMessage::noteOff(1,48),0); process(256);
    const double inactive=p->engineForTests().filter().boardCoreForTests().vcaControl();
    const double selected=p->engineForTests().experimentalSvfDualForTests().vcaControl();
    std::vector<float> silent;
    for(int block=0;block<32;++block) { process(1); silent.insert(silent.end(),audio.getReadPointer(0),audio.getReadPointer(0)+64); }
    const bool deadBefore=p->engineForTests().shruthiPart().voice().amplitude_envelope_dead();
    p->setExperimentalSvfEnabled(false); std::vector<float> resumed;
    for(int block=0;block<32;++block) { process(1); resumed.insert(resumed.end(),audio.getReadPointer(0),audio.getReadPointer(0)+64); }
    std::ofstream csv(out/"frozen-classic.csv"); csv << std::setprecision(15) << "classic_vca_before,classic_vca_inactive,dual_vca,quiet_peak,resumed_peak,resumed_rms,voice_dead_before,voice_dead\n"
        << before << ',' << inactive << ',' << selected << ',' << peak(silent) << ',' << peak(resumed) << ',' << rms(resumed) << ',' << deadBefore << ',' << p->engineForTests().shruthiPart().voice().amplitude_envelope_dead() << '\n';
    check(before==inactive,"inactive Classic retains its old VCA reconstruction state");
    finish(csv,"stale-state CSV written");
    binary(out/"frozen-classic-return.f32",resumed);
}
void controls(const std::filesystem::path& out) {
    std::ofstream csv(out / "controls.csv"); csv << std::setprecision(15) << "note,trim,cutoff_panel,control2,firmware_cutoff,cutoff14,classic_cv_volts,dual1_hz,dual2_hz,firmware_cv1,firmware_cv2,audio_hash\n";
    for (int note : {36,64,96}) for (float trim : {0.f,.5f,1.f}) for (float cutoff : {500.f,20000.f}) for (auto control : {Control::legacyFollow,Control::independent,Control::relative}) {
        auto p=std::make_unique<SwaraXtAudioProcessor>(); neutral(*p); set(*p,IDs::filterKeyTracking,trim); set(*p,IDs::filterCutoff,cutoff);
        Sel s; s.enabled=true; s.routing=Route::serial; s.control2=control; s.cutoff2=64; select(*p,s); p->prepareToPlay(48000,128);
        juce::AudioBuffer<float> b(2,128); juce::MidiBuffer midi; midi.addEvent(juce::MidiMessage::noteOn(1,note,static_cast<juce::uint8>(110)),0);
        std::uint64_t hash=1469598103934665603ull;
        for (int i=0;i<48;++i) { p->processBlock(b,midi); midi.clear(); for(int j=0;j<128;++j) { const float y=b.getSample(0,j); const auto* bytes=reinterpret_cast<const unsigned char*>(&y); for(size_t k=0;k<sizeof(y);++k) { hash^=bytes[k]; hash*=1099511628211ull; } } }
        auto& e=p->engineForTests(); auto& dual=e.experimentalSvfDualForTests(); dual.snapControlsForTests();
        const auto& v=e.shruthiPart().voice();
        csv << note << ',' << trim << ',' << cutoff << ',' << static_cast<int>(control) << ',' << static_cast<int>(v.cutoff()) << ',' << v.filter_cutoff14() << ','
            << e.filter().paramsForTests().boardCutoffCvVolts << ',' << dual.section1().cutoffHz() << ',' << dual.section2().cutoffHz() << ',' << static_cast<int>(v.cv_1()) << ',' << static_cast<int>(v.cv_2()) << ',' << hash << '\n';
    }
    finish(csv,"control CSV written");
    std::puts("Controls: 54 actual-processor steady target observations.");
}
void contracts(const std::filesystem::path& out) {
    delayContracts();
    section2TimingContracts();
    for(int path=-1;path<3;++path) for(int fx:{0,8}) {
        Sel selected; selected.enabled=path>=0; selected.routing=static_cast<Route>(std::max(0,path));
        const auto t=render(48000,16,selected,FilterQuality::normal,true,4096,113,-1,-1,nullptr,false,0,fx);
        fixedPipeline(t,48000,path>=0,fx);
    }
    auto p=std::make_unique<SwaraXtAudioProcessor>(); neutral(*p); p->prepareToPlay(48000,64);
    juce::MemoryBlock before,after; p->getStateInformation(before);
    Sel s; s.enabled=true; s.routing=Route::serial; s.calibration=ShruthiSvfCalibration::eagleHardware; s.control2=Control::relative; s.cutoff2=76;
    select(*p,s); p->getStateInformation(after); check(before==after,"Dual choices absent from serialized state");
    p->setStateInformation(before.getData(),static_cast<int>(before.getSize())); check(Sel::pack(p->experimentalSvfSelection())==Sel::pack(s),"same-instance restore retains current prototype selection");
    auto fresh=std::make_unique<SwaraXtAudioProcessor>(); fresh->setStateInformation(after.getData(),static_cast<int>(after.getSize()));
    check(!fresh->experimentalSvfSelection().enabled,"fresh session restore returns to Classic");
    p->setCurrentProgram(1); check(Sel::pack(p->experimentalSvfSelection())==Sel::pack(s),"factory preset retains prototype selection");
    p->releaseResources(); p->prepareToPlay(192000,16); check(Sel::pack(p->experimentalSvfSelection())==Sel::pack(s),"prepare/reprepare retains prototype request");
    p->setUserPresetDirectoryForTests(juce::File(juce::String((out/"user-presets").string())));
    juce::String error; check(p->saveUserPreset("F2 contract",true,error),"external user preset saved");
    s.routing=Route::parallel; s.calibration=ShruthiSvfCalibration::paperApproved; select(*p,s);
    bool loaded=false; for (const auto& entry:p->getPresetEntries()) if (entry.name=="F2 contract") loaded=p->loadPresetEntry(entry,error);
    check(loaded && Sel::pack(p->experimentalSvfSelection())==Sel::pack(s),"user preset retains current rather than saved prototype selection");
    { auto editor=std::make_unique<SwaraXtAudioProcessorEditor>(*p); editor.reset(); }
    { auto editor=std::make_unique<SwaraXtAudioProcessorEditor>(*p); check(Sel::pack(p->experimentalSvfSelection())==Sel::pack(s),"editor reopen retains prototype selection"); }
    std::puts("Contracts: fresh/same-instance session, factory/user presets, reprepare and editor reopen checked.");
}
void tails(const std::filesystem::path& out) {
    std::ofstream csv(out/"tails.csv"); csv << "path,resonance,fx,off_code,seconds,tail_rms,peak_after_off,dormant,native_blocks,dual_tail,vca,voice_dead,early_tail_rms,vca_at_256ms\n";
    for (int path : {-1,0,1,2}) for (float res : {0.f,1.f}) for (int fx : {0,8}) for (int offCode : {0,120,123}) {
        auto p=std::make_unique<SwaraXtAudioProcessor>(); neutral(*p,false,res,fx); Sel s; s.enabled=path>=0; s.routing=static_cast<Route>(std::max(0,path)); select(*p,s);
        p->prepareToPlay(48000,256); juce::AudioBuffer<float> audio(2,256); std::vector<float> host; host.reserve(240128);
        double afterPeak=0, earlyTailSquares=0, vcaAt256ms=0; int earlyCount=0;
        for (int pos=0;pos<240128;pos+=256) {
            juce::MidiBuffer midi;
            if (pos==0) midi.addEvent(juce::MidiMessage::noteOn(1,48,static_cast<juce::uint8>(110)),0);
            if (pos==4096) midi.addEvent(offCode ? juce::MidiMessage::controllerEvent(1,offCode,0) : juce::MidiMessage::noteOff(1,48),0);
            p->processBlock(audio,midi);
            if(pos==12288) vcaAt256ms=p->engineForTests().experimentalSvfDualForTests().vcaControl();
            for(int i=0;i<256;++i) { const auto y=audio.getSample(0,i); check(std::isfinite(y),"release/FX trajectory finite"); host.push_back(y); if(pos>=4096) afterPeak=std::max(afterPeak,std::abs(static_cast<double>(y))); if(pos+i>=12288 && pos+i<24576) { earlyTailSquares+=static_cast<double>(y)*y; ++earlyCount; } }
        }
        auto& e=p->engineForTests();
        csv << path << ',' << res << ',' << fx << ',' << offCode << ",5.00266667," << rms(host,host.size()-24000) << ',' << afterPeak << ',' << e.dormantForTests() << ',' << e.cpuProfileForTests().nativeBlocksRendered << ',' << e.experimentalSvfDualForTests().tailActive() << ',' << e.experimentalSvfDualForTests().vcaControl() << ',' << e.shruthiPart().voice().amplitude_envelope_dead() << ',' << std::sqrt(earlyTailSquares/earlyCount) << ',' << vcaAt256ms << '\n';
    }
    finish(csv,"tail CSV written");
    std::puts("Tails: 48 five-second actual-processor note-off/CC120/CC123 cases with FX Off/Delay.");
}
}
int main(int argc,char** argv) {
    juce::ScopedJuceInitialiser_GUI init; std::setvbuf(stdout,nullptr,_IONBF,0);
    const std::filesystem::path out=argc>1 ? argv[1] : "dual-svf-integration-audit";
    std::filesystem::create_directories(out);
    const std::string mode=argc>2 ? argv[2] : "contracts";
    if(mode!="all" && mode!="latency" && mode!="transitions" && mode!="controls" && mode!="contracts" && mode!="tails") {
        std::fprintf(stderr,"Expected mode: all, latency, transitions, controls, contracts or tails.\n"); return 2;
    }
    if(mode=="all" || mode=="latency") latency(out);
    if(mode=="all" || mode=="transitions") { transitions(out); frozenClassic(out); }
    if(mode=="all" || mode=="controls") controls(out);
    if(mode=="all" || mode=="contracts") contracts(out);
    if(mode=="all" || mode=="tails") tails(out);
    std::printf("Dual SVF integration measurement fixtures: %d failures. Measurements do not assert production readiness.\n",failures);
    return failures ? 1 : 0;
}
