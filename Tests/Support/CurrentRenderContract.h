// Copyright 2026 MontroneDSP. SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
#include <cstdlib>
#include <cstdio>
#include <vector>
#include <cmath>
namespace swaraxt::test {
template<class Check>
void currentRenderInput(SwaraXtAudioProcessor& p, double rate, int block, int samples,
                        int note, bool release, Check check) {
    p.engineForTests().applyParameters();
    const auto& patch=p.engineForTests().shruthiPart().patch();
    const int sub=int(p.getApvts().getRawParameterValue("mix.sub")->load());
    const int noise=int(p.getApvts().getRawParameterValue("mix.noise")->load());
    check(sub>=0 && sub<=127 && noise>=0 && noise<=127,"current public mixer bounds");
    // Independent nonnegative integer division oracle; do not call PatchBridge's shift.
    check(patch.mix_sub_osc==sub/2 && patch.mix_noise==noise/2,"current independent mixer conversion");
    if(const char* directory=std::getenv("SWARA_CURRENT_REFERENCE_DIR")) {
        static int index=0;
        juce::DynamicObject::Ptr config=new juce::DynamicObject;
        config->setProperty("rate",rate);config->setProperty("block",block);
        config->setProperty("samples",samples);config->setProperty("channels",2);
        config->setProperty("captured_channel",0);config->setProperty("note",note);
        config->setProperty("release",release);config->setProperty("sub_internal",int(patch.mix_sub_osc));
        config->setProperty("noise_internal",int(patch.mix_noise));
        juce::DynamicObject::Ptr values=new juce::DynamicObject;
        for(auto* parameter:p.getParameters()) {
            auto* ranged=dynamic_cast<juce::RangedAudioParameter*>(parameter);
            check(ranged!=nullptr,"current reference parameter type");
            if(ranged)values->setProperty(ranged->paramID,ranged->convertFrom0to1(ranged->getValue()));
        }
        config->setProperty("parameters",juce::var(values.get()));
        const auto file=juce::File(juce::String(directory)).getChildFile("input-"+juce::String(index++)+".json");
        check(file.replaceWithText(juce::JSON::toString(juce::var(config.get()))),"write reference input provenance");
    }
}
template<class Check>
void currentRenderOutput(const char* name,const std::vector<float>& audio,Check check) {
    bool finite=true;
    for(float x:audio)finite=finite && std::isfinite(x);
    check(finite,"current reference samples finite");
    if(const char* directory=std::getenv("SWARA_CURRENT_REFERENCE_DIR")) {
        const auto file=juce::File(juce::String(directory)).getChildFile(juce::String(name)+".f32");
        check(file.replaceWithData(audio.data(),audio.size()*sizeof(float)),"write current raw float reference");
    }
}
} // namespace swaraxt::test
