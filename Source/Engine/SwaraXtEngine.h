// Copyright 2026 MontroneDSP.
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once
#include "MidiOutputCapacity.h"

#include <array>
#include <atomic>
#include <cstdint>
#include <memory>
#include <mutex>
#include <limits>
#include <vector>

namespace juce {
class MidiBuffer;
template <typename SampleType>
class AudioBuffer;
}  // namespace juce

#include "Engine/Filter/ShruthiSvf2p.h"
#include "Engine/Filter/SwaraXtFilter.h"
#include "Engine/DspBoard/BoardPostDacFilter.h"
#include "Engine/DspBoard/BoardProcessor.h"
#include "Engine/HostTransport.h"
#include "Engine/ParameterCache.h"
#include "Engine/PatchBridge.h"
#include "Engine/SequencerMidiRoute.h"
#include "Tuning/PublishedTuning.h"
#include "Engine/SequenceState.h"
#include "Engine/SampleRate/HostResampler.h"
#include "Engine/SampleRate/InternalSampleQueue.h"
#include "Engine/SampleRate/PolyphaseFirResampler.h"

#include "avrlib/random.h"
#include "shruthi/audio_out.h"
#include "shruthi/midi_dispatcher.h"
#include "shruthi/part.h"
#include "shruthi/storage.h"

namespace swaraxt {

// Selects the native -> host rate converter at compile time. Zero builds the
// legacy 4-point Hermite fallback; any other value builds the
// band-limited polyphase FIR with that many taps. This exists so the SRC
// quality study can build both engines from one source tree, and it is
// deliberately not exposed as a parameter, preset field or GUI control.
//
// The production causal SRC adds 128 native samples to the filter's 256;
// nominal FIR latency is 384 / (20e6 / 510) = 9.792 ms.
#ifndef SWARAXT_SRC_FIR_TAPS
#define SWARAXT_SRC_FIR_TAPS 256
#endif
#ifndef SWARAXT_SRC_FIR_PHASES
#define SWARAXT_SRC_FIR_PHASES 256
#endif
#ifndef SWARAXT_SRC_FIR_STOPBAND_DB
#define SWARAXT_SRC_FIR_STOPBAND_DB 100.0
#endif

#if SWARAXT_SRC_FIR_TAPS > 0
using HostRateConverter = PolyphaseFirResampler<SWARAXT_SRC_FIR_TAPS, SWARAXT_SRC_FIR_PHASES, true>;
#define SWARAXT_SRC_CONVERTER_INIT { SWARAXT_SRC_FIR_STOPBAND_DB }
#else
using HostRateConverter = InternalSampleQueue;
#define SWARAXT_SRC_CONVERTER_INIT
#endif

class SwaraXtEngine {
 public:
    static constexpr double kInternalSampleRate = 20000000.0 / 510.0;
    static constexpr int kOutputLatencyNativeSamples = FilterRateConverter::kLatency + SWARAXT_SRC_FIR_TAPS / 2;
    static bool supportsHostSampleRate(double rate) noexcept {
        return std::isfinite(rate) && rate >= 8000.0 && rate <= 384000.0;
    }
    static constexpr int kAudioBlockSize = 40;
    static constexpr int kMaxPendingMidi = 512;

#if SWARAXT_ENABLE_SHRUTHI_DEBUG_TAPS
    struct DebugBlockCapture {
        float rawOsc1[kAudioBlockSize] {};
        float rawOsc2[kAudioBlockSize] {};
        float postShruthiMixer[kAudioBlockSize] {};
        float filterOutput[kAudioBlockSize] {}; // completed analog path, includes IR3109 VCA
        float postVca[kAudioBlockSize] {};
        float vcaTarget[kAudioBlockSize] {};
        float vcaGain[kAudioBlockSize] {}; // reconstructed CV at final oversampled substep, before decimation
        int samples = 0;
        uint32_t nativeBlockIndex = 0;
        uint8_t lfo1 = 0;
        uint8_t lfo2 = 0;
        uint8_t env1 = 0;
        uint8_t env2 = 0;
        int16_t oscCoarse = 0;
        int16_t oscFine = 0;
        int16_t cutoff = 0;
        int16_t resonance = 0;
        int16_t vca = 0;
    };

