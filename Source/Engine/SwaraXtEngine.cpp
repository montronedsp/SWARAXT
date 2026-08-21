// Copyright 2026 MontroneDSP.
// SPDX-License-Identifier: GPL-3.0-or-later

#include "Engine/SwaraXtEngine.h"

#include <JuceHeader.h>
#include <algorithm>
#if SWARAXT_ENABLE_IDLE_CPU_TESTS
#include <chrono>
#endif
#include <cmath>

namespace swaraxt {

namespace {

constexpr double kLfoBeatsPerCycle[] { 0.125, 0.25, 0.5, 1.0, 2.0, 4.0, 8.0, 16.0 };

}

SwaraXtEngine::SwaraXtEngine()
{
    patchBridge_.bind(part_);
}

void SwaraXtEngine::prepare(double hostSampleRate, int maxBlockSize)
{
    hostSampleRate_ = hostSampleRate > 0.0 ? hostSampleRate : 44100.0;
    juce::ignoreUnused(maxBlockSize);
    internalQueue_.setStep(kInternalSampleRate, hostSampleRate_);
    dcBlocker_.prepare(hostSampleRate_);
    pendingMidiCount_ = 0;
    resetHostState();

    // Filter always runs at the Shruthi internal rate (pre-SRC).
    filter_.prepare(kInternalSampleRate);
    applyPendingFilterQuality();
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
    internalQueue_.reset();
    internalQueue_.setStep(kInternalSampleRate, hostSampleRate_);
    filter_.reset();
    dcBlocker_.reset();
    previousVcaGain_ = 0.0f;
    dormant_ = true;
    drainingToDormant_ = false;
    dormantNativeSamples_ = 0.0;
    pendingMidiCount_ = 0;
    resetHostState();
#if SWARAXT_ENABLE_SHRUTHI_DEBUG_TAPS
    debugInternalBlockCount_ = 0;
#endif
}

void SwaraXtEngine::resetResampler() noexcept
{
    audioRing_.Init();
    internalQueue_.reset();
    internalQueue_.setStep(kInternalSampleRate, hostSampleRate_);
    dcBlocker_.reset();
    previousVcaGain_ = 0.0f;
    dormant_ = part_.voice().amplitude_envelope_dead() && part_.voice().vca() == 0;
    drainingToDormant_ = false;
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
    masterGain_ = ParameterCache::load(parameterCache_->master);
    filterCutoffHz_ = ParameterCache::load(parameterCache_->filterCutoff);
    filterResonance_ = ParameterCache::load(parameterCache_->filterResonance);
    filterEnvAmount_ = ParameterCache::load(parameterCache_->filterEnvAmount);
    filterKeyTrack_ = ParameterCache::load(parameterCache_->filterKeyTracking);
    filterModAmount_ = ParameterCache::load(parameterCache_->filterModAmount);
    midiChannel_ = juce::jlimit(0, 16, ParameterCache::loadInt(parameterCache_->midiChannel));
    lfoSync_[0] = ParameterCache::loadInt(parameterCache_->lfo1Sync) != 0;
    lfoSync_[1] = ParameterCache::loadInt(parameterCache_->lfo2Sync) != 0;
    lfoDivision_[0] = juce::jlimit(0, 7, ParameterCache::loadInt(parameterCache_->lfo1Division));
    lfoDivision_[1] = juce::jlimit(0, 7, ParameterCache::loadInt(parameterCache_->lfo2Division));
    sequencerHostSync_ = ParameterCache::loadInt(parameterCache_->seqClockMode) != 0;
    sequencerSwing_ = juce::jlimit(0, 127, ParameterCache::loadInt(parameterCache_->seqSwing));
}

void SwaraXtEngine::resetHostState() noexcept
{
    lastNote_ = 69.0f;
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

void SwaraXtEngine::applyPendingFilterQuality() noexcept
{
    const auto quality = static_cast<FilterQuality>(
        requestedFilterQuality_.load(std::memory_order_relaxed));
    if (filter_.quality() != quality)
        filter_.setQuality(quality);
}

void SwaraXtEngine::updateFilterFromShruthi()
{
    // Envelope 1 is the conventional Shruthi filter-envelope source (0..255).
    const auto& voice = part_.voice();
    const float env1 = static_cast<float>(voice.modulation_source(shruthi::MOD_SRC_ENV_1)) / 255.0f;
    // Combined cutoff destination (includes matrix routes) as optional bias.
    const float cutoffDst = static_cast<float>(voice.cutoff()) / 255.0f;

    SwaraXtFilterParams p;
    p.cutoffHz = filterCutoffHz_;
    p.resonance = juce::jlimit(0.0f, 1.0f, filterResonance_ + static_cast<float>(voice.resonance()) / 512.0f);
    p.keyTrack = filterKeyTrack_;
    p.envAmount = filterEnvAmount_ * 4.0f; // up to ~4 octaves of env depth
    p.modAmount = filterModAmount_ * 2.0f;
    p.envValue = juce::jlimit(0.0f, 1.0f, env1);
    p.modValue = juce::jlimit(-1.0f, 1.0f, (cutoffDst - 0.5f) * 2.0f);
    p.noteNumber = lastNote_;
    p.drive = 1.0f;
    filter_.setParams(p);
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
            lastNote_ = static_cast<float>(event.data1);
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

    // Shruthi updates its VCA at the native control-block cadence. Interpolate
    // adjacent control points here so the audio VCA does not introduce a step;
    // the envelope state and its filter-modulation value remain unchanged.
    const float vcaTarget = static_cast<float>(vca) / 255.0f;
    const float vcaIncrement = read > 0
        ? (vcaTarget - previousVcaGain_) / static_cast<float>(read)
        : 0.0f;
    float vcaGain = previousVcaGain_;
#if SWARAXT_ENABLE_IDLE_CPU_TESTS
    const auto filterStart = ProfileClock::now();
#endif
    for (int i = 0; i < read; ++i)
    {
        vcaGain += vcaIncrement;
        float filtered = temp[i];
        filtered = filter_.processSample(filtered);
        if (! std::isfinite(filtered))
            filtered = 0.0f;
        const float out = filtered * vcaGain * masterGain_;
#if SWARAXT_ENABLE_SHRUTHI_DEBUG_TAPS
        capture.postShruthiMixer[i] = temp[i];
        capture.filterOutput[i] = filtered;
        capture.postVca[i] = out;
        capture.vcaGain[i] = vcaGain;
#endif
        internalQueue_.push(std::isfinite(out) ? out : 0.0f);
    }
#if SWARAXT_ENABLE_IDLE_CPU_TESTS
    cpuProfile_.filterNanoseconds += static_cast<uint64_t>(
        std::chrono::duration_cast<std::chrono::nanoseconds>(ProfileClock::now() - filterStart).count());
    cpuProfile_.filterSamplesProcessed += static_cast<uint64_t>(read);
#endif
    if (read > 0)
        previousVcaGain_ = vcaTarget;

    if (read > 0 && vcaTarget == 0.0f && previousVcaGain_ == 0.0f
        && part_.voice().amplitude_envelope_dead())
    {
        // Give the reader enough future zeros to drain the final VCA ramp
        // without extrapolating past the queue. One kernel length is what it
        // takes for the reconstruction window to move completely past the last
        // real sample, so the tail is never truncated. The heavy voice and
        // filter stay idle while this drains.
        const int tailZeros = internalQueue_.minimumReadableSize();
        for (int i = 0; i < tailZeros; ++i)
            internalQueue_.push(0.0f);
        drainingToDormant_ = true;
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
        return;
    }
#endif
    if (! voiceRequiresAudio())
        return;

    drainingToDormant_ = false;
    if (! dormant_)
        return;

    dormant_ = false;
    audioRing_.Init();
    internalQueue_.reset();
    internalQueue_.setStep(kInternalSampleRate, hostSampleRate_);
    filter_.reset();
    dcBlocker_.reset();
    previousVcaGain_ = 0.0f;
    dormantNativeSamples_ = 0.0;
}

void SwaraXtEngine::enterDormant() noexcept
{
    dormant_ = true;
    drainingToDormant_ = false;
    dormantNativeSamples_ = 0.0;
    audioRing_.Init();
    internalQueue_.reset();
    internalQueue_.setStep(kInternalSampleRate, hostSampleRate_);
    filter_.reset();
    dcBlocker_.reset();
    previousVcaGain_ = 0.0f;
}

void SwaraXtEngine::advanceDormantControl()
{
    dormantNativeSamples_ += kInternalSampleRate / hostSampleRate_;
    while (dormantNativeSamples_ >= static_cast<double>(kAudioBlockSize))
    {
        dormantNativeSamples_ -= static_cast<double>(kAudioBlockSize);
        part_.ProcessControlBlock();
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
            enterDormant();

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
