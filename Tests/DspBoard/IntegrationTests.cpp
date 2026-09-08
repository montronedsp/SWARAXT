// Copyright 2026 MontroneDSP. SPDX-License-Identifier: GPL-3.0-or-later
#include <JuceHeader.h>
#include "Plugin/PluginProcessor.h"
#include <iostream>
#include <stdexcept>
#include <chrono>

namespace {
using namespace swaraxt;
void require(bool condition, const char* label) { if (!condition) throw std::runtime_error(label); }
void set(SwaraXtAudioProcessor& p, const char* id, float value)
{
    auto* parameter = p.getApvts().getParameter(id);
    require(parameter != nullptr,"parameter exists");
    parameter->setValueNotifyingHost(parameter->convertTo0to1(value));
}
float get(const SwaraXtAudioProcessor& p, const char* id)
{
    return p.getApvts().getRawParameterValue(id)->load();
}
void process(SwaraXtAudioProcessor& p, int samples=512, bool note=false)
{
    juce::AudioBuffer<float> audio(2,samples); juce::MidiBuffer midi;
    if(note)midi.addEvent(juce::MidiMessage::noteOn(1,60,juce::uint8(100)),0);
    p.processBlock(audio,midi);
    for(int i=0;i<samples;++i)
        require(std::isfinite(audio.getSample(0,i)) && std::abs(audio.getSample(0,i))<4.f,"finite integrated audio");
}
juce::MemoryBlock legacyState(SwaraXtAudioProcessor& p)
{
    juce::MemoryBlock bytes;p.getStateInformation(bytes);
    auto xml=juce::AudioProcessor::getXmlFromBinary(bytes.getData(),static_cast<int>(bytes.getSize()));
    auto tree=juce::ValueTree::fromXml(*xml);
    for(const auto* id:{IDs::filterModel,IDs::dspFxProgram,IDs::dspFxParam1,IDs::dspFxParam2,IDs::dspBoardRouting})
        tree.removeChild(tree.getChildWithProperty("id",id),nullptr);
    juce::AudioProcessor::copyXmlToBinary(*tree.createXml(),bytes);
    return bytes;
}
void stateTests()
{
    SwaraXtAudioProcessor p;p.prepareToPlay(48000,512);
    require(p.getNumPrograms()==76,"factory count");
    require(get(p,IDs::filterModel)==0 && get(p,IDs::dspFxProgram)==0,"Classic Off default");
    const int count=p.getParameters().size();
    int index=count-5;
    for(const auto* id:{IDs::filterModel,IDs::dspFxProgram,IDs::dspFxParam1,IDs::dspFxParam2,IDs::dspBoardRouting})
    {
        auto* parameter=dynamic_cast<juce::RangedAudioParameter*>(p.getParameters()[index++]);
        require(parameter != nullptr && parameter->paramID==id,"append-only parameter order");
    }
    juce::String error;
    for(int preset=0;preset<76;++preset)
    {
        set(p,IDs::filterModel,1);set(p,IDs::dspFxProgram,15);
        require(p.loadPresetEntry({p.getProgramName(preset),{},preset,true},error),"factory load");
        require(get(p,IDs::filterModel)==0 && get(p,IDs::dspFxProgram)==0,"all old factory presets Classic Off");
    }
    auto old=legacyState(p);
    set(p,IDs::filterModel,1);set(p,IDs::dspFxProgram,15);set(p,IDs::dspFxParam2,63);
    p.setStateInformation(old.getData(),static_cast<int>(old.getSize()));process(p);
    require(get(p,IDs::filterModel)==0 && get(p,IDs::dspFxProgram)==0,"old DAW state overrides current Board");
    require(p.getTailLengthSeconds()==0,"Classic zero tail restored");
    set(p,IDs::filterModel,1);set(p,IDs::dspFxProgram,15);set(p,IDs::dspFxParam1,99);set(p,IDs::dspFxParam2,0);set(p,IDs::dspBoardRouting,3);
    for(int i=0;i<8;++i)process(p,512,i==0);
    require(p.engineForTests().boardProcessorForTests().hasValidLoop(),"integrated loop record");
    juce::MemoryBlock boardState;p.getStateInformation(boardState);
    require(boardState.getSize()<64000,"loop data not serialized");
    for(int i=0;i<3;++i)
    {
        p.setStateInformation(old.getData(),static_cast<int>(old.getSize()));process(p);
        require(get(p,IDs::filterModel)==0 && get(p,IDs::dspFxProgram)==0,"A/B/A Classic");
        p.setStateInformation(boardState.getData(),static_cast<int>(boardState.getSize()));
        set(p,IDs::dspFxParam2,63);process(p);
        require(get(p,IDs::filterModel)==1 && get(p,IDs::dspFxProgram)==15 && get(p,IDs::dspBoardRouting)==3,"A/B/A Board");
        require(!p.engineForTests().boardProcessorForTests().hasValidLoop(),"restore invalidates transient loop");
    }
    SwaraXtAudioProcessor restored;
    restored.setStateInformation(boardState.getData(),static_cast<int>(boardState.getSize()));
    set(restored,IDs::dspFxParam2,63);restored.prepareToPlay(48000,512);process(restored);
    require(!restored.engineForTests().boardProcessorForTests().hasValidLoop(),"recreate invalid loop");
    require(get(restored,IDs::dspFxParam1)==99,"recreated FX controls");
    std::cout<<"State: appended parameters="<<count<<", 76 factories, legacy defaults, A/B/A, loop invalidation PASS\n";
}
void controlTests()
{
    SwaraXtAudioProcessor p;p.prepareToPlay(48000,512);
    set(p,IDs::filterModel,1);set(p,IDs::dspFxProgram,6);set(p,IDs::dspFxParam1,64);set(p,IDs::dspFxParam2,32);
    for(int row=1;row<=12;++row)set(p,("mod.row"+juce::String(row)+".amount").toRawUTF8(),0);
    set(p,"mod.row1.source",float(shruthi::MOD_SRC_OFFSET));
    set(p,"mod.row1.destination",float(shruthi::MOD_DST_CV_1));
    for(int amount:{-63,-1,0,1,63})
    {
        set(p,"mod.row1.amount",float(amount));process(p,2048,true);
        const auto actual=p.engineForTests().boardControlsForTests().cv1;
        if(amount<0)require(actual<128,"negative CV1 modulation preserved");
        if(amount==0)require(actual==128,"CV1 base x2");
        if(amount>0)require(actual>128,"positive CV1 modulation preserved");
    }
    set(p,"mod.row1.destination",float(shruthi::MOD_DST_CV_2));
    set(p,"mod.row1.amount",-1);process(p,2048);
    require(p.engineForTests().boardControlsForTests().cv2<128,"negative CV2 modulation preserved");
    set(p,"mod.row1.amount",0);process(p,2048);
    require(p.engineForTests().boardControlsForTests().cv2==128,"CV2 base x4");
    for(int tempo:{40,80,120,135,174,240})
    {
        set(p,IDs::seqTempo,float(tempo));process(p,2048);
        require(p.engineForTests().boardControlsForTests().tempo==tempo,"internal Board tempo");
    }
    std::cout<<"Native CV1/CV2 signed bases and tempo controls PASS\n";
}
class BoardPlayHead final : public juce::AudioPlayHead {
public:
    Optional<PositionInfo> getPosition() const override
    {
        PositionInfo position;position.setBpm(bpm);position.setIsPlaying(true);
        return position;
    }
    double bpm=120;
};
void hostTempoTests()
{
    SwaraXtAudioProcessor p;BoardPlayHead host;p.setPlayHead(&host);
    set(p,IDs::filterModel,1);set(p,IDs::dspFxProgram,14);set(p,IDs::seqClockMode,1);
    p.prepareToPlay(48000,512);
    for(double bpm:{20.,40.,80.,120.,135.,174.,240.,300.})
    {
        host.bpm=bpm;process(p,2048,true);
        require(p.engineForTests().boardControlsForTests().tempo==std::clamp(int(bpm),40,240),"host tempo native clamp");
    }
    set(p,IDs::seqClockMode,0);set(p,IDs::seqTempo,91);process(p,2048);
    require(p.engineForTests().boardControlsForTests().tempo==91,"free Board tempo ignores host");
    p.setPlayHead(nullptr);
    std::cout<<"Host/free tempo selection and native 40..240 bounds PASS\n";
}
std::vector<float> render(double rate,int blockSize,int effect,int route)
{
    SwaraXtAudioProcessor p;
    set(p,IDs::filterModel,1);set(p,IDs::dspFxProgram,float(effect));set(p,IDs::dspBoardRouting,float(route));
    set(p,IDs::dspFxParam1,82);set(p,IDs::dspFxParam2,24);
    p.prepareToPlay(rate,blockSize);
    std::vector<float> result;
    constexpr int total=8192;
    for(int start=0;start<total;start+=blockSize)
    {
        const int count=std::min(blockSize,total-start);
        juce::AudioBuffer<float> audio(2,count);juce::MidiBuffer midi;
        if(start==0)midi.addEvent(juce::MidiMessage::noteOn(1,60,juce::uint8(120)),0);
        if(start<=4000 && start+count>4000)midi.addEvent(juce::MidiMessage::noteOff(1,60),4000-start);
        p.processBlock(audio,midi);
        result.insert(result.end(),audio.getReadPointer(0),audio.getReadPointer(0)+count);
        for(int i=0;i<count;++i)require(std::isfinite(audio.getSample(0,i))&&std::abs(audio.getSample(0,i))<4,"rate/block safety");
    }
    return result;
}
void streamingTests()
{
    for(double rate:{44100.,48000.,88200.,96000.,176400.,192000.})
    {
        const auto reference=render(rate,32,7,2);
        for(int block:{64,128,256,512,1024})
            require(reference==render(rate,block,7,2),"host block size exact invariance");
    }
    for(int effect=0;effect<17;++effect)
        for(int route=0;route<5;++route)render(48000,256,effect,route);
    SwaraXtAudioProcessor p;p.prepareToPlay(48000,512);
    for(int i=0;i<400;++i)
    {
        set(p,IDs::filterModel,float(i%2));set(p,IDs::dspFxProgram,float(i%17));set(p,IDs::dspBoardRouting,float(i%5));
        set(p,IDs::dspFxParam1,float(i%128));set(p,IDs::dspFxParam2,float(i%64));
        process(p,64,i%31==0);
    }
    set(p,IDs::filterModel,0);set(p,IDs::dspFxProgram,0);
    for(int i=0;i<16;++i)process(p);
    require(p.engineForTests().boardControlsForTests().model==board::Model::classic
        &&p.engineForTests().boardControlsForTests().effect==board::Effect::off,"rapid transition settles to requested topology");
    std::cout<<"Six rates/six blocks exact, 85 routes/programs, 400 rapid transitions PASS\n";
}
void dormantModulationTest()
{
    SwaraXtAudioProcessor p;
    set(p,IDs::filterModel,1);set(p,IDs::dspFxProgram,15);set(p,IDs::dspFxParam2,63);
    set(p,IDs::dspBoardRouting,4);
    for(int row=1;row<=12;++row)set(p,("mod.row"+juce::String(row)+".amount").toRawUTF8(),0);
    set(p,"mod.row2.source",float(shruthi::MOD_SRC_ENV_2));
    set(p,"mod.row2.destination",float(shruthi::MOD_DST_VCA));
    set(p,"mod.row2.amount",63);
    set(p,"mod.row1.source",float(shruthi::MOD_SRC_OFFSET));
    set(p,"mod.row1.destination",float(shruthi::MOD_DST_CV_2));
    p.prepareToPlay(48000,512);
    for(int i=0;i<350;++i)process(p);
    require(p.engineForTests().dormantForTests(),"empty replay can sleep");
    require(!p.engineForTests().boardProcessorForTests().hasValidLoop(),"empty replay remains invalid");
    set(p,"mod.row1.amount",-63);
    for(int i=0;i<8;++i)process(p);
    require(p.engineForTests().boardControlsForTests().cv2<128,"dormant matrix CV reaches Board");
    require(p.engineForTests().boardProcessorForTests().hasValidLoop(),"modulated record wakes empty replay");
    for(int i=0;i<350;++i)process(p);
    require(!p.engineForTests().dormantForTests(),"record clock continues through silence");
    set(p,"mod.row1.amount",0);
    for(int i=0;i<350;++i)process(p);
    require(!p.engineForTests().dormantForTests(),"valid autonomous replay cannot sleep");
    require(std::isinf(p.getTailLengthSeconds()),"replay advertises sustained tail");
    set(p,IDs::filterModel,0);set(p,IDs::dspFxProgram,0);
    for(int i=0;i<350;++i)process(p);
    require(p.engineForTests().dormantForTests() && p.getTailLengthSeconds()==0,"Classic Off returns to original dormancy");
    std::cout<<"Dormant CV modulation, silent record clock, autonomous replay PASS\n";
}
void headroomAndTailTests()
{
    std::uint64_t samples=0,clips=0;
    juce::String error;
    for(int preset=0;preset<76;++preset)
    {
        SwaraXtAudioProcessor p;
        require(p.loadPresetEntry({p.getProgramName(preset),{},preset,true},error),"headroom factory");
        set(p,IDs::filterModel,1);p.prepareToPlay(48000,512);
        for(int i=0;i<40;++i)process(p,512,i==0);
        const auto& input=p.engineForTests().boardProcessorForTests().inputModel();
        samples+=input.processedSamples();clips+=input.clippedSamples();
    }
    for(int op=0;op<14;++op)
    {
        SwaraXtAudioProcessor p;
        set(p,IDs::filterModel,1);set(p,IDs::osc1Option,float(op));
        set(p,IDs::mixSub,127);set(p,IDs::mixNoise,127);
        set(p,IDs::mixBalance,64);p.prepareToPlay(48000,512);
        for(int i=0;i<80;++i)process(p,512,i==0);
        const auto& input=p.engineForTests().boardProcessorForTests().inputModel();
        samples+=input.processedSamples();clips+=input.clippedSamples();
    }
    require(clips==0,"factory/all-operator input headroom");
    std::cout<<"ADC headroom: "<<clips<<'/'<<samples<<" clipped samples (76 factories, 14 operators + maximum sub/noise)\n";

    for(int choice:{0,6})
    {
        SwaraXtAudioProcessor p;
        set(p,IDs::filterModel,1);set(p,IDs::dspFxProgram,float(choice));
        set(p,IDs::dspBoardRouting,4);set(p,IDs::dspFxParam1,127);set(p,IDs::dspFxParam2,63);
        set(p,IDs::env2Release,0);p.prepareToPlay(48000,512);
        for(int i=0;i<20;++i)process(p,512,i==0);
        juce::AudioBuffer<float> audio(2,512);juce::MidiBuffer midi;
        midi.addEvent(juce::MidiMessage::noteOff(1,60),0);p.processBlock(audio,midi);
        require(!p.engineForTests().dormantForTests(),"Board tail survives note off");
        require(std::isfinite(p.getTailLengthSeconds())&&p.getTailLengthSeconds()>0,"finite zero-feedback tail");
        for(int i=0;i<700;++i)process(p);
        require(p.engineForTests().dormantForTests(),"finite Board tail eventually sleeps");
    }
    SwaraXtAudioProcessor a,b,isolated;
    for(auto* p:{&a,&isolated})
    {
        set(*p,IDs::filterModel,1);set(*p,IDs::dspFxProgram,16);set(*p,IDs::dspFxParam2,63);
        p->prepareToPlay(48000,128);
    }
    set(b,IDs::filterModel,1);set(b,IDs::dspFxProgram,15);b.prepareToPlay(48000,128);
    for(int i=0;i<300;++i)
    {
        juce::AudioBuffer<float> first(2,128),reference(2,128);juce::MidiBuffer midi;
        if(i==0)midi.addEvent(juce::MidiMessage::noteOn(1,60,juce::uint8(100)),0);
        auto referenceMidi=midi;
        a.processBlock(first,midi);process(b,128,i==0);isolated.processBlock(reference,referenceMidi);
        for(int j=0;j<128;++j)require(first.getSample(0,j)==reference.getSample(0,j),"instance isolation exact");
    }
    std::cout<<"Finite tail/dormancy and interleaved pitch/looper instance isolation PASS\n";
}
void suspendAndResetSafety()
{
    SwaraXtAudioProcessor p;
    set(p,IDs::filterModel,1);set(p,IDs::dspFxProgram,15);
    set(p,IDs::dspFxParam2,63);set(p,IDs::dspBoardRouting,4);
    juce::MemoryBlock emptyReplay;p.getStateInformation(emptyReplay);
    for(double rate:{44100.,48000.,88200.,96000.,176400.,192000.})
    {
        p.setStateInformation(emptyReplay.getData(),static_cast<int>(emptyReplay.getSize()));
        p.prepareToPlay(rate,256);
        for(int block=0;block<20;++block)
        {
            juce::AudioBuffer<float> audio(2,256);juce::MidiBuffer midi;
            p.processBlock(audio,midi);
            require(audio.getMagnitude(0,256)==0,"restored empty replay host silence");
        }
        set(p,IDs::dspFxParam2,0);
        for(int block=0;block<8;++block)process(p,256,block==0);
        require(p.engineForTests().boardProcessorForTests().hasValidLoop(),"record before suspend");
        set(p,IDs::dspFxParam2,63);process(p,256);
        p.releaseResources();
        juce::AudioBuffer<float> suspended(2,256);juce::MidiBuffer midi;
        p.processBlock(suspended,midi);
        require(suspended.getMagnitude(0,256)==0,"released processor produces silence");
        p.prepareToPlay(rate,256);process(p,256);
        require(!p.engineForTests().boardProcessorForTests().hasValidLoop(),"resume invalidates transient loop");
        p.prepareToPlay(rate,128);process(p,128);
        require(!p.engineForTests().boardProcessorForTests().hasValidLoop(),"block change retains invalid-loop safety");
        p.releaseResources();
    }
    std::cout<<"Six-rate empty replay/state/suspend/resume/block-change safety PASS\n";
}
void compensatedFilterDormancy()
{
    SwaraXtAudioProcessor p;
    set(p,IDs::filterModel,1);set(p,IDs::dspBoardRouting,2);
    set(p,IDs::filterCutoff,100);set(p,IDs::filterResonance,.01f);
    p.prepareToPlay(48000,512);
    for(int i=0;i<350;++i)process(p);
    const auto low=p.engineForTests().boardControlsForTests();
    require(low.resonance!=0 && !board::BoardFilter::hasFeedback(low.cutoff,low.resonance,false),"test reaches nonzero panel / zero effective resonance");
    require(p.engineForTests().dormantForTests(),"compensated post-DCA filter sleeps");
    require(std::isfinite(p.getTailLengthSeconds()),"compensated filter finite host tail");
    set(p,IDs::filterResonance,1);
    for(int i=0;i<8;++i)process(p);
    require(!p.engineForTests().dormantForTests(),"dormant resonance automation wakes autonomous filter");
    require(std::isinf(p.getTailLengthSeconds()),"autonomous filter sustained host tail");
    set(p,IDs::filterResonance,.01f);
    for(int i=0;i<350;++i)process(p);
    require(p.engineForTests().dormantForTests(),"compensated filter returns to dormancy");
    std::cout<<"Post-DCA compensated feedback dormancy, host tail and resonance wake PASS\n";
}
void benchmark()
{
    for(int model=0;model<2;++model)
        for(int effect=0;effect<17;++effect)
        {
            SwaraXtAudioProcessor p;
            set(p,IDs::filterModel,float(model));set(p,IDs::dspFxProgram,float(effect));
            set(p,IDs::dspFxParam2,24);p.prepareToPlay(48000,256);
            juce::AudioBuffer<float> audio(2,256);juce::MidiBuffer midi;
            midi.addEvent(juce::MidiMessage::noteOn(1,60,juce::uint8(110)),0);
            p.processBlock(audio,midi);midi.clear();
            for(int i=0;i<20;++i)p.processBlock(audio,midi);
            const auto start=std::chrono::steady_clock::now();
            for(int i=0;i<400;++i)p.processBlock(audio,midi);
            const double elapsed=std::chrono::duration<double>(std::chrono::steady_clock::now()-start).count();
            std::cout<<"CPU model="<<model<<" fx="<<effect<<" realtime-percent="<<(elapsed/(400.*256/48000)*100)<<'\n';
        }
}
}
int main(int argc,char** argv)
{
    juce::ScopedJuceInitialiser_GUI gui;
    try {
        if(argc>1 && juce::String(argv[1])=="--benchmark")benchmark();
        else {stateTests();controlTests();hostTempoTests();streamingTests();dormantModulationTest();headroomAndTailTests();suspendAndResetSafety();compensatedFilterDormancy();}
    }
    catch(const std::exception& e){std::cerr<<"FAIL: "<<e.what()<<'\n';return 1;}
    return 0;
}
