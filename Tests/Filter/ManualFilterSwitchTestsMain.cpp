// Copyright 2026 MontroneDSP. SPDX-License-Identifier: GPL-3.0-or-later
#include <JuceHeader.h>
#include "Plugin/PluginProcessor.h"
#include "Plugin/SwaraXtParameterLayout.h"
#include "Engine/Filter/ShruthiSvfSection2Control.h"
#include <algorithm>
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
    static bool dual(const SwaraXtEngine& e){return e.experimentalSvfEnabled_;}
    static auto& delay(SwaraXtEngine& e){return e.dualOutputDelay_;}
    static auto& core(SwaraXtEngine& e){return e.experimentalSvf_;}
    static auto installed(const SwaraXtEngine& e){return e.appliedExperimentalSvfSelection_;}
    static auto quality(const SwaraXtEngine& e){return e.filter_.quality();}
    static float gain(const SwaraXtEngine& e){return e.qualityGain_;}
    static int qualityPhase(const SwaraXtEngine& e){return static_cast<int>(e.qualityFadePhase_);}
    static void qualityTick(SwaraXtEngine& e){e.nextQualityGain();}
    static void request(SwaraXtEngine& e,ExperimentalSvfSelection s){e.experimentalSvfControl_.request(s);}
    static void awake(SwaraXtEngine& e){e.dormant_=false;}
    static void sleep(SwaraXtEngine& e){e.enterDormant();}
    static void dcDrain(SwaraXtEngine& e){
        e.dormant_=false;e.drainingToDormant_=true;e.dcDrainingToDormant_=true;
        e.dcBlocker_.reset();e.dcBlocker_.process(1.f);
    }
    static void queuedSrcDrain(SwaraXtEngine& e){
        dcDrain(e);e.dcDrainingToDormant_=false;e.internalQueue_.reset();
        for(int n=0;n<280;++n)e.internalQueue_.push(n<24?static_cast<float>(n+1)*1.e-6f:0.f);
    }
    static bool tail(const SwaraXtEngine& e){return e.selectedFilterTailActive();}
    static float sample(SwaraXtEngine& e,float x,float vca){return e.processSelectedFilterSample(x,vca);}
#if !defined(SWARAXT_MANUAL_SWITCH_BASELINE)
    static int phase(const SwaraXtEngine& e){return static_cast<int>(e.manualSwitchPhase_);}
    static int remaining(const SwaraXtEngine& e){return e.manualSwitchRemaining_;}
    static auto frame(const SwaraXtEngine& e){return e.manualSwitchSample_;}
    static void boundary(SwaraXtEngine& e,ExperimentalSvfSelection s){e.updateManualFilterSelection(s);}
#else
    static int phase(const SwaraXtEngine&){return 0;}
    static void boundary(SwaraXtEngine& e,ExperimentalSvfSelection s){e.applyPendingFilterQuality(s.enabled!=e.experimentalSvfEnabled_);e.applyExperimentalSvfSelection(s);}
