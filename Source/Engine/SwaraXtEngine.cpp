// Copyright 2026 MontroneDSP.
// SPDX-License-Identifier: GPL-3.0-or-later

#include "Engine/SwaraXtEngine.h"
#include "Engine/Filter/ShruthiSvfSection2Control.h"
#include "BuiltinTunings.h"

#include <JuceHeader.h>
#include <algorithm>
#if SWARAXT_ENABLE_IDLE_CPU_TESTS
#include <chrono>
#endif
#include <chrono>
#include <cmath>
#include <cstring>
#include <limits>
#include <thread>

namespace swaraxt {

namespace {

constexpr double kLfoBeatsPerCycle[] { 0.125, 0.25, 0.5, 1.0, 2.0, 4.0, 8.0, 16.0 };

bool sameSequencePayload(const SequenceSnapshot& a, const SequenceSnapshot& b) noexcept
{
    // ARP pattern is an independent live control, applied separately below.
    return a.length == b.length && a.rotation == b.rotation
        && a.grooveTemplate == b.grooveTemplate && a.pitchMode == b.pitchMode
        && a.freePitchReady == b.freePitchReady && a.scaleReady == b.scaleReady
        && a.steps == b.steps && a.freePitch == b.freePitch && a.scaleDegree == b.scaleDegree;
}

constexpr float kMinimumPanelCutoffHz = 10.0f;
constexpr float kMaximumPanelCutoffHz = 20000.0f;

uint8_t shruthiCutoffCode(float cutoffHz)
{
    const float bounded = juce::jlimit(kMinimumPanelCutoffHz, kMaximumPanelCutoffHz, cutoffHz);
    const float semitonesBelowMaximum = 12.0f * std::log2(bounded / kMaximumPanelCutoffHz);
    return static_cast<uint8_t>(juce::jlimit(0, 127,
        static_cast<int>(std::lround(127.0f + semitonesBelowMaximum))));
}

// The 0.7 Hz safety pole reaches 1e-7 from unity in about 3.66 seconds. That is
// below -140 dBFS, while a 25-second backstop also covers decay from the
// largest finite float at every supported host rate.
constexpr float kDcSettlementThreshold = 1.0e-7f;
constexpr double kMaxDcDrainSeconds = 25.0;

}

SwaraXtEngine::SwaraXtEngine()
{
    for (auto& slot : tuningSlots_)
        slot = std::make_unique<tuning::PublishedTuning>();
    stableTuning_ = std::make_unique<tuning::PublishedTuning>();
    for (auto& claim : snapshotClaims_)
        claim.store(-1, std::memory_order_relaxed);
    patchBridge_.bind(part_);
    identityExternal_.fill(255);
    installSequencerMidiTap();
    // Only construction may fill the initial slot and the stable audio copy
    // off-thread. Later imports always use normal retirement and slot selection.
    publishTuningBytes(tuning::kBuiltin12Edo, tuning::kBuiltin12EdoSize, 0, true);
}

void SwaraXtEngine::prepare(double hostSampleRate, int maxBlockSize)
{
    hostSampleRate_ = hostSampleRate > 0.0 ? hostSampleRate : 44100.0;
    juce::ignoreUnused(maxBlockSize);
    internalQueue_.setStep(kInternalSampleRate, hostSampleRate_);
    dcBlocker_.prepare(hostSampleRate_);
    const double boundedRate = std::isfinite(hostSampleRate_) ? hostSampleRate_ : 44100.0;
    maxDcDrainSamples_ = static_cast<uint64_t>(std::ceil(boundedRate * kMaxDcDrainSeconds));
    pendingMidiCount_ = 0;
    resetHostState();

    // Filter always runs at the Shruthi internal rate (pre-SRC).
    filter_.prepare(kInternalSampleRate);
    experimentalSvf_.prepare(kInternalSampleRate);
    dualOutputDelay_.reset();
    qualityRampSamples_ = juce::jmax(1, static_cast<int>(std::lround(0.002 * kInternalSampleRate)));
    masterRampSamples_ = juce::jmax(1, static_cast<int>(std::lround(0.004 * kInternalSampleRate)));
    postMixerRampSamples_ = masterRampSamples_;
    snapFilterQuality();
    applyExperimentalSvfSelection();
    snapMasterOnApply_ = true;
    prepared_ = true;
}

void SwaraXtEngine::reset()
{
    clearSequencePlayback();
    random_.Seed(0x21);
    part_.Init(audioRing_, random_, midiDispatcher_, storage_);
    installSequencerMidiTap();
    // Collapse pending Offs and release held notes so external gear cannot stick
    // across a hard reset. Offs flush on the next processBlock.
    forceFlushSequencerMidiRelease(hostBlockOrigin_);
    identityExternal_.fill(255);
    externalOwners_.fill(0);
    releaseSequencerMidi_ = false;
    midiOutput_ = nullptr;
    // Envelope stage increments are written in UpdateDestinations(), which runs
    // inside ProcessBlock. One warm-up block establishes attack/decay rates so
    // the next NoteOn can trigger with non-zero increments (and a non-zero VCA).
    part_.ProcessBlock();
    // Reset is a hard-silence boundary. Voice::Init() enters RELEASE so the
    // hardware envelope can decay from an existing value; after the warm-up
    // above there is no existing audio to preserve, so canonicalise it to DEAD.
    part_.mutable_voice()->Kill();
    audioRing_.Init();
    patchBridge_.invalidateArpRuntimeSync();
    appliedSequenceRevision_ = std::numeric_limits<uint32_t>::max();
    internalQueue_.reset();
    internalQueue_.setStep(kInternalSampleRate, hostSampleRate_);
    filter_.reset();
    experimentalSvf_.reset();
    dualOutputDelay_.reset();
    snapFilterQuality();
    applyExperimentalSvfSelection();
    dcBlocker_.reset();
    snapMasterOnApply_ = true;
    resetVcaReconstruction();
    snapPostMixerGain(1.0f);
    resetBoardState();
    dormant_ = true;
    drainingToDormant_ = false;
    dcDrainingToDormant_ = false;
    dcDrainSamples_ = 0;
    dormantNativeSamples_ = 0.0;
    pendingMidiCount_ = 0;
    resetHostState();
#if SWARAXT_ENABLE_SHRUTHI_DEBUG_TAPS
    debugInternalBlockCount_ = 0;
#endif
}

void SwaraXtEngine::resetResampler() noexcept
{
    if (manualSwitchPhase_ != ManualSwitchPhase::stable)
    {
        filter_.reset();
        experimentalSvf_.reset();
        snapFilterQuality();
        applyExperimentalSvfSelection();
    }
    dualOutputDelay_.reset();
    resetBoardState();
    audioRing_.Init();
    internalQueue_.reset();
    internalQueue_.setStep(kInternalSampleRate, hostSampleRate_);
    dcBlocker_.reset();
    resetVcaReconstruction();
    snapPostMixerGain(postMixerGainTarget_);
    dormant_ = part_.voice().amplitude_envelope_dead() && part_.voice().vca() == 0;
    drainingToDormant_ = false;
    dcDrainingToDormant_ = false;
    dcDrainSamples_ = 0;
    dormantNativeSamples_ = 0.0;
    pendingMidiCount_ = 0;
#if SWARAXT_ENABLE_SHRUTHI_DEBUG_TAPS
    debugInternalBlockCount_ = 0;
#endif
}

void SwaraXtEngine::touchModulationRates() noexcept
{
    part_.Touch(false);
}

void SwaraXtEngine::applyParameters()
{
    auto sequence = appliedSequence_;
    auto revision = appliedSequenceRevision_;
    captureSequence(sequence, revision);
    applyParameters(sequence, revision);
}

bool SwaraXtEngine::captureSequence(SequenceSnapshot& sequence, uint32_t& revision) const noexcept
{
    sequence = appliedSequence_;
    revision = appliedSequenceRevision_;
    return sequenceState_ == nullptr || sequenceState_->capture(sequence, revision);
}

void SwaraXtEngine::applyParameters(const SequenceSnapshot& sequence, uint32_t revision)
{
    if (parameterCache_ == nullptr || ! parameterCache_->bound)
        return;

    patchBridge_.applyCacheToEngine(*parameterCache_);
    if (sequenceState_ != nullptr)
    {
        // A revision can repeat after rollover. Equal tags alone must not
        // suppress changed notes/CV/degrees while accepting a new mode.
        if (revision != appliedSequenceRevision_ || ! sameSequencePayload(sequence, appliedSequence_))
        {
            auto* settings = part_.mutable_sequencer_settings();
            settings->pattern_size = static_cast<uint8_t>(juce::jlimit(1, 16,
                static_cast<int>(sequence.length)));
            settings->pattern_rotation = static_cast<uint8_t>(sequence.rotation & 0x0f);
            settings->seq_groove_template = static_cast<uint8_t>(juce::jlimit(0, 5,
                static_cast<int>(sequence.grooveTemplate)));
            for (int i = 0; i < SequenceSnapshot::kNumSteps; ++i)
            {
                const auto packed = sequence.steps[static_cast<size_t>(i)];
                settings->steps[i].set_raw(SequenceSnapshot::dataA(packed),
                                           SequenceSnapshot::dataB(packed));
            }
            part_.setSequencerFreeCv(sequence.pitchMode == 1,
                                     sequence.freePitch.data());
            scaleRouteActive_ = sequence.pitchMode == 2;
            quantizedRouteActive_ = sequence.pitchMode == 3;
            installSequencerTuning(sequence.scaleDegree.data(),
                                   quantizedRouteActive_ ? sequence.freePitch.data() : nullptr);
            appliedSequenceRevision_ = revision;
            appliedSequence_ = sequence;
            appliedPitchMode_ = sequence.pitchMode;
        }
        part_.mutable_sequencer_settings()->arp_pattern = sequenceState_->arpPattern();
    }
    const float master = ParameterCache::load(parameterCache_->master);
    masterGain_ = master;
    if (snapMasterOnApply_ || dormant_)
    {
        snapMasterGain(master);
        snapMasterOnApply_ = false;
    }
    else
    {
        setMasterTarget(master);
    }
    const float postMixerDb = juce::jlimit(-18.0f, 0.0f,
        ParameterCache::load(parameterCache_->postMixer));
    const float postMixerLinear = postMixerDb >= 0.0f ? 1.0f
        : std::pow(10.0f, postMixerDb / 20.0f);
    if (dormant_)
        snapPostMixerGain(postMixerLinear);
    else
        setPostMixerTarget(postMixerLinear);
    filterCutoffHz_ = ParameterCache::load(parameterCache_->filterCutoff);
    filterResonance_ = ParameterCache::load(parameterCache_->filterResonance);
    filterKeyTrack_ = ParameterCache::load(parameterCache_->filterKeyTracking);
    auto* patch = part_.mutable_patch();
    patch->filter_cutoff = shruthiCutoffCode(filterCutoffHz_);
    patch->filter_resonance = static_cast<uint8_t>(juce::jlimit(0, 63,
        static_cast<int>(std::lround(filterResonance_ * 63.0f))));
    midiChannel_ = juce::jlimit(0, 16, ParameterCache::loadInt(parameterCache_->midiChannel));
    midiInputMode_ = juce::jlimit(0, 2, ParameterCache::loadInt(parameterCache_->midiInputTuning));
    lfoSync_[0] = ParameterCache::loadInt(parameterCache_->lfo1Sync) != 0;
    lfoSync_[1] = ParameterCache::loadInt(parameterCache_->lfo2Sync) != 0;
    lfoDivision_[0] = juce::jlimit(0, 7, ParameterCache::loadInt(parameterCache_->lfo1Division));
    lfoDivision_[1] = juce::jlimit(0, 7, ParameterCache::loadInt(parameterCache_->lfo2Division));
    const bool wasHostSync = sequencerHostSync_;
    sequencerHostSync_ = ParameterCache::loadInt(parameterCache_->seqClockMode) != 0;
    // A host Stop relinquishes playback. On clock handoff, an already-held key
    // must have the same start semantics as a new key in internal-clock mode.
    if (wasHostSync && !sequencerHostSync_ && !part_.running() && part_.has_held_notes())
    {
        part_.Start(true);
        part_.Clock(true);
    }
    sequencerSwing_ = juce::jlimit(0, 127, ParameterCache::loadInt(parameterCache_->seqSwing));

    const auto nextBoard = parameterCache_->boardControls();
    if (!requestedBoard_.sameTopology(nextBoard) || requestedBoard_.cv1 != nextBoard.cv1
        || requestedBoard_.cv2 != nextBoard.cv2)
        boardWake_ = nextBoard.effect != board::Effect::off
            || !activeBoard_.sameTopology(nextBoard);
    requestedBoard_ = nextBoard;
    if (snapBoardOnApply_)
    {
        activeBoard_ = requestedBoard_;
        boardProcessor_.reset(activeBoard_.effect);
        postDacFilter_.reset();
        boardFadePhase_ = BoardFadePhase::stable;
        boardGain_ = 1.0f;
        boardFadeRemaining_ = 0;
        snapBoardOnApply_ = false;
    }
    boardTailSeconds_.store(board::BoardProcessor::tailSeconds(activeBoard_), std::memory_order_relaxed);
    const bool enabled = parameterCache_ != nullptr
        && ParameterCache::loadInt(parameterCache_->seqMidiOut) != 0;
    const int mode = parameterCache_ != nullptr
        ? ParameterCache::loadInt(parameterCache_->seqMode) : 0;
    const auto route = routeFromStoredPitchMode(
        sequenceState_ != nullptr ? appliedPitchMode_ : 0);
    const bool allowed = enabled
        && mode == shruthi::SEQUENCER_MODE_SEQ
        && sequencerRouteEmitsMidiNotes(route);
    if (sequencerMidiAllowed_ && ! allowed)
        releaseSequencerMidi_ = true;
    midiOutEnabled_ = enabled;
    sequencerMidiAllowed_ = allowed;
}

void SwaraXtEngine::resetBoardState() noexcept
{
    boardProcessor_.reset();
    postDacFilter_.reset();
    activeBoard_ = {};
    boardGain_ = 1.0f;
    boardGainIncrement_ = 0.0f;
    boardFadeRemaining_ = 0;
    boardFadePhase_ = BoardFadePhase::stable;
    snapBoardOnApply_ = true;
    boardWake_ = false;
    boardTailSeconds_.store(0.0, std::memory_order_relaxed);
}

void SwaraXtEngine::updateBoardAtBlockBoundary() noexcept
{
    constexpr int ramp = 120; // Approximately 3ms each side, native-rate invariant.
    if (boardFadePhase_ == BoardFadePhase::switchPending)
    {
        activeBoard_ = requestedBoard_;
        boardProcessor_.reset(activeBoard_.effect);
        postDacFilter_.reset();
        boardFadePhase_ = BoardFadePhase::fadeIn;
        boardFadeRemaining_ = ramp;
        boardGainIncrement_ = 1.0f / static_cast<float>(ramp);
    }
    if (!activeBoard_.sameTopology(requestedBoard_) && boardFadePhase_ != BoardFadePhase::fadeOut)
    {
        boardFadePhase_ = BoardFadePhase::fadeOut;
        boardFadeRemaining_ = ramp;
        boardGainIncrement_ = -boardGain_ / static_cast<float>(ramp);
    }
    boardWake_ = false;
}

float SwaraXtEngine::nextBoardGain() noexcept
{
    if (boardFadeRemaining_ > 0)
    {
        boardGain_ += boardGainIncrement_;
        if (--boardFadeRemaining_ == 0)
        {
            if (boardFadePhase_ == BoardFadePhase::fadeOut)
            {
                boardGain_ = 0.0f;
                boardFadePhase_ = BoardFadePhase::switchPending;
            }
            else
            {
                boardGain_ = 1.0f;
                boardFadePhase_ = BoardFadePhase::stable;
            }
        }
    }
    return boardGain_;
}

bool SwaraXtEngine::boardRequiresAudio() const noexcept
{
    return boardWake_ || boardFadePhase_ != BoardFadePhase::stable
        || !activeBoard_.sameTopology(requestedBoard_)
        || boardProcessor_.needsAudio(activeBoard_);
}

void SwaraXtEngine::resetHostState() noexcept
{
    hostTransportWasPlaying_ = false;
    hostClockNeedsAlignment_ = true;
    nextHostClockTick_ = 0;
    blockStartPpq_ = 0.0;
    expectedNextPpq_ = 0.0;
    fallbackPpq_ = 0.0;
    blockPpqPerSample_ = 0.0;
    hostClockEventCount_ = 0;
    lastHostClockOffset_ = -1;
}

void SwaraXtEngine::updateHostLfoRates(double bpm) noexcept
{
    bpm = sanitizeBpm(bpm);
    for (int index = 0; index < 2; ++index)
    {
        const double beats = kLfoBeatsPerCycle[lfoDivision_[index]];
        const double increment = 65536.0 * static_cast<double>(kAudioBlockSize) * bpm
            / (kInternalSampleRate * 60.0 * beats);
        part_.SetHostLfoSync(static_cast<uint8_t>(index),
                             lfoSync_[index],
                             static_cast<uint16_t>(juce::jlimit(1, 65535,
                                 static_cast<int>(std::lround(increment)))));
    }
}

double SwaraXtEngine::hostTickPpq(uint64_t tick) const noexcept
{
    const uint64_t stepTicks = juce::jmax<uint64_t>(1, part_.clock_ticks_per_step());
    const uint64_t pairTicks = stepTicks * 2;
    const uint64_t pair = tick / pairTicks;
    const uint64_t position = tick % pairTicks;
    const double stepBeats = static_cast<double>(stepTicks) / 24.0;
    const double swing = static_cast<double>(sequencerSwing_) / 127.0 * 0.45;
    const double pairStart = static_cast<double>(pair) * stepBeats * 2.0;
    if (position < stepTicks)
        return pairStart + static_cast<double>(position) / 24.0 * (1.0 + swing);
    return pairStart + stepBeats * (1.0 + swing)
        + static_cast<double>(position - stepTicks) / 24.0 * (1.0 - swing);
}

void SwaraXtEngine::prepareHostClock(const HostTransportSnapshot& transport, int numSamples) noexcept
{
    const double bpm = sanitizeBpm(transport.bpm);
    blockPpqPerSample_ = bpm / (60.0 * hostSampleRate_);
    const bool hasValidPpq = transport.hasPpqPosition && isValidHostPpq(transport.ppqPosition);
    blockStartPpq_ = hasValidPpq ? transport.ppqPosition
        : (isValidHostPpq(fallbackPpq_) ? fallbackPpq_ : 0.0);

    const double tolerance = blockPpqPerSample_ * 2.0;
    const bool discontinuity = hasValidPpq
        && hostTransportWasPlaying_
        && std::abs(blockStartPpq_ - expectedNextPpq_) > tolerance;

    if (! sequencerHostSync_)
    {
        hostTransportWasPlaying_ = false;
        hostClockNeedsAlignment_ = true;
        fallbackPpq_ = blockStartPpq_;
        return;
    }

    if (! transport.isPlaying)
    {
        if (hostTransportWasPlaying_)
            part_.Stop(false, true);
        // Collapse pending Offs into this block and release anything still held.
        // A lead-scheduled Off can otherwise outlive the stop callback while the
        // tracker already cleared heldMidi_ at schedule time.
        forceFlushSequencerMidiRelease(hostBlockOrigin_);
        hostTransportWasPlaying_ = false;
        hostClockNeedsAlignment_ = true;
        fallbackPpq_ = blockStartPpq_;
        return;
    }

    if (! hostTransportWasPlaying_ || discontinuity || hostClockNeedsAlignment_)
    {
        // Alignment clears firmware-generated identities. Retire their external
        // notes first so a seek cannot orphan the previous step's release.
        if (discontinuity)
            forceFlushSequencerMidiRelease(hostBlockOrigin_);
        nextHostClockTick_ = static_cast<uint64_t>(
            juce::jmax(0.0, std::floor(blockStartPpq_ * 24.0)));
        while (hostTickPpq(nextHostClockTick_) < blockStartPpq_ - tolerance)
            ++nextHostClockTick_;
        part_.AlignExternalClock(nextHostClockTick_);
        hostClockNeedsAlignment_ = false;
    }

    hostTransportWasPlaying_ = true;
    expectedNextPpq_ = blockStartPpq_ + blockPpqPerSample_ * static_cast<double>(numSamples);
    fallbackPpq_ = expectedNextPpq_;
}

void SwaraXtEngine::snapMasterGain(float value) noexcept
{
    masterGainTarget_ = value;
    masterGainCurrent_ = value;
    masterGainIncrement_ = 0.0f;
    masterSamplesRemaining_ = 0;
}

void SwaraXtEngine::setMasterTarget(float value) noexcept
{
    if (value == masterGainTarget_)
        return;
    masterGainTarget_ = value;
    masterSamplesRemaining_ = juce::jmax(1, masterRampSamples_);
    masterGainIncrement_ = (value - masterGainCurrent_)
        / static_cast<float>(masterSamplesRemaining_);
}

float SwaraXtEngine::nextMasterGain() noexcept
{
    if (masterSamplesRemaining_ > 0)
    {
        masterGainCurrent_ += masterGainIncrement_;
        --masterSamplesRemaining_;
        if (masterSamplesRemaining_ == 0)
            masterGainCurrent_ = masterGainTarget_;
    }
    return masterGainCurrent_;
}

void SwaraXtEngine::snapPostMixerGain(float linear) noexcept
{
    postMixerGainTarget_ = linear;
    postMixerGainCurrent_ = linear;
    postMixerGainIncrement_ = 0.0f;
    postMixerSamplesRemaining_ = 0;
}

void SwaraXtEngine::setPostMixerTarget(float linear) noexcept
{
    if (linear == postMixerGainTarget_)
        return;
    postMixerGainTarget_ = linear;
    postMixerSamplesRemaining_ = juce::jmax(1, postMixerRampSamples_);
    postMixerGainIncrement_ = (linear - postMixerGainCurrent_)
        / static_cast<float>(postMixerSamplesRemaining_);
}

float SwaraXtEngine::nextPostMixerGain() noexcept
{
    if (postMixerSamplesRemaining_ > 0)
    {
        postMixerGainCurrent_ += postMixerGainIncrement_;
        --postMixerSamplesRemaining_;
        if (postMixerSamplesRemaining_ == 0)
            postMixerGainCurrent_ = postMixerGainTarget_;
    }
    return postMixerGainCurrent_;
}

void SwaraXtEngine::snapFilterQuality() noexcept
{
    const auto quality = renderFilterQuality();
    filter_.setQuality(quality);
    appliedFilterQuality_ = quality;
    pendingFilterQuality_ = quality;
    qualityGain_ = 1.0f;
    qualityGainIncrement_ = 0.0f;
    qualitySamplesRemaining_ = 0;
    qualityFadePhase_ = QualityFadePhase::stable;
    qualityBankAdmission_ = false;
}

void SwaraXtEngine::startQualityFadeOut() noexcept
{
    qualityFadePhase_ = QualityFadePhase::fadeOut;
    qualitySamplesRemaining_ = juce::jmax(1, qualityRampSamples_);
    qualityGainIncrement_ = (0.0f - qualityGain_)
        / static_cast<float>(qualitySamplesRemaining_);
}

void SwaraXtEngine::applyPendingFilterQuality(bool bankChangeRequested) noexcept
{
    // Preserve Classic-only retargeting. Once a bank request owns an active
    // cycle, complete that captured cycle before considering another quality.
    if (qualityBankAdmission_
        || (bankChangeRequested && qualityFadePhase_ != QualityFadePhase::stable))
    {
        qualityBankAdmission_ = true;
        return;
    }
    // Inactive Classic requests remain in the atomic mailbox. They cannot
    // start a shared gain fade, or delay a bank request at a unity boundary.
    if (experimentalSvfEnabled_ || bankChangeRequested)
        return;

    pendingFilterQuality_ = renderFilterQuality();
    if (pendingFilterQuality_ == appliedFilterQuality_
        && qualityFadePhase_ == QualityFadePhase::stable)
        return;

    if (pendingFilterQuality_ != appliedFilterQuality_
        && qualityFadePhase_ != QualityFadePhase::fadeOut)
        startQualityFadeOut();
}

void SwaraXtEngine::applyExperimentalSvfSelection() noexcept
{
    applyExperimentalSvfSelection(experimentalSvfControl_.selection());
}

void SwaraXtEngine::applyExperimentalSvfSelection(const ExperimentalSvfSelection& next) noexcept
{
    if (next.enabled != experimentalSvfEnabled_)
    {
        if (qualityFadePhase_ != QualityFadePhase::stable)
            return;
        // Configure the inactive Classic bank before it can become audible;
        // this operation never fades the outgoing Dual signal.
        if (! next.enabled)
        {
            const auto previousQuality = filter_.quality();
            snapFilterQuality();
            // A changed quality already resets the complete Classic path.
            if (filter_.quality() == previousQuality)
                filter_.reset();
        }
    }
    if (qualityFadePhase_ == QualityFadePhase::stable)
        qualityBankAdmission_ = false;

    manualSwitchPhase_ = ManualSwitchPhase::stable;
    manualSwitchRemaining_ = 0;

    const bool wasEnabled = experimentalSvfEnabled_;
    appliedExperimentalSvfSelection_ = experimentalSvfControl_.applyOnAudioThread(
        experimentalSvf_, experimentalSvfEnabled_, next);
    // A bank boundary discards the old Dual session. Routing/tap changes keep
    // queued output; their transition policy is independent of fixed latency.
    if (wasEnabled != experimentalSvfEnabled_)
        dualOutputDelay_.reset();
}

void SwaraXtEngine::updateManualFilterSelection(const ExperimentalSvfSelection& next) noexcept
{
    if (dormant_)
    {
        applyExperimentalSvfSelection(next);
        return;
    }
    if (manualSwitchPhase_ == ManualSwitchPhase::mutedPreparation
        && manualSwitchRemaining_ == 0)
    {
        if (next.enabled != experimentalSvfEnabled_)
        {
            // Exact zero was emitted before this boundary. The captured tuple
            // configures the incoming path once; later requests stay pending.
            applyExperimentalSvfSelection(next);
            manualSwitchPhase_ = ManualSwitchPhase::mutedPreparation;
            manualSwitchRemaining_ = kManualSwitchPreparationSamples;
        }
        else
        {
            // Reversal before reset preserves the original filter and FIFO.
            manualSwitchPhase_ = ManualSwitchPhase::fadeIn;
            manualSwitchRemaining_ = kManualSwitchFadeSamples;
        }
        return;
    }
    if (manualSwitchPhase_ != ManualSwitchPhase::stable)
        return;

    const bool bankChange = next.enabled != experimentalSvfEnabled_;
    applyPendingFilterQuality(bankChange);
    if (bankChange)
    {
        if (qualityFadePhase_ == QualityFadePhase::stable)
        {
            qualityBankAdmission_ = false;
            manualSwitchPhase_ = ManualSwitchPhase::fadeOut;
            manualSwitchRemaining_ = kManualSwitchFadeSamples;
        }
        return;
    }
    applyExperimentalSvfSelection(next);
}

float SwaraXtEngine::nextQualityGain() noexcept
{
    if (qualitySamplesRemaining_ <= 0)
        return qualityGain_;

    qualityGain_ += qualityGainIncrement_;
    --qualitySamplesRemaining_;
    if (qualitySamplesRemaining_ > 0)
        return qualityGain_;

    if (qualityFadePhase_ == QualityFadePhase::fadeOut)
    {
        qualityGain_ = 0.0f;
        if (filter_.quality() != pendingFilterQuality_)
            filter_.setQuality(pendingFilterQuality_);
        appliedFilterQuality_ = pendingFilterQuality_;
        qualityFadePhase_ = QualityFadePhase::fadeIn;
        qualitySamplesRemaining_ = juce::jmax(1, qualityRampSamples_);
        qualityGainIncrement_ = (1.0f - qualityGain_)
            / static_cast<float>(qualitySamplesRemaining_);
    }
    else if (qualityFadePhase_ == QualityFadePhase::fadeIn)
    {
        qualityGain_ = 1.0f;
        qualityFadePhase_ = QualityFadePhase::stable;
        qualityGainIncrement_ = 0.0f;
        if (pendingFilterQuality_ != appliedFilterQuality_)
            startQualityFadeOut();
    }

    return qualityGain_;
}

void SwaraXtEngine::updateFilterFromShruthi()
{
    const auto& voice = part_.voice();
    // The final firmware CV includes matrix, tracking, envelope, LFO and all
    // original integer clipping. Decode it once; only key tracking trim follows.
    const float nativeCutoffHz = 20000.0f * std::exp2(
        (static_cast<float>(voice.cutoff()) - 254.0f) / 24.0f);
    const float controlNote = static_cast<float>(voice.filter_pitch_value()) / 128.0f;
    // Midpoint preserves firmware tracking exactly. This optional trim operates
    // after firmware clipping and therefore cannot undo a saturated native CV.
    const float trackingTrim = part_.patch().osc[0].option == shruthi::OP_DUO
        ? 0.0f : (2.0f * filterKeyTrack_ - 1.0f) * (controlNote - 64.0f) / 12.0f;

    SwaraXtFilterParams p;
    p.cutoffHz = juce::jlimit(10.0f, 20000.0f, nativeCutoffHz);
    p.resonance = static_cast<float>(voice.resonance()) / 255.0f;
    p.keyTrack = 0.0f;
    p.matrixCutoffOctaves = trackingTrim;
    p.noteNumber = controlNote;
    p.drive = 1.0f;
    const double extraOctaves = p.matrixCutoffOctaves;
    // Transport every native byte directly to the board CV domain; the panel-Hz
    // clamp is not authority for hardware cutoff reconstruction.
    p.boardCutoffCvVolts = std::clamp((static_cast<double>(voice.cutoff())
        + 24.0 * extraOctaves) * (5.0 / 255.0), 0.0, 5.0);
    filter_.setParams(p);
    experimentalSvf_.setFirmwareCutoff(voice.cutoff());
    experimentalSvf_.setFirmwareResonance(voice.resonance());
    const auto& selection = appliedExperimentalSvfSelection_;
    if (experimentalSvfEnabled_ && selection.control2 != ShruthiSvfSection2Control::legacyFollow)
    {
        const auto cv = reconstructSvfSection2(part_.patch(), voice, selection.cutoff2,
            selection.resonance2, selection.control2 == ShruthiSvfSection2Control::relative);
        experimentalSvf_.setFirmwareCutoff2(cv.cutoff);
        experimentalSvf_.setFirmwareResonance2(cv.resonance);
    }
    if (activeBoard_.effect != board::Effect::off)
    {
        activeBoard_.cv1 = board::BoardControl::nativeCv(voice.cv_1());
        activeBoard_.cv2 = board::BoardControl::nativeCv(voice.cv_2());
        activeBoard_.tempo = boardTempo_;
    }
    boardTailSeconds_.store(board::BoardProcessor::tailSeconds(activeBoard_), std::memory_order_relaxed);
}

void SwaraXtEngine::dispatchMidi(const PendingMidi& event)
{
    const uint8_t channel = static_cast<uint8_t>(event.status & 0x0F);
    if (midiChannel_ > 0 && channel != static_cast<uint8_t>(midiChannel_ - 1))
        return;

    const uint8_t statusHi = static_cast<uint8_t>(event.status & 0xF0);

    if (statusHi == 0x90)
    {
        if (event.data2 == 0)
        {
            midiDispatcher_.NoteOff(channel, event.data1, 0);
        }
        else
        {
            midiDispatcher_.NoteOn(channel, event.data1, event.data2);
        }
    }
    else if (statusHi == 0x80)
    {
        midiDispatcher_.NoteOff(channel, event.data1, event.data2);
    }
    else if (statusHi == 0xB0)
    {
        if (event.data1 == 120)
            midiDispatcher_.AllSoundOff(channel);
        else if (event.data1 == 123)
            midiDispatcher_.AllNotesOff(channel);
        else if (event.data1 == 121)
            midiDispatcher_.ResetAllControllers(channel);
        else
            midiDispatcher_.ControlChange(channel, event.data1, event.data2);
    }
    else if (statusHi == 0xE0)
    {
        const uint16_t bend = static_cast<uint16_t>((event.data2 << 7) | event.data1);
        midiDispatcher_.PitchBend(channel, bend);
    }
    else if (statusHi == 0xD0)
    {
        midiDispatcher_.Aftertouch(channel, event.data1);
    }
    else if (statusHi == 0xA0)
    {
        midiDispatcher_.Aftertouch(channel, event.data1, event.data2);
    }
}

void SwaraXtEngine::collectMidiEvents(juce::MidiBufferIterator& cursor,
                                     const juce::MidiBufferIterator& end,
                                     int startSample,
                                     int numSamples)
{
    pendingMidiCount_ = 0;
    const int endSample = startSample + numSamples;

    for (; cursor != end && pendingMidiCount_ < kMaxPendingMidi; ++cursor)
    {
        const auto metadata = *cursor;
        if (metadata.samplePosition < startSample || metadata.samplePosition >= endSample)
            continue;

        if (metadata.numBytes < 1 || metadata.numBytes > 3)
            continue;

        const auto* data = metadata.data;
        const uint8_t status = data[0];
        const uint8_t statusHi = static_cast<uint8_t>(status & 0xF0);
        const int requiredBytes = statusHi == 0xD0 ? 2 : 3;
        if (status < 0x80 || status >= 0xF0 || metadata.numBytes < requiredBytes)
            continue;
        if (statusHi != 0x80 && statusHi != 0x90 && statusHi != 0xA0
            && statusHi != 0xB0 && statusHi != 0xD0 && statusHi != 0xE0)
            continue;

        PendingMidi pending;
        pending.sampleOffset = metadata.samplePosition;
        pending.status = status;
        pending.data1 = static_cast<uint8_t>(data[1] & 0x7F);
        pending.data2 = requiredBytes == 3 ? static_cast<uint8_t>(data[2] & 0x7F) : 0;

        pendingMidi_[static_cast<size_t>(pendingMidiCount_++)] = pending;
    }

}

float SwaraXtEngine::processSelectedFilterSample(float mixer, float vcaTarget) noexcept
{
    if (manualSwitchPhase_ == ManualSwitchPhase::mutedPreparation
        && manualSwitchRemaining_ == 0)
    {
#if defined(SWARAXT_ENABLE_MANUAL_SWITCH_TESTS)
        manualSwitchSample_ = {0, 0, 0, 2, experimentalSvfEnabled_, false};
#endif
        return 0;
    }
    const float raw = experimentalSvfEnabled_
        ? dualOutputDelay_.process(experimentalSvf_.process(mixer, vcaTarget))
        : filter_.processInstrumentSample(mixer, vcaTarget);
    const auto phase = manualSwitchPhase_;
    if (phase == ManualSwitchPhase::stable)
    {
#if defined(SWARAXT_ENABLE_MANUAL_SWITCH_TESTS)
        manualSwitchSample_ = {raw, 1, raw, 0, experimentalSvfEnabled_, true};
#endif
        return raw;
    }
    float gain = 1;
    if (phase == ManualSwitchPhase::fadeOut)
        gain = static_cast<float>(manualSwitchRemaining_ - 1)
            / static_cast<float>(kManualSwitchFadeSamples - 1);
    else if (phase == ManualSwitchPhase::mutedPreparation)
        gain = 0;
    else if (phase == ManualSwitchPhase::fadeIn)
        gain = static_cast<float>(kManualSwitchFadeSamples - manualSwitchRemaining_)
            / static_cast<float>(kManualSwitchFadeSamples - 1);

    const float output = gain == 1 ? raw : (gain == 0 ? 0 : raw * gain);
    if (--manualSwitchRemaining_ == 0)
    {
        if (phase == ManualSwitchPhase::fadeOut)
            manualSwitchPhase_ = ManualSwitchPhase::mutedPreparation;
        else if (phase == ManualSwitchPhase::mutedPreparation)
        {
            manualSwitchPhase_ = ManualSwitchPhase::fadeIn;
            manualSwitchRemaining_ = kManualSwitchFadeSamples;
        }
        else
            manualSwitchPhase_ = ManualSwitchPhase::stable;
    }
#if defined(SWARAXT_ENABLE_MANUAL_SWITCH_TESTS)
    manualSwitchSample_ = {raw, gain, output, static_cast<int>(phase), experimentalSvfEnabled_, true};
#endif
    return output;
}

float SwaraXtEngine::selectedFilterVca() const noexcept
{
    if (experimentalSvfEnabled_)
        return experimentalSvf_.vcaControl();
    return filter_.instrumentVcaControl();
}

bool SwaraXtEngine::selectedFilterTailActive() const noexcept
{
    if (manualSwitchPhase_ != ManualSwitchPhase::stable)
        return true;
    if (experimentalSvfEnabled_)
    {
        // Resonator state cannot sound behind an exactly closed VCA. Keep
        // rendering until the selected VCA and all delayed output are empty;
        // a running internal clock must also advance between sequencer gates.
        if (experimentalSvf_.vcaControl() != 0.0f || dualOutputDelay_.tailActive()
            || (part_.running() && part_.sequencer_settings().seq_tempo >= 40))
            return true;
        const auto& patch = part_.patch();
        if (patch.lfo[1].retrigger_mode == shruthi::LFO_MODE_MASTER)
            return true;
        bool envelopeGatesVca = false;
        for (int i = 0; i < shruthi::kModulationMatrixSize; ++i)
        {
            const auto& row = patch.modulation_matrix.modulation[i];
            if (row.amount == 0)
                continue;
            if (row.source == shruthi::MOD_SRC_AUDIO || row.destination == shruthi::MOD_DST_TRIGGER_ENV_2)
                return true;
            // Firmware VCA rows multiply. A full, unscaled dead-envelope row
            // forces exact zero despite other factors; the last row is wheel-scaled.
            envelopeGatesVca |= i != shruthi::kModulationMatrixSize - 1
                && row.destination == shruthi::MOD_DST_VCA
                && row.source == shruthi::MOD_SRC_ENV_2 && row.amount == 63;
        }
        return !envelopeGatesVca;
    }
    return filter_.instrumentTailActive();
}

void SwaraXtEngine::renderInternalBlock()
{
#if SWARAXT_ENABLE_IDLE_CPU_TESTS
    using ProfileClock = std::chrono::steady_clock;
    ++cpuProfile_.nativeBlocksRendered;
#endif
    // If the host ring is lagging, drain enough samples to free one block
    // instead of Init() (which drops unread audio and desyncs the consumer).
    while (audioRing_.writable() < static_cast<uint16_t>(kAudioBlockSize))
    {
        float discard[kAudioBlockSize];
        if (audioRing_.readBlockRaw(discard, kAudioBlockSize) <= 0)
            break;
    }

#if SWARAXT_ENABLE_IDLE_CPU_TESTS
    const auto voiceStart = ProfileClock::now();
#endif
    part_.ProcessBlock();
#if SWARAXT_ENABLE_IDLE_CPU_TESTS
    cpuProfile_.nativeVoiceNanoseconds += static_cast<uint64_t>(
        std::chrono::duration_cast<std::chrono::nanoseconds>(ProfileClock::now() - voiceStart).count());
#endif
    updateBoardAtBlockBoundary();
    const auto filterSelection = experimentalSvfControl_.selection();
    updateManualFilterSelection(filterSelection);
    updateFilterFromShruthi();

    const uint8_t vca = part_.voice().vca();
    float temp[kAudioBlockSize];
    const int read = audioRing_.readBlockRaw(temp, kAudioBlockSize);

#if SWARAXT_ENABLE_SHRUTHI_DEBUG_TAPS
    DebugBlockCapture capture;
    capture.samples = read;
    capture.nativeBlockIndex = debugInternalBlockCount_;
    const auto* rawOsc1 = part_.voice().debug_osc1_buffer();
    const auto* rawOsc2 = part_.voice().debug_osc2_buffer();
    capture.lfo1 = part_.voice().modulation_source(shruthi::MOD_SRC_LFO_1);
    capture.lfo2 = part_.voice().modulation_source(shruthi::MOD_SRC_LFO_2);
    capture.env1 = part_.voice().modulation_source(shruthi::MOD_SRC_ENV_1);
    capture.env2 = part_.voice().modulation_source(shruthi::MOD_SRC_ENV_2);
    capture.oscCoarse = part_.voice().debug_destination14(shruthi::MOD_DST_VCO_1_2_COARSE);
    capture.oscFine = part_.voice().debug_destination14(shruthi::MOD_DST_VCO_1_2_FINE);
    capture.cutoff = part_.voice().cutoff();
    capture.resonance = part_.voice().resonance();
    capture.vca = part_.voice().vca();
    for (int i = 0; i < kAudioBlockSize; ++i)
    {
        capture.rawOsc1[i] = (static_cast<float>(rawOsc1[i]) - 128.0f) / 128.0f;
        capture.rawOsc2[i] = (static_cast<float>(rawOsc2[i]) - 128.0f) / 128.0f;
    }
#endif

    // Preserve the held native byte target. The selected analog path performs
    // CV reconstruction and VCA multiplication before FIR decimation.
    const float vcaTarget = static_cast<float>(vca) / 255.0f;
#if SWARAXT_ENABLE_IDLE_CPU_TESTS
    const auto filterStart = ProfileClock::now();
#endif
    const bool effectsBypassed = activeBoard_.effect == board::Effect::off
        && boardFadePhase_ == BoardFadePhase::stable;
    bool selectedOutputSilent = true;
    if (effectsBypassed)
    {
        postDacFilter_.reset();
        for (int i = 0; i < read; ++i)
        {
            float mixer = temp[i] * nextPostMixerGain();
            if (! std::isfinite(mixer))
                mixer = 0.0f;
            float filtered = processSelectedFilterSample(mixer, vcaTarget);
            if (! std::isfinite(filtered))
                filtered = 0.0f;
            const float vcaGain = selectedFilterVca();
            vcaCvState_ = vcaGain;
#if defined(SWARAXT_ENABLE_MANUAL_SWITCH_TESTS)
            capture.manualSwitch[i] = manualSwitchSample_;
#endif
            const float out = filtered * nextMasterGain() * nextQualityGain();
#if SWARAXT_ENABLE_SHRUTHI_DEBUG_TAPS
            capture.postShruthiMixer[i] = temp[i];
            capture.filterOutput[i] = filtered;
            capture.postVca[i] = out;
            capture.vcaTarget[i] = vcaTarget;
            capture.vcaGain[i] = vcaGain;
#if defined(SWARAXT_ENABLE_DUAL_SVF_INTEGRATION_TESTS)
            capture.dualUndelayed[i] = dualOutputDelay_.samples[(dualOutputDelay_.cursor + 255u) % 256u];
#endif
#endif
            internalQueue_.push(std::isfinite(out) ? out : 0.0f);
        }
    }
    else
    {
        board::FloatBlock processed{};
        for (int i = 0; i < read; ++i)
        {
            const auto index = static_cast<std::size_t>(i);
            float mixer = temp[i] * nextPostMixerGain();
            if (! std::isfinite(mixer))
                mixer = 0.0f;
            const auto filtered = processSelectedFilterSample(mixer, vcaTarget);
            const float vcaGain = selectedFilterVca();
            vcaCvState_ = vcaGain;
            processed[index] = std::isfinite(filtered) ? filtered : 0.0f;
#if defined(SWARAXT_ENABLE_MANUAL_SWITCH_TESTS)
            capture.manualSwitch[i] = manualSwitchSample_;
#endif
#if SWARAXT_ENABLE_SHRUTHI_DEBUG_TAPS
            capture.vcaGain[i] = vcaGain;
#if defined(SWARAXT_ENABLE_DUAL_SVF_INTEGRATION_TESTS)
            capture.dualUndelayed[i] = dualOutputDelay_.samples[(dualOutputDelay_.cursor + 255u) % 256u];
#endif
#endif
#if SWARAXT_ENABLE_SHRUTHI_DEBUG_TAPS
            capture.postShruthiMixer[i] = temp[i];
            capture.vcaTarget[i] = vcaTarget;
#endif
        }
        boardProcessor_.processClassicFx(processed, activeBoard_);
        // v03 post-DAC baseband model. FX Off never reaches this branch while stable,
        // and processClassicFx itself stays the 12-bit firmware path.
        if (activeBoard_.effect != board::Effect::off)
            postDacFilter_.process(processed, static_cast<std::size_t>(read));
        else
            postDacFilter_.reset();
        if (experimentalSvfEnabled_ && vcaTarget == 0.0f
            && experimentalSvf_.vcaControl() == 0.0f && !dualOutputDelay_.tailActive())
        {
            // A quiet FX flag can still leave a constant quantized output.
            // Replacing that level with drain zeros would excite the DC path.
            selectedOutputSilent = std::all_of(processed.begin(), processed.begin() + read,
                [](float sample) noexcept { return sample == 0.0f; });
        }
        for (int i = 0; i < read; ++i)
        {
            const auto index = static_cast<std::size_t>(i);
            const float masterGain = nextMasterGain();
            const float qualityGain = nextQualityGain();
            const float out = processed[index] * masterGain
                * qualityGain * nextBoardGain();
#if SWARAXT_ENABLE_SHRUTHI_DEBUG_TAPS
            capture.filterOutput[i] = processed[index];
            capture.postVca[i] = out;
#endif
            internalQueue_.push(std::isfinite(out) ? out : 0.0f);
        }
    }
#if SWARAXT_ENABLE_IDLE_CPU_TESTS
    cpuProfile_.filterNanoseconds += static_cast<uint64_t>(
        std::chrono::duration_cast<std::chrono::nanoseconds>(ProfileClock::now() - filterStart).count());
    cpuProfile_.filterSamplesProcessed += static_cast<uint64_t>(read);
#endif
    if (read > 0 && vcaTarget == 0.0f && vcaCvState_ < (1.0f / 512.0f)
        && !selectedFilterTailActive() && selectedOutputSilent
        && part_.voice().amplitude_envelope_dead() && !boardRequiresAudio())
    {
        vcaCvState_ = 0.0f;
        // Give the reader enough future zeros to drain reconstruction state
        // without extrapolating past the queue. One kernel length is what it
        // takes for the reconstruction window to move completely past the last
        // real sample, so the tail is never truncated. The heavy voice and
        // filter stay idle while this drains.
        const int tailZeros = internalQueue_.minimumReadableSize();
        for (int i = 0; i < tailZeros; ++i)
            internalQueue_.push(0.0f);
        drainingToDormant_ = true;
        dcDrainingToDormant_ = false;
        dcDrainSamples_ = 0;
    }

#if SWARAXT_ENABLE_SHRUTHI_DEBUG_TAPS
    if (debugTapSink_ != nullptr)
        debugTapSink_(debugTapContext_, capture);
    ++debugInternalBlockCount_;
#endif
}

bool SwaraXtEngine::voiceRequiresAudio() const noexcept
{
    return ! part_.voice().amplitude_envelope_dead()
        || boardRequiresAudio()
        || part_.voice().vca() != 0
        || patchRequiresAudioSource();
}

bool SwaraXtEngine::patchRequiresAudioSource() const noexcept
{
    const auto& routes = part_.patch().modulation_matrix.modulation;
    for (const auto& route : routes)
        if (route.amount != 0 && route.source == shruthi::MOD_SRC_AUDIO)
            return true;

    return false;
}

void SwaraXtEngine::updateDormantWakeState()
{
#if SWARAXT_ENABLE_IDLE_CPU_TESTS
    if (! dormancyEnabledForTests_)
    {
        dormant_ = false;
        drainingToDormant_ = false;
        dcDrainingToDormant_ = false;
        dcDrainSamples_ = 0;
        return;
    }
#endif
    // Dual control edits can invalidate silence during downstream drainage.
    // A manual request during SRC/DC drainage needs native transactions again.
    // Wait until native input is needed, so a request reversed while queued
    // output drains cannot advance otherwise idle firmware/filter history.
    if (! voiceRequiresAudio()
        && !(experimentalSvfEnabled_ && drainingToDormant_ && selectedFilterTailActive())
        && (! drainingToDormant_
            || experimentalSvfControl_.selection().enabled == experimentalSvfEnabled_
            || internalQueue_.size() >= internalQueue_.queueTargetSize()))
        return;

    drainingToDormant_ = false;
    dcDrainingToDormant_ = false;
    dcDrainSamples_ = 0;
    if (! dormant_)
        return;

    dormant_ = false;
    audioRing_.Init();
    internalQueue_.reset();
    internalQueue_.setStep(kInternalSampleRate, hostSampleRate_);
    filter_.reset();
    experimentalSvf_.reset();
    dualOutputDelay_.reset();
    snapFilterQuality();
    applyExperimentalSvfSelection();
    dcBlocker_.reset();
    resetVcaReconstruction();
    snapMasterOnApply_ = true;
    snapPostMixerGain(postMixerGainTarget_);
    dormantNativeSamples_ = 0.0;
}

void SwaraXtEngine::enterDormant() noexcept
{
    dormant_ = true;
    drainingToDormant_ = false;
    dcDrainingToDormant_ = false;
    dcDrainSamples_ = 0;
    dormantNativeSamples_ = 0.0;
    audioRing_.Init();
    internalQueue_.reset();
    internalQueue_.setStep(kInternalSampleRate, hostSampleRate_);
    filter_.reset();
    experimentalSvf_.reset();
    dualOutputDelay_.reset();
    snapFilterQuality();
    applyExperimentalSvfSelection();
    dcBlocker_.reset();
    resetVcaReconstruction();
    snapMasterOnApply_ = true;
    snapPostMixerGain(postMixerGainTarget_);
}

void SwaraXtEngine::advanceDormantControl()
{
    dormantNativeSamples_ += kInternalSampleRate / hostSampleRate_;
    while (dormantNativeSamples_ >= static_cast<double>(kAudioBlockSize))
    {
        dormantNativeSamples_ -= static_cast<double>(kAudioBlockSize);
        part_.ProcessControlBlock();
        // The physical board reconstructs CV even when its VCA is closed.
        updateManualFilterSelection(experimentalSvfControl_.selection());
        updateFilterFromShruthi();
        filter_.advanceDormantControls(kAudioBlockSize);
#if SWARAXT_ENABLE_IDLE_CPU_TESTS
        ++cpuProfile_.dormantControlBlocksAdvanced;
#endif
        updateDormantWakeState();
        if (! dormant_)
            break;
    }
}

void SwaraXtEngine::process(const juce::MidiBuffer& midi,
                           juce::AudioBuffer<float>& buffer,
                           int startSample,
                           int numSamples,
                           const HostTransportSnapshot& transport,
                           juce::MidiBuffer* midiOut, bool adoptControls,
                           const SequenceSnapshot* sequence, uint32_t revision, bool resetRequested)
{
    const juce::ScopedValueSetter<bool> controlScope(adoptControls_, adoptControls);
    if (adoptControls)
        admittedFilterQuality_ = filterQuality();
#if SWARAXT_ENABLE_IDLE_CPU_TESTS
    using ProfileClock = std::chrono::steady_clock;
    const auto processStart = ProfileClock::now();
#endif
    if (! prepared_ || numSamples <= 0)
        return;

    if (buffer.getNumChannels() <= 0 || startSample < 0)
        return;

    if (startSample + numSamples > buffer.getNumSamples())
        numSamples = buffer.getNumSamples() - startSample;
    if (numSamples <= 0)
        return;

    struct TuningReadScope {
        std::atomic<bool>& flag;
        explicit TuningReadScope(std::atomic<bool>& flagIn) noexcept : flag(flagIn)
        {
            // Entry participates in the same total order as publication and
            // retirement. A release-only entry permits reclamation to miss it
            // across successive publications before the audio latch is stored.
            flag.store(true, std::memory_order_seq_cst);
        }
        ~TuningReadScope() { flag.store(false, std::memory_order_seq_cst); }
    } tuningRead(tuningAudioActive_);

    if (adoptControls)
        latchPublishedTuning();
    if (sequence != nullptr)
    {
        if (resetRequested)
        {
            reset();
            touchModulationRates();
        }
        applyParameters(*sequence, revision);
    }
    midiOutput_ = midiOut;
    if (releaseSequencerMidi_)
    {
        forceFlushSequencerMidiRelease(hostBlockOrigin_);
        releaseSequencerMidi_ = false;
    }
    if (retrySequencerMidiRelease_)
    {
        retrySequencerMidiRelease_ = false;
        // One bounded pass per callback. Retain the original channel until an
        // Off is admitted; never release a pitch that has acquired a new owner.
        for (uint8_t note = 0; note < 128; ++note)
        {
            if (heldMidi_[note] == 0 || externalOwners_[note] != 0)
                continue;
            if (enqueueScheduledMidi(hostBlockOrigin_, false, note, 0, heldOutputChannel(note)))
            {
                heldMidi_[note] = 0;
                heldMidiChannel_[note] = 0;
            }
            else
            {
                // A full all-Off queue cannot admit any other release until
                // it drains later in this callback. Keep the rest for retry.
                retrySequencerMidiRelease_ = true;
                break;
            }
        }
    }
    auto midiCursor = midi.cbegin();
    const auto midiEnd = midi.cend();
    collectMidiEvents(midiCursor, midiEnd, startSample, numSamples);
    boardTempo_ = board::BoardControl::tempoCode(sequencerHostSync_ && transport.hasHostTransport
        ? transport.bpm : static_cast<double>(requestedBoard_.tempo));
    updateHostLfoRates(transport.bpm);
    prepareHostClock(transport, numSamples);
#if SWARAXT_ENABLE_IDLE_CPU_TESTS
    const auto audioStart = ProfileClock::now();
    cpuProfile_.midiAndClockNanoseconds += static_cast<uint64_t>(
        std::chrono::duration_cast<std::chrono::nanoseconds>(audioStart - processStart).count());
#endif
    int midiIndex = 0;
    // Refill fixed storage as it drains. The cursor only advances through the
    // caller's finite input; later events retain their original order/offsets.
    const auto dispatchMidiThrough = [&](int sample) {
        for (;;)
        {
            if (midiIndex == pendingMidiCount_)
            {
                if (midiCursor == midiEnd)
                    return;
                collectMidiEvents(midiCursor, midiEnd, startSample, numSamples);
                midiIndex = 0;
                if (pendingMidiCount_ == 0)
                    return;
            }
            if (pendingMidi_[static_cast<size_t>(midiIndex)].sampleOffset > sample)
                return;
            dispatchMidi(pendingMidi_[static_cast<size_t>(midiIndex++)]);
        }
    };

    auto* left = buffer.getWritePointer(0, startSample);
    float* right = buffer.getNumChannels() > 1 ? buffer.getWritePointer(1, startSample) : nullptr;

    int guard = 0;
    dispatchMidiThrough(startSample);
    updateDormantWakeState();

    const auto queueTarget = [&]() {
        return internalQueue_.queueTargetSize();
    };

    renderHostCursor_ = 0;
    while (! dormant_ && ! drainingToDormant_
           && internalQueue_.size() < queueTarget())
    {
        if (guard >= 16)
        {
            ++guard;
            break;
        }

        ++guard;
        renderInternalBlock();
    }

    for (int i = 0; i < numSamples; ++i)
    {
        const int absoluteSample = startSample + i;
        renderHostCursor_ = i;
        if (sequencerHostSync_ && transport.isPlaying)
        {
            const double samplePpq = blockStartPpq_ + blockPpqPerSample_ * static_cast<double>(i);
            while (hostTickPpq(nextHostClockTick_) <= samplePpq + blockPpqPerSample_ * 0.5)
            {
                part_.Clock(false);
                ++hostClockEventCount_;
                lastHostClockOffset_ = i;
                ++nextHostClockTick_;
            }
            updateDormantWakeState();
        }
        dispatchMidiThrough(absoluteSample);
        updateDormantWakeState();

        if (dormant_)
        {
            advanceDormantControl();
            if (dormant_)
            {
#if SWARAXT_ENABLE_IDLE_CPU_TESTS
                ++cpuProfile_.dormantHostSamplesSkipped;
#endif
                left[i] = 0.0f;
                if (right != nullptr)
                    right[i] = 0.0f;
                continue;
            }
        }

        guard = 0;
        while (! drainingToDormant_
               && internalQueue_.size() < queueTarget())
        {
            if (guard >= 16)
            {
                ++guard;
                break;
            }

            ++guard;
            renderInternalBlock();
        }

        float sample = 0.0f;
        if (internalQueue_.size() >= internalQueue_.minimumReadableSize())
            sample = dcBlocker_.process(internalQueue_.readInterpolated());
        else if (drainingToDormant_)
        {
            dcDrainingToDormant_ = true;
            sample = dcBlocker_.process(0.0f);
            ++dcDrainSamples_;
#if SWARAXT_ENABLE_IDLE_CPU_TESTS
            ++cpuProfile_.dcDrainHostSamples;
#endif
            // After this zero input x1 is canonical zero and y1 equals sample,
            // so the emitted value is the complete future-output state.
            if (! std::isfinite(sample)
                || std::abs(sample) <= kDcSettlementThreshold
                || dcDrainSamples_ >= maxDcDrainSamples_)
                enterDormant();
        }

        if (! std::isfinite(sample))
            sample = 0.0f;

        left[i] = sample;
        if (right != nullptr)
            right[i] = sample;
    }
    // All borrowed input has now been consumed. Clear without surrendering
    // host storage; supported wrappers reserve output capacity before processing.
    if (midiOut == &midi)
        midiOut->clear();
    if (midiOut != nullptr)
        flushSequencerMidi(*midiOut, numSamples);
    else
        hostBlockOrigin_ += static_cast<uint64_t>(numSamples);
    midiOutput_ = nullptr;
    const auto playbackStep = part_.playback_step();
    sequencePlayback_.store(playbackStep < 16 ? static_cast<unsigned>(playbackStep) + 1u : 0u,
                            std::memory_order_relaxed);
#if defined(SWARAXT_ENABLE_TUNING_OWNERSHIP_TESTS)
    tuningPoint(TuningPoint::audioExiting);
#endif
#if SWARAXT_ENABLE_IDLE_CPU_TESTS
    const auto processEnd = ProfileClock::now();
    cpuProfile_.processNanoseconds += static_cast<uint64_t>(
        std::chrono::duration_cast<std::chrono::nanoseconds>(processEnd - processStart).count());
    const auto accounted = cpuProfile_.nativeVoiceNanoseconds + cpuProfile_.filterNanoseconds;
    const auto total = cpuProfile_.processNanoseconds;
    cpuProfile_.srcAndOutputNanoseconds = total > accounted + cpuProfile_.midiAndClockNanoseconds
        ? total - accounted - cpuProfile_.midiAndClockNanoseconds
        : 0;
    cpuProfile_.hostSamplesProduced += static_cast<uint64_t>(numSamples);
#endif
}

void SwaraXtEngine::installSequencerMidiTap() noexcept
{
    midiDispatcher_.setSequencerNoteTap({
        &SwaraXtEngine::sequencerNoteOnTap,
        &SwaraXtEngine::sequencerNoteOffTap,
        this
    });
}

void SwaraXtEngine::sequencerNoteOnTap(void* context, uint8_t note, uint8_t velocity, uint8_t external) noexcept
{
    static_cast<SwaraXtEngine*>(context)->onSequencerNoteOn(note, velocity, external);
}

void SwaraXtEngine::sequencerNoteOffTap(void* context, uint8_t note) noexcept
{
    static_cast<SwaraXtEngine*>(context)->onSequencerNote(false, note, 0);
}

void SwaraXtEngine::onSequencerNote(bool noteOn, uint8_t note, uint8_t velocity) noexcept
{
    if (noteOn)
        onSequencerNoteOn(note, velocity, note);
    else
        onSequencerNoteOff(note);
}

void SwaraXtEngine::onSequencerNoteOn(uint8_t identity, uint8_t velocity, uint8_t external) noexcept
{
    if (! midiOutEnabled_ || ! sequencerMidiAllowed_ || midiOutput_ == nullptr
        || identity >= 128 || external >= 128)
        return;
    if (identityExternal_[identity] != 255)
        onSequencerNoteOff(identity);
    const bool retrigger = externalOwners_[external] > 0 || heldMidi_[external] != 0;
    if (retrigger)
    {
        // Never replace the held channel before its release has storage.
        if (!scheduleSequencerMidi(false, external, 0, heldOutputChannel(external)))
            return;
        heldMidi_[external] = 0;
        heldMidiChannel_[external] = 0;
    }
    const uint8_t midiVelocity = velocity == 0 ? 1 : (velocity > 127 ? 127 : velocity);
    const uint8_t channel = static_cast<uint8_t>(outputMidiChannel());
    if (scheduleSequencerMidi(true, external, midiVelocity, channel))
    {
        identityExternal_[identity] = external;
        if (externalOwners_[external] < 255)
            ++externalOwners_[external];
        heldMidi_[external] = midiVelocity;
        heldMidiChannel_[external] = channel;
    }
}

void SwaraXtEngine::onSequencerNoteOff(uint8_t identity) noexcept
{
    if (identity >= 128)
        return;
    const uint8_t external = identityExternal_[identity];
    identityExternal_[identity] = 255;
    if (external >= 128)
        return;
    if (externalOwners_[external] > 0)
        --externalOwners_[external];
    if (externalOwners_[external] == 0 && heldMidi_[external] != 0)
    {
        const uint8_t channel = heldOutputChannel(external);
        if (scheduleSequencerMidi(false, external, 0, channel))
        {
            heldMidi_[external] = 0;
            heldMidiChannel_[external] = 0;
        }
        else
            retrySequencerMidiRelease_ = true;
    }
}

int SwaraXtEngine::sequencerMidiLeadSamples() const noexcept
{
    const int ahead = juce::jmax(0, internalQueue_.size() - internalQueue_.minimumReadableSize());
    const double native = static_cast<double>(ahead + kAudioBlockSize);
    return static_cast<int>(std::llround(native * hostSampleRate_ / kInternalSampleRate));
}

int SwaraXtEngine::outputMidiChannel() const noexcept
{
    return midiChannel_ >= 1 && midiChannel_ <= 16 ? midiChannel_ : 1;
}

uint8_t SwaraXtEngine::heldOutputChannel(uint8_t external) const noexcept
{
    if (external < 128 && heldMidiChannel_[external] >= 1 && heldMidiChannel_[external] <= 16)
        return heldMidiChannel_[external];
    return static_cast<uint8_t>(outputMidiChannel());
}

bool SwaraXtEngine::enqueueScheduledMidi(uint64_t sample, bool noteOn, uint8_t note, uint8_t velocity,
                                        uint8_t channel) noexcept
{
    if (note >= 128)
        return false;
    const uint8_t safeChannel = channel >= 1 && channel <= 16 ? channel : static_cast<uint8_t>(outputMidiChannel());
    if (scheduledMidiCount_ >= kMaxScheduledMidi)
    {
        // Note Off must not vanish: drop the oldest pending Note On to free a slot.
        if (noteOn)
            return false;
        int drop = -1;
        for (int index = 0; index < scheduledMidiCount_; ++index)
        {
            if (scheduledMidi_[static_cast<size_t>(index)].noteOn != 0)
            {
                drop = index;
                break;
            }
        }
        if (drop < 0)
            return false;
        for (int index = drop + 1; index < scheduledMidiCount_; ++index)
            scheduledMidi_[static_cast<size_t>(index - 1)] = scheduledMidi_[static_cast<size_t>(index)];
        --scheduledMidiCount_;
    }
    if (noteOn)
    {
        const uint8_t midiVelocity = velocity == 0 ? 1 : (velocity > 127 ? 127 : velocity);
        scheduledMidi_[static_cast<size_t>(scheduledMidiCount_++)] =
            { sample, note, midiVelocity, 1, safeChannel };
        return true;
    }
    scheduledMidi_[static_cast<size_t>(scheduledMidiCount_++)] = { sample, note, 0, 0, safeChannel };
    return true;
}

bool SwaraXtEngine::scheduleSequencerMidi(bool noteOn, uint8_t note, uint8_t velocity, uint8_t channel) noexcept
{
    const uint64_t sample = hostBlockOrigin_
        + static_cast<uint64_t>(juce::jmax(0, renderHostCursor_))
        + static_cast<uint64_t>(sequencerMidiLeadSamples());
    return enqueueScheduledMidi(sample, noteOn, note, velocity, channel);
}

void SwaraXtEngine::releaseHeldSequencerNotes(uint64_t sample) noexcept
{
    for (uint8_t note = 0; note < 128; ++note)
    {
        if (heldMidi_[note] == 0)
            continue;
        const uint8_t channel = heldOutputChannel(note);
        if (! enqueueScheduledMidi(sample, false, note, 0, channel))
        {
            // Cleanup has ended ownership even when release storage is full.
            externalOwners_[note] = 0;
            retrySequencerMidiRelease_ = true;
            continue;
        }
        heldMidi_[note] = 0;
        heldMidiChannel_[note] = 0;
        externalOwners_[note] = 0;
    }
    // Cleanup ends every identity's ownership, even when its external Off
    // must be retried. A late old identity must not release a later note.
    identityExternal_.fill(255);
}

void SwaraXtEngine::forceFlushSequencerMidiRelease(uint64_t sample) noexcept
{
    // Drop pending Note Ons and pull pending Note Offs into this block so a
    // host stop / mode cleanup cannot leave an audible hanging external note.
    int kept = 0;
    for (int index = 0; index < scheduledMidiCount_; ++index)
    {
        auto event = scheduledMidi_[static_cast<size_t>(index)];
        if (event.noteOn != 0)
            continue;
        if (event.sample > sample)
            event.sample = sample;
        scheduledMidi_[static_cast<size_t>(kept++)] = event;
    }
    scheduledMidiCount_ = kept;
    releaseHeldSequencerNotes(sample);
}

void SwaraXtEngine::flushSequencerMidi(juce::MidiBuffer& output, int numSamples) noexcept
{
    const uint64_t blockEnd = hostBlockOrigin_ + static_cast<uint64_t>(juce::jmax(0, numSamples));
    int kept = 0;
    for (int index = 0; index < scheduledMidiCount_; ++index)
    {
        const auto event = scheduledMidi_[static_cast<size_t>(index)];
        if (event.sample < blockEnd)
        {
            const int offset = event.sample <= hostBlockOrigin_
                ? 0
                : static_cast<int>(event.sample - hostBlockOrigin_);
            const int channel = event.channel >= 1 && event.channel <= 16 ? event.channel : outputMidiChannel();
            if (event.noteOn != 0)
                output.addEvent(juce::MidiMessage::noteOn(channel, event.note,
                                                          static_cast<juce::uint8>(event.velocity)),
                                offset);
            else
                output.addEvent(juce::MidiMessage::noteOff(channel, event.note), offset);
        }
        else
        {
            scheduledMidi_[static_cast<size_t>(kept++)] = event;
        }
    }
    scheduledMidiCount_ = kept;
    hostBlockOrigin_ = blockEnd;
}

void SwaraXtEngine::latchPublishedTuning() noexcept
{
    // SC active-entry precedes this observation. Until the latch catches up,
    // three slots and the retired predecessor prevent two successors from
    // recycling an observed index. The stored latch then protects the copy.
    int slot = publishedTuning_.load(std::memory_order_seq_cst);
#if defined(SWARAXT_ENABLE_TUNING_OWNERSHIP_TESTS)
    tuningPoint(TuningPoint::audioObserved, slot);
#endif
    if (slot < 0 || slot >= kTuningSlots || tuningSlots_[static_cast<size_t>(slot)] == nullptr)
        slot = 0;
    tuningSlotThisBlock_.store(slot, std::memory_order_seq_cst);
    if (tuningSlots_[static_cast<size_t>(slot)] == nullptr
        || ! tuningSlots_[static_cast<size_t>(slot)]->valid)
    {
        slot = 0;
        tuningSlotThisBlock_.store(slot, std::memory_order_seq_cst);
    }
    const int latest = publishedTuning_.load(std::memory_order_seq_cst);
    if (latest != slot && latest >= 0 && latest < kTuningSlots
        && tuningSlots_[static_cast<size_t>(latest)] != nullptr)
    {
        tuningSlotThisBlock_.store(latest, std::memory_order_seq_cst);
        if (tuningSlots_[static_cast<size_t>(latest)]->valid)
            slot = latest;
        else
            tuningSlotThisBlock_.store(slot, std::memory_order_seq_cst);
    }
    if (stableTuning_ != nullptr && tuningSlots_[static_cast<size_t>(slot)] != nullptr)
        *stableTuning_ = *tuningSlots_[static_cast<size_t>(slot)];
    installSequencerTuning(nullptr, nullptr);
}

bool SwaraXtEngine::snapshotClaimed(int slot) const noexcept
{
    for (const auto& claim : snapshotClaims_)
        if (claim.load(std::memory_order_seq_cst) == slot)
            return true;
    return false;
}

int SwaraXtEngine::freeTuningSlot(int published, int latched, int retired) const noexcept
{
    for (int slot = 0; slot < kTuningSlots; ++slot)
        if (slot != published && slot != latched && slot != retired && ! snapshotClaimed(slot))
            return slot;
    return -1;
}

bool SwaraXtEngine::pinPublished(const tuning::PublishedTuning*& tuning, int& claim) const noexcept
{
    tuning = nullptr;
    claim = -1;
    for (int attempt = 0; attempt < 4; ++attempt)
    {
        int slot = publishedTuning_.load(std::memory_order_seq_cst);
#if defined(SWARAXT_ENABLE_TUNING_OWNERSHIP_TESTS)
        tuningPoint(TuningPoint::queryObserved, slot);
#endif
        if (slot < 0 || slot >= kTuningSlots || tuningSlots_[static_cast<size_t>(slot)] == nullptr)
            slot = 0;
        if (tuningSlots_[static_cast<size_t>(slot)] == nullptr)
            return false;
        int index = -1;
        for (int n = 0; n < kSnapshotClaims; ++n)
        {
            int expected = -1;
            if (snapshotClaims_[static_cast<size_t>(n)].compare_exchange_strong(
                    expected, slot, std::memory_order_seq_cst, std::memory_order_relaxed))
            {
                index = n;
                break;
            }
        }
        if (index < 0)
            return false;
        // The claim is installed before any ordinary field read. A writer that
        // has not yet published this index fails the recheck, so an in-progress
        // assignment of a still-unpublished slot is not observed.
        const bool stillPublished = publishedTuning_.load(std::memory_order_seq_cst) == slot;
        const auto* candidate = tuningSlots_[static_cast<size_t>(slot)].get();
        if (! stillPublished || candidate == nullptr || ! candidate->valid)
        {
            snapshotClaims_[static_cast<size_t>(index)].store(-1, std::memory_order_seq_cst);
            continue;
        }
        tuning = candidate;
        claim = index;
#if defined(SWARAXT_ENABLE_TUNING_OWNERSHIP_TESTS)
        tuningPoint(TuningPoint::queryValidated, slot);
#endif
        return true;
    }
    return false;
}

void SwaraXtEngine::unpinPublished(int claim) const noexcept
{
    if (claim >= 0 && claim < kSnapshotClaims)
        snapshotClaims_[static_cast<size_t>(claim)].store(-1, std::memory_order_release);
}

void SwaraXtEngine::installSequencerTuning(const int16_t* degrees, const int32_t* quantizedUnits) noexcept
{
    const tuning::PublishedTuning* tuning = stableTuning_ != nullptr ? stableTuning_.get() : nullptr;
    part_.setSequencerScale(scaleRouteActive_, degrees, tuning);
    part_.setSequencerQuantizedCv(quantizedRouteActive_, quantizedUnits, tuning);
    part_.setMidiInputTuning(static_cast<uint8_t>(midiInputMode_), tuning);
}

std::vector<uint8_t> SwaraXtEngine::tuningBytesForState() const
{
    const std::lock_guard<std::mutex> lock(tuningBytesMutex_);
    return tuningBytes_;
}

bool SwaraXtEngine::loadTuningBytes(const uint8_t* data, size_t size)
{
    return publishTuningBytes(data, size, -1);
}

SwaraXtEngine::TuningReservation::~TuningReservation()
{
    if (owner_ != nullptr)
        owner_->tuningWriteBusy_.store(false, std::memory_order_release);
}

void SwaraXtEngine::TuningReservation::publish()
{
    if (owner_ == nullptr || slot_ < 0)
        return;
    // All decoding/allocation happened before musical state mutation. Never hold
    // this bytes mutex across APVTS callbacks or reentrant state serialization.
    const std::lock_guard<std::mutex> lock(owner_->tuningBytesMutex_);
    *owner_->tuningSlots_[static_cast<size_t>(slot_)] = decoded_;
    owner_->retiredTuning_.store(owner_->publishedTuning_.load(std::memory_order_seq_cst),
                                std::memory_order_seq_cst);
    owner_->publishedTuning_.store(slot_, std::memory_order_seq_cst);
    owner_->tuningBytes_.swap(bytes_);
    owner_->builtinTuningIndex_.store(builtin_, std::memory_order_relaxed);
    slot_ = -1; // Publication is single-use; writer admission lasts through destruction.
}

SwaraXtEngine::TuningResult SwaraXtEngine::reserveBuiltinTuning(int index, TuningReservation& reservation)
{
    if (index < 0 || index >= builtinTuningCount())
        return TuningResult::invalid;
    const auto& item = tuning::kBuiltinTunings[static_cast<size_t>(index)];
    return reserveTuning(item.data, item.size, reservation, index);
}

SwaraXtEngine::TuningResult SwaraXtEngine::reserveTuning(const uint8_t* data, size_t size,
                                                       TuningReservation& reservation, int builtin)
{
    jassert(reservation.owner_ == nullptr);
    if (reservation.owner_ != nullptr)
        return TuningResult::busy;
    tuning::TuningSnapshot snapshot;
    tuning::ParseError error;
    if (! tuning::readTuningTable(data, size, snapshot, error))
        return TuningResult::invalid;
    if (! reservation.decoded_.fillFrom(snapshot))
        return TuningResult::unsupported;
    reservation.bytes_.assign(data, data + size);
    if (tuningWriteBusy_.exchange(true, std::memory_order_acq_rel))
        return TuningResult::busy;
    reservation.owner_ = this;
    reservation.builtin_ = builtin;
    int slot = -1;
    for (int attempt = 0; attempt < 8 && slot < 0; ++attempt)
    {
        if (! tuningAudioActive_.load(std::memory_order_seq_cst))
        {
            // Fold only retirement. The persistent latch and SC callback entry
            // bridge the next callback's observation-to-latch interval.
            const int current = publishedTuning_.load(std::memory_order_seq_cst);
            retiredTuning_.store(current, std::memory_order_seq_cst);
        }
        const int published = publishedTuning_.load(std::memory_order_seq_cst);
        const int latched = tuningSlotThisBlock_.load(std::memory_order_seq_cst);
        const int retired = retiredTuning_.load(std::memory_order_seq_cst);
        const int candidate = freeTuningSlot(published, latched, retired);
        if (candidate < 0)
        {
            if (attempt + 1 < 8)
                std::this_thread::sleep_for(std::chrono::milliseconds(1));
            continue;
        }
        const int publishedNow = publishedTuning_.load(std::memory_order_seq_cst);
        const int latchedNow = tuningSlotThisBlock_.load(std::memory_order_seq_cst);
        const int retiredNow = retiredTuning_.load(std::memory_order_seq_cst);
        if (candidate == publishedNow || candidate == latchedNow || candidate == retiredNow
            || snapshotClaimed(candidate))
        {
            if (attempt + 1 < 8)
                std::this_thread::sleep_for(std::chrono::milliseconds(1));
            continue;
        }
        slot = candidate;
    }

    if (slot < 0 || slot >= kTuningSlots)
        return TuningResult::unavailable;

    // Recheck after the gap between selection and the write. The slot is still
    // unpublished, unlatched, unretired and unpinned, so no reader may use it.
    if (slot == publishedTuning_.load(std::memory_order_seq_cst)
        || slot == tuningSlotThisBlock_.load(std::memory_order_seq_cst)
        || slot == retiredTuning_.load(std::memory_order_seq_cst)
        || snapshotClaimed(slot))
        return TuningResult::unavailable;
#if defined(SWARAXT_ENABLE_TUNING_OWNERSHIP_TESTS)
    tuningPoint(TuningPoint::writerSelected, slot);
#endif
    reservation.slot_ = slot;
    return TuningResult::success;
}

bool SwaraXtEngine::publishTuningBytes(const uint8_t* data, size_t size, int builtinIndex, bool initializing)
{
    if (! initializing)
    {
        TuningReservation reservation;
        if (reserveTuning(data, size, reservation, builtinIndex) != TuningResult::success)
            return false;
        reservation.publish();
        return true;
    }
    // Construction only, before any reader can exist.
    tuning::TuningSnapshot snapshot;
    tuning::ParseError error;
    tuning::PublishedTuning decoded;
    if (! tuning::readTuningTable(data, size, snapshot, error) || ! decoded.fillFrom(snapshot))
        return false;
    std::vector<uint8_t> bytes(data, data + size);
    *tuningSlots_[0] = decoded;
    *stableTuning_ = decoded;
    publishedTuning_.store(0, std::memory_order_seq_cst);
    retiredTuning_.store(0, std::memory_order_seq_cst);
    tuningSlotThisBlock_.store(0, std::memory_order_seq_cst);
    tuningBytes_.swap(bytes);
    builtinTuningIndex_.store(builtinIndex, std::memory_order_relaxed);
    return true;
}

bool SwaraXtEngine::loadBuiltinTuning(int index)
{
    if (index < 0 || index >= builtinTuningCount())
        return false;
    const auto& item = tuning::kBuiltinTunings[static_cast<size_t>(index)];
    return publishTuningBytes(item.data, item.size, index);
}

int SwaraXtEngine::builtinTuningCount() const noexcept
{
    return static_cast<int>(std::size(tuning::kBuiltinTunings));
}

const char* SwaraXtEngine::builtinTuningName(int index) const noexcept
{
    if (index < 0 || index >= builtinTuningCount())
        return "12-EDO";
    return tuning::kBuiltinTunings[static_cast<size_t>(index)].name;
}

juce::String SwaraXtEngine::activeTuningName() const
{
    const tuning::PublishedTuning* tuning = nullptr;
    int claim = -1;
    char name[tuning::kNameBytes] {};
    if (pinPublished(tuning, claim) && tuning != nullptr && tuning->valid && tuning->name[0] != '\0')
        std::memcpy(name, tuning->name, tuning::kNameBytes);
    unpinPublished(claim);
    if (name[0] == '\0')
        return "12-EDO";
    name[tuning::kNameBytes - 1] = '\0';
    return juce::String(name);
}

void SwaraXtEngine::copyActiveTuningDegrees(int16_t* midiDegree, int count, bool* midiMapped) const noexcept
{
    if (midiDegree == nullptr || count <= 0)
        return;
    const tuning::PublishedTuning* tuning = nullptr;
    int claim = -1;
    const bool pinned = pinPublished(tuning, claim) && tuning != nullptr && tuning->valid;
    const int limit = juce::jmin(count, tuning::kMidiNotes);
    for (int note = 0; note < limit; ++note)
    {
        midiDegree[note] = pinned ? tuning->midiDegree[note] : static_cast<int16_t>(note - 60);
        if (midiMapped != nullptr)
            midiMapped[note] = pinned ? tuning->midiMapped[note] : note >= 60;
    }
    unpinPublished(claim);
    for (int note = limit; note < count; ++note)
    {
        midiDegree[note] = -1;
        if (midiMapped != nullptr)
            midiMapped[note] = false;
    }
}

double SwaraXtEngine::hzForActiveDegree(int degree) const noexcept
{
    const tuning::PublishedTuning* tuning = nullptr;
    int claim = -1;
    const double hz = pinPublished(tuning, claim) && tuning != nullptr ? tuning->hzForDegree(degree) : 0.0;
    unpinPublished(claim);
    return hz;
}

tuning::PublishedTuning::QuantizedPitch SwaraXtEngine::quantizeActiveHz(double sourceHz) const noexcept
{
    const tuning::PublishedTuning* tuning = nullptr;
    int claim = -1;
    const auto chosen = pinPublished(tuning, claim) && tuning != nullptr
        ? tuning->quantizeHz(sourceHz) : tuning::PublishedTuning::QuantizedPitch {};
    unpinPublished(claim);
    return chosen;
}

SequenceState::ScaleRandomView SwaraXtEngine::scaleRandomView() const noexcept
{
    static_assert(SequenceState::kMaxScaleDegreeCents == tuning::PublishedTuning::kMaxInterior);
    SequenceState::ScaleRandomView view;
    const tuning::PublishedTuning* tuning = nullptr;
    int claim = -1;
    if (! pinPublished(tuning, claim) || tuning == nullptr || ! tuning->valid)
    {
        unpinPublished(claim);
        view.formalPeriod = 12;
        view.periodCents = 1200.0;
        view.degreeCount = 0;
        return view;
    }
    view.formalPeriod = tuning->formalPeriod;
    view.periodCents = tuning->periodCents;
    const int count = juce::jlimit(0, SequenceState::kMaxScaleDegreeCents, static_cast<int>(tuning->degreeCount));
    view.degreeCount = count;
    for (int index = 0; index < count; ++index)
        view.degreeCents[static_cast<size_t>(index)] = tuning->degreeCents[index];
    unpinPublished(claim);
    return view;
}

void SwaraXtEngine::clearSequencerMidiOutput() noexcept
{
    // releaseResources has no output buffer. Keep releases for the next render
    // instead of forgetting notes already delivered to the host.
    forceFlushSequencerMidiRelease(hostBlockOrigin_);
    identityExternal_.fill(255);
    externalOwners_.fill(0);
    releaseSequencerMidi_ = false;
    midiOutput_ = nullptr;
}

}  // namespace swaraxt
