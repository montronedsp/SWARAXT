// Copyright 2026 MontroneDSP.
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include <array>
#include <atomic>
#include <cstdint>

namespace juce {
class MidiBuffer;
template <typename SampleType>
class AudioBuffer;
}  // namespace juce

#include "Engine/Filter/SwaraXtFilter.h"
#include "Engine/HostTransport.h"
#include "Engine/ParameterCache.h"
#include "Engine/PatchBridge.h"
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
// docs/engineering/SRC_QUALITY_COMPARISON.md records why 64 taps is the
// default on this branch.
#ifndef SWARAXT_SRC_FIR_TAPS
#define SWARAXT_SRC_FIR_TAPS 64
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
    static constexpr int kAudioBlockSize = 40;
    static constexpr int kMaxPendingMidi = 512;

#if SWARAXT_ENABLE_SHRUTHI_DEBUG_TAPS
    struct DebugBlockCapture {
        float rawOsc1[kAudioBlockSize] {};
        float rawOsc2[kAudioBlockSize] {};
        float postShruthiMixer[kAudioBlockSize] {};
        float filterOutput[kAudioBlockSize] {};
        float postVca[kAudioBlockSize] {};
        float vcaGain[kAudioBlockSize] {};
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
    void applyParameters();

    void process(const juce::MidiBuffer& midi,
                 juce::AudioBuffer<float>& buffer,
                 int startSample,
                 int numSamples,
                 const HostTransportSnapshot& transport = HostTransportSnapshot {});

    SwaraXtFilter& filter() noexcept { return filter_; }
    const SwaraXtFilter& filter() const noexcept { return filter_; }
    void setFilterQuality(FilterQuality quality) noexcept
    {
        requestedFilterQuality_.store(static_cast<uint8_t>(quality), std::memory_order_relaxed);
    }
    FilterQuality filterQuality() const noexcept
    {
        return static_cast<FilterQuality>(requestedFilterQuality_.load(std::memory_order_relaxed));
    }
    shruthi::Part& shruthiPart() noexcept { return part_; }
    const shruthi::Part& shruthiPart() const noexcept { return part_; }
    uint16_t randomStateForTests() const noexcept { return random_.state(); }
    uint8_t advanceRandomForTests() noexcept { return random_.GetByte(); }
    uint64_t hostClockEventCountForTests() const noexcept { return hostClockEventCount_; }
    int lastHostClockOffsetForTests() const noexcept { return lastHostClockOffset_; }
#if SWARAXT_ENABLE_IDLE_CPU_TESTS
    bool dormantForTests() const noexcept { return dormant_; }
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
    void advanceDormantControl();
    void updateDormantWakeState();
    void enterDormant() noexcept;
    bool voiceRequiresAudio() const noexcept;
    bool patchRequiresAudioSource() const noexcept;
    void dispatchMidi(const PendingMidi& event);
    void collectMidiEvents(const juce::MidiBuffer& midi, int startSample, int numSamples);
    void updateFilterFromShruthi();
    void applyPendingFilterQuality() noexcept;
    void snapFilterQuality() noexcept;
    void startQualityFadeOut() noexcept;
    void setMasterTarget(float value) noexcept;
    void snapMasterGain(float value) noexcept;
    float nextMasterGain() noexcept;
    float nextQualityGain() noexcept;
    void updateHostLfoRates(double bpm) noexcept;
    void prepareHostClock(const HostTransportSnapshot& transport, int numSamples) noexcept;
    void resetHostState() noexcept;
    double hostTickPpq(uint64_t tick) const noexcept;

    shruthi::HostAudioRing audioRing_;
    avrlib::Random random_;
    shruthi::MidiDispatcher midiDispatcher_;
    shruthi::Storage storage_;
    shruthi::Part part_;
    PatchBridge patchBridge_;

    HostRateConverter internalQueue_ SWARAXT_SRC_CONVERTER_INIT;
    DcBlocker dcBlocker_;
    SwaraXtFilter filter_;
    std::atomic<uint8_t> requestedFilterQuality_ {
        static_cast<uint8_t>(FilterQuality::high)
    };
    FilterQuality appliedFilterQuality_ { FilterQuality::high };
    FilterQuality pendingFilterQuality_ { FilterQuality::high };
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
    float previousVcaGain_ = 0.0f;
    float filterCutoffHz_ = 8000.0f;
    float filterResonance_ = 0.2f;
    float filterEnvAmount_ = 0.35f;
    float filterKeyTrack_ = 0.5f;
    float filterModAmount_ = 0.0f;
    float lastNote_ = 69.0f;
    int midiChannel_ = 1;
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
