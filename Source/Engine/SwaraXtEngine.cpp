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
    loadBuiltinTuning(0);
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
    applyExperimentalSvfSelection();
    qualityRampSamples_ = juce::jmax(1, static_cast<int>(std::lround(0.002 * kInternalSampleRate)));
    masterRampSamples_ = juce::jmax(1, static_cast<int>(std::lround(0.004 * kInternalSampleRate)));
    postMixerRampSamples_ = masterRampSamples_;
    snapFilterQuality();
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
    applyExperimentalSvfSelection();
    snapFilterQuality();
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
    if (parameterCache_ == nullptr || ! parameterCache_->bound)
        return;

    patchBridge_.applyCacheToEngine(*parameterCache_);
    if (sequenceState_ != nullptr)
    {
        SequenceSnapshot sequence;
        uint32_t revision = 0;
        if (sequenceState_->capture(sequence, revision) && revision != appliedSequenceRevision_)
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
        sequenceState_ != nullptr ? sequenceState_->snapshot().pitchMode : 0);
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
    const auto quality = static_cast<FilterQuality>(
        requestedFilterQuality_.load(std::memory_order_relaxed));
    filter_.setQuality(quality);
    appliedFilterQuality_ = quality;
    pendingFilterQuality_ = quality;
    qualityGain_ = 1.0f;
    qualityGainIncrement_ = 0.0f;
    qualitySamplesRemaining_ = 0;
    qualityFadePhase_ = QualityFadePhase::stable;
}

void SwaraXtEngine::startQualityFadeOut() noexcept
{
    qualityFadePhase_ = QualityFadePhase::fadeOut;
    qualitySamplesRemaining_ = juce::jmax(1, qualityRampSamples_);
    qualityGainIncrement_ = (0.0f - qualityGain_)
        / static_cast<float>(qualitySamplesRemaining_);
}

void SwaraXtEngine::applyPendingFilterQuality() noexcept
{
    pendingFilterQuality_ = static_cast<FilterQuality>(
        requestedFilterQuality_.load(std::memory_order_relaxed));
    if (pendingFilterQuality_ == appliedFilterQuality_
        && qualityFadePhase_ == QualityFadePhase::stable)
        return;

    if (pendingFilterQuality_ != appliedFilterQuality_
        && qualityFadePhase_ != QualityFadePhase::fadeOut)
        startQualityFadeOut();
}

void SwaraXtEngine::applyExperimentalSvfSelection() noexcept
{
    appliedExperimentalSvfSelection_ = experimentalSvfControl_.applyOnAudioThread(
        experimentalSvf_, experimentalSvfEnabled_);
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
    if (experimentalSvfEnabled_)
        return experimentalSvf_.process(mixer, vcaTarget);
    return filter_.processInstrumentSample(mixer, vcaTarget);
}

float SwaraXtEngine::selectedFilterVca() const noexcept
{
    if (experimentalSvfEnabled_)
        return experimentalSvf_.vcaControl();
    return filter_.instrumentVcaControl();
}

