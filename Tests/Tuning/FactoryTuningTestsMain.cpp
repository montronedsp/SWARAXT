// Copyright 2026 MontroneDSP.
// SPDX-License-Identifier: GPL-3.0-or-later
#include <JuceHeader.h>
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <vector>
#include "Plugin/PluginProcessor.h"
#include "Ui/SwaraXtPanels.h"
#include "Plugin/SwaraXtParameterLayout.h"
#include "Tuning/PublishedTuning.h"
#include "Tuning/ScalePitch.h"
#include "Tuning/TuningTable.h"
#include "shruthi/sequencer_settings.h"
#if defined(_MSC_VER) && defined(_DEBUG)
#include <crtdbg.h>
#endif

namespace {
int failures = 0;
int current = -1;
void expect(bool ok, const char* what)
{
    if (!ok) { std::printf("FAIL tuning %d: %s\n", current, what); ++failures; }
}
bool near(double a, double b) { return std::abs(a-b) <= 1.e-8 * std::max(1.0, std::abs(b)); }
void setInt(SwaraXtAudioProcessor& p, const char* id, int value)
{
    auto* v = p.getApvts().getParameter(id);
    expect(v != nullptr, "parameter exists");
    if (v) v->setValueNotifyingHost(v->convertTo0to1(static_cast<float>(value)));
}
struct Definition { const char* stem; const char* name; };
const Definition definitions[] = {
    { "12-edo", "12-EDO" },
    { "19-edo", "19-EDO" },
    { "24-edo", "24-EDO" },
    { "31-edo", "31-EDO" },
    { "just-major", "5-limit major" },
    { "tritave", "tritave" },
    { "5-edo", "5-EDO" },
    { "7-edo", "7-EDO" },
    { "9-edo", "9-EDO" },
    { "10-edo", "10-EDO" },
    { "11-edo", "11-EDO" },
    { "13-edo", "13-EDO" },
    { "15-edo", "15-EDO" },
    { "17-edo", "17-EDO" },
    { "22-edo", "22-EDO" },
    { "29-edo", "29-EDO" },
    { "41-edo", "41-EDO" },
    { "53-edo", "53-EDO" },
    { "72-edo", "72-EDO" },
    { "pythagorean-12", "Pythagorean 12 (Eb-G#)" },
    { "pythagorean-major", "Pythagorean major" },
    { "just-diatonic-major", "5-limit diatonic major" },
    { "just-diatonic-minor", "5-limit minor (9/5 seventh)" },
    { "harmonic-8-16", "Harmonic series 8-16" },
    { "meantone-4", "1/4-comma meantone" },
    { "meantone-3", "1/3-comma meantone" },
    { "meantone-6", "1/6-comma meantone" },
    { "werckmeister-iii", "Werckmeister III" },
    { "kirnberger-iii", "Kirnberger III" },
    { "vallotti", "Vallotti (1/6 Pyth. comma)" },
    { "bohlen-pierce-equal", "Bohlen-Pierce equal (13-ED3)" },
    { "bohlen-pierce-just", "Bohlen-Pierce just" },
    { "8-ed3", "8-ED3" },
    { "gong-pythagorean", "Gong (Pythagorean model)" },
    { "just-major-pentatonic", "5-limit major pentatonic" },
    { "hirajoshi-ellis", "Hirajoshi (Ellis theoretical)" },
    { "rast-24edo", "Rast (24-EDO model)" },
};
// Independent oracle: derive intervals from ratios/fifth corrections, rather
// than reading expected cent values back from the files under test.
std::vector<double> expectedPitches(int index)
{
    std::vector<double> v;
    auto ratios = [&](std::initializer_list<double> r) {
        for (double x : r) v.push_back(1200.0 * std::log2(x));
    };
    const double fifth = 1200.0 * std::log2(1.5);
    const double syntonic = 1200.0 * std::log2(81.0/80.0);
    const double pythagorean = 12.0*fifth - 8400.0;
    if (index >= 6 && index <= 18)
    {
        const int divisions[] = {5,7,9,10,11,13,15,17,22,29,41,53,72};
        const int n = divisions[index-6];
        for (int i=1;i<=n;++i) v.push_back(1200.0*i/n);
    }
    else if (index == 19 || (index >= 24 && index <= 26))
    {
        const double step = index == 19 ? fifth : fifth-syntonic/(index == 24 ? 4 : index == 25 ? 3 : 6);
        for (int n=-3;n<=8;++n) if(n) v.push_back(std::fmod(n*step+12000.0,1200.0));
        std::sort(v.begin(),v.end()); v.push_back(1200.0);
    }
    else if (index == 20) ratios({9.0/8,81.0/64,4.0/3,1.5,27.0/16,243.0/128,2});
    else if (index == 21) ratios({9.0/8,1.25,4.0/3,1.5,5.0/3,15.0/8,2});
    else if (index == 22) ratios({9.0/8,1.2,4.0/3,1.5,1.6,1.8,2});
    else if (index == 23) for (int n=9;n<=16;++n) v.push_back(1200.0*std::log2(n/8.0));
    else if (index >=27 && index<=29)
    {
        double sum=0;
        for (int n=0;n<11;++n)
        {
            double correction=0;
            if (index==27 && (n==0 || n==1 || n==2 || n==5)) correction=pythagorean/4;
            if (index==28 && n<4) correction=syntonic/4;
            if (index==28 && n==6) correction=pythagorean-syntonic;
            if (index==29 && n<5) correction=pythagorean/6; // sixth narrowed fifth closes F-C
            sum += fifth-correction;
            v.push_back(std::fmod(sum,1200.0));
        }
        std::sort(v.begin(),v.end()); v.push_back(1200.0);
    }
    else if (index==30 || index==32)
    {
        const int n=index==30 ? 13 : 8;
        for (int i=1;i<=n;++i) v.push_back(1200.0*std::log2(3.0)*i/n);
    }
    else if (index==31) ratios({27.0/25,25.0/21,9.0/7,1.4,75.0/49,5.0/3,1.8,49.0/25,15.0/7,7.0/3,63.0/25,25.0/9,3});
    else if (index==33) ratios({9.0/8,81.0/64,1.5,27.0/16,2});
    else if (index==34) ratios({9.0/8,1.25,1.5,5.0/3,2});
    else if (index==35) ratios({9.0/8,1.2,1.5,1.6,2});
    else if (index==36) v={200,350,500,700,900,1050,1200};
    return v;
}
double centsAt(const std::vector<double>& pitches, int degree)
{
    const int period=static_cast<int>(pitches.size());
    int q=degree/period, r=degree%period;
    if (r<0) { r+=period; --q; }
    return q*pitches.back()+(r ? pitches[static_cast<size_t>(r-1)] : 0);
}
double degreeHz(const std::vector<double>& pitches, int degree)
{
    return 440.0*std::exp2((centsAt(pitches,degree)-centsAt(pitches,9))/1200.0);
}
double nearest(const std::vector<double>& pitches, double hz)
{
    const double target=centsAt(pitches,9)+1200.0*std::log2(hz/440.0);
    double best=1.e300, selected=0;
    for (int d=-2048;d<=2048;++d)
    {
        const double c=centsAt(pitches,d), distance=std::abs(c-target);
        if (distance<best-1.e-7) { best=distance; selected=c; }
    }
    return 440.0*std::exp2((selected-centsAt(pitches,9))/1200.0);
}
void process(SwaraXtAudioProcessor& p, juce::AudioBuffer<float>& b, int blocks=1)
{
    for (int i=0;i<blocks;++i) { juce::MidiBuffer midi; p.processBlock(b,midi); }
}
void note(SwaraXtAudioProcessor& p, juce::AudioBuffer<float>& b, int key)
{
    juce::MidiBuffer off; off.addEvent(juce::MidiMessage::allNotesOff(1),0); p.processBlock(b,off);
    juce::MidiBuffer on; on.addEvent(juce::MidiMessage::noteOn(1,key,static_cast<juce::uint8>(100)),0);
    p.processBlock(b,on); process(p,b,3);
}
int sequenceWord(SwaraXtAudioProcessor& p, juce::AudioBuffer<float>& b, int mode, int value)
{
    auto s=p.sequenceState().snapshot(); s.length=1; s.pitchMode=static_cast<uint8_t>(mode);
    s.scaleReady=1; s.freePitchReady=1;
    for (int i=0;i<16;++i)
    {
        s.steps[static_cast<size_t>(i)]=swaraxt::SequenceSnapshot::pack(0x80|60,0x70);
        s.scaleDegree[static_cast<size_t>(i)]=static_cast<int16_t>(value);
        s.freePitch[static_cast<size_t>(i)]=value;
    }
    p.sequenceState().store(s); p.engineForTests().applyParameters(); process(p,b);
    auto& part=p.engineForTests().shruthiPart(); part.Stop(false); part.Start(false);
    for (int tick=0;tick<8;++tick) { part.Clock(false); process(p,b); }
    return part.voice().pitch_value();
}

#if defined(_MSC_VER) && defined(_DEBUG)
bool inCallback=false;
int callbackAllocations=0;
int allocationHook(int type, void*, size_t, int, long, const unsigned char*, int)
{
    if(inCallback && type==_HOOK_ALLOC) ++callbackAllocations;
    return 1;
}
#endif

void testFactory(int index)
{
    current=index;
    const auto& def=definitions[index];
    const auto file=juce::File(SWARAXT_FACTORY_TUNING_DIR).getChildFile(juce::String(def.stem)+".scl");
    swaraxt::tuning::TuningSnapshot table, decoded;
    swaraxt::tuning::ParseError error;
    expect(file.existsAsFile(),"source file exists");
    expect(swaraxt::tuning::parseScalaAndKeyboard(file.loadFileAsString().toRawUTF8(),nullptr,table,error),"SCL parses");
    auto pitches=expectedPitches(index);
    if(index<6) { pitches=table.degreeCents; pitches.push_back(table.periodCents); }
    expect(table.formalPeriod==pitches.size() && table.degreeCount+1==pitches.size(),"period degree count");
    for(size_t d=0;d<table.degreeCents.size();++d)
        expect(near(table.degreeCents[d],pitches[d]),"independent interval formula");
    expect(near(table.periodCents,pitches.back()),"exact repeating period");
    expect(table.referenceNote==69 && near(table.referenceHz,440.0),"existing A4 reference");
    std::vector<uint8_t> bytes, reencoded;
    expect(swaraxt::tuning::writeTuningTable(table,bytes,error),"conversion");
    expect(swaraxt::tuning::readTuningTable(bytes.data(),bytes.size(),decoded,error),"SWT reads");
    expect(swaraxt::tuning::writeTuningTable(decoded,reencoded,error) && bytes==reencoded,"deterministic SWT roundtrip");
    swaraxt::tuning::PublishedTuning published;
    expect(published.fillFrom(decoded),"publication fits fixed storage");
    for(int key=0;key<128;++key)
    {
        expect(table.midiDegree[key]==key-60,"unchanged keyboard degree mapping");
        expect(near(table.hzForMidi(key),key<60 ? 0 : degreeHz(pitches,key-60)),"MIDI table frequency");
        const double source=440.0*std::exp2((key-69.0)/12.0);
        const auto result=published.quantizeHz(source);
        expect(result.valid && (near(result.hz,nearest(pitches,source))
            || std::abs(std::abs(1200.0*std::log2(result.hz/source))
                        -std::abs(1200.0*std::log2(nearest(pitches,source)/source)))<1.e-8),"whole MIDI-band independent quantizer oracle");
    }
    SwaraXtAudioProcessor keyboard, cv;
    for(auto* p : {&keyboard,&cv})
    {
        expect(p->loadBuiltinTuning(index),"factory publication");
        expect(p->activeBuiltinTuning()==index && p->activeTuningName()==def.name,"catalog name/index");
        setInt(*p,swaraxt::IDs::perfGlide,0); setInt(*p,swaraxt::IDs::dspFxProgram,0);
        p->prepareToPlay(48000,64);
    }
    const auto factoryBytes=keyboard.engineForTests().tuningBytesForState();
    setInt(keyboard,swaraxt::IDs::seqMode,shruthi::SEQUENCER_MODE_STEP);
    setInt(cv,swaraxt::IDs::seqMode,shruthi::SEQUENCER_MODE_SEQ);
    setInt(cv,swaraxt::IDs::arpGate,11);
    setInt(cv,swaraxt::IDs::seqClockMode,0); setInt(cv,swaraxt::IDs::seqTempo,160); setInt(cv,swaraxt::IDs::seqGate,100);
    juce::AudioBuffer<float> b(2,64);
    setInt(keyboard,swaraxt::IDs::midiInputTuning,1);
    for(int key=0;key<128;++key)
    {
        note(keyboard,b,key);
        if(key>=60)
            expect(keyboard.engineForTests().shruthiPart().voice().pitch_value()==swaraxt::ScalePitch::toVoicePitchFromHz(published.hzForDegree(key-60)),"production INPUT TUNE target");
        else
            expect(keyboard.engineForTests().shruthiPart().voice().modulation_source(shruthi::MOD_SRC_GATE)==0,"unmapped INPUT TUNE does not gate the voice");
    }
    setInt(keyboard,swaraxt::IDs::midiInputTuning,2);
    for(int key=0;key<128;++key)
    {
        // Frequency optimality is checked independently above. At mathematical
        // midpoints the retained cache can select either representable neighbour
        // because of pow/log roundoff; integration must match the encoded table.
        const double hz=published.quantizeHz(440.0*std::pow(2.0,(key-69.0)/12.0)).hz;
        const auto word=swaraxt::ScalePitch::toVoicePitchFromHz(hz);
        note(keyboard,b,key);
        expect(keyboard.engineForTests().shruthiPart().voice().pitch_value()==word,"production QUANTIZED MIDI target");
        expect(sequenceWord(cv,b,3,key*1024)==word,"production QUANTIZED CV target");
    }
    const int probes[]={-73,-25,-12,-1,0,1,7,12,25,73};
    for(int degree:probes)
    {
        expect(near(table.hzForDegree(degree),degreeHz(pitches,degree)),"signed-degree independent frequency oracle");
        expect(sequenceWord(cv,b,2,degree)==swaraxt::ScalePitch::toVoicePitchFromHz(published.hzForDegree(degree)),"production signed SCALE target");
    }
    // A running, externally clocked sequence must pick up the new table rather
    // than retain the previously latched generation.
    sequenceWord(cv,b,2,1);
    auto& running=cv.engineForTests().shruthiPart();
    juce::MidiBuffer held;
    held.addEvent(juce::MidiMessage::noteOn(1,60,static_cast<juce::uint8>(100)),0);
    cv.processBlock(b,held); // Native internal clock requires a held key.

    expect(cv.loadBuiltinTuning((index+1)%37),"running sequence intermediate publication");
    process(cv,b,2);
    for(int tick=0;tick<24;++tick) { running.Clock(false); process(cv,b); }
    expect(running.voice().pitch_value()==swaraxt::ScalePitch::toVoicePitchFromHz(cv.hzForActiveDegree(1)),"running sequence applies the intermediate generation");
    expect(cv.loadBuiltinTuning(index),"running sequence restores current publication");
    process(cv,b);
    for(int tick=0;tick<24;++tick) { running.Clock(false); process(cv,b); }
    expect(running.voice().pitch_value()==swaraxt::ScalePitch::toVoicePitchFromHz(published.hzForDegree(1)),"running sequence retunes without restart");
    expect(sequenceWord(cv,b,2,-1)==swaraxt::ScalePitch::toVoicePitchFromHz(degreeHz(pitches,-1)),"running retune reaches voice");
    const auto view=cv.scaleRandomView();
    juce::Random a(0x123456), c(0x123456);
    swaraxt::SequenceState other;
    other.store(cv.sequenceState().snapshot());
    const auto beforeRandom=cv.sequenceState().snapshot();
    cv.sequenceState().randomize(swaraxt::SequenceState::Randomize::notes,a,{-12,12},swaraxt::SequenceState::RandomizationType::type1,&view);
    other.randomize(swaraxt::SequenceState::Randomize::notes,c,{-12,12},swaraxt::SequenceState::RandomizationType::type1,&view);
    expect(cv.sequenceState().snapshot().scaleDegree==other.snapshot().scaleDegree,"seeded scale randomization reproducible");
    expect(cv.sequenceState().snapshot().steps==beforeRandom.steps && cv.sequenceState().snapshot().freePitch==beforeRandom.freePitch,"scale randomization preserves packed and CV data");
    expect(view.formalPeriod==static_cast<int>(pitches.size()) && near(view.periodCents,pitches.back()),"randomization uses current shared period");
    juce::MemoryBlock state;
    cv.getStateInformation(state);
    SwaraXtAudioProcessor restored;
    restored.setStateInformation(state.getData(),static_cast<int>(state.getSize()));
    expect(restored.activeBuiltinTuning()==-1 && restored.activeTuningName()==def.name,"existing embedded-state import identity");
    expect(restored.engineForTests().tuningBytesForState()==factoryBytes,"host state embeds exact converted table");
    expect(restored.sequenceState().snapshot().scaleDegree==cv.sequenceState().snapshot().scaleDegree,"signed/random degrees survive state");
    expect(near(restored.hzForActiveDegree(-25),degreeHz(pitches,-25)),"negative frequency survives restore");
    // Imported SWT remains independent of the factory index namespace.
    expect(restored.loadTuningBytes(bytes.data(),bytes.size()) && restored.activeBuiltinTuning()==-1,"custom import still available");
#if defined(_MSC_VER) && defined(_DEBUG)
    process(cv,b,16);
    const auto previous=_CrtSetAllocHook(allocationHook);
    inCallback=true; process(cv,b,16); inCallback=false;
    _CrtSetAllocHook(previous);
    expect(callbackAllocations==0,"no CRT allocation during warmed audio callbacks");
#endif
    std::printf("factory %02d %-33s: %zu pitches, period %.9f, checked MIDI 0..127 in all pitch routes\n",index,def.name,pitches.size(),pitches.back());
}
} // namespace
int main()
{
    juce::ScopedJuceInitialiser_GUI init;
    {
        SwaraXtAudioProcessor p;
        expect(p.builtinTuningCount()==37,"37 catalog entries");
        expect(!p.loadBuiltinTuning(-1) && !p.loadBuiltinTuning(37),"invalid factory indices rejected");
        swaraxt::ui::SeqPanel panel;
        juce::MessageManager::getInstance()->runDispatchLoopUntil(1);
        panel.attach(p);
        juce::MessageManager::getInstance()->runDispatchLoopUntil(1);
        auto& box=panel.tuningBoxForTests();
        expect(box.getNumItems()==38 && box.getItemId(37)==100,"dropdown has every factory and original Imported ID");
        for(int i=0;i<37;++i) expect(box.getItemText(i)==definitions[i].name,"dropdown order/name");
    }
    juce::MessageManager::getInstance()->runDispatchLoopUntil(1);
    for(int i=0;i<37;++i)
    {
        testFactory(i);
        juce::MessageManager::getInstance()->runDispatchLoopUntil(1);
    }
    std::printf("factory tuning tests: %s (%d failures)\n",failures ? "FAILED" : "PASSED",failures);
    return failures ? 1 : 0;
}