    using DebugTapSink = void (*)(void*, const DebugBlockCapture&);
#endif

#if SWARAXT_ENABLE_IDLE_CPU_TESTS
    struct CpuProfile {
        uint64_t processNanoseconds = 0;
        uint64_t midiAndClockNanoseconds = 0;
        uint64_t nativeVoiceNanoseconds = 0;
        uint64_t filterNanoseconds = 0;
        uint64_t srcAndOutputNanoseconds = 0;
        uint64_t nativeBlocksRendered = 0;
        uint64_t dormantControlBlocksAdvanced = 0;
        uint64_t dormantHostSamplesSkipped = 0;
        uint64_t filterSamplesProcessed = 0;
        uint64_t hostSamplesProduced = 0;
        uint64_t dcDrainHostSamples = 0;
    };

    void resetCpuProfileForTests() noexcept { cpuProfile_ = {}; }
    const CpuProfile& cpuProfileForTests() const noexcept { return cpuProfile_; }
#endif

    SwaraXtEngine();
    SwaraXtEngine(const SwaraXtEngine&) = delete;
    SwaraXtEngine& operator=(const SwaraXtEngine&) = delete;
    SwaraXtEngine(SwaraXtEngine&&) = delete;
    SwaraXtEngine& operator=(SwaraXtEngine&&) = delete;

    void prepare(double hostSampleRate, int maxBlockSize);
    void reset();
    void resetResampler() noexcept;
    void touchModulationRates() noexcept;
    void bindParameters(ParameterCache& cache) noexcept { parameterCache_ = &cache; }
    void bindSequenceState(SequenceState& state) noexcept { sequenceState_ = &state; }
    void applyParameters();
    uint8_t appliedSequencerPitchModeForTests() const noexcept { return appliedPitchMode_; }
#if defined(SWARAXT_ENABLE_SEQUENCE_SNAPSHOT_TESTS)
    const SequenceSnapshot& appliedSequenceForTests() const noexcept { return appliedSequence_; }
#endif
    double boardTailSeconds() const noexcept { return boardTailSeconds_.load(std::memory_order_relaxed); }
    const board::BoardControl& boardControlsForTests() const noexcept { return activeBoard_; }
    const board::BoardProcessor& boardProcessorForTests() const noexcept { return boardProcessor_; }

    void process(const juce::MidiBuffer& midi,
                 juce::AudioBuffer<float>& buffer,
                 int startSample,
                 int numSamples,
                 const HostTransportSnapshot& transport = HostTransportSnapshot {},
                 juce::MidiBuffer* midiOut = nullptr);
    void clearSequencerMidiOutput() noexcept;
    bool loadTuningBytes(const uint8_t* data, size_t size);
    bool loadBuiltinTuning(int index);
    int builtinTuningCount() const noexcept;
    const char* builtinTuningName(int index) const noexcept;
    int activeBuiltinTuning() const noexcept { return builtinTuningIndex_.load(std::memory_order_relaxed); }
    juce::String activeTuningName() const;
    void copyActiveTuningDegrees(int16_t* midiDegree, int count) const noexcept;
    SequenceState::ScaleRandomView scaleRandomView() const noexcept;
    double hzForActiveDegree(int degree) const noexcept;
    tuning::PublishedTuning::QuantizedPitch quantizeActiveHz(double sourceHz) const noexcept;
    std::vector<uint8_t> tuningBytesForState() const;

    SwaraXtFilter& filter() noexcept { return filter_; }
    const SwaraXtFilter& filter() const noexcept { return filter_; }

