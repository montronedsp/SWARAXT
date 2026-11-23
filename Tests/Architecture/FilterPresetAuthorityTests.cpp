// Copyright 2026 MontroneDSP. SPDX-License-Identifier: GPL-3.0-or-later
#include <JuceHeader.h>
#include "Plugin/PluginProcessor.h"
#include "Plugin/PluginEditor.h"
#include "Plugin/SwaraXtParameterLayout.h"
#include <cstring>
#include "Plugin/ShruthiFactoryPresets.h"
#include "Plugin/ShruthiFactoryPresetData.h"
#include "Ui/SwaraXtPanels.h"
#include <filesystem>
#include <fstream>
#include <iostream>
#include <iomanip>
#include <stdexcept>

namespace {
void check(bool ok, const char* message) { if (!ok) throw std::runtime_error(message); }
void set(SwaraXtAudioProcessor& p, const char* id, float value) {
    auto* parameter=p.getApvts().getParameter(id); check(parameter!=nullptr,"parameter exists");
    parameter->setValueNotifyingHost(parameter->convertTo0to1(value));
}
float get(SwaraXtAudioProcessor& p, const char* id) {
    auto* value=p.getApvts().getRawParameterValue(id); return value ? value->load() : 0;
}
class Clock final : public juce::AudioPlayHead {
public:
    int64_t samples=0;
    juce::Optional<PositionInfo> getPosition() const override {
        PositionInfo info; info.setIsPlaying(true); info.setBpm(120);
        info.setTimeInSamples(samples); info.setTimeInSeconds(static_cast<double>(samples)/48000);
        info.setPpqPosition(static_cast<double>(samples)/24000); return info;
    }
};
struct Capture {
    SwaraXtAudioProcessor* processor=nullptr;
    std::ofstream trajectory, native;
    static void sink(void* context, const swaraxt::SwaraXtEngine::DebugBlockCapture& block) {
        auto& c=*static_cast<Capture*>(context); auto& engine=c.processor->engineForTests();
        const auto& v=engine.shruthiPart().voice();
        const auto& params=engine.filter().paramsForTests();
        c.trajectory<<block.nativeBlockIndex<<','<<static_cast<int>(v.cutoff())<<','
            <<static_cast<int>(v.modulation_source(shruthi::MOD_SRC_ENV_1))<<','
            <<static_cast<int>(v.modulation_source(shruthi::MOD_SRC_LFO_2))<<','
            <<params.boardCutoffCvVolts<<','<<engine.filter().boardCutoffCvForTests()<<','
            <<v.filter_pitch_value()<<'\n';
        c.native.write(reinterpret_cast<const char*>(block.postShruthiMixer),sizeof(block.postShruthiMixer));
    }
};
void capture(const std::filesystem::path& folder, int only=-1, int env=-1, int lfo=-1) {
    std::filesystem::create_directories(folder);
    std::ofstream metadata(folder/"presets.csv");
    metadata<<"index,name,native_env,native_lfo,key_track\n";
    SwaraXtAudioProcessor catalog;
    for(int program=0;program<catalog.getNumPrograms();++program) {
        if(only>=0 && program!=only) continue;
        if(only==-2 && program>=11 && program<51) continue;
        SwaraXtAudioProcessor p; p.setCurrentProgram(program);
        if(env>=0) set(p,"filter.shruthi_env",static_cast<float>(env));
        if(lfo>=0) set(p,"filter.shruthi_lfo",static_cast<float>(lfo));
        Clock clock; p.setPlayHead(&clock); p.prepareToPlay(48000,128);
        const auto prefix=std::to_string(program);
        Capture c; c.processor=&p;
        c.trajectory.open(folder/(prefix+"-cv.csv")); c.trajectory<<std::setprecision(17);
        c.trajectory<<"native_block,cutoff,env1,lfo2,target_cv,board_cv,pitch\n";
        c.native.open(folder/(prefix+"-native.f32"),std::ios::binary);
        p.engineForTests().setDebugTapSink(&c,&Capture::sink);
        std::ofstream host(folder/(prefix+"-host.f32"),std::ios::binary);
        juce::AudioBuffer<float> buffer(2,128);
        for(int block=0;block<2250;++block) {
            juce::MidiBuffer midi;
            if(block==0) midi.addEvent(juce::MidiMessage::noteOn(1,60,juce::uint8(110)),0);
            if(block==750) midi.addEvent(juce::MidiMessage::noteOn(1,48,juce::uint8(100)),0);
            if(block==1500) { midi.addEvent(juce::MidiMessage::noteOff(1,48),0); midi.addEvent(juce::MidiMessage::noteOff(1,60),0); }
            p.processBlock(buffer,midi); clock.samples+=128;
            for(int i=0;i<128;++i) check(std::isfinite(buffer.getSample(0,i)),"factory render finite");
            host.write(reinterpret_cast<const char*>(buffer.getReadPointer(0)),128*sizeof(float));
        }
        metadata<<program<<','<<p.getProgramName(program)<<','<<get(p,"filter.shruthi_env")<<','
            <<get(p,"filter.shruthi_lfo")<<','<<get(p,"filter_key_tracking")<<'\n';
        const auto& patch=p.engineForTests().shruthiPart().patch();
        std::ofstream bytes(folder/(prefix+"-patch.bin"),std::ios::binary);
        bytes.write(reinterpret_cast<const char*>(&patch),sizeof(patch));
        check(host.good()&&c.native.good()&&c.trajectory.good()&&bytes.good(),"factory artifacts saved");
        std::cout<<"captured "<<program<<' '<<p.getProgramName(program)<<'\n';
        p.engineForTests().setDebugTapSink(nullptr,nullptr); p.setPlayHead(nullptr);
    }
}
juce::Slider* depthSlider(juce::Component& component, const juce::String& tooltip) {
    if (auto* slider=dynamic_cast<juce::Slider*>(&component))
        if (slider->getTooltip()==tooltip) return slider;
    for (auto* child:component.getChildren())
        if (auto* slider=depthSlider(*child,tooltip)) return slider;
    return nullptr;
}
void publish(SwaraXtAudioProcessor& processor) {
    juce::AudioBuffer<float> audio(2,128); juce::MidiBuffer midi;
    processor.processBlock(audio,midi);
    juce::MessageManager::getInstance()->runDispatchLoopUntil(10);
}
void factoryVisibilityAndState() {
    using namespace swaraxt;
    SwaraXtAudioProcessor processor;
    for (const auto* id:{IDs::filterEnvDepth,IDs::filterLfoDepth}) {
        auto* parameter=processor.getApvts().getParameter(id);
        check(parameter && parameter->convertFrom0to1(0)==0 && parameter->convertFrom0to1(1)==63,
              "native direct depths have the original 0..63 user domain");
    }
    check(processor.getApvts().getParameter(IDs::filterEnvDepth)->convertFrom0to1(
          processor.getApvts().getParameter(IDs::filterEnvDepth)->getDefaultValue())==32,"native ENV default 32");
    check(processor.getApvts().getParameter(IDs::filterLfoDepth)->convertFrom0to1(
          processor.getApvts().getParameter(IDs::filterLfoDepth)->getDefaultValue())==0,"native LFO default zero");
    check(!processor.getApvts().getParameter("filter_env_amount") &&
          !processor.getApvts().getParameter("filter_mod_amount"),"retired parameters absent from host schema");
    processor.prepareToPlay(48000,128);
    SwaraXtAudioProcessorEditor editor(processor);
    auto* env=depthSlider(editor,"ENV 1 -> Filter Cutoff");
    auto* lfo=depthSlider(editor,"LFO 2 -> Filter Cutoff");
    check(env && lfo,"visible FILTER controls attach to native depths with explicit source tooltips");
    int withEnv=0,withLfo=0,neither=0;
    for (std::size_t index=0;index<kShruthiFactoryPresetCount;++index) {
        const auto& record=kShruthiFactoryPresets[index];
        shruthi::Patch expected{};
        check(ShruthiFactoryPresets::decodePatch(record.bytes.data(),record.bytes.size(),expected),"factory bytes decode");
        processor.setCurrentProgram(kShruthiFactoryPresetStart+static_cast<int>(index)); publish(processor);
        check(get(processor,IDs::filterEnvDepth)==expected.filter_env &&
              get(processor,IDs::filterLfoDepth)==expected.filter_lfo,"factory APVTS exposes the original depths");
        check(env->getValue()==expected.filter_env && lfo->getValue()==expected.filter_lfo,
              "open editor follows every imported preset without reopening");
        const auto& patch=processor.engineForTests().shruthiPart().patch();
        check(patch.filter_env==expected.filter_env && patch.filter_lfo==expected.filter_lfo,"PatchBridge retains exact native depths");
        withEnv+=expected.filter_env!=0; withLfo+=expected.filter_lfo!=0;
        neither+=expected.filter_env==0 && expected.filter_lfo==0;
    }
    set(processor,IDs::filterEnvDepth,0); set(processor,IDs::filterLfoDepth,0); publish(processor);
    check(env->getValue()==0 && lfo->getValue()==0,"host automation updates both visible controls");
    env->setValue(47,juce::sendNotificationSync); lfo->setValue(40,juce::sendNotificationSync); publish(processor);
    check(get(processor,IDs::filterEnvDepth)==47 && get(processor,IDs::filterLfoDepth)==40,
          "GUI depths reach APVTS and faithful patch");
    auto state=processor.getApvts().copyState();
    for (const auto* id:{"filter_env_amount","filter_mod_amount"}) {
        juce::ValueTree obsolete("PARAM"); obsolete.setProperty("id",id,nullptr);
        obsolete.setProperty("value",1.0f,nullptr); state.addChild(obsolete,-1,nullptr);
    }
    juce::MemoryBlock bytes; juce::AudioProcessor::copyXmlToBinary(*state.createXml(),bytes);
    processor.setStateInformation(bytes.getData(),static_cast<int>(bytes.getSize())); publish(processor);
    const auto restored=processor.getApvts().copyState();
    check(!restored.getChildWithProperty("id","filter_env_amount").isValid() &&
          !restored.getChildWithProperty("id","filter_mod_amount").isValid(),"obsolete state records discarded without retaining a second authority");
    check(get(processor,IDs::filterEnvDepth)==47 && get(processor,IDs::filterLfoDepth)==40,
          "active native depths survive state restoration");
    std::cout<<"official imports=40 nonzero ENV="<<withEnv<<" nonzero LFO="<<withLfo<<" zero/zero="<<neither<<'\n';
}
std::vector<float> renderDepths(int env,int lfo) {
    using namespace swaraxt;
    SwaraXtAudioProcessor p; p.setCurrentProgram(14); // woblbass: ENV 26 / LFO 40.
    set(p,IDs::filterEnvDepth,static_cast<float>(env)); set(p,IDs::filterLfoDepth,static_cast<float>(lfo));
    p.prepareToPlay(48000,128);
    publish(p);
    const auto matrix=p.engineForTests().shruthiPart().patch().modulation_matrix;
    std::vector<float> result; juce::AudioBuffer<float> audio(2,128); juce::MidiBuffer midi;
    midi.addEvent(juce::MidiMessage::noteOn(1,60,juce::uint8(110)),0);
    for (int block=0;block<400;++block) {
        p.processBlock(audio,midi); midi.clear();
        const auto& engine=p.engineForTests(); const auto& voice=engine.shruthiPart().voice();
        const auto& params=engine.filter().paramsForTests();
        const double expected=std::clamp((voice.cutoff()+24.0*params.matrixCutoffOctaves)*5.0/255.0,0.0,5.0);
        check(std::abs(params.boardCutoffCvVolts-expected)<1.e-12,"only native cutoff and independent tracking trim reach board CV");
        check(engine.shruthiPart().patch().filter_env==env && engine.shruthiPart().patch().filter_lfo==lfo,"visible zero removes the dedicated native depth");
        result.insert(result.end(),audio.getReadPointer(0),audio.getReadPointer(0)+128);
    }
    check(std::memcmp(&matrix,&p.engineForTests().shruthiPart().patch().modulation_matrix,sizeof(matrix))==0,
          "direct depth edits never mutate modulation matrix rows");
    return result;
}
void dedicatedDepthRegression() {
    const auto zero=renderDepths(0,0);
    check(zero!=renderDepths(26,0),"ENV alone restores audible dedicated modulation");
    check(zero!=renderDepths(0,40),"LFO alone restores audible dedicated modulation");
    const auto both=renderDepths(26,40);
    check(both!=zero && both==renderDepths(26,40),"restored direct depths reproduce deterministic audible modulation");
}

}
int main(int argc,char** argv) {
    juce::ScopedJuceInitialiser_GUI init;
    try {
        if(argc>=3 && juce::String(argv[1])=="--capture") {
            capture(argv[2],argc>3?std::stoi(argv[3]):-1,argc>4?std::stoi(argv[4]):-1,argc>5?std::stoi(argv[5]):-1);
            return 0;
        }
        factoryVisibilityAndState(); dedicatedDepthRegression();
        std::cout<<"Filter preset authority PASS\n";
        return 0;
    } catch(const std::exception& error) { std::cerr<<"FAIL: "<<error.what()<<'\n';return 1; }
}