#endif
};
}
namespace {
using namespace swaraxt;
using Access=ManualSwitchTestAccess;
using Sel=ExperimentalSvfSelection;
using Q=FilterQuality;
int failures=0;
thread_local bool inAudio=false;
thread_local unsigned heapOperations=0;
#if defined(_MSC_VER) && defined(_DEBUG)
int __cdecl heapHook(int kind,void*,size_t,int,long,const unsigned char*,int){
    if(inAudio&&(kind==_HOOK_ALLOC||kind==_HOOK_REALLOC||kind==_HOOK_FREE))++heapOperations;
    return 1;
}
#endif
void check(bool ok,const char* what){if(!ok){if(failures<32)std::printf("FAIL: %s\n",what);++failures;}}
[[maybe_unused]] bool bits(float a,float b){return std::memcmp(&a,&b,sizeof(a))==0;}
void drainMessages(){
    check(juce::MessageManager::getInstance()->isThisTheMessageThread(),"fixture lifecycle and async delivery use message thread");
    juce::MessageManager::getInstance()->runDispatchLoopUntil(1);
}
void set(SwaraXtAudioProcessor& p,const char* id,float value){auto* a=p.getApvts().getParameter(id);check(a!=nullptr,id);if(a)a->setValueNotifyingHost(a->convertTo0to1(value));}
void setup(SwaraXtAudioProcessor& p,int fx=0,float resonance=.2f){
    set(p,IDs::osc1Shape,1);set(p,IDs::osc1Param,0);set(p,IDs::osc1Range,0);set(p,IDs::osc2Shape,0);
    set(p,IDs::mixBalance,0);set(p,IDs::mixSub,0);set(p,IDs::mixNoise,0);set(p,IDs::filterCutoff,10000);
    set(p,IDs::filterResonance,resonance);set(p,IDs::filterKeyTracking,.5f);set(p,IDs::filterEnvDepth,0);set(p,IDs::filterLfoDepth,0);
    set(p,IDs::perfGlide,0);set(p,IDs::postMixer,0);set(p,IDs::master,.5f);set(p,IDs::seqMode,0);
    set(p,IDs::dspFxProgram,static_cast<float>(fx));set(p,IDs::dspFxParam1,64);set(p,IDs::dspFxParam2,32);
    for(const auto* id:{IDs::env1Attack,IDs::env1Decay,IDs::env1Release,IDs::env2Attack,IDs::env2Decay,IDs::env2Release})set(p,id,0);
    set(p,IDs::env1Sustain,127);set(p,IDs::env2Sustain,127);
    for(int row=1;row<=12;++row){const auto id="mod.row"+std::to_string(row)+".amount";set(p,id.c_str(),row==9?63.f:0.f);}
}
void process(SwaraXtAudioProcessor& p,juce::AudioBuffer<float>& b,juce::MidiBuffer& m){inAudio=true;p.processBlock(b,m);inAudio=false;m.clear();}
[[maybe_unused]] Sel selection(bool dual){Sel s;s.enabled=dual;return s;}
double peak(const std::vector<float>& a){double r=0;for(float x:a)r=std::max(r,std::abs(static_cast<double>(x)));return r;}
double rms(const std::vector<float>& a){double sum=0;for(float x:a)sum+=static_cast<double>(x)*x;return std::sqrt(sum/static_cast<double>(std::max<size_t>(1,a.size())));}

#if !defined(SWARAXT_MANUAL_SWITCH_BASELINE)
std::unique_ptr<SwaraXtEngine> engine(bool dual){auto e=std::make_unique<SwaraXtEngine>();Access::request(*e,selection(dual));e->prepare(48000,64);e->reset();Access::awake(*e);return e;}
float input(int n){return static_cast<float>((n*73)%509-254)/512.f;}
void exactCountsAndCancellation(){
    for(bool initial:{false,true})for(bool cancel:{false,true}){
        auto e=engine(initial);auto original=engine(initial);
        for(int i=0;i<1024;++i){Access::sample(*e,input(i),.8f);Access::sample(*original,input(i),.8f);}
        auto requested=selection(!initial);Access::boundary(*e,requested);
        check(Access::phase(*e)==1&&Access::remaining(*e)==157,"request begins a captured 157-sample fade");
        for(int n=0;n<160;++n){
            if(cancel&&n==40)requested=selection(initial);
            if(n&&n%40==0)Access::boundary(*e,requested);
            const float raw=n<157?Access::sample(*original,input(n+1024),.8f):0;
            const float y=Access::sample(*e,input(n+1024),.8f);const auto f=Access::frame(*e);
            check(Access::dual(*e)==initial,"outgoing selection holds through exact zero boundary");
            if(n<157){const float gain=static_cast<float>(156-n)/156.f;check(bits(f.raw,raw)&&bits(f.gain,gain)&&bits(y,gain==0?0.f:(gain==1?raw:raw*gain)),"independent fade-out oracle and unchanged outgoing state");}
            else check(y==0&&!f.processed,"outgoing engine stops immediately after zero, before reset boundary");
            check(Access::tail(*e),"switch protects drainage while work remains");
        }
        const auto fifo=Access::delay(*e);Access::boundary(*e,requested);
        if(cancel){
            check(Access::phase(*e)==3&&Access::dual(*e)==initial,"reversal resumes original bank without priming");
            check(fifo.samples==Access::delay(*e).samples&&fifo.cursor==Access::delay(*e).cursor,"reversal preserves entire FIFO state");
        }else check(Access::phase(*e)==2&&Access::remaining(*e)==512&&Access::dual(*e)!=initial,"incoming bank resets only at muted transaction boundary");
        SwaraXtFilter classic;classic.prepare(SwaraXtEngine::kInternalSampleRate);
        ShruthiSvfDual dual;dual.prepare(SwaraXtEngine::kInternalSampleRate);
        std::array<float,256> delay{};size_t cursor=0;
        for(int n=0;n<(cancel?157:669);++n){
            if(n%40==0)Access::boundary(*e,selection(initial)); // Cannot interrupt a committed operation.
            const float x=input(n+1184);const float y=Access::sample(*e,x,.8f);const auto f=Access::frame(*e);
            if(cancel){
                const float raw=Access::sample(*original,x,.8f);check(bits(f.raw,raw),"cancellation never destroys original filter history");
            }else{
                float raw;
                if(initial)raw=classic.processInstrumentSample(x,.8f);
                else {raw=delay[cursor];delay[cursor]=dual.process(x,.8f);cursor=(cursor+1)%256;}
                check(bits(f.raw,raw),"incoming complete filter and FIFO match independent cold-state replay");
            }
            const int fadeIndex=cancel?n:n-512;
            const float gain=fadeIndex<0?0.f:static_cast<float>(fadeIndex)/156.f;
            check(bits(f.gain,gain)&&bits(y,gain==0?0.f:(gain==1?f.raw:f.raw*gain)),"512 muted samples then exact 0..1 inclusive fade-in");
        }
        check(Access::phase(*e)==0&&Access::remaining(*e)==0,"operation completes without restarting pending requests");
        check(Access::dual(*e)==(cancel?initial:!initial),"committed selection remains coherent");
    }
}
void qualityAndLifecycle(){
    auto e=engine(false);e->setFilterQuality(Q::eco);Access::boundary(*e,selection(false));
    for(int i=0;i<40;++i)Access::qualityTick(*e);
    Access::boundary(*e,selection(true));check(Access::phase(*e)==0,"manual fade waits for active quality cycle");
    for(int i=0;i<160;++i){if(i%40==0){e->setFilterQuality(i%80?Q::high:Q::normal);Access::boundary(*e,selection(true));}Access::qualityTick(*e);}
    Access::boundary(*e,selection(true));check(Access::phase(*e)==1&&bits(Access::gain(*e),1.f),"quality flood yields admission at exact unity");
    for(int i=0;i<829;++i){if(i%40==0)Access::boundary(*e,selection(true));Access::sample(*e,input(i),.8f);}
    e->setFilterQuality(Q::eco);Access::boundary(*e,selection(false));
    for(int i=0;i<160;++i){if(i%40==0)Access::boundary(*e,selection(false));Access::sample(*e,input(i),.8f);}
    e->setFilterQuality(Q::high);Access::boundary(*e,selection(false));
    check(Access::quality(*e)==Q::high&&Access::phase(*e)==2,"latest Classic quality resolves at muted reset before preparation");
    e->setFilterQuality(Q::normal);
    for(int i=0;i<669;++i){if(i%40==0)Access::boundary(*e,selection(false));Access::sample(*e,input(i),.8f);check(Access::quality(*e)==Q::high&&bits(Access::gain(*e),1.f),"quality cannot reset priming or fade the manual envelope");}
    Access::boundary(*e,selection(false));check(Access::qualityPhase(*e)==1,"newer quality resumes ordinary Classic controller after completion");
    for(int i=0;i<78;++i)Access::qualityTick(*e);check(Access::quality(*e)==Q::normal,"ordinary quality changes at sample 78");
    for(int i=0;i<78;++i)Access::qualityTick(*e);check(bits(Access::gain(*e),1.f),"ordinary quality returns to unity at sample 156");
    for(int elapsed:{40,200,720})for(int op=0;op<4;++op){
        auto reset=engine(false);Access::request(*reset,selection(true));
        for(int i=0;i<elapsed;++i){if(i%40==0)Access::boundary(*reset,selection(true));Access::sample(*reset,input(i),.8f);}
        if(op==0)reset->reset();else if(op==1)reset->prepare(96000,16);else if(op==2)reset->resetResampler();else Access::sleep(*reset);
        check(Access::phase(*reset)==0&&Access::dual(*reset)&&bits(Access::gain(*reset),1.f),"all hard lifecycle paths collapse each active phase coherently");
    }
}
void fifoAndRequests(){
    for(int fill:{1,37,256}){
        auto e=engine(true);std::array<float,256> expected{};
        for(int i=0;i<fill;++i){const float x=static_cast<float>(i+1)/1024.f;Access::delay(*e).process(x);expected[static_cast<size_t>(i)]=x;}
        Access::boundary(*e,selection(false));
        for(int i=0;i<160;++i){const float y=Access::sample(*e,0,0);const auto f=Access::frame(*e);
            if(i<157){const auto index=static_cast<size_t>((fill+i)%256);check(bits(f.raw,expected[index]),"request-time FIFO follows independent indexed stream until retirement");}
            else check(y==0&&!f.processed,"unrendered outgoing FIFO is silent after exact zero");
        }
        Access::boundary(*e,selection(false));check(!Access::delay(*e).tailActive()&&Access::delay(*e).cursor==0,"muted retirement discards remaining outgoing FIFO by policy");
        for(int i=0;i<669;++i)Access::sample(*e,0,0);
        Access::boundary(*e,selection(true));for(int i=0;i<160;++i)Access::sample(*e,0,0);Access::boundary(*e,selection(true));
        for(int i=0;i<669;++i){Access::sample(*e,0,0);check(Access::frame(*e).raw==0,"reactivation cannot replay retired FIFO data");}
    }
    auto e=engine(false);Sel first=selection(true);first.routing=ShruthiSvfDualRouting::serial;
    first.mode1=ShruthiSvf2pMode::bandPass;first.mode2=ShruthiSvf2pMode::highPass;first.calibration=ShruthiSvfCalibration::eagleHardware;first.control2=ShruthiSvfSection2Control::independent;first.cutoff2=39;first.resonance2=51;
    Access::boundary(*e,first);for(int i=0;i<160;++i)Access::sample(*e,input(i),.8f);
    Access::request(*e,first);const auto captured=e->experimentalSvfSelection();
    Sel later=first;later.enabled=false;later.routing=ShruthiSvfDualRouting::parallel;later.mode1=ShruthiSvf2pMode::highPass;later.mode2=ShruthiSvf2pMode::lowPass;later.calibration=ShruthiSvfCalibration::paperApproved;later.control2=ShruthiSvfSection2Control::relative;later.cutoff2=83;later.resonance2=9;
    Access::request(*e,later);Access::boundary(*e,captured);
    for(int i=0;i<669;++i){if(i%40==0)Access::boundary(*e,e->experimentalSvfSelection());Access::sample(*e,input(i),.8f);
        check(Sel::pack(Access::installed(*e))==Sel::pack(first),"all incoming settings remain frozen after captured muted-boundary application");
        check(Access::core(*e).routing()==first.routing&&Access::core(*e).mode1()==first.mode1&&Access::core(*e).mode2()==first.mode2&&Access::core(*e).calibration()==first.calibration,"actual incoming DSP retains the captured topology and modes");
        check(Sel::pack(e->experimentalSvfSelection())==Sel::pack(later),"newer full request remains in the single atomic mailbox");
    }
    Access::boundary(*e,later);for(int i=0;i<829;++i){if(i%40==0)Access::boundary(*e,later);Access::sample(*e,input(i),.8f);}
    check(Access::phase(*e)==0&&!Access::dual(*e),"finite compound request burst completes to latest bank");
    for(bool dual:{false,true}){auto silent=engine(dual);Access::boundary(*silent,selection(!dual));check(Access::phase(*silent)==1,"closed VCA and dead voice alone cannot trigger dormant fast path");for(int i=0;i<829;++i){if(i%40==0)Access::boundary(*silent,selection(!dual));check(Access::sample(*silent,0,0)==0,"awake silent switching stays silent");}check(Access::phase(*silent)==0,"awake silent switching remains bounded");}
}
#endif

struct Capture {
    SwaraXtEngine* engine=nullptr;
    Sel expected{};
    std::vector<float> native,host;
#if !defined(SWARAXT_MANUAL_SWITCH_BASELINE)
    std::vector<SwaraXtEngine::ManualSwitchSample> frames;
    SwaraXtFilter classic;
    ShruthiSvfDual dual;
    std::array<float,256> delay{};size_t cursor=0;
    bool replaying=false,wasDual=false;
    bool configurationReady=false;
    board::BoardProcessor fx;board::BoardPostDacFilter dac;
    bool fxReady=false;board::Effect effect=board::Effect::off;
#endif
    Capture(){native.reserve(40000);host.reserve(40000);
#if !defined(SWARAXT_MANUAL_SWITCH_BASELINE)
        frames.reserve(40000);classic.prepare(SwaraXtEngine::kInternalSampleRate);dual.prepare(SwaraXtEngine::kInternalSampleRate);
#endif
    }
    static void tap(void* context,const SwaraXtEngine::DebugBlockCapture& b){
        auto& t=*static_cast<Capture*>(context);
#if !defined(SWARAXT_MANUAL_SWITCH_BASELINE)
        const auto& voice=t.engine->shruthiPart().voice();
        if(!t.configurationReady){
            t.dual.setRouting(t.expected.routing);t.dual.setMode1(t.expected.mode1);t.dual.setMode2(t.expected.mode2);t.dual.setCalibration(t.expected.calibration);t.dual.setSection2FollowsSection1(t.expected.control2==ShruthiSvfSection2Control::legacyFollow);t.configurationReady=true;
        }
        t.classic.setParams(t.engine->filter().paramsForTests());
        if(b.manualSwitch[0].phase==2&&b.manualSwitch[0].processed&&!t.replaying){
            t.classic.setQuality(Access::quality(*t.engine));t.classic.reset();
            dualReset(t);t.replaying=true;t.wasDual=b.manualSwitch[0].dual;
        }
        t.dual.setFirmwareCutoff(voice.cutoff());t.dual.setFirmwareResonance(voice.resonance());
        if(b.manualSwitch[0].dual&&t.expected.control2!=ShruthiSvfSection2Control::legacyFollow){
            const auto cv=reconstructSvfSection2(t.engine->shruthiPart().patch(),voice,t.expected.cutoff2,t.expected.resonance2,t.expected.control2==ShruthiSvfSection2Control::relative);
            t.dual.setFirmwareCutoff2(cv.cutoff);t.dual.setFirmwareResonance2(cv.resonance);
        }
        board::FloatBlock block{};
        for(int i=0;i<b.samples;++i){const auto f=b.manualSwitch[i];t.frames.push_back(f);block[static_cast<size_t>(i)]=f.output;
            check(std::isfinite(f.raw)&&std::isfinite(f.output),"selected states and manual output remain finite before shared FX");
            if(t.replaying&&f.processed&&f.dual==t.wasDual){float raw;
                if(f.dual){raw=t.delay[t.cursor];t.delay[t.cursor]=t.dual.process(b.postShruthiMixer[i],b.vcaTarget[i]);t.cursor=(t.cursor+1)%256;}
                else raw=t.classic.processInstrumentSample(b.postShruthiMixer[i],b.vcaTarget[i]);
                check(bits(raw,f.raw),"actual current mixer/CV preparation matches independent filter/FIFO replay");
            }
            if(f.phase==0)t.replaying=false;
        }
        const auto controls=t.engine->boardControlsForTests();
        if(!t.fxReady||t.effect!=controls.effect){t.fx.reset(controls.effect);t.dac.reset();t.fxReady=true;t.effect=controls.effect;}
        if(controls.effect!=board::Effect::off){t.fx.processClassicFx(block,controls);t.dac.process(block,static_cast<size_t>(b.samples));}
        else t.dac.reset();
        for(int i=0;i<b.samples;++i)check(bits(block[static_cast<size_t>(i)],b.filterOutput[i]),"manual envelope feeds exactly one continuous FX/post-DAC chain");
#endif
        t.native.insert(t.native.end(),b.filterOutput,b.filterOutput+b.samples);
    }
#if !defined(SWARAXT_MANUAL_SWITCH_BASELINE)
    static void dualReset(Capture& t){t.dual.setRouting(t.expected.routing);t.dual.setMode1(t.expected.mode1);t.dual.setMode2(t.expected.mode2);t.dual.setCalibration(t.expected.calibration);t.dual.setSection2FollowsSection1(t.expected.control2==ShruthiSvfSection2Control::legacyFollow);t.dual.reset();t.delay.fill(0);t.cursor=0;}
#endif
};
std::vector<float> stale(bool change,const std::filesystem::path& out){
    auto p=std::make_unique<SwaraXtAudioProcessor>();setup(*p);p->prepareToPlay(48000,64);
    juce::AudioBuffer<float> b(2,64);juce::MidiBuffer m;m.ensureSize(4096);
    const auto run=[&](int n){for(int i=0;i<n;++i)process(*p,b,m);};
    m.addEvent(juce::MidiMessage::noteOn(1,48,static_cast<juce::uint8>(110)),0);run(64);
    const auto vca=p->engineForTests().filter().instrumentVcaControl();p->setExperimentalSvfEnabled(true);m.addEvent(juce::MidiMessage::noteOff(1,48),0);run(288);
    check(p->engineForTests().shruthiPart().voice().amplitude_envelope_dead(),"stale return starts after voice death");
    if(change)p->setExperimentalSvfEnabled(false);
    Capture capture;capture.engine=&p->engineForTests();p->engineForTests().setDebugTapSink(&capture,Capture::tap);
    for(int i=0;i<32;++i){process(*p,b,m);capture.host.insert(capture.host.end(),b.getReadPointer(0),b.getReadPointer(0)+64);}
    p->engineForTests().setDebugTapSink(nullptr,nullptr);
    std::printf("Stale return=%d frozen-start-VCA %.9g peak %.9g RMS %.9g\n",change?1:0,vca,peak(capture.host),rms(capture.host));
    if(change&&!out.empty()){std::ofstream file(out/"stale-return.f32",std::ios::binary);file.write(reinterpret_cast<const char*>(capture.host.data()),static_cast<std::streamsize>(capture.host.size()*sizeof(float)));check(file.good(),"external stale evidence written");}
    return capture.host;
}
void staleContract(const std::filesystem::path& out){const auto a=stale(false,{});drainMessages();const auto b=stale(true,out);check(a.size()==b.size()&&std::memcmp(a.data(),b.data(),a.size()*sizeof(float))==0,"dead-voice return matches untouched Dual/DC-tail control exactly");}

#if !defined(SWARAXT_MANUAL_SWITCH_BASELINE)
void processorContracts(){
    int cases=0;
    for(bool initial:{false,true})for(int route=0;route<3;++route)for(int cv=0;cv<3;++cv)for(int fx:{0,3,6,7,8}){
        auto p=std::make_unique<SwaraXtAudioProcessor>();setup(*p,fx,1.f);p->setExperimentalSvfEnabled(initial);p->setExperimentalSvfRouting(static_cast<ShruthiSvfDualRouting>(route));
        p->setExperimentalSvfControl2(static_cast<ShruthiSvfSection2Control>(cv));p->setExperimentalSvfCutoff2(39);p->setExperimentalSvfResonance2(51);p->prepareToPlay(48000,32);
        Capture t;t.engine=&p->engineForTests();t.expected=selection(true);t.expected.routing=static_cast<ShruthiSvfDualRouting>(route);t.expected.control2=static_cast<ShruthiSvfSection2Control>(cv);t.expected.cutoff2=39;t.expected.resonance2=51;
        p->engineForTests().setDebugTapSink(&t,Capture::tap);
        juce::AudioBuffer<float> b(2,32);juce::MidiBuffer m;m.ensureSize(4096);
        for(int pos=0;pos<12288;pos+=32){
            if(pos==0)m.addEvent(juce::MidiMessage::noteOn(1,48,static_cast<juce::uint8>(110)),0);
            if(pos==2048)p->setExperimentalSvfEnabled(!initial);
            if(pos==2400)m.addEvent(juce::MidiMessage::noteOn(1,60,static_cast<juce::uint8>(80)),0);
            if(pos==6144)m.addEvent(juce::MidiMessage::allNotesOff(1),0);
            if(pos==8192)p->setExperimentalSvfEnabled(initial);
            process(*p,b,m);if(Access::phase(p->engineForTests()))check(!p->engineForTests().dormantForTests(),"active switch cannot enter dormant drainage");
        }
        p->engineForTests().setDebugTapSink(nullptr,nullptr);
        check(Access::phase(p->engineForTests())==0&&Access::dual(p->engineForTests())==initial,"real processor route/control/FX round trip completes");
        check(peak(t.native)>1.e-4,"processor switching cases contain audible content");
        ++cases;p.reset();drainMessages();
    }
    std::printf("Processor round trips: %d audible route/control/FX cases.\n",cases);
}
std::vector<float> partition(double rate,int size){
    auto p=std::make_unique<SwaraXtAudioProcessor>();setup(*p,8);p->prepareToPlay(rate,size);std::vector<float> output;output.reserve(16384);
    juce::AudioBuffer<float> b(2,size);juce::MidiBuffer m;m.ensureSize(4096);
    for(int pos=0;pos<16384;pos+=size){if(pos==0)m.addEvent(juce::MidiMessage::noteOn(1,48,static_cast<juce::uint8>(110)),0);if(pos==4096)p->setExperimentalSvfEnabled(true);if(pos==8192)m.addEvent(juce::MidiMessage::noteOff(1,48),0);if(pos==12288)p->setExperimentalSvfEnabled(false);process(*p,b,m);output.insert(output.end(),b.getReadPointer(0),b.getReadPointer(0)+size);}
    check(p->getLatencySamples()==static_cast<int>(std::ceil(384*rate/SwaraXtEngine::kInternalSampleRate)),"manual switch preserves fixed host PDC");return output;
}
void partitionsAndDormancy(){
    for(double rate:{44100.,48000.,96000.,192000.}){const auto reference=partition(rate,1);drainMessages();for(int block:{16,32,64,128}){const auto a=partition(rate,block);check(a.size()==reference.size()&&std::memcmp(a.data(),reference.data(),a.size()*sizeof(float))==0,"event-identical manual switches are independent of host block partition");drainMessages();}}
    auto p=std::make_unique<SwaraXtAudioProcessor>();setup(*p);p->prepareToPlay(48000,32);juce::AudioBuffer<float> b(2,32);juce::MidiBuffer m;m.ensureSize(4096);
    check(p->engineForTests().dormantForTests(),"dormant fixture begins genuinely asleep");
    for(bool dual:{true,false,true}){p->setExperimentalSvfEnabled(dual);p->setFilterQuality(Q::high);for(int n=0;n<4;++n)process(*p,b,m);check(p->engineForTests().dormantForTests()&&Access::phase(p->engineForTests())==0&&Access::dual(p->engineForTests())==dual,"dormant control path switches without fade or audible priming");check(peak(std::vector<float>(b.getReadPointer(0),b.getReadPointer(0)+32))==0,"dormant switch remains silent");}
    m.addEvent(juce::MidiMessage::noteOn(1,48,static_cast<juce::uint8>(110)),17);process(*p,b,m);p->setExperimentalSvfEnabled(false);
    for(int n=0;n<100;++n)process(*p,b,m);check(!p->engineForTests().dormantForTests()&&Access::phase(p->engineForTests())==0&&!Access::dual(p->engineForTests()),"wake followed by manual request completes normally");
    p->setExperimentalSvfEnabled(true);for(int i=0;i<4;++i)process(*p,b,m);check(Access::phase(p->engineForTests())!=0,"restore fixture reaches in-flight switch");
    juce::MemoryBlock state;p->getStateInformation(state);p->setStateInformation(state.getData(),static_cast<int>(state.getSize()));process(*p,b,m);check(Access::phase(p->engineForTests())==0&&bits(Access::gain(p->engineForTests()),1.f),"host restore collapses manual operation on audio reset");
    p->setExperimentalSvfEnabled(false);for(int i=0;i<4;++i)process(*p,b,m);p->releaseResources();p->prepareToPlay(96000,16);check(Access::phase(p->engineForTests())==0,"resource release then reprepare cannot retain partial operation");
}
void requestDuringDcDrain(){
    auto p=std::make_unique<SwaraXtAudioProcessor>();auto control=std::make_unique<SwaraXtAudioProcessor>();
    for(auto* a:{p.get(),control.get()}){setup(*a);a->prepareToPlay(48000,32);Access::dcDrain(a->engineForTests());}
    juce::AudioBuffer<float> x(2,32),y(2,32);juce::MidiBuffer mx,my;mx.ensureSize(4096);my.ensureSize(4096);
    p->setExperimentalSvfEnabled(true);process(*p,x,mx);process(*control,y,my);
    check(Access::phase(p->engineForTests())!=0&&p->engineForTests().debugInternalBlockCount()>0,"manual request during DC drainage resumes native switch work before dormancy");
    for(int n=0;n<64;++n){
        check(std::memcmp(x.getReadPointer(0),y.getReadPointer(0),32*sizeof(float))==0,"silent manual request preserves existing host DC drainage samples");
        process(*p,x,mx);process(*control,y,my);
    }
    check(Access::dual(p->engineForTests())&&Access::phase(p->engineForTests())==0,"DC-drain request completes without waiting for DC settlement");
}
void cancelledRequestDuringSrcDrain(){
    auto p=std::make_unique<SwaraXtAudioProcessor>();auto control=std::make_unique<SwaraXtAudioProcessor>();
    for(auto* a:{p.get(),control.get()}){setup(*a);a->prepareToPlay(48000,64);Access::queuedSrcDrain(a->engineForTests());}
    juce::AudioBuffer<float> first(2,1),twinFirst(2,1),x(2,64),y(2,64);juce::MidiBuffer mx,my;mx.ensureSize(4096);my.ensureSize(4096);
    p->setExperimentalSvfEnabled(true);process(*p,first,mx);process(*control,twinFirst,my);
    check(bits(first.getSample(0,0),twinFirst.getSample(0,0))&&Access::phase(p->engineForTests())==0,"queued SRC output precedes native bank admission");
    check(p->engineForTests().drainingForTests(),"unneeded native input cannot resume firmware before admission");
    p->setExperimentalSvfEnabled(false);bool identical=true;
    for(int n=0;n<32;++n){process(*p,x,mx);process(*control,y,my);identical&=std::memcmp(x.getReadPointer(0),y.getReadPointer(0),64*sizeof(float))==0;}
    check(identical,"cancelled pre-admission request preserves queued SRC and DC output exactly");
    mx.addEvent(juce::MidiMessage::noteOn(1,48,static_cast<juce::uint8>(110)),17);my.addEvent(juce::MidiMessage::noteOn(1,48,static_cast<juce::uint8>(110)),17);
    float maximum=0;identical=true;
    for(int n=0;n<64;++n){process(*p,x,mx);process(*control,y,my);identical&=std::memcmp(x.getReadPointer(0),y.getReadPointer(0),64*sizeof(float))==0;for(int i=0;i<64;++i)maximum=std::max(maximum,std::abs(x.getSample(0,i)));}
    check(identical&&maximum>1.e-4f,"cancelled pre-admission request preserves the following audible NoteOn bit-for-bit");
}
void longStatesAndFxChanges(){
    for(bool initial:{false,true})for(bool release:{false,true}){
        auto p=std::make_unique<SwaraXtAudioProcessor>();setup(*p,7,1.f);p->setExperimentalSvfEnabled(initial);p->prepareToPlay(48000,64);
        juce::AudioBuffer<float> b(2,64);juce::MidiBuffer m;m.ensureSize(4096);m.addEvent(juce::MidiMessage::noteOn(1,48,static_cast<juce::uint8>(110)),0);
        for(int n=0;n<2250;++n)process(*p,b,m); // Three seconds of high-resonance state before the manual request.
        p->setExperimentalSvfEnabled(!initial);bool preparing=false,finished=false;float maximum=0;
        for(int n=0;n<128;++n){
            if(release&&n==4)m.addEvent(juce::MidiMessage::noteOff(1,48),0);
            process(*p,b,m);preparing|=Access::phase(p->engineForTests())==2;
            finished|=preparing&&Access::phase(p->engineForTests())==0;
            for(int i=0;i<64;++i){check(std::isfinite(b.getSample(0,i)),"long self-oscillating and feedback-tail switch remains finite");maximum=std::max(maximum,std::abs(b.getSample(0,i)));}
        }
        check(preparing&&finished&&Access::dual(p->engineForTests())!=initial,"long-state switch completes without historical phase reconstruction");
        std::printf("Long state initialDual=%d release=%d hostPeak=%.9g.\n",initial?1:0,release?1:0,maximum);p.reset();drainMessages();
    }
    auto p=std::make_unique<SwaraXtAudioProcessor>();setup(*p,6);p->prepareToPlay(48000,32);
    Capture t;t.engine=&p->engineForTests();p->engineForTests().setDebugTapSink(&t,Capture::tap);
    juce::AudioBuffer<float> b(2,32);juce::MidiBuffer m;m.ensureSize(4096);
    for(int pos=0;pos<8192;pos+=32){
        if(pos==0)m.addEvent(juce::MidiMessage::noteOn(1,48,static_cast<juce::uint8>(110)),0);
        if(pos==2048)p->setExperimentalSvfEnabled(true);
        if(pos==2176)set(*p,IDs::dspFxProgram,7);
        if(pos==6144)p->setExperimentalSvfEnabled(false);
        if(pos==6272)set(*p,IDs::dspFxProgram,0);
        process(*p,b,m);
    }
    p->engineForTests().setDebugTapSink(nullptr,nullptr);check(Access::phase(p->engineForTests())==0,"manual switching and existing board-FX program fades finish independently");
}
#endif
}
int main(int argc,char** argv){
    juce::ScopedJuceInitialiser_GUI init;std::setvbuf(stdout,nullptr,_IONBF,0);
#if defined(_MSC_VER)&&defined(_DEBUG)
    const auto previous=_CrtSetAllocHook(heapHook);
#endif
    const std::string mode=argc>1?argv[1]:"contracts";const std::filesystem::path out=argc>2?argv[2]:"";if(!out.empty())std::filesystem::create_directories(out);
    if(mode=="stale")staleContract(out);
    else if(mode=="contracts"){
#if !defined(SWARAXT_MANUAL_SWITCH_BASELINE)
        exactCountsAndCancellation();qualityAndLifecycle();fifoAndRequests();processorContracts();partitionsAndDormancy();requestDuringDcDrain();cancelledRequestDuringSrcDrain();longStatesAndFxChanges();
#endif
        staleContract(out);
    }
#if !defined(SWARAXT_MANUAL_SWITCH_BASELINE)
    else if(mode=="drain")requestDuringDcDrain();
#endif
    else return 2;
    drainMessages();check(heapOperations==0,"no callback allocation, reallocation or free");
#if defined(_MSC_VER)&&defined(_DEBUG)
    _CrtSetAllocHook(previous);
#endif
    std::printf("Manual filter switching: %d failures; callback heap operations %u.\n",failures,heapOperations);return failures?1:0;
}
