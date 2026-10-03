// Copyright 2026 MontroneDSP.
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include <JuceHeader.h>

#include <atomic>
#include <vector>

#include "Engine/ParameterCache.h"
#include "Engine/HostTransport.h"
#include "Engine/SwaraXtEngine.h"
#include "Engine/SequenceState.h"
#include "Plugin/SwaraXtParameterLayout.h"
#include "Plugin/ShruthiFactoryPresetData.h"
#include "Plugin/ApvtsFactoryPresetData.h"
class SwaraXtAudioProcessor : public juce::AudioProcessor,
                              private juce::AudioProcessorValueTreeState::Listener {
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
    double getTailLengthSeconds() const override;

    int getNumPrograms() override
    {
        return swaraxt::kUserFactoryPresetStart
            + static_cast<int>(swaraxt::kUserFactoryPresetCount);
    }
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
    swaraxt::SequenceState& sequenceState() noexcept { return sequenceState_; }
    const swaraxt::SequenceState& sequenceState() const noexcept { return sequenceState_; }
    bool sequenceLocked() const noexcept { return sequenceLocked_.load(std::memory_order_relaxed); }
    void setSequenceLocked(bool locked) noexcept { sequenceLocked_.store(locked, std::memory_order_relaxed); }

    swaraxt::SequenceState::NoteRange randomNoteRange() const noexcept
    {
        const auto packed = randomNoteRange_.load(std::memory_order_relaxed);
        return { static_cast<int>(packed & 255u) - 48, static_cast<int>(packed >> 8) };
    }
    void setRandomNoteRange(swaraxt::SequenceState::NoteRange range) noexcept
    {
        const auto lower = static_cast<unsigned>(juce::jlimit(-48, 0, range.lower) + 48);
        const auto upper = static_cast<unsigned>(juce::jlimit(0, 48, range.upper));
        randomNoteRange_.store(lower | (upper << 8), std::memory_order_relaxed);
    }

    std::vector<PresetEntry> getPresetEntries() const;
    bool loadPresetEntry(const PresetEntry& entry, juce::String& error);
    bool saveUserPreset(const juce::String& name, bool overwrite, juce::String& error);
    juce::String currentPresetName() const;
    bool currentPresetIsUser() const noexcept { return currentUserPresetName_.isNotEmpty(); }
    juce::File userPresetDirectory() const;
    void setUserPresetDirectoryForTests(const juce::File& directory);
    void setFilterQuality(swaraxt::FilterQuality quality) noexcept { engine_.setFilterQuality(quality); }
    swaraxt::FilterQuality filterQuality() const noexcept { return engine_.filterQuality(); }

    static constexpr int kStateVersion = 3;
    static constexpr const char* kStateRoot = "SWARAXT_STATE";

 private:
    void loadFactoryPreset(int index);
    void restoreState(juce::ValueTree tree, bool fromPreset);
    swaraxt::HostTransportSnapshot captureHostTransport() noexcept;
    static bool validatePresetName(const juce::String& name, juce::String& cleanName);
    bool decodePresetData(const void* data, int sizeInBytes, juce::ValueTree& state) const;
    void parameterChanged(const juce::String& parameterID, float newValue) override;

    juce::AudioProcessorValueTreeState apvts_;
    swaraxt::ParameterCache parameterCache_;
    swaraxt::SequenceState sequenceState_;
    swaraxt::SwaraXtEngine engine_;
    int currentProgram_ = 0;
    std::atomic<bool> requestEngineReset_ { false };
    std::atomic<bool> sequenceLocked_ { false };
    std::atomic<unsigned> randomNoteRange_ { 36u | (12u << 8) };
    bool isPrepared_ = false;
    bool engineInitialized_ = false;
    // True after setStateInformation restores a host session; blocks setCurrentProgram
    // from reloading factory presets until prepareToPlay() completes initialization.
    bool hostSessionStateRestored_ = false;
    double lastValidHostBpm_ = 120.0;
    juce::String currentUserPresetName_;
    juce::File presetDirectoryOverride_;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR(SwaraXtAudioProcessor)
};
