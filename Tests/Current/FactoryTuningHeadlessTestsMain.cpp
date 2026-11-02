// Copyright2026 MontroneDSP. SPDX-License-Identifier: GPL-3.0-or-later
// Retain the37 factory/parser/MIDI/audio/state cases; exclude SeqPanel assertions.
#define main swaraxtUnusedFactoryTuningGuiMain
#include "../Tuning/FactoryTuningTestsMain.cpp"
#undef main
int main() {
    juce::ScopedJuceInitialiser_GUI init;
    { SwaraXtAudioProcessor p;
      expect(p.builtinTuningCount()==37,"37 catalog entries");
      expect(!p.loadBuiltinTuning(-1) && !p.loadBuiltinTuning(37),"invalid factory indices rejected"); }
    for(int i=0;i<37;++i) {
        testFactory(i);
        juce::MessageManager::getInstance()->runDispatchLoopUntil(1);
    }
    std::printf("CURRENT_HEADLESS FactoryTuning failures=%d\n",failures);
    return failures?1:0;
}
