// Copyright2026 MontroneDSP. SPDX-License-Identifier: GPL-3.0-or-later
// Existing exhaustive arithmetic assertions are reused; no GUI path is called.
#define main swaraxtControlIntegrityUnusedMain
#include "../Regression/ControlIntegrityTestsMain.cpp"
#undef main

Capture currentMixedCapture(int dest,int level,const int (&amounts)[4]) {
    pump();P p;p.setFilterQuality(swaraxt::FilterQuality::eco);p.prepareToPlay(48000,128);
    set(p,swaraxt::IDs::dspFxProgram,0);set(p,swaraxt::IDs::filterCutoff,20000);set(p,swaraxt::IDs::seqMode,0);
    set(p,swaraxt::IDs::mixSub,0);set(p,swaraxt::IDs::mixNoise,0);
    set(p,dest==shruthi::MOD_DST_MIX_SUB_OSC?swaraxt::IDs::mixSub:swaraxt::IDs::mixNoise,float(level));
    for(int row=1;row<=12;++row)set(p,("mod.row"+juce::String(row)+".amount").toRawUTF8(),0);
    for(int row=0;row<4;++row) {
        const auto key="mod.row"+juce::String(row+1);
        set(p,(key+".source").toRawUTF8(),shruthi::MOD_SRC_OFFSET);
        set(p,(key+".destination").toRawUTF8(),float(dest));
        set(p,(key+".amount").toRawUTF8(),float(amounts[row]));
    }
    p.engineForTests().reset();p.engineForTests().applyParameters();
    Capture result;juce::AudioBuffer<float> audio(2,128);juce::MidiBuffer midi;midi.ensureSize(2048);
    for(int block=0;block<16;++block) {
        midi.clear();audio.clear();
        if(block==0)midi.addEvent(juce::MidiMessage::noteOn(1,60,juce::uint8(100)),0);
        p.processBlock(audio,midi);
        for(int sample=0;sample<128;++sample) {
            const float value=audio.getSample(0,sample);
            check(std::isfinite(value)&&std::isfinite(audio.getSample(1,sample)),"mixed-row stereo samples finite");
            result.audio[std::size_t(block*128+sample)]=value;
        }
    }
    result.dst=p.engineForTests().shruthiPart().voice().debug_destination14(uint8_t(dest));
    if(const char* folder=std::getenv("SWARA_CONTROL_CAPTURE_DIR")) {
        juce::String name="mixed-"+juce::String(dest)+"-"+juce::String(level);
        for(int amount:amounts)name+="-"+juce::String(amount);
        check(juce::File(juce::String(folder)).getChildFile(name+".f32").replaceWithData(result.audio.data(),sizeof(result.audio)),"save mixed-row private capture");
    }
    return result;
}
void currentMixedRows() {
    const int patterns[6][4]={{63,-63,0,0},{-63,63,0,0},{63,-63,63,-63},{-63,63,-63,63},{1,-1,2,-2},{-1,1,-2,2}};
    int distinguishesFinalClamp=0;
    for(int dest:{shruthi::MOD_DST_MIX_SUB_OSC,shruthi::MOD_DST_MIX_NOISE})
        for(int level:{0,1,2,63,64,126,127})for(const auto& pattern:patterns) {
            const auto actual=currentMixedCapture(dest,level,pattern);
            int expected=(level/2)*256;int unbounded=expected;
            for(int amount:pattern) {
                expected=std::clamp(expected+amount*255,0,16383);
                unbounded+=amount*255;
            }
            check(actual.dst==expected,"mixed row order and per-row saturation match independent arithmetic");
            distinguishesFinalClamp+=expected!=std::clamp(unbounded,0,16383);
            std::printf("CURRENT_MIXED dest=%d public=%d amounts=%d/%d/%d/%d actual=%d expected=%d\n",dest,level,pattern[0],pattern[1],pattern[2],pattern[3],actual.dst,expected);
        }
    check(distinguishesFinalClamp>0,"mixed-row controls reject a single final clamp implementation");
}

int main(int argc,char** argv) {
    juce::ScopedJuceInitialiser_GUI init;
    const juce::String mode=argc>1?argv[1]:"mixer";
    if(mode=="factory")factoryImport();else if(mode=="numeric")numeric();else if(mode=="text")text();else if(mode=="parity")parity();else {mixer();currentMixedRows();}
    std::printf("CURRENT_MIXER checks=%d failures=%d\n",checks,failures);
    return failures?1:0;
}
