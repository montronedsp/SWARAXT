// Copyright 2026 MontroneDSP. SPDX-License-Identifier: GPL-3.0-or-later
#include <JuceHeader.h>
#include "Plugin/PluginProcessor.h"
#include "Plugin/PluginEditor.h"
#include "Plugin/ShruthiFactoryPresets.h"
#include "Plugin/ShruthiFactoryPresetData.h"
#include "Engine/ParameterCache.h"
#include "Engine/PatchBridge.h"
#include "Ui/SwaraXtModulationNames.h"
#include "shruthi/resources.h"
#include "Tuning/TuningTable.h"
#include "shruthi/patch.h"
#include <algorithm>
#include <array>
#include <cmath>
#include <cstdio>
#include <thread>
#include <map>

namespace swaraxt {
struct SequencerMidiTestAccess {
    static void tap(swaraxt::SwaraXtEngine& e, shruthi::MidiDispatcher::SequencerNoteTap tap) { e.midiDispatcher_.setSequencerNoteTap(tap); }
};
}
namespace {
using P=SwaraXtAudioProcessor;
int failures=0,checks=0;
void check(bool ok,const char* what){++checks;if(!ok){++failures;std::printf("FAIL: %s\n",what);}}
void set(P& p,const char* id,float value){auto* a=p.getApvts().getParameter(id);check(a!=nullptr,id);if(a)a->setValueNotifyingHost(a->convertTo0to1(value));}
float get(P& p,const char* id){return p.getApvts().getRawParameterValue(id)->load();}
void pump(){juce::MessageManager::getInstance()->runDispatchLoopUntil(1);}
void numeric(){
    using namespace swaraxt::tuning;
    TuningSnapshot t;ParseError error;
    for(const auto& value: {std::string("600,5"),std::string("600.5"),std::string("600,5")+std::string(400,'0')}){
        const auto text="Probe\n2\n"+value+"\n2/1\n";
        check(parseScalaAndKeyboard(text.c_str(),nullptr,t,error)&&std::abs(t.degreeCents[0]-600.5)<1.e-10,"complete finite decimal parsing");
    }
    for(const auto* value:{"6e2,5","600.1,5","600,,5","600,",",5","600junk","nan","inf","1e999"}){
        const auto text=std::string("Probe\n2\n")+value+"\n2/1\n";
        check(!parseScalaAndKeyboard(text.c_str(),nullptr,t,error),"malformed real token rejected");
    }
    const char* scl="Probe\n2\n600.0\n2/1\n";
    check(!parseScalaAndKeyboard(scl,"2\n60\n61\n60\n60\n-0,5\n2\n0\n1\n",t,error),"negative comma reference cannot become positive");
    check(parseScalaAndKeyboard(scl,"2\n60\n61\n60\n60\n440,5\n2\n0\n1\n",t,error)&&t.referenceHz==440.5,"legacy comma KBM reference positive control");
    check(parseScalaAndKeyboard("Integer ratio\n2\n3/2\n2\n",nullptr,t,error,ScalaSyntax::standard)&&std::abs(t.periodCents-1200)<1.e-9,"standard bare integer retains ratio semantics");
}
void text(){
    P p;auto* a=p.getApvts().getParameter(swaraxt::IDs::filterCutoff);
    for(const auto& pair:std::array<std::pair<const char*,float>,14>{{{"2000",2000},{"2000 Hz",2000},{"2k",2000},{"2 k",2000},{"2khz",2000},{"2 kHz",2000},{"2.5k",2500},{"2.5 kHz",2500},{"10",10},{"20000",20000},{" 2 KHZ ",2000},{"2e3",2000},{"+2k",2000},{"30000",20000}}}){
        const auto n=a->getValueForText(pair.first);const auto hz=a->convertFrom0to1(n);
        check(std::isfinite(n)&&n>=0&&n<=1&&std::abs(hz-pair.second)<.03f,"production cutoff physical/normalized value");
        const auto back=a->convertFrom0to1(a->getValueForText(a->getText(n,40)));
        check(std::abs(back-hz)<.03f,"cutoff display roundtrip");
    }
    for(const auto* invalid:{"2xyz","2.5.9k","2 000","2,5k","nan","NaN Hz","inf","1e999k","1e","2kk","--2k","+-2k",""}){
        check(a->convertFrom0to1(a->getValueForText(invalid))==10.0f,"invalid cutoff text has deterministic minimum fallback");
    }
}
struct Capture{std::array<float,2048> audio{};int dst=0;double rms=0,peak=0;};
Capture render(int dest,int level,int source,int amount,int rows){
    pump();P p;p.setFilterQuality(swaraxt::FilterQuality::eco);p.prepareToPlay(48000,128);
    set(p,swaraxt::IDs::dspFxProgram,0);set(p,swaraxt::IDs::filterCutoff,20000);set(p,swaraxt::IDs::seqMode,0);
    set(p,swaraxt::IDs::mixSub,0);set(p,swaraxt::IDs::mixNoise,0);
    set(p,dest==shruthi::MOD_DST_MIX_SUB_OSC?swaraxt::IDs::mixSub:swaraxt::IDs::mixNoise,float(level));
    for(int r=1;r<=12;++r){const auto prefix="mod.row"+juce::String(r);set(p,(prefix+".amount").toRawUTF8(),0);}
    for(int r=1;r<=rows;++r){const auto prefix="mod.row"+juce::String(r);set(p,(prefix+".source").toRawUTF8(),float(source));set(p,(prefix+".destination").toRawUTF8(),float(dest));set(p,(prefix+".amount").toRawUTF8(),float(amount));}
    p.engineForTests().reset();p.engineForTests().applyParameters();
    Capture c;juce::AudioBuffer<float>b(2,128);juce::MidiBuffer m;m.ensureSize(2048);
    for(int block=0;block<16;++block){m.clear();if(!block)m.addEvent(juce::MidiMessage::noteOn(1,60,juce::uint8(100)),0);p.processBlock(b,m);for(int i=0;i<128;++i){auto v=b.getSample(0,i);check(std::isfinite(v),"mixer finite actual processor audio");c.audio[size_t(block*128+i)]=v;c.peak=std::max(c.peak,double(std::abs(v)));c.rms+=double(v)*v;}}
    if(const char* folder=std::getenv("SWARA_CONTROL_CAPTURE_DIR")){
        const auto file=juce::File(juce::String(folder)).getChildFile(juce::String(dest)+"-"+juce::String(level)+"-"+juce::String(source)+"-"+juce::String(amount)+"-"+juce::String(rows)+".f32");
        check(file.replaceWithData(c.audio.data(),sizeof(c.audio)),"write private parity capture");
    }
    c.rms=std::sqrt(c.rms/c.audio.size());c.dst=p.engineForTests().shruthiPart().voice().debug_destination14(uint8_t(dest));return c;
}
void mixer(){
    std::puts("mixer,destination,level,style,dst14,rms,peak");
    for(int dest:{shruthi::MOD_DST_MIX_SUB_OSC,shruthi::MOD_DST_MIX_NOISE})for(int v=0;v<128;++v){
        const auto base=render(dest,v,shruthi::MOD_SRC_WHEEL,0,0);
        check(base.rms>1.e-5,"nontrivial audible mixer fixture");
        check(base.dst==(v>>1)*256,"public mixer domain maps to six-bit firmware destination");
        const auto zero=render(dest,v,shruthi::MOD_SRC_WHEEL,63,1);
        check(zero.dst==base.dst&&zero.audio==base.audio,"nonzero row with zero wheel contribution preserves exact base audio");
        const auto disabled=render(dest,v,shruthi::MOD_SRC_OFFSET,0,1);
        check(disabled.audio==base.audio,"disabled amount preserves exact base audio");
        std::printf("mixer,%d,%d,base,%d,%.12g,%.12g\n",dest,v,base.dst,base.rms,base.peak);
        for(int polarity:{-63,63})for(int rows:{1,4}){
            const auto mod=render(dest,v,shruthi::MOD_SRC_OFFSET,polarity,rows);
            int expected=(v>>1)*256;for(int i=0;i<rows;++i)expected=std::clamp(expected+polarity*255,0,16383);
            check(mod.dst==expected,"independent additive modulation and per-row saturation");
            std::printf("mixer,%d,%d,%d,%d,%.12g,%.12g\n",dest,v,polarity*rows,mod.dst,mod.rms,mod.peak);
        }
        if(v%16==0)pump();
    }
    for(int dest:{shruthi::MOD_DST_MIX_SUB_OSC,shruthi::MOD_DST_MIX_NOISE})for(int src:{shruthi::MOD_SRC_LFO_1,shruthi::MOD_SRC_ENV_1,shruthi::MOD_SRC_VELOCITY})for(int amount:{-63,1,63}){
        const auto c=render(dest,63,src,amount,1);check(c.dst>=0&&c.dst<=16383,"live LFO/envelope/velocity destinations remain clipped");
    }
}
struct Host:juce::AudioPlayHead{double ppq=0;bool playing=true;juce::Optional<PositionInfo> getPosition()const override{PositionInfo p;p.setIsPlaying(playing);p.setBpm(240);p.setPpqPosition(ppq);return p;}};
struct InternalNotes {
    int ons=0,offs=0;std::array<bool,128> held{};
    static void on(void* c,uint8_t id,uint8_t velocity,uint8_t external) noexcept {auto& t=*static_cast<InternalNotes*>(c);++t.ons;t.held[id]=true;if(velocity>127||external>127)std::abort();}
    static void off(void* c,uint8_t id) noexcept {auto& t=*static_cast<InternalNotes*>(c);++t.offs;t.held[id]=false;}
};
void arp(){
    for(int pattern=0;pattern<16;++pattern){
        pump();P p;Host host;p.setPlayHead(&host);p.prepareToPlay(48000,128);
        set(p,swaraxt::IDs::seqMode,1);set(p,swaraxt::IDs::seqClockMode,1);set(p,swaraxt::IDs::seqMidiOut,1);set(p,swaraxt::IDs::arpGate,11);
        auto* a=p.getApvts().getParameter(swaraxt::IDs::arpPattern);
        std::unique_ptr<SwaraXtAudioProcessorEditor> ed(static_cast<SwaraXtAudioProcessorEditor*>(p.createEditor()));ed->setModuleViewsForTests(false,true);
        ed->setSequencerPatternForTests(pattern);check(get(p,swaraxt::IDs::arpPattern)==pattern,"GUI reaches all16 APVTS values");
        a->setValueNotifyingHost(float(pattern)/15);pump();check(get(p,swaraxt::IDs::arpPattern)==pattern&&ed->sequencerPatternForTests()==pattern,"normalized host parameter reaches GUI");
        set(p,swaraxt::IDs::arpPattern,float(pattern));check(p.sequenceState().snapshot().arpPattern==pattern,"physical host parameter reaches sequence publication");
        // ARP intentionally does not emit external MIDI in the existing product.
        // Observe the actual firmware-generated note callbacks, then test the
        // supported external SEQ route without changing that policy.
        InternalNotes internal;swaraxt::SequencerMidiTestAccess::tap(p.engineForTests(),{InternalNotes::on,InternalNotes::off,&internal});
        for(int step=0;step<16;++step)p.sequenceState().setStep(step,uint8_t(0x80|60),0x70);
        juce::AudioBuffer<float>b(2,128);juce::MidiBuffer m;m.ensureSize(2048);std::map<std::pair<int,int>,bool> held;int ons=0;
        for(int block=0;block<256;++block){m.clear();if(!block)for(int note:{60,64,67})m.addEvent(juce::MidiMessage::noteOn(1,note,juce::uint8(100)),0);if(block==128)host.ppq+=2.0;p.processBlock(b,m);if(block%16==0)pump();for(auto e:m){auto n=e.getMessage();if(n.isNoteOnOrOff()){held[{n.getChannel(),n.getNoteNumber()}]=n.isNoteOn();ons+=n.isNoteOn();check(e.samplePosition>=0&&e.samplePosition<128,"arp MIDI event timestamp");}}host.ppq+=128.0/12000.0;}
        check(p.engineForTests().shruthiPart().sequencer_settings().arp_pattern==pattern,"all16 values reach firmware during playback");
        host.playing=false;m.clear();p.processBlock(b,m);for(auto e:m){auto n=e.getMessage();if(n.isNoteOff())held[{n.getChannel(),n.getNoteNumber()}]=false;}
        check(internal.ons>0,"every ROM/Sequence pattern executes real firmware note callbacks");
        check(ons==0,"existing ARP external-MIDI suppression is preserved");
        // Mid-playback host changes must reach the firmware without restarting.
        host.playing=true;set(p,swaraxt::IDs::arpPattern,float((pattern+1)%16));m.clear();p.processBlock(b,m);
        check(p.engineForTests().shruthiPart().sequencer_settings().arp_pattern==(pattern+1)%16,"pattern change during playback reaches firmware");
        set(p,swaraxt::IDs::arpPattern,float(pattern));
        if(pattern==15){
            for(int step=0;step<16;++step)p.sequenceState().setStep(step,60,0x70);
            for(int k=0;k<4;++k){m.clear();p.processBlock(b,m);if(k%16==0)pump();host.ppq+=128.0/12000.0;}
            const int before=internal.ons;
            for(int k=0;k<256;++k){m.clear();p.processBlock(b,m);if(k%16==0)pump();host.ppq+=128.0/12000.0;}
            check(internal.ons==before,"pattern15 follows closed Sequence gates instead of ROM");
            for(int step=0;step<16;++step)p.sequenceState().setStep(step,uint8_t(0x80|60),0x70);
            for(int k=0;k<256;++k){m.clear();p.processBlock(b,m);if(k%16==0)pump();host.ppq+=128.0/12000.0;}
            check(internal.ons>before,"pattern15 follows reopened Sequence gates");
        }
        // reset restores the production tap and starts an independent MIDI ledger.
        p.engineForTests().reset();set(p,swaraxt::IDs::seqMode,2);p.sequenceState().setPitchMode(0);
        for(int k=0;k<256;++k){m.clear();p.processBlock(b,m);if(k%16==0)pump();host.ppq+=128.0/12000.0;for(auto event:m){auto n=event.getMessage();if(n.isNoteOnOrOff()){held[{n.getChannel(),n.getNoteNumber()}]=n.isNoteOn();ons+=n.isNoteOn();}}}
        host.playing=false;for(int k=0;k<8;++k){m.clear();p.processBlock(b,m);for(auto event:m){auto n=event.getMessage();if(n.isNoteOff())held[{n.getChannel(),n.getNoteNumber()}]=false;}}
        check(ons>0,"supported external SEQ route emits actual MIDI with every pattern value");
        check(std::none_of(held.begin(),held.end(),[](const auto& v){return v.second;}),"stop discharges external SEQ ledger");
        juce::MemoryBlock state;p.getStateInformation(state);P fresh;fresh.setStateInformation(state.getData(),int(state.getSize()));fresh.prepareToPlay(96000,64);check(get(fresh,swaraxt::IDs::arpPattern)==pattern&&fresh.sequenceState().snapshot().arpPattern==pattern,"all16 state restore and reprepare");
        ed.reset();pump();ed.reset(static_cast<SwaraXtAudioProcessorEditor*>(p.createEditor()));check(ed->sequencerPatternForTests()==pattern,"editor reopen retains all16 values");
        p.setCurrentProgram(1);pump();check(p.sequenceState().snapshot().arpPattern==int(get(p,swaraxt::IDs::arpPattern)),"factory load leaves coherent arp parameter");p.setPlayHead(nullptr);
        std::printf("arp pattern=%d MIDI ons=%d PASS checks so far=%d\n",pattern,ons,checks);
    }
}
std::array<float,4096> factoryAudio(int index,bool zeroRow){
    pump();P p;p.setFilterQuality(swaraxt::FilterQuality::eco);p.prepareToPlay(48000,128);
    juce::String error;check(p.loadPresetEntry({"raw factory",{},index,true},error),"production factory loader");
    if(zeroRow){
        for(int row=1;row<=11;++row){const auto prefix="mod.row"+juce::String(row);
            if(get(p,(prefix+".amount").toRawUTF8())==0){
                set(p,(prefix+".source").toRawUTF8(),shruthi::MOD_SRC_WHEEL);
                set(p,(prefix+".destination").toRawUTF8(),shruthi::MOD_DST_MIX_SUB_OSC);
                set(p,(prefix+".amount").toRawUTF8(),63);break;
            }
        }
    }
    std::array<float,4096> output{};juce::MidiBuffer midi;midi.ensureSize(2048);juce::AudioBuffer<float> buffer(2,128);
    for(int block=0;block<32;++block){midi.clear();if(block==0)midi.addEvent(juce::MidiMessage::noteOn(1,60,juce::uint8(100)),0);if(block==24)midi.addEvent(juce::MidiMessage::noteOff(1,60),0);
        p.processBlock(buffer,midi);for(int i=0;i<128;++i){const auto v=buffer.getSample(0,i);check(std::isfinite(v),"factory audio finite");output[size_t(block*128+i)]=v;}
    }
    if(const char* folder=std::getenv("SWARA_CONTROL_CAPTURE_DIR"))check(juce::File(juce::String(folder)).getChildFile("factory-"+juce::String(index)+"-"+juce::String(int(zeroRow))+".f32").replaceWithData(output.data(),sizeof(output)),"save private factory comparison");
    return output;
}
void factoryImport(){
    pump();P p;p.prepareToPlay(48000,128);swaraxt::ParameterCache cache;cache.bind(p.getApvts());swaraxt::PatchBridge bridge;bridge.bind(p.engineForTests().shruthiPart());
    shruthi::Patch patch{};const auto& first=swaraxt::kShruthiFactoryPresets[0];
    check(swaraxt::ShruthiFactoryPresets::decodePatch(first.bytes.data(),first.bytes.size(),patch),"independent raw firmware seed patch decode");
    for(int value=0;value<64;++value){
        patch.mix_sub_osc=uint8_t(value);patch.mix_noise=uint8_t(63-value);patch.mix_balance=uint8_t(value);
        swaraxt::ShruthiFactoryPresets::applyPatchToApvts(patch,p.getApvts());bridge.applyCacheToEngine(cache);
        const auto& actual=p.engineForTests().shruthiPart().patch();
        check(get(p,swaraxt::IDs::mixSub)==2*value&&get(p,swaraxt::IDs::mixNoise)==2*(63-value)&&get(p,swaraxt::IDs::mixBalance)==2*value,"raw import inverse covers every 0..63 value");
        check(actual.mix_sub_osc==value&&actual.mix_noise==63-value&&actual.mix_balance==value,"raw import and bridge preserve all three firmware mixer fields");
    }
    int extendedLfos=0;
    for(size_t i=0;i<swaraxt::kShruthiFactoryPresetCount;++i){
        const auto& record=swaraxt::kShruthiFactoryPresets[i];shruthi::Patch original{};
        check(swaraxt::ShruthiFactoryPresets::decodePatch(record.bytes.data(),record.bytes.size(),original),"every shipped Shruthi patch decodes");
        juce::String error;check(p.loadPresetEntry({record.displayName,{},swaraxt::kShruthiFactoryPresetStart+int(i),true},error),"every raw factory entry loads through processor");bridge.applyCacheToEngine(cache);
        const auto& actual=p.engineForTests().shruthiPart().patch();
        check(actual.mix_sub_osc==original.mix_sub_osc&&actual.mix_noise==original.mix_noise&&actual.mix_balance==original.mix_balance,"every factory mixer roundtrip matches original decoded fields");
        extendedLfos+=original.lfo[0].rate>127||original.lfo[1].rate>127;
        std::printf("factory import %zu %s raw sub=%u noise=%u balance=%u LFO=%u/%u\n",i,record.displayName,unsigned(original.mix_sub_osc),unsigned(original.mix_noise),unsigned(original.mix_balance),unsigned(original.lfo[0].rate),unsigned(original.lfo[1].rate));
        const auto base=factoryAudio(swaraxt::kShruthiFactoryPresetStart+int(i),false);
        check(factoryAudio(swaraxt::kShruthiFactoryPresetStart+int(i),true)==base,"factory zero-effective row preserves exact audio");
    }
    std::printf("factory patches with extended LFO rate: %d\n",extendedLfos);
}
void upstreamParameters(){
    // Exercise the same noncontiguous item-ID attachment used by ModPanel.
    pump();P p;p.prepareToPlay(48000,128);juce::ComboBox sources;
    swaraxt::ui::populatePluginModulationSourceCombo(sources);
    swaraxt::ui::EngineIdComboAttachment attachment(p.getApvts(),"mod.row1.source",sources,swaraxt::ui::modulationSourceName);
    juce::AudioBuffer<float> audio(2,128);juce::MidiBuffer midi;midi.ensureSize(2048);
    for(int id=0;id<32;++id){
        set(p,"mod.row1.source",float(id));pump();
        if(!swaraxt::ui::isHardwareOnlyModulationSource(id)){
            check(sources.getSelectedId()==id+1,"all visible source item IDs retain engine enum identity");
            sources.setSelectedId(1,juce::sendNotificationSync);sources.setSelectedId(id+1,juce::sendNotificationSync);
            check(get(p,"mod.row1.source")==id,"GUI source selection retains stable host value");
        }else check(sources.getSelectedId()==0,"hardware-only automation has an explicit nonselectable display");
        midi.clear();p.processBlock(audio,midi);
        check(p.engineForTests().shruthiPart().patch().modulation_matrix.modulation[0].source==id,"source host value reaches firmware");
        juce::MemoryBlock state;p.getStateInformation(state);P fresh;fresh.setStateInformation(state.getData(),int(state.getSize()));check(get(fresh,"mod.row1.source")==id,"every source ID survives state restore");
        if(id>=28)check(p.engineForTests().shruthiPart().voice().modulation_source(uint8_t(id))==(4<<(id-28)),"constant source byte equals its label");
    }
    for(int rate:{0,15,16,63,127,128,143})for(bool sync:{false,true}){
        pump();P l;l.prepareToPlay(48000,128);auto& part=l.engineForTests().shruthiPart();
        auto* patch=part.mutable_patch();for(auto& row:patch->modulation_matrix.modulation)row.amount=0;
        patch->lfo[0].rate=uint8_t(rate);patch->lfo[0].waveform=0;patch->lfo[0].retrigger_mode=0;
        part.SetHostLfoSync(0,sync,1234);l.engineForTests().touchModulationRates();
        uint64_t phaseAdvance=0;uint16_t phase=part.lfo_phase_for_tests(0);
        for(int i=0;i<4096;++i){part.ProcessBlock();const auto next=part.lfo_phase_for_tests(0);phaseAdvance+=uint16_t(next-phase);phase=next;}
        const int modulation=part.voice().modulation_destination(shruthi::MOD_DST_LFO_1);
        const int index=std::clamp(rate+modulation-32,0,127);
        const auto increment=part.lfo_phase_increment_for_tests(0);
        if(sync)check(increment==1234,"host sync overrides stored LFO rate");
        else if(rate>=16)check(increment==shruthi::lut_res_lfo_increments[index],"free LFO clamps its actual ROM index without exceeding table bounds");
        std::printf("LFO raw=%d hostSync=%d increment=%u measuredHz=%.9g\n",rate,int(sync),unsigned(increment),double(phaseAdvance)/65536.0/(4096.0*40.0/swaraxt::SwaraXtEngine::kInternalSampleRate));
    }
    for(int raw:{0,15,16,127})for(int blockSize:{64,256}){
        pump();P l;Host host;host.ppq=0;l.setPlayHead(&host);l.prepareToPlay(48000,blockSize);
        set(l,swaraxt::IDs::lfo1Rate,float(raw));set(l,swaraxt::IDs::lfo1Sync,1);set(l,swaraxt::IDs::lfo1Division,3);
        juce::AudioBuffer<float> b(2,blockSize);juce::MidiBuffer events;events.ensureSize(2048);uint64_t advance=0;uint16_t previousPhase=0;unsigned minimum=65535,maximum=0;
        for(int k=0;k<256;++k){events.clear();if(k==0)events.addEvent(juce::MidiMessage::noteOn(1,60,juce::uint8(100)),0);l.processBlock(b,events);host.ppq+=blockSize/12000.0;
            const auto& part=l.engineForTests().shruthiPart();const auto phase=part.lfo_phase_for_tests(0);advance+=uint16_t(phase-previousPhase);previousPhase=phase;const unsigned inc=part.lfo_phase_increment_for_tests(0);check(inc==267,"processor host sync owns low-rate and free-rate increments");minimum=std::min(minimum,inc);maximum=std::max(maximum,inc);
        }
        std::printf("processor hostSync raw=%d block=%d phaseHz=%.9g incrementMin=%u max=%u\n",raw,blockSize,double(advance)/65536.0/(256.0*blockSize/48000.0),minimum,maximum);l.setPlayHead(nullptr);
    }
    for(int glide:{0,1,15,63,64,127})for(int range:{-48,0,48}){
        pump();P g;g.prepareToPlay(48000,128);set(g,swaraxt::IDs::perfGlide,float(glide));set(g,swaraxt::IDs::osc1Range,float(range));set(g,swaraxt::IDs::osc2Range,float(range));
        int firstPitch=0,lastPitch=0;double maxStep=0;float previous=0;
        for(int block=0;block<128;++block){midi.clear();if(block%32==0){midi.addEvent(juce::MidiMessage::allNotesOff(1),0);midi.addEvent(juce::MidiMessage::noteOn(1,block%64==0?0:127,juce::uint8(100)),0);}
            g.processBlock(audio,midi);const int pitch=g.engineForTests().shruthiPart().voice().pitch_value();check(pitch>=0&&pitch<=16383,"extreme note glide stays in voice pitch-word bounds");if(block==32)firstPitch=pitch;lastPitch=pitch;
            for(int i=0;i<128;++i){const float value=audio.getSample(0,i);check(std::isfinite(value),"extended range/glide actual output finite");maxStep=std::max(maxStep,double(std::abs(value-previous)));previous=value;}
        }
        check(g.engineForTests().shruthiPart().system_settings().portamento==glide,"full glide parameter reaches 128-entry table domain");
        std::printf("glide=%d range=%d firstTargetSample=%d last=%d maxAudioStep=%.9g\n",glide,range,firstPitch,lastPitch,maxStep);
    }
}
void legacyState(bool write){
    const char* folder=std::getenv("SWARA_CONTROL_CAPTURE_DIR");if(!folder){check(false,"private state fixture folder required");return;}
    for(int value:{0,1,32,63,64,96,127}){
        pump();P p;const auto file=juce::File(juce::String(folder)).getChildFile("state-"+juce::String(value)+".bin");juce::MemoryBlock bytes;
        if(write){set(p,swaraxt::IDs::mixSub,float(value));set(p,swaraxt::IDs::mixNoise,float(value));p.getStateInformation(bytes);check(file.replaceWithData(bytes.getData(),bytes.getSize()),"save real baseline state payload");}
        else{check(file.loadFileAsData(bytes),"load baseline state bytes");p.setStateInformation(bytes.getData(),int(bytes.getSize()));}
        p.prepareToPlay(48000,128);juce::AudioBuffer<float> b(2,128);juce::MidiBuffer m;p.processBlock(b,m);
        const auto& patch=p.engineForTests().shruthiPart().patch();
        check(get(p,swaraxt::IDs::mixSub)==value&&get(p,swaraxt::IDs::mixNoise)==value,"serialized physical mixer values are unchanged");
        std::printf("state public=%d firmwareSub=%u firmwareNoise=%u schema=%d\n",value,unsigned(patch.mix_sub_osc),unsigned(patch.mix_noise),P::kStateVersion);
    }
}
void parity(){
    for(double rate:{44100.,48000.,96000.,192000.})for(int block:{64,127})for(int bank=0;bank<4;++bank)for(int quality=0;quality<3;++quality)for(int fx:{0,6}){
        pump();P p;set(p,swaraxt::IDs::mixSub,0);set(p,swaraxt::IDs::mixNoise,0);set(p,swaraxt::IDs::seqMode,0);set(p,swaraxt::IDs::dspFxProgram,float(fx));
        p.setFilterQuality(static_cast<swaraxt::FilterQuality>(quality));
        p.setExperimentalSvfEnabled(bank!=0);if(bank)p.setExperimentalSvfRouting(static_cast<swaraxt::ShruthiSvfDualRouting>(bank-1));
        p.prepareToPlay(rate,block);std::array<float,8192> output{};juce::MidiBuffer midi;midi.ensureSize(4096);double energy=0;
        for(int offset=0;offset<int(output.size());offset+=block){
            const int n=std::min(block,int(output.size())-offset);juce::AudioBuffer<float> audio(2,n);midi.clear();
            if(offset==0)midi.addEvent(juce::MidiMessage::noteOn(1,60,juce::uint8(100)),0);
            if(offset<=6144&&offset+n>6144)midi.addEvent(juce::MidiMessage::noteOff(1,60),6144-offset);
            p.processBlock(audio,midi);for(int i=0;i<n;++i){const float v=audio.getSample(0,i);check(std::isfinite(v),"unaffected path finite audio");output[size_t(offset+i)]=v;energy+=double(v)*v;}
        }
        check(energy>1.e-6,"unaffected comparison contains audible content");
        if(const char* folder=std::getenv("SWARA_CONTROL_CAPTURE_DIR")){
            const auto name="parity-"+juce::String(int(rate))+"-"+juce::String(block)+"-"+juce::String(bank)+"-"+juce::String(quality)+"-"+juce::String(fx)+".f32";
            check(juce::File(juce::String(folder)).getChildFile(name).replaceWithData(output.data(),sizeof(output)),"save private independent binary comparison");
        }
    }
}
void files(){
    const auto dir=juce::File::getSpecialLocation(juce::File::tempDirectory).getChildFile("swara-fs-"+juce::Uuid().toString());check(dir.createDirectory().wasOk(),"fixture directory");P p;p.setUserPresetDirectoryForTests(dir);juce::String error;
    for(const auto* name:{"CON","con.foo.bar","PRN.txt","AUX","NUL","COM1","COM9","LPT1","LPT9","../escape","a/b","a\\b","a.",""," "})check(!p.saveUserPreset(name,false,error),"invalid/reserved preset name rejected");
    for(int i=1;i<=9;++i){check(!p.saveUserPreset("COM"+juce::String(i),false,error),"COM reserved");check(!p.saveUserPreset("LPT"+juce::String(i),false,error),"LPT reserved");}
    check(!p.saveUserPreset(juce::String::repeatedString("a",250),false,error),"full preset filename length bounded");
    check(!p.saveUserPreset(juce::String::repeatedString(juce::String::fromUTF8("\xe9\x9f\xb3"),85),false,error),"UTF8 byte length includes the preset suffix");
    check(p.saveUserPreset(juce::String::fromUTF8("T\xc3\xa4st \xe9\x9f\xb3"),false,error),"Unicode filename");
    check(p.saveUserPreset(" Trim ",false,error)&&dir.getChildFile("Trim.swaraxtpreset").existsAsFile(),"whitespace trimming produces one canonical name");
    check(!p.saveUserPreset("Trim",false,error),"trimmed name collision remains create-only");
    check(p.saveUserPreset("Valid.name",false,error),"ordinary embedded dots remain valid");
    const auto outside=dir.getSiblingFile(dir.getFileName()+"-outside");
    check(outside.replaceWithText("outside sentinel"),"isolated symlink target fixture");
    if(outside.createSymbolicLink(dir.getChildFile("Link.swaraxtpreset"),false)){
        check(!p.saveUserPreset("Link",false,error)&&outside.loadFileAsString()=="outside sentinel","create-only cannot follow or overwrite a symlink");
        check(p.saveUserPreset("Link",true,error)&&outside.loadFileAsString()=="outside sentinel","overwrite replaces link entry and preserves outside target");
    }else std::puts("NOT_RUN: symlink fixture unavailable on this filesystem/account");
    check(outside.deleteFile(),"remove own outside target fixture");
    set(p,swaraxt::IDs::osc1Param,17);check(p.saveUserPreset("Keep",false,error),"fresh save");const auto target=dir.getChildFile("Keep.swaraxtpreset");const auto first=target.loadFileAsString();
    set(p,swaraxt::IDs::osc1Param,91);check(!p.saveUserPreset("Keep",false,error)&&target.loadFileAsString()==first,"create-only collision preserves old bytes");
    check(p.saveUserPreset("Keep",true,error),"explicit overwrite");P fresh;check(fresh.loadPresetEntry({"Keep",target,-1,false},error)&&get(fresh,swaraxt::IDs::osc1Param)==91,"fresh processor loads actual saved value");
    check(dir.deleteRecursively(),"remove only own preset fixture");
}
#if !defined(SWARA_CONTROL_BASELINE)
void fileFaults(int kind){
    const auto root=juce::File::getSpecialLocation(juce::File::tempDirectory).getChildFile("swara-fault-"+juce::Uuid().toString());
    const auto dir=root.getChildFile("presets");check(dir.createDirectory().wasOk(),"fault fixture");
    P p;p.setUserPresetDirectoryForTests(dir);juce::String error;
    set(p,swaraxt::IDs::osc1Param,17);check(p.saveUserPreset("Keep",true,error),"protected preset before fault");
    juce::MemoryBlock old;check(dir.getChildFile("Keep.swaraxtpreset").loadFileAsData(old),"capture protected preset bytes");
    const auto metadata=p.currentPresetName();
    struct Fault {
        int kind;juce::File dir,aside;
        static void hook(void* c,P::StatePublicationPoint point){auto& f=*static_cast<Fault*>(c);
            if(f.kind==0&&point==P::StatePublicationPoint::userPresetTemporaryChosen){
                check(f.dir.moveFileTo(f.aside),"retire only fixture directory for injected write failure");
                check(f.dir.replaceWithText("blocked"),"block temporary write with a real file");
            }
            if(f.kind==1&&point==P::StatePublicationPoint::userPresetTemporaryWritten){
                const auto target=f.dir.getChildFile("New.swaraxtpreset");check(target.createDirectory().wasOk()&&target.getChildFile("sentinel").replaceWithText("protected"),"nonempty directory blocks actual publication");
            }
        }
    } fault{kind,dir,root.getChildFile("old-presets")};
    p.setStatePublicationHook(Fault::hook,&fault);set(p,swaraxt::IDs::osc1Param,91);
#if JUCE_LINUX
    if(kind==4)setenv("SWARA_TEST_SHORT_WRITE","1",1);
#endif
    const bool saved=p.saveUserPreset("New",false,error);
#if JUCE_LINUX
    if(kind==4)unsetenv("SWARA_TEST_SHORT_WRITE");
#endif
p.setStatePublicationHook(nullptr,nullptr);
    if(kind==0){check(dir.deleteFile()&&fault.aside.moveFileTo(dir),"restore fixture path after injected write failure");}
    if(kind==3){
        check(saved,"cleanup failure does not invalidate already published complete file");
        P fresh;check(fresh.loadPresetEntry({"New",dir.getChildFile("New.swaraxtpreset"),-1,false},error)&&get(fresh,swaraxt::IDs::osc1Param)==91,"published file survives temporary cleanup failure");
        juce::Array<juce::File> remaining;dir.findChildFiles(remaining,juce::File::findFiles,false,"*.tmp");check(remaining.size()==1,"failed temporary cleanup leaves identifiable non-preset residue");
    }else{
        check(!saved&&error.isNotEmpty(),"actual filesystem fault rejects with an error");
        check(p.currentPresetName()==metadata,"failed save preserves accepted metadata");
    }
    juce::MemoryBlock after;check(dir.getChildFile("Keep.swaraxtpreset").loadFileAsData(after)&&after==old,"filesystem fault preserves earlier valid file byte-for-byte");
    if(kind<2)check(root.deleteRecursively(),"remove only fault fixture");
    // Interposed cleanup faults remain outside Git for inspection by the runner.
}
void concurrentFiles(){
    const auto dir=juce::File::getSpecialLocation(juce::File::tempDirectory).getChildFile("swara-race-"+juce::Uuid().toString());check(dir.createDirectory().wasOk(),"race fixture directory");
    P first,second;first.setUserPresetDirectoryForTests(dir);second.setUserPresetDirectoryForTests(dir);
    set(first,swaraxt::IDs::osc1Param,17);set(second,swaraxt::IDs::osc1Param,91);
    struct Pause {
        juce::WaitableEvent chosen,written,allowWrite,allowPublish;
        static void hook(void* context,P::StatePublicationPoint point){auto& x=*static_cast<Pause*>(context);
            if(point==P::StatePublicationPoint::userPresetTemporaryChosen){x.chosen.signal();if(!x.allowWrite.wait(15000))std::abort();}
            if(point==P::StatePublicationPoint::userPresetTemporaryWritten){x.written.signal();if(!x.allowPublish.wait(15000))std::abort();}
        }
    } a,b;
    first.setStatePublicationHook(Pause::hook,&a);second.setStatePublicationHook(Pause::hook,&b);
    juce::String ea,eb;bool sa=false,sb=false;
    std::thread wa([&]{sa=first.saveUserPreset("Race",false,ea);});check(a.chosen.wait(10000),"first selected temporary name");
    std::thread wb([&]{sb=second.saveUserPreset("Race",false,eb);});check(b.chosen.wait(10000),"second selected name before either write");
    a.allowWrite.signal();check(a.written.wait(10000),"first complete temporary payload");
    b.allowWrite.signal();check(b.written.wait(10000),"second complete temporary payload");
    auto entries=first.getPresetEntries();check(std::none_of(entries.begin(),entries.end(),[](const auto& e){return !e.isFactory;}),"in-progress temporary files never enumerate as presets");
    a.allowPublish.signal();wa.join();b.allowPublish.signal();wb.join();
    check(sa&&!sb&&eb.isNotEmpty(),"create-only has exactly one controlled winner");
    P fresh;juce::String error;check(fresh.loadPresetEntry({"Race",dir.getChildFile("Race.swaraxtpreset"),-1,false},error)&&get(fresh,swaraxt::IDs::osc1Param)==17,"successful writer publishes its OWN complete state");
    first.setStatePublicationHook(nullptr,nullptr);second.setStatePublicationHook(nullptr,nullptr);check(dir.deleteRecursively(),"remove own race fixture");
}
#endif
}
int main(int argc,char** argv){juce::ScopedJuceInitialiser_GUI init;const juce::String mode=argc>1?argv[1]:"numeric";if(mode=="numeric")numeric();else if(mode=="text")text();else if(mode=="mixer")mixer();else if(mode=="arp")arp();else if(mode=="parity")parity();else if(mode=="factory-import")factoryImport();else if(mode=="upstream")upstreamParameters();else if(mode=="legacy-state-write")legacyState(true);else if(mode=="legacy-state-read")legacyState(false);else if(mode=="files"){files();
#if !defined(SWARA_CONTROL_BASELINE)
concurrentFiles();
#endif
}
#if !defined(SWARA_CONTROL_BASELINE)
else if(mode=="files-write-failure")fileFaults(0);else if(mode=="files-publish-failure")fileFaults(1);else if(mode=="files-link-failure")fileFaults(2);else if(mode=="files-cleanup-failure")fileFaults(3);else if(mode=="files-partial-write-failure")fileFaults(4);
#endif
else return 2;std::printf("Control integrity %s: %d checks %d failures\n",mode.toRawUTF8(),checks,failures);return failures?1:0;}