bool SwaraXtEngine::selectedFilterTailActive() const noexcept
{
    if (experimentalSvfEnabled_)
        return experimentalSvf_.tailActive();
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
    applyPendingFilterQuality();
    applyExperimentalSvfSelection();
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
            const float out = filtered * nextMasterGain() * nextQualityGain();
#if SWARAXT_ENABLE_SHRUTHI_DEBUG_TAPS
            capture.postShruthiMixer[i] = temp[i];
            capture.filterOutput[i] = filtered;
            capture.postVca[i] = out;
            capture.vcaTarget[i] = vcaTarget;
            capture.vcaGain[i] = vcaGain;
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
#if SWARAXT_ENABLE_SHRUTHI_DEBUG_TAPS
            capture.vcaGain[i] = vcaGain;
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
        && !selectedFilterTailActive()
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
    if (! voiceRequiresAudio())
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
    applyExperimentalSvfSelection();
    snapFilterQuality();
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
    applyExperimentalSvfSelection();
    snapFilterQuality();
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
        applyExperimentalSvfSelection();
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
                           juce::MidiBuffer* midiOut)
{
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
            flag.store(true, std::memory_order_release);
        }
        ~TuningReadScope() { flag.store(false, std::memory_order_release); }
    } tuningRead(tuningAudioActive_);

    latchPublishedTuning();
    midiOutput_ = midiOut;
    if (releaseSequencerMidi_)
    {
        forceFlushSequencerMidiRelease(hostBlockOrigin_);
        releaseSequencerMidi_ = false;
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
    identityExternal_[identity] = external;
    const bool retrigger = externalOwners_[external] > 0 || heldMidi_[external] != 0;
    if (externalOwners_[external] < 255)
        ++externalOwners_[external];
    if (retrigger)
        scheduleSequencerMidi(false, external, 0, heldOutputChannel(external));
    const uint8_t midiVelocity = velocity == 0 ? 1 : (velocity > 127 ? 127 : velocity);
    const uint8_t channel = static_cast<uint8_t>(outputMidiChannel());
    if (scheduleSequencerMidi(true, external, midiVelocity, channel))
    {
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
            continue;
        heldMidi_[note] = 0;
        heldMidiChannel_[note] = 0;
        externalOwners_[note] = 0;
    }
    for (uint8_t identity = 0; identity < 128; ++identity)
    {
        const uint8_t external = identityExternal_[identity];
        if (external < 128 && heldMidi_[external] == 0)
            identityExternal_[identity] = 255;
    }
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

const tuning::PublishedTuning* SwaraXtEngine::publishedTuningSlot() const noexcept
{
    const int slot = publishedTuning_.load(std::memory_order_acquire);
    if (slot < 0 || slot >= kTuningSlots || tuningSlots_[static_cast<size_t>(slot)] == nullptr
        || ! tuningSlots_[static_cast<size_t>(slot)]->valid)
        return tuningSlots_[0].get();
    return tuningSlots_[static_cast<size_t>(slot)].get();
}

void SwaraXtEngine::latchPublishedTuning() noexcept
{
    // Claim the published index before copying it. The writer rechecks this
    // latch immediately before mutating a slot, so the copy below cannot race
    // with publication. The claim stays in place until the next callback.
    int slot = publishedTuning_.load(std::memory_order_seq_cst);
    if (slot < 0 || slot >= kTuningSlots || tuningSlots_[static_cast<size_t>(slot)] == nullptr
        || ! tuningSlots_[static_cast<size_t>(slot)]->valid)
        slot = 0;
    tuningSlotThisBlock_.store(slot, std::memory_order_seq_cst);
    const int latest = publishedTuning_.load(std::memory_order_seq_cst);
    if (latest != slot && latest >= 0 && latest < kTuningSlots
        && tuningSlots_[static_cast<size_t>(latest)] != nullptr
        && tuningSlots_[static_cast<size_t>(latest)]->valid)
    {
        tuningSlotThisBlock_.store(latest, std::memory_order_seq_cst);
        slot = latest;
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
        if (slot < 0 || slot >= kTuningSlots || tuningSlots_[static_cast<size_t>(slot)] == nullptr
            || ! tuningSlots_[static_cast<size_t>(slot)]->valid)
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
        // The slot was published before the claim. If it still is, the writer
        // cannot have selected it: a writer either observed this claim or
        // observed the slot as published and chose a different one.
        if (publishedTuning_.load(std::memory_order_seq_cst) != slot)
        {
            snapshotClaims_[static_cast<size_t>(index)].store(-1, std::memory_order_seq_cst);
            continue;
        }
        tuning = tuningSlots_[static_cast<size_t>(slot)].get();
        claim = index;
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

bool SwaraXtEngine::publishTuningBytes(const uint8_t* data, size_t size, int builtinIndex)
{
    if (data == nullptr || size == 0)
        return false;
    tuning::TuningSnapshot snapshot;
    tuning::ParseError error;
    if (! tuning::readTuningTable(data, size, snapshot, error))
        return false;
    tuning::PublishedTuning decoded;
    if (! decoded.fillFrom(snapshot))
        return false;

    // Single-writer gate: GUI / preset / import share the message thread, but
    // nested callbacks must not select the same free slot concurrently.
    if (tuningWriteBusy_.exchange(true, std::memory_order_acq_rel))
        return false;

    if (tuningSlots_[0] == nullptr || ! tuningSlots_[0]->valid)
    {
        *tuningSlots_[0] = decoded;
        publishedTuning_.store(0, std::memory_order_seq_cst);
        retiredTuning_.store(0, std::memory_order_seq_cst);
        tuningSlotThisBlock_.store(0, std::memory_order_seq_cst);
        if (stableTuning_ != nullptr)
            *stableTuning_ = *tuningSlots_[0];
        {
            const std::lock_guard<std::mutex> lock(tuningBytesMutex_);
            tuningBytes_.assign(data, data + size);
        }
        builtinTuningIndex_.store(builtinIndex, std::memory_order_relaxed);
        tuningWriteBusy_.store(false, std::memory_order_release);
        return true;
    }

    int slot = -1;
    int published = 0;
    for (int attempt = 0; attempt < 8 && slot < 0; ++attempt)
    {
        if (! tuningAudioActive_.load(std::memory_order_seq_cst))
        {
            // The callback is not inside its read scope. Keep the latched slot,
            // which the sequencer may still reference, and only fold the retired
            // index so an idle engine still has a free slot.
            const int current = publishedTuning_.load(std::memory_order_seq_cst);
            retiredTuning_.store(current, std::memory_order_seq_cst);
        }
        published = publishedTuning_.load(std::memory_order_seq_cst);
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
        published = publishedNow;
    }

    if (slot < 0 || slot >= kTuningSlots)
    {
        tuningWriteBusy_.store(false, std::memory_order_release);
        return false;
    }

    // Recheck after the gap between selection and the write. The slot is still
    // unpublished, unlatched, unretired and unpinned, so no reader may use it.
    if (slot == publishedTuning_.load(std::memory_order_seq_cst)
        || slot == tuningSlotThisBlock_.load(std::memory_order_seq_cst)
        || slot == retiredTuning_.load(std::memory_order_seq_cst)
        || snapshotClaimed(slot))
    {
        tuningWriteBusy_.store(false, std::memory_order_release);
        return false;
    }
    *tuningSlots_[static_cast<size_t>(slot)] = decoded;
    retiredTuning_.store(published, std::memory_order_seq_cst);
    publishedTuning_.store(slot, std::memory_order_seq_cst);
    {
        const std::lock_guard<std::mutex> lock(tuningBytesMutex_);
        tuningBytes_.assign(data, data + size);
    }
    builtinTuningIndex_.store(builtinIndex, std::memory_order_relaxed);
    tuningWriteBusy_.store(false, std::memory_order_release);
    return true;
}

bool SwaraXtEngine::loadBuiltinTuning(int index)
{
    struct Item { const char* name; const uint8_t* data; size_t size; };
    const Item items[] = {
        { "12-EDO", tuning::kBuiltin12Edo, tuning::kBuiltin12EdoSize },
        { "19-EDO", tuning::kBuiltin19Edo, tuning::kBuiltin19EdoSize },
        { "24-EDO", tuning::kBuiltin24Edo, tuning::kBuiltin24EdoSize },
        { "31-EDO", tuning::kBuiltin31Edo, tuning::kBuiltin31EdoSize },
        { "5-limit major", tuning::kBuiltinJustMajor, tuning::kBuiltinJustMajorSize },
        { "tritave", tuning::kBuiltinTritave, tuning::kBuiltinTritaveSize }
    };
    if (index < 0 || index >= static_cast<int>(std::size(items)))
        return false;
    return publishTuningBytes(items[static_cast<size_t>(index)].data,
                              items[static_cast<size_t>(index)].size, index);
}

int SwaraXtEngine::builtinTuningCount() const noexcept
{
    return 6;
}

const char* SwaraXtEngine::builtinTuningName(int index) const noexcept
{
    static const char* names[] = { "12-EDO", "19-EDO", "24-EDO", "31-EDO", "5-limit major", "tritave" };
    if (index < 0 || index >= 6)
        return "12-EDO";
    return names[index];
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

void SwaraXtEngine::copyActiveTuningDegrees(int16_t* midiDegree, int count) const noexcept
{
    if (midiDegree == nullptr || count <= 0)
        return;
    const tuning::PublishedTuning* tuning = nullptr;
    int claim = -1;
    const bool pinned = pinPublished(tuning, claim) && tuning != nullptr && tuning->valid;
    const int limit = juce::jmin(count, tuning::kMidiNotes);
    for (int note = 0; note < limit; ++note)
        midiDegree[note] = pinned ? tuning->midiDegree[note] : static_cast<int16_t>(note - 60);
    unpinPublished(claim);
    for (int note = limit; note < count; ++note)
        midiDegree[note] = -1;
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
    heldMidi_.fill(0);
    heldMidiChannel_.fill(0);
    identityExternal_.fill(255);
    externalOwners_.fill(0);
    scheduledMidiCount_ = 0;
    releaseSequencerMidi_ = false;
    midiOutput_ = nullptr;
}

}  // namespace swaraxt