    // Experimental native-rate Shruthi Dual-SVF. Default off: Classic.
    // Session-only; not a public parameter contract. Setters publish a packed
    // atomic selection. Dual DSP is applied on the audio/prepare/reset thread.
    void setExperimentalSvfEnabledForTests(bool enabled) noexcept
    {
        experimentalSvfControl_.patch([enabled](ExperimentalSvfSelection& s) { s.enabled = enabled; });
    }
    bool experimentalSvfEnabledForTests() const noexcept
    {
        return experimentalSvfControl_.selection().enabled;
    }
    void setExperimentalSvfModeForTests(ShruthiSvf2pMode mode) noexcept
    {
        experimentalSvfControl_.patch([mode](ExperimentalSvfSelection& s) { s.mode1 = mode; });
    }
    void setExperimentalSvfMode2ForTests(ShruthiSvf2pMode mode) noexcept
    {
        experimentalSvfControl_.patch([mode](ExperimentalSvfSelection& s) { s.mode2 = mode; });
    }
    void setExperimentalSvfRoutingForTests(ShruthiSvfDualRouting routing) noexcept
    {
        experimentalSvfControl_.patch([routing](ExperimentalSvfSelection& s) { s.routing = routing; });
    }
    void setExperimentalSvfCalibrationForTests(ShruthiSvfCalibration calibration) noexcept
    {
        experimentalSvfControl_.patch([calibration](ExperimentalSvfSelection& s) {
            s.calibration = calibration;
        });
    }
    ExperimentalSvfSelection experimentalSvfSelection() const noexcept
    {
        return experimentalSvfControl_.selection();
    }
    void setExperimentalSvfControl2(ShruthiSvfSection2Control control) noexcept
    {
        experimentalSvfControl_.patch([control](ExperimentalSvfSelection& s) { s.control2 = control; });
    }
    void setExperimentalSvfCutoff2(int value) noexcept
    {
        experimentalSvfControl_.patch([value](ExperimentalSvfSelection& s) {
            s.cutoff2 = static_cast<std::uint8_t>(std::clamp(value, 0, 127));
        });
    }
    void setExperimentalSvfResonance2(int value) noexcept
    {
        experimentalSvfControl_.patch([value](ExperimentalSvfSelection& s) {
            s.resonance2 = static_cast<std::uint8_t>(std::clamp(value, 0, 63));
        });
    }
    ShruthiSvf2p& experimentalSvfForTests() noexcept { return experimentalSvf_.section1(); }
    const ShruthiSvf2p& experimentalSvfForTests() const noexcept { return experimentalSvf_.section1(); }
    ShruthiSvfDual& experimentalSvfDualForTests() noexcept { return experimentalSvf_; }
    const ShruthiSvfDual& experimentalSvfDualForTests() const noexcept { return experimentalSvf_; }
    void setFilterQuality(FilterQuality quality) noexcept
    {
        requestedFilterQuality_.store(static_cast<uint8_t>(quality), std::memory_order_relaxed);
    }
    FilterQuality filterQuality() const noexcept
    {
        return static_cast<FilterQuality>(requestedFilterQuality_.load(std::memory_order_relaxed));
    }
    // One self-contained observation: zero is inactive, 1..16 are sequence steps.
    int sequencePlaybackStep() const noexcept
    {
        return static_cast<int>(sequencePlayback_.load(std::memory_order_relaxed)) - 1;
    }
    void clearSequencePlayback() noexcept { sequencePlayback_.store(0, std::memory_order_relaxed); }

    shruthi::Part& shruthiPart() noexcept { return part_; }
    const shruthi::Part& shruthiPart() const noexcept { return part_; }
    uint16_t randomStateForTests() const noexcept { return random_.state(); }
    uint8_t advanceRandomForTests() noexcept { return random_.GetByte(); }
    uint64_t hostClockEventCountForTests() const noexcept { return hostClockEventCount_; }
    int lastHostClockOffsetForTests() const noexcept { return lastHostClockOffset_; }
#if SWARAXT_ENABLE_IDLE_CPU_TESTS
    bool dormantForTests() const noexcept { return dormant_; }
    bool drainingForTests() const noexcept { return drainingToDormant_; }
    bool dcDrainingForTests() const noexcept { return dcDrainingToDormant_; }
    void setDormancyEnabledForTests(bool enabled) noexcept
    {
        dormancyEnabledForTests_ = enabled;
        if (! enabled)
        {
            dormant_ = false;
            drainingToDormant_ = false;
            dcDrainingToDormant_ = false;
            dcDrainSamples_ = 0;
        }
    }
#endif
#if SWARAXT_ENABLE_SHRUTHI_DEBUG_TAPS
    void setDebugTapSink(void* context, DebugTapSink sink) noexcept
    {
        debugTapContext_ = context;
        debugTapSink_ = sink;
    }
    uint32_t debugInternalBlockCount() const noexcept { return debugInternalBlockCount_; }
#endif

