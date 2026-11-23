// Copyright 2026 MontroneDSP. SPDX-License-Identifier: GPL-3.0-or-later
// Current schema only. No historical session detection or parameter reinterpretation.
#include <JuceHeader.h>
#include "Plugin/PluginProcessor.h"
#include "../Support/ParameterContractV2.h"
#include <array>
#include <cmath>
#include <cstdio>
#include <vector>

namespace {
using P=SwaraXtAudioProcessor;
int failures=0,checks=0;
void check(bool valid,const char* label) {
    ++checks;if(!valid){++failures;std::printf("FAIL: %s\n",label);}
}
void set(P& p,const char* id,int value) {
    auto* a=p.getApvts().getParameter(id);check(a!=nullptr,id);
    if(a)a->setValueNotifyingHost(a->convertTo0to1(float(value)));
}
std::vector<float> values(P& p) {
    std::vector<float> v;
    for(auto* a:p.getParameters()) {
        auto* r=dynamic_cast<juce::RangedAudioParameter*>(a);
        check(r!=nullptr,"current ranged parameter");
        v.push_back(r?r->convertFrom0to1(r->getValue()):0);
    }
    return v;
}
void internal(P& p,int sub,int noise,int arp) {
    p.engineForTests().applyParameters();
    const auto& patch=p.engineForTests().shruthiPart().patch();
    check(patch.mix_sub_osc==sub/2 && patch.mix_noise==noise/2,"current restored mixer independent division oracle");
    check(p.getApvts().getRawParameterValue("arp.pattern")->load()==arp,"current physical arp value restored");
    check(p.sequenceState().snapshot().arpPattern==arp,"current sequence transaction agrees");
}
void stateAndPreset() {
    P source,independent;
    source.prepareToPlay(48000,128);
    independent.prepareToPlay(48000,128);
    const auto independentBefore=values(independent);
    const auto dir=juce::File::getSpecialLocation(juce::File::userHomeDirectory).getChildFile("phase3-current-presets");
    check(dir.createDirectory().wasOk(),"private current preset directory");
    source.setUserPresetDirectoryForTests(dir);
    for(int n=0;n<128;++n) {
        set(source,"mix.sub",n);set(source,"mix.noise",127-n);set(source,"arp.pattern",n%16);
        const auto wanted=values(source);internal(source,n,127-n,n%16);
        juce::MemoryBlock bytes;source.getStateInformation(bytes);
        auto xml=juce::AudioProcessor::getXmlFromBinary(bytes.getData(),int(bytes.getSize()));
        check(xml!=nullptr,"current serialized XML present");
        if(xml) {
            check(xml->hasTagName(P::kStateRoot),"current state root");
            check(xml->getIntAttribute("stateVersion")==P::kStateVersion,"current state schema3");
        }
        P restored;restored.setStateInformation(bytes.getData(),int(bytes.getSize()));
        check(restored.lastRestoreResult()==P::RestoreResult::success,"current state restore succeeds");
        check(values(restored)==wanted,"every current physical parameter survives state");
        restored.prepareToPlay(48000,128);
        check(values(restored)==wanted,"current parameters survive preparation after restore");
        internal(restored,n,127-n,n%16);
        juce::String error;
        check(source.saveUserPreset("Phase3 current",true,error),"current user preset saved");
        const auto entries=source.getPresetEntries();
        const auto it=std::find_if(entries.begin(),entries.end(),[](const auto& entry){return !entry.isFactory && entry.name=="Phase3 current";});
        check(it!=entries.end(),"saved current preset discoverable");
        if(it!=entries.end()) {
            P loaded;loaded.setUserPresetDirectoryForTests(dir);
            check(loaded.loadPresetEntry(*it,error),"current user preset loaded");
            check(values(loaded)==wanted,"current preset preserves every parameter exactly");
            loaded.prepareToPlay(48000,128);
            check(values(loaded)==wanted,"current preset values survive preparation");
            internal(loaded,n,127-n,n%16);
        }
        check(values(independent)==independentBefore,"current multiple-instance independence");
        const auto before=values(restored);
        for(int truncated:{0,1,8}) {
            restored.setStateInformation(bytes.getData(),truncated);
            check(restored.lastRestoreResult()==P::RestoreResult::invalid,"truncated current state rejected");
            check(values(restored)==before,"invalid current state leaves complete state intact");
        }
        std::printf("CURRENT_STATE sub=%d noise=%d arp=%d bytes=%zu\n",n,127-n,n%16,bytes.getSize());
    }
}
void automationAndText() {
    P p;swaraxt::test::checkParameterContract(p,check);
    for(auto* a:p.getParameters()) {
        auto* r=dynamic_cast<juce::RangedAudioParameter*>(a);if(!r)continue;
        for(float unit:{0.f,.5f,1.f}) {
            r->setValueNotifyingHost(unit);
            check(std::isfinite(r->getValue()) && r->getValue()>=0 && r->getValue()<=1,"current normalized automation stays bounded");
            const auto text=r->getText(r->getValue(),128);
            const auto parsed=r->getValueForText(text);
            check(std::isfinite(parsed) && parsed>=0 && parsed<=1,"current text conversion is finite and normalized");
            check(r->getText(parsed,128)==text,"current display text is a stable parse-format roundtrip");
        }
    }
    for(int n=0;n<16;++n) {
        auto* a=p.getApvts().getParameter("arp.pattern");
        const auto unit=float(n)/15.f;a->setValueNotifyingHost(unit);
        check(a->convertFrom0to1(a->getValue())==n,"all16 current normalized automation steps");
        check(a->convertFrom0to1(a->getValueForText(a->getText(unit,128)))==n,"all16 arp text steps");
    }
}
}
int main() {
    juce::ScopedJuceInitialiser_GUI init;
    automationAndText();stateAndPreset();
    std::printf("CURRENT_CONTRACT checks=%d failures=%d\n",checks,failures);
    return failures?1:0;
}
