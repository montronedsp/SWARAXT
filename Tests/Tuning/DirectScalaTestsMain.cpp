// Copyright 2026 MontroneDSP.
// SPDX-License-Identifier: GPL-3.0-or-later
#include <JuceHeader.h>
#include "Plugin/PluginProcessor.h"
#include "Engine/MidiInputTuning.h"
#include "Plugin/SwaraXtParameterLayout.h"
#include "shruthi/sequencer_settings.h"
#include "Tuning/TuningTable.h"
#include <cmath>
#include <cstdio>
#include <string>
#include <vector>
#if defined(_MSC_VER) && defined(_DEBUG)
#include <crtdbg.h>
#endif
namespace {
int failures = 0;
#if defined(_MSC_VER) && defined(_DEBUG)
bool inCallback = false;
int allocations = 0;
int allocationHook(int type, void*, size_t, int, long, const unsigned char*, int)
{
    if (inCallback && type == _HOOK_ALLOC) ++allocations;
    return 1;
}
#endif
void expect(bool ok, const char* text)
{
    if (!ok) { std::printf("FAIL: %s\n", text); ++failures; }
}
bool near(double a, double b) { return std::abs(a-b) <= 1.e-10 * std::max(1.0, std::abs(b)); }
constexpr const char* twelve = "Twelve\n12\n100.\n200.\n300.\n400.\n500.\n600.\n700.\n800.\n900.\n1000.\n1100.\n2\n";
using namespace swaraxt::tuning;
void parserContracts()
{
    ParseError error;
    TuningSnapshot s;
    const auto parse = [&](const char* scale, const char* map = nullptr) {
        return parseScalaAndKeyboard(scale, map, s, error, ScalaSyntax::standard);
    };
    expect(parse(twelve), error.message);
    for (int n=0; n<128; ++n)
        expect(s.mapped(n) && near(s.hzForMidi(n), 440.0*std::exp2((n-69)/12.0)), "all 128 default MIDI frequencies, including negative degrees");
    const char* annotated = "! comment\r\n\r\n! blank description retained\r\n3 ! count\r\n  3 / 2 fifth\r\n1901.9550008653874 tritave\r\n 4 quadruple\r\n";
    expect(parse(annotated), error.message);
    expect(s.name[0] == 0 && near(s.periodCents, 2400.0), "blank name and bare integer period");
    expect(near(s.degreeCents[0], 1200.0*std::log2(1.5)), "annotated spaced ratio");
    expect(near(s.hzForMidi(63), s.hzForMidi(60)*4.0), "non-octave period");
    expect(parse("Irregular\n4\n-100.0\n700.0\n500.0\n2/1\n"), "valid nonmonotonic Scala is representable");
    const char* map = "3\n0\n127\n60\n61\n300.0\n7\n0\n2\nx\n";
    expect(parse(twelve, map), error.message);
    for (int n=0; n<128; ++n)
    {
        const int delta=n-60;
        const int cycle=static_cast<int>(std::floor(delta/3.0));
        const int key=delta-cycle*3;
        if (key==2) expect(!s.mapped(n), "x repeats below and above middle");
        else {
            const int degree=cycle*7+(key==0 ? 0 : 2);
            expect(s.mapped(n) && s.midiDegree[n]==degree, "middle anchored repeated KBM degree");
            expect(near(s.hzForMidi(n), 300.0*std::exp2((degree-2)/12.0)), "independent repeated KBM frequency");
        }
    }
    expect(s.formalPeriod==12, "KBM formal advance does not replace scale period");
    std::vector<uint8_t> bytes;
    expect(writeTuningTable(s,bytes,error), error.message);
    TuningSnapshot restored;
    expect(readTuningTable(bytes.data(),bytes.size(),restored,error), error.message);
    for (int n=0;n<128;++n) expect(restored.mapped(n)==s.mapped(n) && near(restored.hzForMidi(n),s.hzForMidi(n)), "signed mapping SWT roundtrip");
    PublishedTuning published;
    expect(published.fillFrom(restored), "signed mapping publication");
    const auto input=swaraxt::resolveMidiInputPitch(swaraxt::MidiInputTuningMode::tuned,&published,57);
    expect(input.valid && input.word==swaraxt::ScalePitch::toVoicePitchFromHz(300.0*std::exp2(-9.0/12.0)), "INPUT TUNE signed mapped degree");
    expect(!swaraxt::resolveMidiInputPitch(swaraxt::MidiInputTuningMode::tuned,&published,59).valid, "INPUT TUNE x stays silent");
    expect(parse(twelve,"0\n0\n127\n60\n57\n432.0\n0\n"), "zero size linear KBM and negative reference degree");
    expect(near(s.hzForMidi(69),864.0), "linear mapping reference frequency");
    expect(parse(twelve,"2\n0\n127\n60\n60\n440\n-12\n0\n"), "negative formal advance and omitted trailing x");
    expect(!s.mapped(61) && near(s.hzForMidi(62),220.0) && near(s.hzForMidi(58),880.0), "negative formal period independent oracle");
    expect(parse(twelve,"2\n0\n127\n60\n60\n440\n0\n0\n1\n"), "zero formal advance");
    expect(near(s.hzForMidi(62),440.0), "zero formal advance repeats pattern");
    expect(parse("Large ratio\n2\n2147483647/2147483646 annotation\n2/1\n"), "31-bit ratio operands");
    expect(near(s.degreeCents[0],1200.0*std::log2(2147483647.0/2147483646.0)), "large ratio independent reference");
    expect(parse(twelve,"0\n0\n127\n60\n59\n440\n12\n"), "mapped degree minus one can be a reference");
    expect(s.mapped(59) && s.midiDegree[59]==-1 && near(s.hzForMidi(60),440.0*std::exp2(1.0/12.0)), "negative one is distinct from x");
    expect(writeTuningTable(s,bytes,error) && readTuningTable(bytes.data(),bytes.size(),restored,error)
           && restored.mapped(59) && near(restored.hzForMidi(59),440.0), "negative reference degree serialized without a new format");
    const char* bad[] = {"No count\n", "Empty\n0\n", "Single\n1\n2\n", "Bad\n2\n3/0\n2\n", "Bad\n2\n-3\n2\n", "Bad\n2\n100,0\n2\n", "Bad\n2\n100.0\n", "Bad\n2\n100.0\nNaN\n", "Bad\n2\n3/2/1\n2\n", "Bad\n2\n100.0\n0.0\n"};
    for (auto* text:bad) expect(!parse(text), "invalid/unsupported scale rejected");
    expect(!parse(twelve,"1\n0\n127\n60\n60\n440\n12\nx\n"), "unmapped reference rejected");
    expect(!parse(twelve,"1\n0\n127\n60\n60\n440\n2147483647\n0\n"), "unrepresentable degrees rejected without narrowing");
    // The default dialect remains the original converter contract.
    expect(parseScalaAndKeyboard("Legacy\n2\n100\n1200\n",nullptr,s,error), "legacy integer cents retained");
    expect(s.degreeCents[0]==100.0 && s.periodCents==1200.0 && !s.mapped(59), "legacy mapping and cent semantics unchanged");
}
void processorContracts(const juce::File& directory)
{
    const auto scale=directory.getChildFile("test.scl");
    const auto map=directory.getChildFile("test.kbm");
    expect(scale.replaceWithText(twelve), "write scale fixture");
    expect(map.replaceWithText("3\n0\n127\n60\n61\n300.0\n7\n0\n2\nx\n"), "write mapping fixture");
    SwaraXtAudioProcessor p;
    p.prepareToPlay(48000,64);
    juce::String error;
    expect(p.builtinTuningCount()==37, "37 factory tunings retained");
    expect(p.importTuningFile(scale,map,error), "processor imports selected SCL/KBM pair");
    expect(p.activeBuiltinTuning()==-1 && p.activeTuningName()=="Twelve", "Imported tuning metadata");
    expect(near(p.hzForActiveDegree(-7),300.0*std::exp2(-9.0/12.0)), "processor signed-degree frequency");
    auto q=p.quantizeActiveHz(300.0);
    expect(q.valid && near(q.hz,300.0), "QUANTIZED CV uses imported reference");
    int16_t degrees[128] {}; bool mapped[128] {};
    p.copyTuningDegrees(degrees,128,mapped);
    expect(mapped[57] && degrees[57]==-7 && !mapped[59], "GUI degree snapshot preserves signed validity");
    auto pattern=p.sequenceState().snapshot();
    pattern.steps[0]=swaraxt::SequenceSnapshot::pack(57,100);
    pattern.scaleReady=0;
    p.sequenceState().store(pattern);
    p.sequenceState().setPitchMode(2,degrees,mapped);
    expect(p.sequenceState().snapshot().scaleDegree[0]==-7, "initial SCALE conversion preserves negative mapped degree");
    const auto set = [&](const char* id, int value) {
        auto* parameter=p.getApvts().getParameter(id);
        expect(parameter!=nullptr,"input parameter exists");
        if (parameter) parameter->setValueNotifyingHost(parameter->convertTo0to1(static_cast<float>(value)));
    };
    set(swaraxt::IDs::seqMode,shruthi::SEQUENCER_MODE_STEP);
    set(swaraxt::IDs::midiInputTuning,1);
    juce::AudioBuffer<float> audio(2,64);
    juce::MidiBuffer midi;
    midi.addEvent(juce::MidiMessage::noteOn(1,57,static_cast<juce::uint8>(100)),0);
    p.processBlock(audio,midi);
    midi.clear();
    for(int block=0;block<16;++block) p.processBlock(audio,midi);
    expect(p.engineForTests().shruthiPart().voice().pitch_value()
           ==swaraxt::ScalePitch::toVoicePitchFromHz(300.0*std::exp2(-9.0/12.0)), "actual processor INPUT TUNE signed MIDI key");
#if defined(_MSC_VER) && defined(_DEBUG)
    const auto previous=_CrtSetAllocHook(allocationHook);
    inCallback=true;
    for(int block=0;block<32;++block) p.processBlock(audio,midi);
    inCallback=false;
    _CrtSetAllocHook(previous);
    expect(allocations==0,"signed tuning callbacks allocate nothing");
#endif
    juce::MemoryBlock state;
    p.getStateInformation(state);
    SwaraXtAudioProcessor reopened;
    reopened.setStateInformation(state.getData(),static_cast<int>(state.getSize()));
    expect(reopened.activeTuningName()=="Twelve" && near(reopened.hzForActiveDegree(-7),p.hzForActiveDegree(-7)), "session embedded signed tuning restored");
    const auto before=p.activeTuningName();
    expect(scale.replaceWithText("Broken\n2\n100.\n"), "write truncated scale");
    expect(!p.importTuningFile(scale,map,error) && error.isNotEmpty(), "truncated import useful error");
    expect(p.activeTuningName()==before && near(p.hzForActiveDegree(-7),300.0*std::exp2(-9.0/12.0)), "failed parse leaves tuning unchanged");
    expect(scale.replaceWithText(juce::String::repeatedString("x",256*1024+1)), "write oversized scale");
    expect(!p.importTuningFile(scale,{},error) && error.contains("limit"), "bounded file read");
    const char nul[]={'A',0,'B'};
    expect(scale.replaceWithData(nul,sizeof(nul)), "write embedded NUL");
    expect(!p.importTuningFile(scale,{},error) && error.contains("NUL"), "NUL cannot truncate accepted input");
    expect(!p.importTuningFile(map,{},error), "mapping alone needs a selected scale");
    TuningSnapshot legacy; ParseError e; std::vector<uint8_t> swt;
    expect(parseScalaAndKeyboard("Legacy\n2\n100.0\n1200.0\n",nullptr,legacy,e) && writeTuningTable(legacy,swt,e), "legacy SWT fixture");
    const auto binary=directory.getChildFile("legacy.swt");
    expect(binary.replaceWithData(swt.data(),swt.size()) && p.importTuningFile(binary,{},error), "existing SWT import remains supported");
    p.releaseResources();
}
}
int main(int argc,char** argv)
{
#if defined(_MSC_VER) && defined(_DEBUG)
    _CrtSetReportMode(_CRT_ASSERT,_CRTDBG_MODE_FILE);
    _CrtSetReportFile(_CRT_ASSERT,_CRTDBG_FILE_STDERR);
#endif
    juce::ScopedJuceInitialiser_GUI gui;
    parserContracts();
    const auto dir=juce::File::getSpecialLocation(juce::File::tempDirectory).getNonexistentChildFile("swaraxt-scala","",false);
    expect(dir.createDirectory().wasOk(), "create temporary fixture directory");
    processorContracts(dir);
    // External archive fixtures stay outside the source tree.
    for (int i=1;i<argc;++i) {
        SwaraXtAudioProcessor p; juce::String error;
        const bool imported=p.importTuningFile(juce::File(argv[i]),{},error);
        if (!imported) std::printf("External fixture: %s: %s\n",argv[i],error.toRawUTF8());
        expect(imported,"external archive fixture imports");
    }
    expect(dir.deleteRecursively(), "remove owned fixtures after processors terminate");
    juce::MessageManager::getInstance()->runDispatchLoopUntil(20);
    std::printf("Direct Scala: %d failures\n",failures);
    return failures==0 ? 0 : 1;
}
