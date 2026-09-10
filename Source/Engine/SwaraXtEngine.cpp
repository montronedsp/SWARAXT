// Copyright 2026 MontroneDSP.
// SPDX-License-Identifier: GPL-3.0-or-later

#include "Engine/SwaraXtEngine.h"

#include <JuceHeader.h>
#include <algorithm>
#if SWARAXT_ENABLE_IDLE_CPU_TESTS
#include <chrono>
#endif
#include <cmath>
#include <limits>

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

// The reference pole reaches 1e-7 from unity in about 3.65 seconds. That is
// below -140 dBFS, while a 25-second backstop also covers decay from the
// largest finite float at every supported host rate.
constexpr float kDcSettlementThreshold = 1.0e-7f;
constexpr double kMaxDcDrainSeconds = 25.0;

}

SwaraXtEngine::SwaraXtEngine()
{
    patchBridge_.bind(part_);
    constexpr double twoPi = 6.28318530717958647692;
    vcaCvCoeff_ = static_cast<float>(1.0 - std::exp(-twoPi * kSmr4VcaCvCutoffHz / kInternalSampleRate));
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
    qualityRampSamples_ = juce::jmax(1, static_cast<int>(std::lround(0.002 * kInternalSampleRate)));
    masterRampSamples_ = juce::jmax(1, static_cast<int>(std::lround(0.004 * kInternalSampleRate)));
    postMixerRampSamples_ = masterRampSamples_;
    conditioningRampSamples_ = juce::jmax(1, static_cast<int>(std::lround(0.003 * kInternalSampleRate)));
    snapFilterQuality();
    snapMasterOnApply_ = true;
    prepared_ = true;
}

void SwaraXtEngine::reset()
{
    random_.Seed(0x21);
    part_.Init(audioRing_, random_, midiDispatcher_, storage_);
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
    snapFilterQuality();
    dcBlocker_.reset();
    snapMasterOnApply_ = true;
    resetVcaReconstruction();
    snapPostMixerGain(1.0f);
    conditioningMix_ = 0.0f;
    conditioningMixTarget_ = 0.0f;
    conditioningMixIncrement_ = 0.0f;
    conditioningMixRemaining_ = 0;
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
    conditioningMix_ = conditioningMixTarget_;
    conditioningMixIncrement_ = 0.0f;
    conditioningMixRemaining_ = 0;
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
    const float conditioningTarget =
        ParameterCache::loadInt(parameterCache_->inputConditioning) != 0 ? 1.0f : 0.0f;
    if (conditioningTarget != conditioningMixTarget_)
    {
        conditioningMixTarget_ = conditioningTarget;
        if (dormant_)
        {
            conditioningMix_ = conditioningMixTarget_;
            conditioningMixIncrement_ = 0.0f;
            conditioningMixRemaining_ = 0;
        }
        else
        {
            conditioningMixRemaining_ = juce::jmax(1, conditioningRampSamples_);
            conditioningMixIncrement_ = (conditioningMixTarget_ - conditioningMix_)
                / static_cast<float>(conditioningMixRemaining_);
        }
    }
    filterCutoffHz_ = ParameterCache::load(parameterCache_->filterCutoff);
    filterResonance_ = ParameterCache::load(parameterCache_->filterResonance);
    filterEnvAmount_ = ParameterCache::load(parameterCache_->filterEnvAmount);
    filterKeyTrack_ = ParameterCache::load(parameterCache_->filterKeyTracking);
    filterModAmount_ = ParameterCache::load(parameterCache_->filterModAmount);
    auto* patch = part_.mutable_patch();
    patch->filter_cutoff = shruthiCutoffCode(filterCutoffHz_);
    patch->filter_resonance = static_cast<uint8_t>(juce::jlimit(0, 63,
        static_cast<int>(std::lround(filterResonance_ * 63.0f))));
    midiChannel_ = juce::jlimit(0, 16, ParameterCache::loadInt(parameterCache_->midiChannel));
    lfoSync_[0] = ParameterCache::loadInt(parameterCache_->lfo1Sync) != 0;
    lfoSync_[1] = ParameterCache::loadInt(parameterCache_->lfo2Sync) != 0;
    lfoDivision_[0] = juce::jlimit(0, 7, ParameterCache::loadInt(parameterCache_->lfo1Division));
    lfoDivision_[1] = juce::jlimit(0, 7, ParameterCache::loadInt(parameterCache_->lfo2Division));
    sequencerHostSync_ = ParameterCache::loadInt(parameterCache_->seqClockMode) != 0;
    sequencerSwing_ = juce::jlimit(0, 127, ParameterCache::loadInt(parameterCache_->seqSwing));

    const auto nextBoard = parameterCache_->boardControls();
    if (!requestedBoard_.sameTopology(nextBoard) || requestedBoard_.cv1 != nextBoard.cv1
        || requestedBoard_.cv2 != nextBoard.cv2)
        boardWake_ = nextBoard.model == board::Model::dspBoard || nextBoard.effect != board::Effect::off
            || !activeBoard_.sameTopology(nextBoard);
    requestedBoard_ = nextBoard;
    if (snapBoardOnApply_)
    {
        activeBoard_ = requestedBoard_;
        boardProcessor_.reset(activeBoard_.effect);
        boardFadePhase_ = BoardFadePhase::stable;
        boardGain_ = 1.0f;
        boardFadeRemaining_ = 0;
        snapBoardOnApply_ = false;
    }
    boardTailSeconds_.store(board::BoardProcessor::tailSeconds(activeBoard_), std::memory_order_relaxed);
}

