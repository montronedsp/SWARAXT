// Copyright2026 MontroneDSP. SPDX-License-Identifier: GPL-3.0-or-later
// Reuse the original headless assertions. Editor-only functions remain NOT_RUN.
#define main swaraxtUnusedOriginalMain
#include "../LiveEngine/MultiInstanceTestsMain.cpp"
#undef main
int main() {
    juce::ScopedJuceInitialiser_GUI init;
    testIndependentPatchState();
    testIndependentNoteAndFilterState();
    testIndependentRandomAndSequencerState();
    testIndependentStateRestore();
    testConcurrentProcessingAndParameterThread();
    testDestructionOrder();
    const int n=gFailures.load(std::memory_order_relaxed);
    std::printf("CURRENT_HEADLESS MultiInstance failures=%d\n",n);
    return n?1:0;
}
