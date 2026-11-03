// Copyright 2026 MontroneDSP. SPDX-License-Identifier: GPL-3.0-or-later
#include <JuceHeader.h>
#include "NativeStdio.h"
#include <windows.h>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <set>
#include <stdexcept>
#include <vector>
namespace {
void require(bool value, const char* text) { if (!value) throw std::runtime_error(text); }
struct Transport final : juce::AudioPlayHead {
    double ppq = 0;
    juce::Optional<PositionInfo> getPosition() const override {
        PositionInfo p; p.setBpm(120);p.setIsPlaying(true);p.setPpqPosition(ppq);return p;
    }
};
juce::AudioProcessorParameter& parameter(juce::AudioPluginInstance& p,const char* name) {
    for(auto* v:p.getParameters()) if(v->getName(128)==name)return *v;
    throw std::runtime_error("Missing hosted parameter");
}
void booleanState(juce::AudioPluginInstance& p) {
    auto& v=parameter(p,"MIDI Out");
    require(v.getNumSteps()==2,"MIDI Out domain");
    for(float input:{0.f,.331463f,std::nextafter(.5f,0.f),.5f,.9f,1.f}) {
        const float expected=input>=.5f?1.f:0.f;
        v.setValueNotifyingHost(input);juce::MemoryBlock state;p.getStateInformation(state);
        p.setStateInformation(state.getData(),static_cast<int>(state.getSize()));
        require(v.getValue()==expected,"Canonical boolean readback");
        v.setValueNotifyingHost(1.f-expected);
        p.setStateInformation(state.getData(),static_cast<int>(state.getSize()));
        require(v.getValue()==expected,"Boolean state restore");
    }
    std::puts("BOOLEAN_NORMALIZATION_PASS");
}
std::vector<float> render(juce::AudioPluginInstance& p,double rate,int block,bool requireMidi=false) {
    p.prepareToPlay(rate,block);Transport host;p.setPlayHead(&host);
    juce::AudioBuffer<float> audio(2,block);juce::MidiBuffer midi;midi.ensureSize(65536);
    std::vector<float> result;double energy=0;int midiEvents=0;
    for(int b=0;b<256;++b) {
        audio.clear();midi.clear();
        if(b==0)midi.addEvent(juce::MidiMessage::noteOn(1,60,juce::uint8(100)),0);
        if(b==192)midi.addEvent(juce::MidiMessage::noteOff(1,60),block/2);
        p.processBlock(audio,midi);
        for(int i=0;i<block;++i){const auto f=audio.getSample(0,i);require(std::isfinite(f)&&std::isfinite(audio.getSample(1,i)),"Nonfinite audio");result.push_back(f);energy+=double(f)*f;}
        for(const auto event:midi){require(event.samplePosition>=0&&event.samplePosition<block,"Illegal MIDI timestamp");++midiEvents;}
        host.ppq+=double(block)*2.0/rate;
    }
    p.setPlayHead(nullptr);p.releaseResources();
    require(energy>1.e-8,"Silent MIDI render");
    require(!requireMidi || midiEvents>0,"No hosted MIDI output");
    std::printf("HEADLESS_RENDER rate=%.0f block=%d samples=%zu midi=%d energy=%.9g\n",rate,block,result.size(),midiEvents,energy);
    return result;
}
}
int main(int argc,char** argv) {
    try {
        require(prepareNativeOutput(),"Output setup");require(argc==3,"Arguments: VST3 bundle metadata-output");
        juce::ScopedJuceInitialiser_GUI infrastructure;
        const juce::File bundle(argv[1]);
        {
            juce::AudioPluginFormatManager manager;manager.addFormat(std::make_unique<juce::VST3PluginFormat>());
            juce::OwnedArray<juce::PluginDescription> descriptions;manager.getFormat(0)->findAllTypesForFile(descriptions,bundle.getFullPathName());
            require(descriptions.size()==1,"VST3 discovery/factory");
            juce::String error;
            auto create=[&]{auto p=manager.createPluginInstance(*descriptions[0],48000,128,error);require(p!=nullptr,"VST3 instance creation");return p;};
            auto first=create();auto second=create();
            std::printf("HOSTED_PARAMETER_COUNT %d\n",first->getParameters().size());
            // 95 public parameters, bypass/program, and 16 x130 JUCE MIDI proxies.
            require(first->getParameters().size()==95+2+16*130,"Hosted parameter count");
            juce::Array<juce::var> metadata;std::set<juce::String> ids;
            for(auto* p:first->getParameters()){
                auto* hosted=dynamic_cast<juce::HostedAudioProcessorParameter*>(p);require(hosted!=nullptr,"Hosted parameter metadata");
                require(ids.insert(hosted->getParameterID()).second,"Duplicate hosted parameter ID");
                auto* row=new juce::DynamicObject;row->setProperty("id",hosted->getParameterID());row->setProperty("name",p->getName(128));
                row->setProperty("default",p->getDefaultValue());row->setProperty("steps",p->getNumSteps());row->setProperty("automatable",p->isAutomatable());metadata.add(juce::var(row));
            }
            require(juce::File(argv[2]).replaceWithText(juce::JSON::toString(juce::var(metadata))),"Metadata export");
            booleanState(*first);
            juce::MemoryBlock state;first->getStateInformation(state);require(state.getSize()>0,"State capture");
            second->setStateInformation(state.getData(),static_cast<int>(state.getSize()));
            auto a=render(*first,48000,128);auto b=render(*second,48000,128);
            require(a==b,"Independent-instance deterministic audio");
            first.reset();render(*second,96000,64);render(*second,44100,256);second.reset();
            auto reopened=create();reopened->setStateInformation(state.getData(),static_cast<int>(state.getSize()));
            auto c=render(*reopened,48000,128);require(a==c,"Fresh instance state/audio restoration");
            parameter(*reopened,"Sequencer Mode").setValueNotifyingHost(1.f);
            parameter(*reopened,"Sequencer Clock").setValueNotifyingHost(1.f);
            parameter(*reopened,"MIDI Out").setValueNotifyingHost(1.f);
            render(*reopened,48000,128,true);
            require(reopened->getActiveEditor()==nullptr,"Unexpected editor construction");
        }
        const auto dll=bundle.getChildFile("Contents/x86_64-win/Swara XT.vst3");
        require(GetModuleHandleW(dll.getFullPathName().toWideCharPointer())==nullptr,"VST3 module remains loaded");
        std::puts("WINDOWS_HEADLESS_VST3_PASS editor=never-created module=unloaded");return 0;
    }catch(const std::exception& e){std::fprintf(stderr,"FAIL: %s\n",e.what());return 1;}
}