 private:
    struct PendingMidi {
        int sampleOffset = 0;
        uint8_t status = 0;
        uint8_t data1 = 0;
        uint8_t data2 = 0;
    };

    void renderInternalBlock();
    float processSelectedFilterSample(float mixer, float vcaTarget) noexcept;
    float selectedFilterVca() const noexcept;
    bool selectedFilterTailActive() const noexcept;
    void updateBoardAtBlockBoundary() noexcept;
    void resetBoardState() noexcept;
    bool boardRequiresAudio() const noexcept;
    float nextBoardGain() noexcept;
    void advanceDormantControl();
    void updateDormantWakeState();
    void enterDormant() noexcept;
    bool voiceRequiresAudio() const noexcept;
    bool patchRequiresAudioSource() const noexcept;
    void dispatchMidi(const PendingMidi& event);
    void collectMidiEvents(juce::MidiBufferIterator& cursor, const juce::MidiBufferIterator& end,
                           int startSample, int numSamples);
    void updateFilterFromShruthi();
    void applyPendingFilterQuality() noexcept;
    void applyExperimentalSvfSelection() noexcept;
    void snapFilterQuality() noexcept;
    void startQualityFadeOut() noexcept;
    void setMasterTarget(float value) noexcept;
    void snapMasterGain(float value) noexcept;
    float nextMasterGain() noexcept;
    float nextQualityGain() noexcept;
    void setPostMixerTarget(float linear) noexcept;
    void snapPostMixerGain(float linear) noexcept;
    float nextPostMixerGain() noexcept;
    void updateHostLfoRates(double bpm) noexcept;
    void prepareHostClock(const HostTransportSnapshot& transport, int numSamples) noexcept;
    void resetHostState() noexcept;
    double hostTickPpq(uint64_t tick) const noexcept;
    void installSequencerMidiTap() noexcept;
    static void sequencerNoteOnTap(void* context, uint8_t note, uint8_t velocity, uint8_t external) noexcept;
    static void sequencerNoteOffTap(void* context, uint8_t note) noexcept;
    void onSequencerNote(bool noteOn, uint8_t note, uint8_t velocity) noexcept;
    void onSequencerNoteOn(uint8_t identity, uint8_t velocity, uint8_t external) noexcept;
    void onSequencerNoteOff(uint8_t identity) noexcept;
    bool enqueueScheduledMidi(uint64_t sample, bool noteOn, uint8_t note, uint8_t velocity,
                              uint8_t channel) noexcept;
    bool scheduleSequencerMidi(bool noteOn, uint8_t note, uint8_t velocity, uint8_t channel) noexcept;
    void flushSequencerMidi(juce::MidiBuffer& output, int numSamples) noexcept;
    void releaseHeldSequencerNotes(uint64_t sample) noexcept;
    void forceFlushSequencerMidiRelease(uint64_t sample) noexcept;
    int sequencerMidiLeadSamples() const noexcept;
    int outputMidiChannel() const noexcept;
    uint8_t heldOutputChannel(uint8_t external) const noexcept;

    shruthi::HostAudioRing audioRing_;
    avrlib::Random random_;
    struct ScheduledMidi {
        uint64_t sample = 0;
        uint8_t note = 0;
        uint8_t velocity = 0;
        uint8_t noteOn = 0;
        uint8_t channel = 1;
    };
    static constexpr int kMaxScheduledMidi = 128;
    static_assert(kMaxScheduledMidi * (3 + sizeof(int32_t) + sizeof(uint16_t))
                      <= kMidiOutputReserveBytes,
                  "Scheduled MIDI must fit supported wrapper storage");