void SwaraXtEngine::resetBoardState() noexcept
{
    dspBoardDelay_.fill(0);
    dspBoardDelayHead_ = 0;
    dspBoardDelayRemaining_ = 0;
    dspBoardPreviousInput_ = 0;
    boardProcessor_.reset();
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
        const bool changedModel = activeBoard_.model != requestedBoard_.model;
        activeBoard_ = requestedBoard_;
        boardProcessor_.reset(activeBoard_.effect);
        dspBoardDelay_.fill(0);
        dspBoardDelayHead_ = 0;
        dspBoardDelayRemaining_ = 0;
        dspBoardPreviousInput_ = 0;
        if (changedModel && activeBoard_.model == board::Model::classic) filter_.reset();
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
        || dspBoardDelayRemaining_ > 0 || boardProcessor_.needsAudio(activeBoard_);
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
    blockStartPpq_ = transport.hasPpqPosition ? transport.ppqPosition : fallbackPpq_;

    const double tolerance = blockPpqPerSample_ * 2.0;
    const bool discontinuity = transport.hasPpqPosition
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
            part_.Stop(false);
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

float SwaraXtEngine::nextConditioningMix() noexcept
{
    if (conditioningMixRemaining_ > 0)
    {
        conditioningMix_ += conditioningMixIncrement_;
        --conditioningMixRemaining_;
        if (conditioningMixRemaining_ == 0)
            conditioningMix_ = conditioningMixTarget_;
    }
    return conditioningMix_;
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

float SwaraXtEngine::nextVcaCv(float target) noexcept
{
    // Analog-style one-pole on the VCA *control*, not a block-wide audio ramp.
    // Target is held for the native Shruthi control block; the state responds
    // sample-by-sample at the native audio rate.
    vcaCvState_ += vcaCvCoeff_ * (target - vcaCvState_);
    if (! std::isfinite(vcaCvState_))
        vcaCvState_ = 0.0f;
    return vcaCvState_;
}

void SwaraXtEngine::updateFilterFromShruthi()
{
    const auto& voice = part_.voice();
    const float env1 = static_cast<float>(voice.modulation_source(shruthi::MOD_SRC_ENV_1)) / 255.0f;
    const float lfo2 = static_cast<float>(
        static_cast<int>(voice.modulation_source(shruthi::MOD_SRC_LFO_2)) - 128) / 128.0f;
    // The final firmware CV includes matrix, tracking, envelope, LFO and all
    // original integer clipping. Decode it once; plugin controls only augment it.
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
    p.envAmount = filterEnvAmount_ * 4.0f; // up to ~4 octaves of env depth
    p.modAmount = filterModAmount_ * 2.0f;
    p.envValue = juce::jlimit(0.0f, 1.0f, env1);
    p.modValue = juce::jlimit(-1.0f, 1.0f, lfo2);
    p.matrixCutoffOctaves = trackingTrim;
    p.noteNumber = controlNote;
    p.drive = 1.0f;
    const double extraOctaves = static_cast<double>(p.envAmount) * p.envValue
        + static_cast<double>(p.modAmount) * p.modValue + p.matrixCutoffOctaves;
    // Transport every native byte directly to the board CV domain. The RAW
    // panel-Hz clamp is not authority for hardware cutoff reconstruction.
    p.boardCutoffCvVolts = std::clamp((static_cast<double>(voice.cutoff())
        + 24.0 * extraOctaves) * (5.0 / 255.0), 0.0, 5.0);
    filter_.setParams(p);
    if (activeBoard_.model == board::Model::dspBoard || activeBoard_.effect != board::Effect::off)
    {
        const double modOctaves = static_cast<double>(p.envAmount) * p.envValue
            + static_cast<double>(p.modAmount) * p.modValue + p.matrixCutoffOctaves;
        const auto hz = CutoffMapper{}.mapCutoffHz(p.cutoffHz, p.keyTrack, p.noteNumber, modOctaves);
        activeBoard_.cutoff = board::BoardControl::cutoffCode(hz);
        activeBoard_.resonance = board::BoardControl::nativeCv(static_cast<int>(std::lround(p.resonance * 254)));
        activeBoard_.cv1 = board::BoardControl::nativeCv(voice.cv_1());
        activeBoard_.cv2 = board::BoardControl::nativeCv(voice.cv_2());
        activeBoard_.dca = board::BoardControl::nativeCv(voice.vca());
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

void SwaraXtEngine::collectMidiEvents(const juce::MidiBuffer& midi,
                                     int startSample,
                                     int numSamples)
{
    pendingMidiCount_ = 0;
    const int endSample = startSample + numSamples;

    for (const auto metadata : midi)
    {
        if (metadata.samplePosition < startSample || metadata.samplePosition >= endSample)
            continue;
        if (pendingMidiCount_ >= kMaxPendingMidi)
            break;

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
    const bool unmodifiedClassic = activeBoard_.model == board::Model::classic
        && activeBoard_.effect == board::Effect::off && boardFadePhase_ == BoardFadePhase::stable;
    if (unmodifiedClassic)
    {
        for (int i = 0; i < read; ++i)
        {
            float mixer = temp[i] * nextPostMixerGain();
            if (! std::isfinite(mixer))
                mixer = 0.0f;
            const float mix = nextConditioningMix();
            float filtered = filter_.processInstrumentSample(mixer, vcaTarget, mix);
            if (! std::isfinite(filtered))
                filtered = 0.0f;
            const float vcaGain = filter_.instrumentVcaControl();
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
        float hardwareMix[kAudioBlockSize] {};
        for (int i = 0; i < read; ++i)
        {
            const auto index = static_cast<std::size_t>(i);
            float mixer = temp[i] * nextPostMixerGain();
            if (! std::isfinite(mixer))
                mixer = 0.0f;
            const float mix = nextConditioningMix();
            hardwareMix[index] = mix;
            if (activeBoard_.model == board::Model::classic)
            {
                const auto filtered = filter_.processInstrumentSample(mixer, vcaTarget, mix);
                const float vcaGain = filter_.instrumentVcaControl();
                vcaCvState_ = vcaGain;
                processed[index] = std::isfinite(filtered) ? filtered : 0.0f;
#if SWARAXT_ENABLE_SHRUTHI_DEBUG_TAPS
                capture.vcaGain[i] = vcaGain;
#endif
            }
            else
            {
                processed[index] = mixer;
                nextVcaCv(vcaTarget);
#if SWARAXT_ENABLE_SHRUTHI_DEBUG_TAPS
                capture.vcaGain[i] = vcaTarget;
#endif
            }
#if SWARAXT_ENABLE_SHRUTHI_DEBUG_TAPS
            capture.postShruthiMixer[i] = temp[i];
            capture.vcaTarget[i] = vcaTarget;
#endif
        }
        if (activeBoard_.model == board::Model::dspBoard)
            boardProcessor_.processBoard(processed, activeBoard_, hardwareMix);
        else boardProcessor_.processClassicFx(processed, activeBoard_);
        for (int i = 0; i < read; ++i)
        {
            const auto index = static_cast<std::size_t>(i);
            const float masterGain = nextMasterGain();
            const float qualityGain = nextQualityGain();
            if (activeBoard_.model == board::Model::dspBoard) {
                const float delayed = dspBoardDelay_[dspBoardDelayHead_];
                const float value = processed[index];
                dspBoardDelay_[dspBoardDelayHead_] = value;
                dspBoardDelayHead_ = (dspBoardDelayHead_ + 1) % dspBoardDelay_.size();
                // Like BoardProcessor's tail observer, drain changes rather
                // than a constant quantization bias. The host DC drain owns
                // that bias once both the board and delay have settled.
                dspBoardDelayRemaining_ = std::abs(value - dspBoardPreviousInput_) > 1.e-8f ? FilterRateConverter::kLatency
                    : std::max(0, dspBoardDelayRemaining_ - 1);
                dspBoardPreviousInput_ = value;
                processed[index] = delayed;
            }
            const float out = processed[index] * masterGain
                * (activeBoard_.model == board::Model::classic ? qualityGain : 1.0f) * nextBoardGain();
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
        && (activeBoard_.model != board::Model::classic || !filter_.instrumentTailActive())
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
    snapFilterQuality();
    dcBlocker_.reset();
    resetVcaReconstruction();
    snapMasterOnApply_ = true;
    snapPostMixerGain(postMixerGainTarget_);
    conditioningMix_ = conditioningMixTarget_;
    conditioningMixIncrement_ = 0.0f;
    conditioningMixRemaining_ = 0;
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
    snapFilterQuality();
    dcBlocker_.reset();
    resetVcaReconstruction();
    snapMasterOnApply_ = true;
    snapPostMixerGain(postMixerGainTarget_);
    conditioningMix_ = conditioningMixTarget_;
    conditioningMixIncrement_ = 0.0f;
    conditioningMixRemaining_ = 0;
}

void SwaraXtEngine::advanceDormantControl()
{
    dormantNativeSamples_ += kInternalSampleRate / hostSampleRate_;
    while (dormantNativeSamples_ >= static_cast<double>(kAudioBlockSize))
    {
        dormantNativeSamples_ -= static_cast<double>(kAudioBlockSize);
        part_.ProcessControlBlock();
        // The physical board reconstructs CV even when its VCA is closed.
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
                           const HostTransportSnapshot& transport)
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

    collectMidiEvents(midi, startSample, numSamples);
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

    auto* left = buffer.getWritePointer(0, startSample);
    float* right = buffer.getNumChannels() > 1 ? buffer.getWritePointer(1, startSample) : nullptr;

    int guard = 0;
    while (midiIndex < pendingMidiCount_
           && pendingMidi_[static_cast<size_t>(midiIndex)].sampleOffset <= startSample)
    {
        dispatchMidi(pendingMidi_[static_cast<size_t>(midiIndex)]);
        ++midiIndex;
    }
    updateDormantWakeState();

    const auto queueTarget = [&]() {
        return internalQueue_.queueTargetSize();
    };

    while (! dormant_ && ! drainingToDormant_
           && internalQueue_.size() < queueTarget() && guard++ < 16)
        renderInternalBlock();

    for (int i = 0; i < numSamples; ++i)
    {
        const int absoluteSample = startSample + i;
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
        while (midiIndex < pendingMidiCount_
               && pendingMidi_[static_cast<size_t>(midiIndex)].sampleOffset <= absoluteSample)
        {
            dispatchMidi(pendingMidi_[static_cast<size_t>(midiIndex)]);
            ++midiIndex;
        }
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
               && internalQueue_.size() < queueTarget() && guard++ < 16)
            renderInternalBlock();

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

}  // namespace swaraxt
