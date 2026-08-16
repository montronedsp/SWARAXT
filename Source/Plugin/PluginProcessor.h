// Copyright 2026 MontroneDSP.
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include <JuceHeader.h>

#include <atomic>
#include <vector>

#include "Engine/ParameterCache.h"
#include "Engine/HostTransport.h"
#include "Engine/SwaraXtEngine.h"
#include "Plugin/SwaraXtParameterLayout.h"
class SwaraXtAudioProcessor : public juce::AudioProcessor {
 public:
    struct PresetEntry {
        juce::String name;
        juce::File file;
        int factoryIndex = -1;
        bool isFactory = false;
    };

    SwaraXtAudioProcessor();
    ~SwaraXtAudioProcessor() override;

    void prepareToPlay(double sampleRate, int samplesPerBlock) override;
    void releaseResources() override;
    void processBlock(juce::AudioBuffer<float>&, juce::MidiBuffer&) override;

    juce::AudioProcessorEditor* createEditor() override;
    bool hasEditor() const override { return true; }

    const juce::String getName() const override { return "Swara XT"; }
    bool acceptsMidi() const override { return true; }
    bool producesMidi() const override { return false; }
    bool isMidiEffect() const override { return false; }
    double getTailLengthSeconds() const override { return 0.0; }

    int getNumPrograms() override { return 11; }
    int getCurrentProgram() override { return currentProgram_; }
    void setCurrentProgram(int index) override;
    const juce::String getProgramName(int index) override;
    void changeProgramName(int, const juce::String&) override {}

    void getStateInformation(juce::MemoryBlock& destData) override;
    void setStateInformation(const void* data, int sizeInBytes) override;

    juce::AudioProcessorValueTreeState& getApvts() { return apvts_; }
    const juce::AudioProcessorValueTreeState& getApvts() const { return apvts_; }
    swaraxt::SwaraXtEngine& engineForTests() noexcept { return engine_; }
    const swaraxt::SwaraXtEngine& engineForTests() const noexcept { return engine_; }

    std::vector<PresetEntry> getPresetEntries() const;
    bool loadPresetEntry(const PresetEntry& entry, juce::String& error);
    bool saveUserPreset(const juce::String& name, bool overwrite, juce::String& error);
    juce::String currentPresetName() const;
    bool currentPresetIsUser() const noexcept { return currentUserPresetName_.isNotEmpty(); }
    juce::File userPresetDirectory() const;
    void setUserPresetDirectoryForTests(const juce::File& directory);

    static constexpr int kStateVersion = 2;
    static constexpr const char* kStateRoot = "SWARAXT_STATE";

 private:
    void loadFactoryPreset(int index);
    swaraxt::HostTransportSnapshot captureHostTransport() noexcept;
    static bool validatePresetName(const juce::String& name, juce::String& cleanName);
    bool decodePresetData(const void* data, int sizeInBytes, juce::ValueTree& state) const;

    juce::AudioProcessorValueTreeState apvts_;
    swaraxt::ParameterCache parameterCache_;
    swaraxt::SwaraXtEngine engine_;
    int currentProgram_ = 0;
    std::atomic<bool> requestEngineReset_ { false };
    bool isPrepared_ = false;
    bool engineInitialized_ = false;
    double lastValidHostBpm_ = 120.0;
    juce::String currentUserPresetName_;
    juce::File presetDirectoryOverride_;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR(SwaraXtAudioProcessor)
};