    shruthi::MidiDispatcher midiDispatcher_;
    bool midiOutEnabled_ = false;
    bool sequencerMidiAllowed_ = false;
    bool releaseSequencerMidi_ = false;
    int renderHostCursor_ = 0;
    uint64_t hostBlockOrigin_ = 0;
    std::array<uint8_t, 128> heldMidi_ {};
    // Channel captured at Note On. Note Off must use this, never the live parameter.
    std::array<uint8_t, 128> heldMidiChannel_ {};
    std::array<uint8_t, 128> identityExternal_ {};
    std::array<uint8_t, 128> externalOwners_ {};
    std::array<ScheduledMidi, kMaxScheduledMidi> scheduledMidi_ {};
    int scheduledMidiCount_ = 0;
    juce::MidiBuffer* midiOutput_ = nullptr;
    static constexpr int kTuningSlots = 3;
    static constexpr int kSnapshotClaims = 2;
    // Heap slots keep the processor off a large stack frame. Allocated once
    // in the constructor and never resized. Audio reads only an immutable slot.
    std::array<std::unique_ptr<tuning::PublishedTuning>, kTuningSlots> tuningSlots_;
    // Stable copy installed into the sequencer/input consumers. The audio
    // thread fills it while it owns the latched slot. Part never keeps a
    // pointer into a slot the writer is allowed to recycle.
    std::unique_ptr<tuning::PublishedTuning> stableTuning_;
    std::atomic<int> publishedTuning_ { 0 };
    std::atomic<int> tuningSlotThisBlock_ { 0 };
    // Previous published index. Kept immutable until the next publication
    // replaces it. The latched index is not cleared when the callback ends.
    std::atomic<int> retiredTuning_ { 0 };
    // Off-audio readers pin a slot here for the duration of one copy.
    // -1 means the claim is free. The writer will not recycle a pinned slot.
    mutable std::array<std::atomic<int>, kSnapshotClaims> snapshotClaims_;
    std::atomic<bool> tuningAudioActive_ { false };
    std::atomic<bool> tuningWriteBusy_ { false };
    std::atomic<int> builtinTuningIndex_ { 0 };
    bool scaleRouteActive_ = false;
    bool quantizedRouteActive_ = false;
    // State serialization and tuning imports are non-audio operations. Audio
    // reads the immutable slots above and never acquires this mutex.
    mutable std::mutex tuningBytesMutex_;
    std::vector<uint8_t> tuningBytes_;
    bool publishTuningBytes(const uint8_t* data, size_t size, int builtinIndex);
    void latchPublishedTuning() noexcept;
    int freeTuningSlot(int published, int latched, int retired) const noexcept;
    bool snapshotClaimed(int slot) const noexcept;
    bool pinPublished(const tuning::PublishedTuning*& tuning, int& claim) const noexcept;
    void unpinPublished(int claim) const noexcept;
    void installSequencerTuning(const int16_t* degrees, const int32_t* quantizedUnits) noexcept;
    shruthi::Storage storage_;
    static_assert(std::atomic<unsigned>::is_always_lock_free);
    std::atomic<unsigned> sequencePlayback_ { 0 };
    shruthi::Part part_;
    PatchBridge patchBridge_;
    SequenceState* sequenceState_ = nullptr;
    uint32_t appliedSequenceRevision_ = std::numeric_limits<uint32_t>::max();
    SequenceSnapshot appliedSequence_;
    // Last pitch mode taken from a capture that succeeded. Audio routing uses
    // this when a later capture finds a writer in progress.
    uint8_t appliedPitchMode_ = 0;

