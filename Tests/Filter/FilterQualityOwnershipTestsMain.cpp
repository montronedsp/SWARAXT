// Copyright 2026 MontroneDSP. SPDX-License-Identifier: GPL-3.0-or-later
#include <JuceHeader.h>
#include "Plugin/PluginProcessor.h"
#include "Plugin/SwaraXtParameterLayout.h"
#include <algorithm>
#include <array>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <numeric>
#include <vector>
#if defined(_MSC_VER) && defined(_DEBUG)
#include <crtdbg.h>
#endif

namespace swaraxt {
struct FilterQualityTestAccess {
    struct State { int phase, remaining; FilterQuality applied, pending, actual; float gain; bool dual, admission; };
    static State state(const SwaraXtEngine& e) {
        return {static_cast<int>(e.qualityFadePhase_),e.qualitySamplesRemaining_,e.appliedFilterQuality_,
                e.pendingFilterQuality_,e.filter_.quality(),e.qualityGain_,e.experimentalSvfEnabled_,
#if defined(SWARAXT_QUALITY_ORIGINAL)
                false
#else
                e.qualityBankAdmission_
#endif
        };
    }
    static void boundary(SwaraXtEngine& e) {
#if defined(SWARAXT_QUALITY_ORIGINAL)
        e.applyPendingFilterQuality();e.applyExperimentalSvfSelection();
#else
        const auto next=e.experimentalSvfControl_.selection();
        e.applyPendingFilterQuality(next.enabled!=e.experimentalSvfEnabled_);e.applyExperimentalSvfSelection(next);
#endif
    }
    static float gain(SwaraXtEngine& e){return e.nextQualityGain();}
    static int ramp(const SwaraXtEngine& e){return e.qualityRampSamples_;}
    static void sleep(SwaraXtEngine& e){e.enterDormant();}
    static auto& delay(SwaraXtEngine& e){return e.dualOutputDelay_;}
    static void applyCaptured(SwaraXtEngine& e,const ExperimentalSvfSelection& s){
#if defined(SWARAXT_QUALITY_ORIGINAL)
        (void)s;e.applyExperimentalSvfSelection();
#else
        e.applyExperimentalSvfSelection(s);
#endif
    }
};
}
namespace {
using namespace swaraxt;
using Access=FilterQualityTestAccess;
using Q=FilterQuality;
using Clock=std::chrono::steady_clock;
int failures=0;
thread_local bool audioScope=false;
thread_local uint64_t audioAllocations=0;
#if defined(_MSC_VER) && defined(_DEBUG)
int __cdecl heapAudit(int kind,void*,size_t,int,long,const unsigned char*,int){if(audioScope && kind==_HOOK_ALLOC)++audioAllocations;return 1;}
#endif
void check(bool ok,const char* message){if(!ok){++failures;std::printf("FAIL: %s\n",message);}}
bool sameBits(float a,float b){return std::memcmp(&a,&b,sizeof(a))==0;}
bool unity(float gain){return sameBits(gain,1.f);}
bool sameBits(const std::vector<float>& a,const std::vector<float>& b){return a.size()==b.size() && (a.empty()||std::memcmp(a.data(),b.data(),a.size()*sizeof(float))==0);}
void set(SwaraXtAudioProcessor& p,const char* id,float value){auto* v=p.getApvts().getParameter(id);check(v!=nullptr,id);if(v)v->setValueNotifyingHost(v->convertTo0to1(value));}
void neutral(SwaraXtAudioProcessor& p,int fx){
    set(p,IDs::osc1Shape,1);set(p,IDs::osc1Param,0);set(p,IDs::osc1Range,0);set(p,IDs::osc2Shape,0);
    set(p,IDs::mixBalance,0);set(p,IDs::mixSub,0);set(p,IDs::mixNoise,0);set(p,IDs::filterCutoff,10000);
    set(p,IDs::filterResonance,1);set(p,IDs::filterKeyTracking,.5f);set(p,IDs::filterEnvDepth,0);set(p,IDs::filterLfoDepth,0);
    set(p,IDs::perfGlide,0);set(p,IDs::postMixer,0);set(p,IDs::master,.5f);set(p,IDs::seqMode,0);
    set(p,IDs::dspFxProgram,static_cast<float>(fx));set(p,IDs::dspFxParam1,64);set(p,IDs::dspFxParam2,32);
    for(const auto* id:{IDs::env1Attack,IDs::env1Decay,IDs::env1Release,IDs::env2Attack,IDs::env2Decay,IDs::env2Release})set(p,id,0);
    set(p,IDs::env1Sustain,127);set(p,IDs::env2Sustain,127);
    for(int row=1;row<=12;++row){const auto id="mod.row"+std::to_string(row)+".amount";set(p,id.c_str(),row==9?63.f:0.f);}
}
void process(SwaraXtAudioProcessor& p,juce::AudioBuffer<float>& audio,juce::MidiBuffer& midi){
    audioScope=true;p.processBlock(audio,midi);audioScope=false;midi.clear();
}

// Gain-only reference preserves the original controller's retarget/restart
// arithmetic. Audio comparisons additionally run the original engine source.
struct LegacyQuality {
    Q requested=Q::normal,pending=Q::normal,applied=Q::normal;
    int phase=0,remaining=0,ramp=78;float gain=1,increment=0;
    void fadeOut(){phase=1;remaining=ramp;increment=(0.f-gain)/static_cast<float>(remaining);}
    void boundary(){pending=requested;if(pending==applied && phase==0)return;if(pending!=applied && phase!=1)fadeOut();}
    float next(){
        if(remaining<=0)return gain;gain+=increment;if(--remaining>0)return gain;
        if(phase==1){gain=0;applied=pending;phase=2;remaining=ramp;increment=(1.f-gain)/static_cast<float>(remaining);}
        else if(phase==2){gain=1;phase=0;increment=0;if(pending!=applied)fadeOut();}
        return gain;
    }
};
void classicTiming(){
    for(auto initial:{Q::eco,Q::normal,Q::high})for(int pattern=0;pattern<4;++pattern){
        auto e=std::make_unique<SwaraXtEngine>();e->setFilterQuality(initial);e->prepare(48000,64);
        LegacyQuality old;old.requested=old.pending=old.applied=initial;old.ramp=Access::ramp(*e);
        check(old.ramp==78,"quality ramp remains 78 native samples");
        for(int n=0;n<1600;++n){
            if(n%40==0){
                if(n>=80 && n<720 && (pattern==3 || n%((pattern+1)*80)==0))old.requested=static_cast<Q>((n/40+pattern)%3);
                e->setFilterQuality(old.requested);old.boundary();Access::boundary(*e);
            }
            const auto a=Access::gain(*e),b=old.next();const auto s=Access::state(*e);
            check(sameBits(a,b)&&s.phase==old.phase&&s.remaining==old.remaining&&s.pending==old.pending&&s.applied==old.applied&&s.actual==old.applied,
                  "Classic-only sample timing and quality match the legacy controller");
        }
    }
}

struct Render { std::vector<float> audio;std::vector<double> times;std::vector<uint32_t> trace;uint64_t allocations=0; };
Render render(double rate,int block,bool dual,int route,int fx,Q initial,int pattern,bool release,int count=8192){
    auto p=std::make_unique<SwaraXtAudioProcessor>();neutral(*p,fx);p->setFilterQuality(initial);
    p->setExperimentalSvfEnabled(dual);p->setExperimentalSvfRouting(static_cast<ShruthiSvfDualRouting>(route));p->prepareToPlay(rate,block);
    Render out;out.audio.reserve(static_cast<size_t>(count));out.times.reserve(static_cast<size_t>(count/block+1));out.trace.reserve(static_cast<size_t>(count/block+1)*8);
    bool changedOut=false,changedIn=false;
    juce::AudioBuffer<float> audio(2,block);juce::MidiBuffer midi;midi.ensureSize(4096);
    for(int pos=0;pos<count;pos+=block){
        if(pos==0)midi.addEvent(juce::MidiMessage::noteOn(1,48,static_cast<juce::uint8>(110)),0);
        if(release && pos==4096)midi.addEvent(juce::MidiMessage::noteOff(1,48),0);
        if(pattern==1 && pos>=1024 && pos<=6144 && pos%512==0)
            p->setFilterQuality(static_cast<Q>((static_cast<int>(initial)+5-((pos-1024)/512)%3)%3));
        if(pattern==2 && pos>=1024 && pos<3072 && pos%64==0)p->setFilterQuality(static_cast<Q>((pos/64)%3));
        if(pattern==3){
            if(pos==1024)p->setFilterQuality(static_cast<Q>((static_cast<int>(initial)+1)%3));
            const auto s=Access::state(p->engineForTests());
            if(s.phase==1 && !changedOut){p->setFilterQuality(Q::high);changedOut=true;}
            if(s.phase==2 && !changedIn){p->setFilterQuality(Q::eco);changedIn=true;}
        }
        audioScope=true;const auto start=Clock::now();p->processBlock(audio,midi);const auto end=Clock::now();audioScope=false;midi.clear();
        out.times.push_back(std::chrono::duration<double,std::micro>(end-start).count());
        const int take=std::min(block,count-pos);out.audio.insert(out.audio.end(),audio.getReadPointer(0),audio.getReadPointer(0)+take);
        const auto s=Access::state(p->engineForTests());uint32_t gain=0;std::memcpy(&gain,&s.gain,sizeof(gain));
        for(auto v:{static_cast<uint32_t>(s.phase),static_cast<uint32_t>(s.remaining),static_cast<uint32_t>(s.applied),static_cast<uint32_t>(s.pending),static_cast<uint32_t>(s.actual),gain,static_cast<uint32_t>(s.dual),p->engineForTests().debugInternalBlockCount()})out.trace.push_back(v);
        check(std::all_of(audio.getReadPointer(0),audio.getReadPointer(0)+take,[](float x){return std::isfinite(x);}),"processor output finite");
#if !defined(SWARAXT_QUALITY_ORIGINAL)
        if(dual && pattern!=0){check(s.phase==0&&unity(s.gain),"inactive Classic quality cannot own shared gain");}
#endif
    }
    out.allocations=audioAllocations;audioAllocations=0;check(out.allocations==0,"quality operations allocate no callback memory");
    check(p->getLatencySamples()==static_cast<int>(std::ceil(384*rate/SwaraXtEngine::kInternalSampleRate)),"host latency remains 384 native samples");
    return out;
}
void dualIndependence(bool one=false){
    for(int route=0;route<(one?1:3);++route)for(int fx:{0,8})for(auto q:{Q::normal,Q::high,Q::eco})for(bool release:{false,true}){
        const auto a=render(48000,32,true,route,fx,q,0,release),b=render(48000,32,true,route,fx,q,1,release);
        check(std::any_of(a.audio.begin(),a.audio.end(),[](float x){return std::abs(x)>1.e-4f;}),"Dual independence fixture contains audible signal");
        check(sameBits(a.audio,b.audio),"all Dual samples remain byte-identical under inactive Classic requests");
        if(one)return;
    }
}
void bankConflicts(){
    {
        auto e=std::make_unique<SwaraXtEngine>();e->prepare(48000,64);
        e->setExperimentalSvfEnabledForTests(true);e->setFilterQuality(Q::high);Access::boundary(*e);
        const auto s=Access::state(*e);
        check(s.dual&&s.phase==0&&unity(s.gain)&&s.actual==Q::normal&&e->filterQuality()==Q::high,
              "bank request at unity wins over starting a new Classic quality cycle");
    }
    for(int elapsed:{40,100})for(bool reverse:{false,true}){
        auto e=std::make_unique<SwaraXtEngine>();e->prepare(48000,64);e->setFilterQuality(Q::eco);Access::boundary(*e);
        for(int n=0;n<elapsed;++n)Access::gain(*e);
        e->setExperimentalSvfEnabledForTests(true);e->setFilterQuality(Q::high);Access::boundary(*e);
        check(!Access::state(*e).dual,"bank waits for an active quality cycle");
        int admitted=-1;
        for(int n=0;n<400;++n){
            if(n%40==0){e->setFilterQuality(n%80?Q::high:Q::normal);e->setExperimentalSvfEnabledForTests(!(reverse && n<80));Access::boundary(*e);}
            const auto s=Access::state(*e);const auto gain=Access::gain(*e);
            if(s.dual){if(admitted<0)admitted=n;check(s.phase==0&&unity(gain),"Dual admission never inherits a quality fade");}
        }
        check(admitted>=0&&admitted<=160,"quality flood cannot starve bank admission");
        auto& delay=Access::delay(*e);delay.process(.5f);const auto samples=delay.samples;const auto cursor=delay.cursor,pending=delay.pending;
        e->setFilterQuality(Q::eco);Access::boundary(*e);
        check(delay.samples==samples&&delay.cursor==cursor&&delay.pending==pending,"quality request leaves Dual FIFO untouched");
        e->setFilterQuality(Q::high);e->setExperimentalSvfEnabledForTests(false);Access::boundary(*e);
        const auto s=Access::state(*e);check(!s.dual&&s.phase==0&&unity(s.gain)&&s.actual==Q::high&&s.pending==Q::high,"Classic admission applies latest deferred quality at unity");
        for(int n=0;n<240;++n){if(n%40==0)Access::boundary(*e);check(unity(Access::gain(*e)),"no obsolete fade restarts after Classic admission");}
    }
    auto e=std::make_unique<SwaraXtEngine>();e->prepare(48000,64);const auto captured=e->experimentalSvfSelection();
    e->setExperimentalSvfEnabledForTests(true);Access::applyCaptured(*e,captured);
    check(!Access::state(*e).dual,"activation uses exactly the admitted selection snapshot");Access::boundary(*e);check(Access::state(*e).dual,"later coherent request is admitted at next boundary");
    {
        auto canceled=std::make_unique<SwaraXtEngine>();canceled->prepare(48000,64);
        canceled->setFilterQuality(Q::eco);Access::boundary(*canceled);Access::gain(*canceled);
        canceled->setExperimentalSvfEnabledForTests(true);Access::boundary(*canceled);
        canceled->setExperimentalSvfEnabledForTests(false);canceled->setFilterQuality(Q::high);
        for(int n=0;n<480;++n){if(n%40==0)Access::boundary(*canceled);Access::gain(*canceled);check(!Access::state(*canceled).dual,"canceled bank request never activates later");}
        const auto s=Access::state(*canceled);check(s.phase==0&&unity(s.gain)&&s.actual==Q::high&&!s.admission,"reversal returns ownership to the latest Classic request");
    }
}
void resetAndIdle(){
    for(int operation=0;operation<3;++operation){
        auto e=std::make_unique<SwaraXtEngine>();e->prepare(48000,64);e->reset();e->setFilterQuality(Q::eco);Access::boundary(*e);Access::gain(*e);
        e->setExperimentalSvfEnabledForTests(true);Access::boundary(*e);e->setFilterQuality(Q::high);
        if(operation==0)e->reset();else if(operation==1)e->prepare(96000,128);else Access::sleep(*e);
        const auto s=Access::state(*e);check(s.phase==0&&unity(s.gain)&&s.actual==Q::high&&s.dual&&!s.admission,"hard lifecycle boundary snaps latest requests and releases admission");
    }
    auto p=std::make_unique<SwaraXtAudioProcessor>();neutral(*p,0);p->setExperimentalSvfEnabled(true);p->prepareToPlay(48000,32);
    juce::AudioBuffer<float> audio(2,32);juce::MidiBuffer midi;
    p->setFilterQuality(Q::high);p->setExperimentalSvfEnabled(false);
    for(int n=0;n<200;++n)process(*p,audio,midi);
    auto s=Access::state(p->engineForTests());check(!s.dual&&s.actual==Q::high&&unity(s.gain),"dormant bank admission retains deferred quality");
    midi.addEvent(juce::MidiMessage::noteOn(1,60,static_cast<juce::uint8>(100)),17);for(int n=0;n<40;++n)process(*p,audio,midi);
    check(!p->engineForTests().dormantForTests()&&unity(Access::state(p->engineForTests()).gain),"wake starts with coherent unity quality ownership");
    p->setFilterQuality(Q::eco);for(int n=0;n<4;++n)process(*p,audio,midi);p->setExperimentalSvfEnabled(true);
    juce::MemoryBlock state;p->getStateInformation(state);p->setStateInformation(state.getData(),static_cast<int>(state.getSize()));process(*p,audio,midi);
    s=Access::state(p->engineForTests());check(s.dual&&s.phase==0&&unity(s.gain)&&s.actual==Q::eco,"host restore cancels old quality ownership on audio reset");
    p->setExperimentalSvfEnabled(false);process(*p,audio,midi);p->setFilterQuality(Q::high);
    for(int n=0;n<4;++n)process(*p,audio,midi);
    p->setExperimentalSvfEnabled(true);p->setFilterQuality(Q::normal);p->prepareToPlay(96000,128);
    s=Access::state(p->engineForTests());check(s.dual&&s.phase==0&&unity(s.gain)&&s.actual==Q::normal&&!s.admission,"processor reprepare and resampler reset snap pending ownership");
    p->releaseResources();p->setExperimentalSvfEnabled(false);p->setFilterQuality(Q::high);p->prepareToPlay(48000,32);
    s=Access::state(p->engineForTests());check(!s.dual&&s.phase==0&&unity(s.gain)&&s.actual==Q::high,"resource release and prepare retain the latest requests");
}
template<class T>void binary(const std::filesystem::path& path,const std::vector<T>& data){std::ofstream stream(path,std::ios::binary);stream.write(reinterpret_cast<const char*>(data.data()),static_cast<std::streamsize>(data.size()*sizeof(T)));stream.flush();check(stream.good(),"measurement file written");}
void exportClassic(const std::filesystem::path& out){
    int cases=0;
    for(double rate:{44100.,48000.,96000.,192000.})for(int block:{1,32,128})for(auto q:{Q::normal,Q::high,Q::eco})for(int pattern=0;pattern<4;++pattern)for(int fx:{0,8}){
        auto a=render(rate,block,false,0,fx,q,pattern,true,8192);
        const auto name=std::to_string(static_cast<int>(rate))+"-"+std::to_string(block)+"-"+std::to_string(static_cast<int>(q))+"-"+std::to_string(pattern)+"-"+std::to_string(fx);
        binary(out/(name+".f32"),a.audio);binary(out/(name+".state"),a.trace);++cases;
    }
    std::printf("Classic audio/state exports: %d cases.\n",cases);
}
void benchmark(const std::filesystem::path& out){
    std::ofstream csv(out/"cpu.csv");csv<<"trial,rate,block,dual,quality,changes,window,count,max_us,p99_us,mean_us,deadline_us\n";
    for(int trial=0;trial<3;++trial)for(double rate:{44100.,48000.,96000.,192000.})for(int block:{64,128,256})for(bool dual:{false,true})for(auto q:{Q::normal,Q::high})for(int changes:{0,1}){
        const auto a=render(rate,block,dual,1,8,q,changes,false,static_cast<int>(rate/2));
        for(int window=0;window<3;++window){
            if(window==2 && (dual || !changes))continue;
            std::vector<double> times;
            for(size_t i=0;i<a.times.size();++i){
                const bool native=a.trace[i*8+7]!=(i?a.trace[(i-1)*8+7]:0);
                const bool cycle=a.trace[i*8]!=0 || (i && (a.trace[(i-1)*8]!=0 || a.trace[(i-1)*8+2]!=a.trace[i*8+2]));
                if(window==0 || (window==1 && native) || (window==2 && cycle))times.push_back(a.times[i]);
            }
            if(times.empty())continue;std::sort(times.begin(),times.end());
            csv<<trial<<','<<rate<<','<<block<<','<<dual<<','<<static_cast<int>(q)<<','<<changes<<','<<window<<','<<times.size()<<','<<times.back()<<','<<times[times.size()*99/100]<<','<<std::accumulate(times.begin(),times.end(),0.)/static_cast<double>(times.size())<<','<<block/rate*1e6<<'\n';
        }
    }
    csv.flush();check(csv.good(),"benchmark CSV written");
}
void stress(const std::filesystem::path& out){
    std::ofstream csv(out/"stress.csv");csv<<"rate,block,dual,max_us,p99_us,deadline_us\n";
    for(double rate:{44100.,48000.,96000.,192000.,384000.})for(int block:{1,16,32,64,128,256})for(bool dual:{false,true}){
        auto a=render(rate,block,dual,2,8,Q::normal,2,true,8192);check(!a.audio.empty(),"bounded stress render completes");
        std::sort(a.times.begin(),a.times.end());csv<<rate<<','<<block<<','<<dual<<','<<a.times.back()<<','<<a.times[a.times.size()*99/100]<<','<<block/rate*1e6<<'\n';
    }
    csv.flush();check(csv.good(),"stress callback measurements written");
}
}
int main(int argc,char** argv){
    juce::ScopedJuceInitialiser_GUI init;std::setvbuf(stdout,nullptr,_IONBF,0);
#if defined(_MSC_VER) && defined(_DEBUG)
    const auto oldHook=_CrtSetAllocHook(heapAudit);
#endif
    const std::string mode=argc>1?argv[1]:"contracts";
    if(mode=="contracts"){classicTiming();dualIndependence();bankConflicts();resetAndIdle();}
    else if(mode=="reproduce")dualIndependence(true);
    else if((mode=="export"||mode=="bench"||mode=="stress")&&argc==3){std::filesystem::path out=argv[2];std::filesystem::create_directories(out);if(mode=="export")exportClassic(out);else if(mode=="stress")stress(out);else benchmark(out);}
    else return 2;
#if defined(_MSC_VER) && defined(_DEBUG)
    _CrtSetAllocHook(oldHook);
#endif
    std::printf("Quality ownership: %d failures.\n",failures);return failures?1:0;
}
