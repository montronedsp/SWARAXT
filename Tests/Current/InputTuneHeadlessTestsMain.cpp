// Copyright2026 MontroneDSP. SPDX-License-Identifier: GPL-3.0-or-later
// Reuse the original headless assertions. Editor-only functions remain NOT_RUN.
#define main swaraxtUnusedOriginalMain
#include "../Sequencer/MidiInputTuningTestsMain.cpp"
#undef main
int main() {
    juce::ScopedJuceInitialiser_GUI init;
    testOffIsOriginalTune();
    testTunedMapping();
    testQuantizedInput();
    testUnmappedAndAllocation();
    testSustainBendAndPortamento();
    testHeldPitchAndSequencerIsolation();
    testLegacySequenceIgnoresInputTuning();
    testArpeggiatorUsesInputTuning();
    testStateDefaultsOff();
    testAudiblePitchBendGate();
    testCompletePortamentoGate();
    testUnmappedKeySafetyGate();
    const int n=failures;
    std::printf("CURRENT_HEADLESS InputTune failures=%d\n",n);
    return n?1:0;
}