    HostRateConverter internalQueue_ SWARAXT_SRC_CONVERTER_INIT;
    DcBlocker dcBlocker_;
    SwaraXtFilter filter_;
    ShruthiSvfDual experimentalSvf_;
    ExperimentalSvfControl experimentalSvfControl_;
    ExperimentalSvfSelection appliedExperimentalSvfSelection_;
    bool experimentalSvfEnabled_ = false;
    board::BoardProcessor boardProcessor_;
    // Native-rate v03 post-DAC coloration. Runs only while a DSP effect is active.
    board::BoardPostDacFilter postDacFilter_;
    board::BoardControl requestedBoard_, activeBoard_;
    static_assert(std::atomic<double>::is_always_lock_free, "Host tail publication must be realtime lock-free");
    std::atomic<double> boardTailSeconds_ { 0.0 };
    enum class BoardFadePhase : uint8_t { stable, fadeOut, switchPending, fadeIn };
    BoardFadePhase boardFadePhase_ = BoardFadePhase::stable;
    float boardGain_ = 1.0f, boardGainIncrement_ = 0.0f;
    int boardFadeRemaining_ = 0;
    bool snapBoardOnApply_ = true, boardWake_ = false;
    std::uint8_t boardTempo_ = 120;
    std::atomic<uint8_t> requestedFilterQuality_ {
        static_cast<uint8_t>(FilterQuality::normal)
    };
    FilterQuality appliedFilterQuality_ { FilterQuality::normal };
    FilterQuality pendingFilterQuality_ { FilterQuality::normal };
    enum class QualityFadePhase : uint8_t { stable, fadeOut, fadeIn };
    QualityFadePhase qualityFadePhase_ { QualityFadePhase::stable };
    float qualityGain_ = 1.0f;
    float qualityGainIncrement_ = 0.0f;
    int qualityRampSamples_ = 1;
    int qualitySamplesRemaining_ = 0;

    ParameterCache* parameterCache_ = nullptr;

    // Fixed capacity — never allocate on the audio thread.
    std::array<PendingMidi, kMaxPendingMidi> pendingMidi_ {};
    int pendingMidiCount_ = 0;

    double hostSampleRate_ = 44100.0;
    float masterGain_ = 0.85f;
    float masterGainCurrent_ = 0.85f;
    float masterGainTarget_ = 0.85f;
    float masterGainIncrement_ = 0.0f;
    int masterRampSamples_ = 1;
    int masterSamplesRemaining_ = 0;
    bool snapMasterOnApply_ = true;
    float vcaCvState_ = 0.0f;
    void resetVcaReconstruction() noexcept {
        vcaCvState_ = 0;
    }
    float postMixerGainCurrent_ = 1.0f;
    float postMixerGainTarget_ = 1.0f;
    float postMixerGainIncrement_ = 0.0f;
    int postMixerRampSamples_ = 1;
    int postMixerSamplesRemaining_ = 0;
    float filterCutoffHz_ = 8000.0f;
    float filterResonance_ = 0.2f;
    float filterKeyTrack_ = 0.5f;
    int midiChannel_ = 1;
    int midiInputMode_ = 0;
    bool prepared_ = false;
    bool dormant_ = true;
    bool drainingToDormant_ = false;
    bool dcDrainingToDormant_ = false;
    uint64_t dcDrainSamples_ = 0;
    uint64_t maxDcDrainSamples_ = 0;
    double dormantNativeSamples_ = 0.0;
    bool lfoSync_[2] {};
    int lfoDivision_[2] { 3, 3 };
    bool sequencerHostSync_ = false;
    int sequencerSwing_ = 0;
    bool hostTransportWasPlaying_ = false;
    bool hostClockNeedsAlignment_ = true;
    uint64_t nextHostClockTick_ = 0;
    double blockStartPpq_ = 0.0;
    double expectedNextPpq_ = 0.0;
    double fallbackPpq_ = 0.0;
    double blockPpqPerSample_ = 0.0;
    uint64_t hostClockEventCount_ = 0;
    int lastHostClockOffset_ = -1;

#if SWARAXT_ENABLE_SHRUTHI_DEBUG_TAPS
    void* debugTapContext_ = nullptr;
    DebugTapSink debugTapSink_ = nullptr;
    uint32_t debugInternalBlockCount_ = 0;
#endif
#if SWARAXT_ENABLE_IDLE_CPU_TESTS
    CpuProfile cpuProfile_ {};
    bool dormancyEnabledForTests_ = true;
#endif
};

}  // namespace swaraxt
