// Copyright2026 MontroneDSP. SPDX-License-Identifier: GPL-3.0-or-later
// Reuse the original headless assertions. Editor-only functions remain NOT_RUN.
#define main swaraxtUnusedOriginalMain
#include "../Presets/PresetStateRestoreTestsMain.cpp"
#undef main
int main() {
    juce::ScopedJuceInitialiser_GUI init;
    testMalformedStatePreservesParameters();
    testProcessorOnlyRestore();
    testHostProgramReassertDoesNotReload();
    testExplicitPresetSelectionAfterPrepare();
    testMultiplePresetRoundtrips();
    testCustomizedFactoryPresetRestore();
    testAudioEquivalence();
    testBulkPublicationKeepsPreviousEngineState();
    testHostSaveDuringBulkPublicationMatchesPreviousState();
    const int n=failures;
    std::printf("CURRENT_HEADLESS PresetRestore failures=%d\n",n);
    return n?1:0;
}
