// Copyright 2026 MontroneDSP.
// SPDX-License-Identifier: GPL-3.0-or-later
// Compile-time native-only entry point; original GUI main remains available.
#include <JuceHeader.h>
#include "Plugin/PluginProcessor.h"
#include <vector>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstdint>
#include <stdexcept>
#include <limits>
#include <string>
#if !defined(SWARAXT_DUAL_SVF_HEADLESS)
#error This target requires the explicit headless compile-time configuration
#endif

namespace {
std::vector<float> headlessParameters;
std::uint64_t headlessSelection = 0;
int headlessSamples = 0;
std::string negativeControl;
bool matrixCase = false;
void headlessCheck(bool okay, const char* reason)
{
    if (!okay) throw std::runtime_error(reason);
}
void headlessConfigured(SwaraXtAudioProcessor& p,
    swaraxt::ShruthiSvfSection2Control control, int cut, int res,
    int route, int calibration, int m1, int m2)
{
    const auto selection=p.experimentalSvfSelection();
    headlessCheck(selection.enabled && selection.control2==control
        && int(selection.cutoff2)==cut && int(selection.resonance2)==res
        && int(selection.routing)==route && int(selection.calibration)==calibration
        && int(selection.mode1)==m1 && int(selection.mode2)==m2,"selected mode/calibration/control mismatch");
    headlessSelection=swaraxt::ExperimentalSvfSelection::pack(selection);
    headlessParameters.clear();
    for (auto* parameter:p.getParameters()) headlessParameters.push_back(parameter->getValue());
    headlessSamples=0;
}
void headlessBlock(SwaraXtAudioProcessor& p,juce::AudioBuffer<float>& buffer)
{
    if (matrixCase && headlessSamples==0 && negativeControl=="nonfinite")
        buffer.setSample(0,0,std::numeric_limits<float>::quiet_NaN());
    if (matrixCase && headlessSamples==0 && negativeControl=="changed-sample")
        buffer.setSample(0,0,buffer.getSample(0,0)+0.125f);
    for (int channel=0;channel<buffer.getNumChannels();++channel)
        for (int sample=0;sample<buffer.getNumSamples();++sample)
            headlessCheck(std::isfinite(buffer.getSample(channel,sample)),"nonfinite audio before Q20 conversion");
    headlessCheck(buffer.getNumChannels()==2 && buffer.getNumSamples()==256,"invalid audio block layout/count");
    headlessCheck(swaraxt::ExperimentalSvfSelection::pack(p.experimentalSvfSelection())==headlessSelection,
        "unexpected Section2 selection change");
    if (matrixCase) {
        const auto& patch=p.engineForTests().shruthiPart().patch();
        const int sub=int(p.getApvts().getRawParameterValue("mix.sub")->load());
        const int noise=int(p.getApvts().getRawParameterValue("mix.noise")->load());
        headlessCheck(p.getCurrentProgram()==0 && sub==64 && noise==0,"current factory0 mixer fixture");
        // Independent division oracle; do not call PatchBridge's shift helper.
        headlessCheck(int(patch.mix_sub_osc)==sub/2 && int(patch.mix_noise)==noise/2,
            "current firmware mixer conversion");
    }
    headlessSamples+=buffer.getNumSamples();
}
void headlessFinished(SwaraXtAudioProcessor& p)
{
    headlessCheck(headlessSamples==25600,"invalid rendered sample count");
    headlessCheck(p.getParameters().size()==int(headlessParameters.size()),"parameter count changed");
    for (int i=0;i<p.getParameters().size();++i)
        headlessCheck(p.getParameters()[i]->getValue()==headlessParameters[std::size_t(i)],"unexpected DSP parameter change");
}
}

// Reuse the exact production render/control functions. GUI include/function,
// legacy main and legacy print records are removed by compile-time guards.
#include "DualSvfSection2TestsMain.cpp"

#if defined(__linux__)
extern "C" void __wrap__ZN4juce9ComponentC1Ev(void*);
extern "C" void __wrap__ZN4juce9ComponentC2Ev(void*);
extern "C" void __wrap__ZN4juce9ComponentC1Ev(void*) { std::fputs("FAIL: forbidden Component construction\n",stderr); std::abort(); }
extern "C" void __wrap__ZN4juce9ComponentC2Ev(void*) { std::fputs("FAIL: forbidden Component construction\n",stderr); std::abort(); }
#endif

int main(int argc,char** argv)
{
    try {
        if (argc==3 && std::string(argv[1])=="--negative") negativeControl=argv[2];
        else headlessCheck(argc==1,"unexpected command arguments");
        headlessCheck(negativeControl.empty() || negativeControl=="wrong-calibration"
            || negativeControl=="wrong-mode" || negativeControl=="nonfinite"
            || negativeControl=="changed-sample", "unknown negative control");
        // JUCE's Linux initialiser creates message infrastructure, no Component.
        // No dispatch loop, native window, editor or snapshot routine is called.
        juce::ScopedJuceInitialiser_GUI infrastructure;
        if (negativeControl.empty()) {
            referenceControls(); packedControls(); audioControls(); rateMatrix(); dormantTargets();
        }
        struct Case { const char* label; Control mode; int cut,res,m2; };
        const Case cases[]={{"Cut40_Res32",Control::independent,40,32,1},
            {"Cut90_Res32",Control::independent,90,32,1},{"Cut90_Res63",Control::independent,90,63,1},
            {"Minus12",Control::relative,52,0,0},{"Center64",Control::relative,64,0,0},{"Plus12",Control::relative,76,0,0}};
        int count=0;
        for (int calibration=0;calibration<2;++calibration) for (int index=0;index<6;++index) {
            const auto& c=cases[index]; const bool independent=c.mode==Control::independent;
            const auto expected=independent?dualIndependentGoldens[calibration][index]:dualRelativeGoldens[calibration][index-3];
            const int actualCalibration=negativeControl=="wrong-calibration"?1-calibration:calibration;
            const auto actualMode=negativeControl=="wrong-mode"?(independent?Control::relative:Control::independent):c.mode;
            matrixCase=true;
            const auto first=render(actualMode,c.cut,c.res,1,actualCalibration,0,c.m2);
            const auto second=render(actualMode,c.cut,c.res,1,actualCalibration,0,c.m2);
            matrixCase=false;
            headlessCheck(actualCalibration==calibration,"wrong calibration identity");
            headlessCheck(actualMode==c.mode,"wrong Section2 mode identity");
            headlessCheck(first.hash==second.hash,"non-deterministic Q20 render");
            expect(first.hash==expected,"unapproved Q20 audio fingerprint");
            std::printf("DUAL_SVF_HEADLESS {\"case\":\"Dual%s_%s_%s\",\"mode\":\"%s\",\"calibration\":\"%s\",\"samples\":%d,\"hash\":\"%016llx\",\"finite\":true,\"deterministic\":true,\"parameters_unchanged\":true,\"repeats\":2}\n",
                independent?"Independent":"Relative",calibration==0?"Paper":"Eagle",c.label,
                independent?"Independent":"Relative",calibration==0?"Paper":"Eagle",headlessSamples,
                static_cast<unsigned long long>(first.hash));
            ++count;
        }
        headlessCheck(count==12 && failures==0,"incorrect case count or shared assertions");
        std::puts("DUAL_SVF_HEADLESS_PASS cases=12 samples=25600 repeats=2 gui=excluded");
        return 0;
    } catch (const std::exception& error) { std::fprintf(stderr,"FAIL: %s\n",error.what()); return 1; }
}
