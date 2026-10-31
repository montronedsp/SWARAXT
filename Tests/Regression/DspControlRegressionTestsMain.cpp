// Copyright 2026 MontroneDSP.
// SPDX-License-Identifier: GPL-3.0-or-later
#include <JuceHeader.h>
#include "Plugin/PluginProcessor.h"
#include "Plugin/SwaraXtParameterLayout.h"
#include "shruthi/patch.h"
#include <array>
#include <cmath>
#include <cstdio>
#include <thread>
#include <vector>
#if defined(_MSC_VER) && defined(_DEBUG)
#include <crtdbg.h>
#endif

namespace {
using P = SwaraXtAudioProcessor;
using namespace swaraxt;
int failures = 0, checks = 0;
void expect(bool ok, const char* what) { ++checks; if (!ok) { ++failures; if (failures < 30) std::printf("FAIL: %s\n", what); } }
void set(P& p, const char* id, float value) {
    auto* parameter = p.getApvts().getParameter(id);
    if (!parameter) std::abort();
    parameter->setValueNotifyingHost(parameter->convertTo0to1(value));
}
void neutral(P& p) {
    set(p, IDs::seqMode, 0); set(p, IDs::perfGlide, 0); set(p, IDs::osc1Option, 0);
    set(p, IDs::osc1Shape, 1); set(p, IDs::osc2Shape, 0); set(p, IDs::osc1Range, 0);
    set(p, IDs::mixBalance, 0); set(p, IDs::mixSub, 0); set(p, IDs::mixNoise, 0);
    set(p, IDs::filterCutoff, 1000); set(p, IDs::filterResonance, 0);
    set(p, IDs::filterEnvDepth, 0); set(p, IDs::filterLfoDepth, 0); set(p, IDs::filterKeyTracking, .5f);
    set(p, IDs::env1Attack, 0); set(p, IDs::env1Sustain, 127);
    set(p, IDs::env2Attack, 0); set(p, IDs::env2Sustain, 127); set(p, IDs::env2Release, 80);
    set(p, IDs::dspFxProgram, 0);
    for (int row = 1; row <= 12; ++row)
        set(p, ("mod.row" + std::to_string(row) + ".amount").c_str(), row == 9 ? 63.f : 0.f);
}
void pump() { juce::MessageManager::getInstance()->runDispatchLoopUntil(1); }
#if defined(_MSC_VER) && defined(_DEBUG)
thread_local bool inAudio = false;
int allocations = 0;
int allocationHook(int type, void*, size_t, int, long, const unsigned char*, int) {
    if (inAudio && (type == _HOOK_ALLOC || type == _HOOK_REALLOC)) ++allocations;
    return 1;
}
#endif
void process(P& p, juce::AudioBuffer<float>& a, juce::MidiBuffer& m) {
#if defined(_MSC_VER) && defined(_DEBUG)
    inAudio = true;
#endif
    p.processBlock(a, m);
#if defined(_MSC_VER) && defined(_DEBUG)
    inAudio = false;
#endif
}
void render(P& p, int note = -1, int offset = 0, int blocks = 4) {
    juce::AudioBuffer<float> a(2, 128); juce::MidiBuffer m; m.ensureSize(2048);
    if (note >= 0) m.addEvent(juce::MidiMessage::noteOn(1, note, juce::uint8(100)), offset);
    for (int b = 0; b < blocks; ++b) { process(p, a, m); m.clear(); }
}
struct Host : juce::AudioPlayHead {
    double ppq = 0;
    juce::Optional<PositionInfo> getPosition() const override {
        PositionInfo i; i.setBpm(240); i.setPpqPosition(ppq); i.setIsPlaying(true); return i;
    }
};
void sourcePreservation() {
    P p; neutral(p); p.prepareToPlay(48000, 128); render(p, 60);
    auto& v = *p.engineForTests().shruthiPart().mutable_voice();
    // Every pending facade source, not only STEP, must survive the release wrapper.
    for (int repeat = 0; repeat < 4; ++repeat) {
        for (uint8_t i = 0; i < shruthi::MOD_SRC_LAST; ++i) v.set_modulation_source(i, static_cast<uint8_t>(i * 7 + repeat));
        v.NoteOff(); v.NoteOff();
        for (uint8_t i = 0; i < shruthi::MOD_SRC_LAST; ++i)
            expect(v.modulation_source(i) == static_cast<uint8_t>(i * 7 + repeat), "NoteOff preserves every pending facade source");
    }
}
void patternOrder() {
    constexpr uint16_t masks[] = {21845,30583,62965,46517,28527,2313,18761,21065,18725,56283,30431,43281,54553,28609,53505};
    for (int pattern = 0; pattern < 16; ++pattern) {
        pump(); P p; neutral(p); set(p, IDs::seqMode, 1); set(p, IDs::seqClockMode, 1);
        p.prepareToPlay(48000, 128); juce::AudioBuffer<float> a(2,128); juce::MidiBuffer m;
        for (int note : {67,60,64}) m.addEvent(juce::MidiMessage::noteOn(1,note,juce::uint8(100)),0);
        process(p,a,m); auto& part = p.engineForTests().shruthiPart(); auto* q = part.mutable_sequencer_settings();
        q->arp_pattern = static_cast<uint8_t>(pattern); q->arp_direction = 0; q->arp_range = 1;
        q->arp_clock_division = 11; q->pattern_size = 16;
        for (int i=0;i<16;++i) q->steps[i].set_raw(static_cast<uint8_t>(0x80|60),0x70);
        part.Start(false); int count=0; constexpr int notes[] = {60,64,67};
        for (int tick=0;tick<64;++tick) {
            part.Clock(false); part.ProcessControlBlock();
            const bool on = pattern == 15 || ((masks[pattern] >> (tick%16)) & 1) != 0;
            expect(part.voice().modulation_source(shruthi::MOD_SRC_STEP) == (on ? 255 : 0), "ROM pattern STEP follows independent mask including rests");
            if (on) { expect(part.voice().pitch_value() == notes[count%3]*128, "rests do not alter sorted arp pitch order"); ++count; }
        }
    }
}
struct Trace {
    P& p; bool sequence; int active=0, rests=0; double tailPeak=0;
    static void capture(void* context, const SwaraXtEngine::DebugBlockCapture& b) {
        auto& t=*static_cast<Trace*>(context); auto& part=t.p.engineForTests().shruthiPart(); const auto step=part.step();
        if (step>=16) return;
        // Pattern 15 has active, tied, rest, rest; pattern 0 alternates.
        const bool on=t.sequence ? step%4==0 : step%2==0;
        expect(part.voice().modulation_source(shruthi::MOD_SRC_STEP)==(on?255:0), "live callback STEP source follows rest/tie policy");
        const int base = part.patch().filter_cutoff * 128;
        const int delta = std::min(16383, base + (on ? 32 * 255 : 0)) - base;
        expect(part.voice().cutoff_matrix_delta()==delta, "STEP-to-cutoff matrix consumes current source with firmware clipping");
        if (on) ++t.active; else { ++t.rests; for (int i=0;i<b.samples;++i) t.tailPeak=std::max(t.tailPeak,std::abs(double(b.filterOutput[i]))); }
    }
};
void liveSteps() {
    for (bool host : {false,true}) for (bool duo : {false,true}) for (bool sequence : {false,true}) {
        pump(); P p; neutral(p); Host h; p.setPlayHead(&h);
        set(p,IDs::seqMode,1); set(p,IDs::seqClockMode,host?1.f:0.f); set(p,IDs::seqTempo,240);
        set(p,IDs::arpGate,11); set(p,IDs::arpPattern,sequence?15.f:0.f); set(p,IDs::seqGate,50);
        set(p,IDs::osc1Option,duo?float(shruthi::OP_DUO):0.f);
        set(p,"mod.row1.source",float(shruthi::MOD_SRC_STEP)); set(p,"mod.row1.destination",float(shruthi::MOD_DST_FILTER_CUTOFF)); set(p,"mod.row1.amount",32);
        auto s=SequenceState::defaultSnapshot(); s.length=16;
        for(int i=0;i<16;++i) s.steps[size_t(i)]=SequenceSnapshot::pack(static_cast<uint8_t>((i%4<2?0x80:0)|60),static_cast<uint8_t>(i%4==1?0xf0:0x70));
        s.arpPattern = static_cast<uint8_t>(sequence ? 15 : 0);
        p.sequenceState().store(s); p.prepareToPlay(48000,64); Trace trace{p,sequence}; p.engineForTests().setDebugTapSink(&trace,Trace::capture);
        juce::AudioBuffer<float>a(2,64); juce::MidiBuffer m; m.ensureSize(2048);
        for(int pos=0;pos<24000;pos+=64) {
            m.clear(); if(!pos) for(int note:{60,64,67})m.addEvent(juce::MidiMessage::noteOn(1,note,juce::uint8(100)),0);
            h.ppq=pos/12000.; process(p,a,m);
            for(int i=0;i<64;++i)expect(std::isfinite(a.getSample(0,i)),"release-tail output finite");
        }
        expect(trace.active>0&&trace.rests>0,"both sounding and resting blocks measured");
        expect(trace.tailPeak>.001,"nonzero release audio exercises rest modulation");
        p.engineForTests().setDebugTapSink(nullptr,nullptr); p.setPlayHead(nullptr);
    }
}

struct Fixture {
    juce::File directory = juce::File::getSpecialLocation(juce::File::tempDirectory).getChildFile("swara-control-"+juce::Uuid().toString());
    Fixture() { directory.createDirectory(); juce::String scale="Twelve\n12\n"; for(int i=1;i<=12;++i)scale+=juce::String(100*i)+".\n";
        expect(directory.getChildFile("test.scl").replaceWithText(scale),"write SCL fixture");
        expect(directory.getChildFile("test.kbm").replaceWithText("3\n0\n127\n60\n61\n300.0\n7\n0\n2\nx\n"),"write KBM fixture"); }
    ~Fixture() { directory.deleteRecursively(); }
    void load(P& p) { juce::String error; expect(p.importTuningFile(directory.getChildFile("test.scl"),directory.getChildFile("test.kbm"),error),"transactional SCL/KBM import succeeds"); }
};
// Independent mapping/quantization oracle: a repeating [0,2,x] pattern with
// seven-degree formal advance, reference degree 2 at 300 Hz.
int expectedWord(int mode,int note) {
    double midi=double(note);
    if(mode==1) {
        int cycle=(note-60)/3, key=(note-60)%3; if(key<0){key+=3;--cycle;}
        if(key==2)return -1;
        midi=69+12*std::log2(300./440.)+cycle*7+(key==1?2:0)-2;
    } else if(mode==2) {
        double best=1.e9, chosen=0;
        for(int degree=-200;degree<=200;++degree) {
            const double candidate=69+12*std::log2(300./440.)+degree-2;
            if(std::abs(candidate-note)<best){best=std::abs(candidate-note);chosen=candidate;}
        }
        midi=chosen;
    }
    return std::clamp(int(std::lround(midi*128)),1,16383);
}
void modes(Fixture& fixture) {
    expect(expectedWord(1,57)==6831,"independent KBM reference is pitch word 6831");
    for(int old=0;old<3;++old)for(int mode=0;mode<3;++mode)for(int note:{57,58,59})for(int offset:{0,17,63,127}) {
        pump(); P p; neutral(p); fixture.load(p); set(p,IDs::midiInputTuning,float(old)); p.prepareToPlay(48000,128); render(p);
        const auto bytes=p.engineForTests().tuningBytesForState(); const int slot=p.engineForTests().publishedTuningForTests();
        set(p,IDs::midiInputTuning,float(mode)); render(p,note,offset);
        const int expected=expectedWord(mode,note); const auto& v=p.engineForTests().shruthiPart().voice();
        expect(expected<0?v.amplitude_envelope_dead():v.pitch_value()==expected,"same callback mode and MIDI use current mapped/quantized/off policy");
        expect(p.engineForTests().tuningBytesForState()==bytes&&p.engineForTests().publishedTuningForTests()==slot,"mode change does not republish or alter tuning bytes/slot");
    }
    // Multiple MIDI offsets within one admitted callback; final held note must
    // use the same mode as its predecessors, not the previous callback's mode.
    P p; neutral(p);fixture.load(p);p.prepareToPlay(48000,128);render(p);set(p,IDs::midiInputTuning,1);
    juce::AudioBuffer<float>a(2,128);juce::MidiBuffer m;m.ensureSize(2048);
    m.addEvent(juce::MidiMessage::noteOn(1,57,juce::uint8(100)),0);m.addEvent(juce::MidiMessage::noteOff(1,57),35);
    m.addEvent(juce::MidiMessage::noteOn(1,58,juce::uint8(100)),67);process(p,a,m);render(p);
    expect(p.engineForTests().shruthiPart().voice().pitch_value()==expectedWord(1,58),"multiple event offsets use admitted mode");
    set(p,IDs::midiInputTuning,0);render(p);
    expect(p.engineForTests().shruthiPart().voice().pitch_value()==expectedWord(1,58),"mode edits do not retroactively retune held notes");
    render(p,60);expect(p.engineForTests().shruthiPart().voice().pitch_value()==60*128,"next note uses newly admitted off mode");
}
void restoreAndAdmission(Fixture& fixture) {
    pump();P src;neutral(src);fixture.load(src);set(src,IDs::midiInputTuning,1);juce::MemoryBlock state;src.getStateInformation(state);
    for(bool prepareFirst:{false,true}) {P p; if(prepareFirst)p.prepareToPlay(48000,128);p.setStateInformation(state.getData(),int(state.getSize()));if(!prepareFirst)p.prepareToPlay(48000,128);render(p,57);
        expect(p.engineForTests().shruthiPart().voice().pitch_value()==6831,"restored imported tuning/mode applies to first note");}
    P p;neutral(p);fixture.load(p);p.prepareToPlay(48000,128);render(p);
    const auto oldSlot=p.engineForTests().publishedTuningForTests();
    struct Pause {juce::WaitableEvent entered,release; static void hook(void*c,P::StatePublicationPoint point){if(point!=P::StatePublicationPoint::restoreBeforeTuningPublish)return;auto&x=*static_cast<Pause*>(c);x.entered.signal();if(!x.release.wait(15000))std::abort();}} pause;
    p.setStatePublicationHook(Pause::hook,&pause);std::thread writer([&]{p.setStateInformation(state.getData(),int(state.getSize()));});
    expect(pause.entered.wait(15000),"writer held at existing state publication gate");render(p,57);
    expect(p.engineForTests().shruthiPart().voice().pitch_value()==7296,"failed audio control admission retains prior input mode");
    expect(p.engineForTests().publishedTuningForTests()==oldSlot,"failed admission leaves tuning publication intact");
    pause.release.signal();writer.join();p.setStatePublicationHook(nullptr,nullptr);render(p,57);
    expect(p.engineForTests().shruthiPart().voice().pitch_value()==6831,"later admitted restore applies tuning and mode together");
}
}
int main(int argc,char**argv) {
    juce::ScopedJuceInitialiser_GUI init;std::setvbuf(stdout,nullptr,_IONBF,0);
#if defined(_MSC_VER) && defined(_DEBUG)
    const auto previous=_CrtSetAllocHook(allocationHook);
#endif
    const juce::String mode=argc>1?argv[1]:"all";
    if(mode=="all"||mode=="step"){sourcePreservation();patternOrder();liveSteps();}
    if(mode=="all"||mode=="tuning"){Fixture fixture;modes(fixture);restoreAndAdmission(fixture);}
#if defined(_MSC_VER) && defined(_DEBUG)
    _CrtSetAllocHook(previous);expect(allocations==0,"no callback allocation/reallocation");std::printf("Callback allocations=%d\n",allocations);
#endif
    std::printf("DSP control regression %s checks=%d failures=%d\n",mode.toRawUTF8(),checks,failures);return failures?1:0;
}
