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
    // Expose base processBlock overloads; float override remains the active path.
    using juce::AudioProcessor::processBlock;
    void processBlock(juce::AudioBuffer<float>&, juce::MidiBuffer&) override;

    juce::AudioProcessorEditor* createEditor() override;
    bool hasEditor() const override { return true; }

    const juce::String getName() const override { return "Swara XT"; }
    bool acceptsMidi() const override { return true; }
    bool producesMidi() const override { return true; }
    bool isMidiEffect() const override { return false; }
    double getTailLengthSeconds() const override;

    int getNumPrograms() override
    {
        return swaraxt::kUserFactoryPresetStart
            + static_cast<int>(swaraxt::kUserFactoryPresetCount);
    }
    int getCurrentProgram() override;
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
    int sequencePlaybackStep() const noexcept { return engine_.sequencePlaybackStep(); }
    bool sequenceLocked() const noexcept { return sequenceLocked_.load(std::memory_order_relaxed); }
    void setSequenceLocked(bool locked) noexcept { sequenceLocked_.store(locked, std::memory_order_relaxed); }
    bool loadTuningBytes(const void* data, size_t size) { return engine_.loadTuningBytes(static_cast<const uint8_t*>(data), size); }
    bool loadBuiltinTuning(int index) { return engine_.loadBuiltinTuning(index); }
    int builtinTuningCount() const noexcept { return engine_.builtinTuningCount(); }
    const char* builtinTuningName(int index) const noexcept { return engine_.builtinTuningName(index); }
    int activeBuiltinTuning() const noexcept { return engine_.activeBuiltinTuning(); }
    juce::String activeTuningName() const { return engine_.activeTuningName(); }
    void copyTuningDegrees(int16_t* midiDegree, int count) const noexcept { engine_.copyActiveTuningDegrees(midiDegree, count); }
    swaraxt::SequenceState::ScaleRandomView scaleRandomView() const noexcept { return engine_.scaleRandomView(); }
    double hzForActiveDegree(int degree) const noexcept { return engine_.hzForActiveDegree(degree); }
    swaraxt::tuning::PublishedTuning::QuantizedPitch quantizeActiveHz(double sourceHz) const noexcept
    {
        return engine_.quantizeActiveHz(sourceHz);
    }

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
    bool currentPresetIsUser() const;
    juce::File userPresetDirectory() const;
    void setUserPresetDirectoryForTests(const juce::File& directory);
    void setFilterQuality(swaraxt::FilterQuality quality) noexcept { engine_.setFilterQuality(quality); }
    swaraxt::ExperimentalSvfSelection experimentalSvfSelection() const noexcept
    {
        return engine_.experimentalSvfSelection();
    }
    void setExperimentalSvfEnabled(bool enabled) noexcept
    {
        engine_.setExperimentalSvfEnabledForTests(enabled);
    }
    void setExperimentalSvfControl2(swaraxt::ShruthiSvfSection2Control control) noexcept
    {
        engine_.setExperimentalSvfControl2(control);
    }
    void setExperimentalSvfCutoff2(int value) noexcept { engine_.setExperimentalSvfCutoff2(value); }
    void setExperimentalSvfResonance2(int value) noexcept { engine_.setExperimentalSvfResonance2(value); }
    void setExperimentalSvfMode1(swaraxt::ShruthiSvf2pMode mode) noexcept
    {
        engine_.setExperimentalSvfModeForTests(mode);
    }
    void setExperimentalSvfMode2(swaraxt::ShruthiSvf2pMode mode) noexcept
    {
        engine_.setExperimentalSvfMode2ForTests(mode);
    }
    void setExperimentalSvfRouting(swaraxt::ShruthiSvfDualRouting routing) noexcept
    {
        engine_.setExperimentalSvfRoutingForTests(routing);
    }
    void setExperimentalSvfCalibration(swaraxt::ShruthiSvfCalibration calibration) noexcept
    {
        engine_.setExperimentalSvfCalibrationForTests(calibration);
    }
    swaraxt::FilterQuality filterQuality() const noexcept { return engine_.filterQuality(); }

    static constexpr int kStateVersion = 3;
    static constexpr const char* kStateRoot = "SWARAXT_STATE";

#if defined(SWARAXT_ENABLE_STATE_PUBLICATION_TESTS)
    enum class StatePublicationPoint {
        programPublicationLocked, restoreGateLocked, bulkEpochPublished, userPresetFileReady,
        stateReadBeforeLock
    };
    using StatePublicationHook = void (*)(void*, StatePublicationPoint);
    void setStatePublicationHook(StatePublicationHook hook, void* context) noexcept
    {
        statePublicationHook_ = hook;
        statePublicationHookContext_ = context;
    }
#endif

 private:
    bool loadFactoryPreset(int index);
    void applyFactoryPreset(int index);
    bool restoreState(juce::ValueTree tree, bool fromPreset, const juce::String& presetName = {});
    void beginBulkPublication();
    void endBulkPublication() noexcept;
    void writeStateInformation(juce::MemoryBlock& destData);

    struct PresetMetadata {
        int program = 0;
        juce::String userName;
    };
    PresetMetadata presetMetadata() const;

    // Non-audio writers acquire publication before the audio admission gate.
    // Synchronous callbacks may read/save the committed generation, but cannot
    // recursively start another writer transaction.
    struct StateWriterScope {
        explicit StateWriterScope(SwaraXtAudioProcessor& processor);
        ~StateWriterScope();
        explicit operator bool() const noexcept { return acquired_; }
        StateWriterScope(const StateWriterScope&) = delete;
        StateWriterScope& operator=(const StateWriterScope&) = delete;
        SwaraXtAudioProcessor& processor_;
        juce::ScopedLock publication_;
        bool acquired_ = false;
    };

    struct BulkPublicationScope {
        explicit BulkPublicationScope(SwaraXtAudioProcessor& processor)
            : processor_(processor)
        {
            processor_.beginBulkPublication();
        }
        ~BulkPublicationScope() { processor_.endBulkPublication(); }
        BulkPublicationScope(const BulkPublicationScope&) = delete;
        BulkPublicationScope& operator=(const BulkPublicationScope&) = delete;
        SwaraXtAudioProcessor& processor_;
    };
    swaraxt::HostTransportSnapshot captureHostTransport() noexcept;
    static bool validatePresetName(const juce::String& name, juce::String& cleanName);
    bool decodePresetData(const void* data, int sizeInBytes, juce::ValueTree& state) const;
    void parameterChanged(const juce::String& parameterID, float newValue) override;

    juce::AudioProcessorValueTreeState apvts_;
    swaraxt::ParameterCache parameterCache_;
    swaraxt::SequenceState sequenceState_;
    swaraxt::SwaraXtEngine engine_;
    int currentProgram_ = 0;
    std::atomic_flag stateUpdateGate_ = ATOMIC_FLAG_INIT;
    // The immediate pre-transaction blob/metadata are installed before this
    // becomes odd. Active-transaction readers never enter APVTS serialization.
    std::atomic<uint32_t> bulkPublicationEpoch_ { 0 };
    // Covers preset metadata and direct state serialization. Recursive so a
    // save that re-enters from a parameter listener can return the committed
    // snapshot. Not taken by the audio callback.
    juce::CriticalSection publicationLock_;
    juce::CriticalSection committedStateLock_;
    juce::MemoryBlock committedState_;
    PresetMetadata committedMetadata_;
    bool stateWriterActive_ = false;       // protected by publicationLock_
    // Advances for bulk/metadata changes and new save requests, not lock scopes
    // or stale file completions, which must not invalidate a newer save ticket.
    uint64_t stateWriterGeneration_ = 0;   // protected by publicationLock_
    std::atomic<bool> requestEngineReset_ { false };
    std::atomic<bool> sequenceLocked_ { false };
    std::atomic<unsigned> randomNoteRange_ { 36u | (12u << 8) };
    bool isPrepared_ = false;
    bool engineInitialized_ = false;
    // True after setStateInformation restores a host session; blocks setCurrentProgram
    // from reloading factory presets until prepareToPlay() completes initialization.
    std::atomic<bool> hostSessionStateRestored_ { false };
    double lastValidHostBpm_ = 120.0;
    juce::String currentUserPresetName_;
    juce::File presetDirectoryOverride_;

#if defined(SWARAXT_ENABLE_STATE_PUBLICATION_TESTS)
    void statePublicationPoint(StatePublicationPoint point)
    {
        if (statePublicationHook_ != nullptr)
            statePublicationHook_(statePublicationHookContext_, point);
    }
    StatePublicationHook statePublicationHook_ = nullptr;
    void* statePublicationHookContext_ = nullptr;
#endif

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR(SwaraXtAudioProcessor)
};
