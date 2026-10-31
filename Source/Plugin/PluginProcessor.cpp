// Copyright 2026 MontroneDSP.
// SPDX-License-Identifier: GPL-3.0-or-later

#include "Plugin/PluginProcessor.h"

#include "Plugin/PluginEditor.h"
#include "Plugin/ApvtsFactoryPresets.h"
#include "Plugin/ShruthiFactoryPresetData.h"
#include "Plugin/ShruthiFactoryPresets.h"

#include "shruthi/patch.h"

#include <cmath>
#include <algorithm>
#include <cstring>
#include <filesystem>

bool SwaraXtAudioProcessor::importTuningFile(const juce::File& file, const juce::File& keyboard,
                                            juce::String& error)
{
    error.clear();
    const auto fail = [&error](const juce::String& message) { error = message; return false; };
    constexpr int limit = 256 * 1024;
    const auto read = [&](const juce::File& input, juce::MemoryBlock& bytes) {
        auto stream = input.createInputStream();
        if (stream == nullptr || stream->getStatus().failed())
            return fail("Cannot open " + input.getFileName());
        if (stream->getTotalLength() > limit)
            return fail(input.getFileName() + " exceeds the 256 KiB import limit");
        stream->readIntoMemoryBlock(bytes, limit + 1);
        if (stream->getStatus().failed() || bytes.getSize() == 0 || bytes.getSize() > limit)
            return fail("Empty, oversized or unreadable file: " + input.getFileName());
        return true;
    };
    const bool scala = file.hasFileExtension("scl");
    if (! scala && ! file.hasFileExtension("swt"))
        return fail("Select one .scl or .swt file, with an optional .kbm for Scala");
    if (keyboard != juce::File() && (! scala || ! keyboard.hasFileExtension("kbm")))
        return fail("A .kbm mapping can only accompany a .scl scale");
    juce::MemoryBlock bytes;
    if (! read(file, bytes))
        return false;
    swaraxt::tuning::ParseError parseError;
    swaraxt::tuning::TuningSnapshot snapshot;
    std::vector<uint8_t> converted;
    if (scala)
    {
        juce::MemoryBlock mapping;
        if (keyboard != juce::File() && ! read(keyboard, mapping))
            return false;
        const auto text = [&](const juce::MemoryBlock& data, juce::String& result) {
            if (std::memchr(data.getData(), 0, data.getSize()) != nullptr)
                return fail("Scala text contains a NUL byte; use ASCII, Latin-1 or UTF-8 text");
            result = juce::String::createStringFromData(data.getData(), static_cast<int>(data.getSize()));
            return true;
        };
        juce::String scaleText, mapText;
        if (! text(bytes, scaleText) || (mapping.getSize() != 0 && ! text(mapping, mapText)))
            return false;
        if (! swaraxt::tuning::parseScalaAndKeyboard(scaleText.toRawUTF8(),
                keyboard == juce::File() ? nullptr : mapText.toRawUTF8(), snapshot, parseError,
                swaraxt::tuning::ScalaSyntax::standard))
            return fail(parseError.message);
        // Copy a complete UTF-8 prefix into the existing fixed-size name field.
        const auto description = juce::String::fromUTF8(snapshot.name);
        const auto name = description.isEmpty() ? file.getFileNameWithoutExtension() : description;
        name.copyToUTF8(snapshot.name, sizeof(snapshot.name));
        if (! swaraxt::tuning::writeTuningTable(snapshot, converted, parseError))
            return fail(parseError.message);
    }
    else if (! swaraxt::tuning::readTuningTable(static_cast<const uint8_t*>(bytes.getData()),
                                               bytes.getSize(), snapshot, parseError))
        return fail(parseError.message);
    if (snapshot.degreeCount > swaraxt::tuning::PublishedTuning::kMaxInterior)
        return fail("This engine supports at most 256 interior pitches plus the repeating period");
    if (! loadTuningBytes(scala ? converted.data() : bytes.getData(),
                          scala ? converted.size() : bytes.getSize()))
        return fail("Tuning publication is busy or unavailable; the current tuning is unchanged. Please retry");
    return true;
}

namespace {

constexpr const char* kFilterQualityProperty = "filterQuality";

bool decodeStoredTuning(const juce::String& encoded, juce::MemoryBlock& decoded)
{
    // SWT1 has a 32-byte header, name, degrees and fixed MIDI degree/Hz maps.
    constexpr size_t maxBytes = 32 + swaraxt::tuning::kNameBytes
        + 8 * swaraxt::tuning::kMaxDegrees + 10 * swaraxt::tuning::kMidiNotes;
    if (encoded.length() > static_cast<int>((maxBytes * 8 + 5) / 6 + 12))
        return false;
    const int dot = encoded.indexOfChar('.');
    if (dot <= 0)
        return false;
    size_t length = 0;
    for (int i = 0; i < dot; ++i)
    {
        const auto c = encoded[i];
        if (c < '0' || c > '9' || length > maxBytes / 10)
            return false;
        length = length * 10 + static_cast<size_t>(c - '0');
        if (length > maxBytes)
            return false;
    }
    if (static_cast<size_t>(encoded.length() - dot - 1) != (length * 8 + 5) / 6)
        return false;
    for (int i = dot + 1; i < encoded.length(); ++i)
    {
        const auto c = encoded[i];
        if (c != '.' && c != '+' && ! (c >= 'A' && c <= 'Z')
            && ! (c >= 'a' && c <= 'z') && ! (c >= '0' && c <= '9'))
            return false;
    }
    return decoded.fromBase64Encoding(encoded);
}

class StateUpdateScope {
public:
    StateUpdateScope(std::atomic_flag& gate, bool wait) noexcept : gate_(gate)
    {
        do {
            acquired_ = !gate_.test_and_set(std::memory_order_acquire);
            if (acquired_ || !wait) break;
            juce::Thread::yield();
        } while (true);
    }
    ~StateUpdateScope() { if (acquired_) gate_.clear(std::memory_order_release); }
    explicit operator bool() const noexcept { return acquired_; }
    StateUpdateScope(const StateUpdateScope&) = delete;
    StateUpdateScope& operator=(const StateUpdateScope&) = delete;
private:
    std::atomic_flag& gate_;
    bool acquired_ = false;
};

void canonicaliseLegacyModes(juce::ValueTree& state)
{
    for (auto child : state)
    {
        const auto id = child.getProperty("id").toString();
        if (id == swaraxt::IDs::inputConditioning)
            child.setProperty("value", 1.0f, nullptr);
        else if (id == swaraxt::IDs::filterModel || id == swaraxt::IDs::dspBoardRouting)
            child.setProperty("value", 0.0f, nullptr);
    }
}

swaraxt::FilterQuality decodeFilterQuality(const juce::var& value)
{
    const int encoded = static_cast<int>(value);
    if (encoded < static_cast<int>(swaraxt::FilterQuality::high)
        || encoded > static_cast<int>(swaraxt::FilterQuality::eco))
        return swaraxt::FilterQuality::normal;
    return static_cast<swaraxt::FilterQuality>(encoded);
}

}  // namespace

SwaraXtAudioProcessor::StateWriterScope::StateWriterScope(SwaraXtAudioProcessor& processor)
    : processor_(processor), publication_(processor.publicationLock_)
{
    if (processor_.stateWriterActive_)
        return;
    while (processor_.stateUpdateGate_.test_and_set(std::memory_order_acquire))
    {
#if defined(SWARAXT_ENABLE_STATE_PUBLICATION_TESTS)
        processor_.statePublicationPoint(StatePublicationPoint::writerContended);
#endif
        juce::Thread::yield();
    }
    processor_.stateWriterActive_ = true;
    acquired_ = true;
}

SwaraXtAudioProcessor::StateWriterScope::~StateWriterScope()
{
    if (acquired_)
    {
        processor_.stateWriterActive_ = false;
        processor_.stateUpdateGate_.clear(std::memory_order_release);
    }
}

SwaraXtAudioProcessor::SwaraXtAudioProcessor()
    : AudioProcessor(BusesProperties().withOutput("Output", juce::AudioChannelSet::stereo(), true)),
      apvts_(*this, nullptr, kStateRoot, swaraxt::createSwaraXtParameterLayout())
{
    parameterCache_.bind(apvts_);
    engine_.bindParameters(parameterCache_);
    engine_.bindSequenceState(sequenceState_);
    apvts_.addParameterListener(swaraxt::IDs::arpPattern, this);
    // Preset values only — engine reset happens in prepareToPlay().
    loadFactoryPreset(0);
}

SwaraXtAudioProcessor::~SwaraXtAudioProcessor()
{
    apvts_.removeParameterListener(swaraxt::IDs::arpPattern, this);
}

void SwaraXtAudioProcessor::parameterChanged(const juce::String& parameterID, float newValue)
{
    if (parameterID == swaraxt::IDs::arpPattern)
        sequenceState_.setArpPatternFromParameter(static_cast<int>(std::lround(newValue)));
}

void SwaraXtAudioProcessor::prepareToPlay(double sampleRate, int samplesPerBlock)
{
    const StateWriterScope writer(*this);
    if (!writer) return;
    if (! swaraxt::SwaraXtEngine::supportsHostSampleRate(sampleRate)) {
        isPrepared_ = false;
        setLatencySamples(0);
        juce::Logger::writeToLog("SWARA XT: unsupported host sample rate " + juce::String(sampleRate)
            + "; supported range is 8000..384000 Hz. Output is silent until a supported prepare.");
        return;
    }
    const double rate = sampleRate;
    const int block = samplesPerBlock > 0 ? samplesPerBlock : 512;

    // Host session restore is complete once audio starts; program changes are intentional.
    hostSessionStateRestored_.store(false, std::memory_order_release);

    engine_.prepare(rate, block);
    setLatencySamples(static_cast<int>(std::ceil(
        swaraxt::SwaraXtEngine::kOutputLatencyNativeSamples * rate / swaraxt::SwaraXtEngine::kInternalSampleRate)));

    const bool resetRequested = requestEngineReset_.exchange(false, std::memory_order_acq_rel);
    if (! engineInitialized_ || resetRequested)
    {
        engine_.reset();
        engineInitialized_ = true;
    }
    else
    {
        // Sample-rate / block-size changes: refresh conversion and drain rings.
        engine_.resetResampler();
    }

    engine_.applyParameters();
    engine_.touchModulationRates();
    isPrepared_ = true;
}

void SwaraXtAudioProcessor::releaseResources()
{
    engine_.clearSequencePlayback();
    engine_.clearSequencerMidiOutput();
    isPrepared_ = false;
}

void SwaraXtAudioProcessor::processBlock(juce::AudioBuffer<float>& buffer, juce::MidiBuffer& midi)
{
    juce::ScopedNoDenormals noDenormals;
    buffer.clear();

    if (! isPrepared_)
        return;

    const auto transport = captureHostTransport();
    // Keep restoration excluded through this render. Audio never waits; a failed
    // admission retains the complete preceding working cohort, including tuning.
    const StateUpdateScope publication(stateUpdateGate_, false);
    swaraxt::SequenceSnapshot sequence;
    uint32_t revision = 0;
    const bool captured = publication && engine_.captureSequence(sequence, revision);
    // Independent sequence edits retain the previous payload while live ARP
    // controls update. A restore's reset intent requires a complete capture.
    const bool admitted = publication && (captured || !requestEngineReset_.load(std::memory_order_acquire));
    const int numSamples = buffer.getNumSamples();
    if (numSamples <= 0)
        return;

    // The engine snapshots input before replacing this host-owned MIDI buffer.
    const bool reset = admitted && requestEngineReset_.exchange(false, std::memory_order_acq_rel);
    engine_.process(midi, buffer, 0, numSamples, transport, &midi, admitted,
                    admitted ? &sequence : nullptr, revision, reset);
}

swaraxt::HostTransportSnapshot SwaraXtAudioProcessor::captureHostTransport() noexcept
{
    swaraxt::HostTransportSnapshot snapshot;
    snapshot.bpm = lastValidHostBpm_;

    auto* hostPlayHead = getPlayHead();
    if (hostPlayHead == nullptr)
        return snapshot;

    const auto position = hostPlayHead->getPosition();
    if (! position.hasValue())
        return snapshot;

    snapshot.hasHostTransport = true;
    snapshot.isPlaying = position->getIsPlaying();
    if (const auto bpm = position->getBpm())
    {
        lastValidHostBpm_ = swaraxt::sanitizeBpm(*bpm);
        snapshot.bpm = lastValidHostBpm_;
    }
    if (const auto ppq = position->getPpqPosition())
    {
        snapshot.ppqPosition = *ppq;
        snapshot.hasPpqPosition = swaraxt::isValidHostPpq(*ppq);
    }
    return snapshot;
}

juce::AudioProcessorEditor* SwaraXtAudioProcessor::createEditor()
{
    return new SwaraXtAudioProcessorEditor(*this);
}

double SwaraXtAudioProcessor::getTailLengthSeconds() const
{
    const auto controls = parameterCache_.boardControls();
    return swaraxt::SwaraXtEngine::kOutputLatencyNativeSamples / swaraxt::SwaraXtEngine::kInternalSampleRate
        + std::max(swaraxt::board::BoardProcessor::tailSeconds(controls), engine_.boardTailSeconds());
}

void SwaraXtAudioProcessor::setCurrentProgram(int index)
{
    const int clamped = juce::jlimit(0, getNumPrograms() - 1, index);
    // Hosts commonly re-assert the current program when an editor opens.
    // Do not reload or clear a user preset when the index is unchanged.
    const StateWriterScope writer(*this);
    if (! writer)
        return;
#if defined(SWARAXT_ENABLE_STATE_PUBLICATION_TESTS)
    statePublicationPoint(StatePublicationPoint::programPublicationLocked);
#endif
    if (clamped == currentProgram_)
        return;

    if (hostSessionStateRestored_.load(std::memory_order_acquire))
    {
        // APVTS from setStateInformation is authoritative; sync program index only.
        ++stateWriterGeneration_;
        currentProgram_ = clamped;
        if (currentUserPresetName_.isEmpty())
            return;
        currentUserPresetName_.clear();
        return;
    }

    applyFactoryPreset(clamped);
}

const juce::String SwaraXtAudioProcessor::getProgramName(int index)
{
    static const char* nativeNames[] = {
        "Saw Bass",
        "Pulse Lead",
        "Wavetable Tone",
        "CZ Metallic",
        "FM Bell",
        "Vowel Formant",
        "8-Bit Land",
        "Noise Perc",
        "Sequenced Motion",
        "Resonant Bass",
        "Self-Osc Filter"
    };
    if (index >= 0 && index < swaraxt::kSwaraNativeFactoryPresets)
        return nativeNames[index];
    const int shruthiIndex = index - swaraxt::kShruthiFactoryPresetStart;
    if (shruthiIndex >= 0
        && shruthiIndex < static_cast<int>(swaraxt::kShruthiFactoryPresetCount))
        return swaraxt::kShruthiFactoryPresets[static_cast<std::size_t>(shruthiIndex)].displayName;
    const int userIndex = index - swaraxt::kUserFactoryPresetStart;
    if (userIndex >= 0 && userIndex < static_cast<int>(swaraxt::kUserFactoryPresetCount))
        return swaraxt::kUserFactoryPresets[static_cast<std::size_t>(userIndex)].displayName;
    return {};
}

bool SwaraXtAudioProcessor::loadFactoryPreset(int index)
{
    const StateWriterScope writer(*this);
    if (! writer)
        return false;
    hostSessionStateRestored_.store(false, std::memory_order_release);
    applyFactoryPreset(index);
    return true;
}

void SwaraXtAudioProcessor::applyFactoryPreset(int index)
{
    const BulkPublicationScope bulk(*this);
    const int programCount = getNumPrograms();
    index = juce::jlimit(0, programCount - 1, index);
    // Snapshot above still carries the previous program and user-preset name.
    currentUserPresetName_.clear();
    currentProgram_ = index;

    // Start from APVTS defaults (Shruthi-aligned init), then specialize.
    const bool preserveSequence = sequenceLocked();
    // Do not publish transient preset values for the protected subsystem.
    if (!preserveSequence) sequenceState_.resetToDefault();
    for (auto* param : getParameters())
    {
        if (auto* ranged = dynamic_cast<juce::RangedAudioParameter*>(param))
            if (!preserveSequence || !swaraxt::isSequenceParameter(ranged->paramID))
                ranged->setValueNotifyingHost(ranged->getDefaultValue());
    }

    auto setInt = [this, preserveSequence](const char* id, int value) {
        if (preserveSequence && swaraxt::isSequenceParameter(id)) return;
        if (auto* p = dynamic_cast<juce::AudioParameterInt*>(apvts_.getParameter(id)))
            *p = value;
    };
    auto setFloat = [this](const char* id, float value) {
        if (auto* p = dynamic_cast<juce::AudioParameterFloat*>(apvts_.getParameter(id)))
            *p = value;
    };

    if (index >= swaraxt::kUserFactoryPresetStart)
    {
        const int userIndex = index - swaraxt::kUserFactoryPresetStart;
        const auto& record = swaraxt::kUserFactoryPresets[static_cast<std::size_t>(userIndex)];
        swaraxt::ApvtsFactoryPresets::applyParams(record.params, record.paramCount, apvts_, preserveSequence);
        if (!preserveSequence) requestEngineReset_.store(true, std::memory_order_release);
        return;
    }

    if (index >= swaraxt::kShruthiFactoryPresetStart)
    {
        const int shruthiIndex = index - swaraxt::kShruthiFactoryPresetStart;
        const auto& record = swaraxt::kShruthiFactoryPresets[static_cast<std::size_t>(shruthiIndex)];
        shruthi::Patch patch {};
        if (swaraxt::ShruthiFactoryPresets::decodePatch(record.bytes.data(),
                                                        record.bytes.size(), patch))
            swaraxt::ShruthiFactoryPresets::applyPatchToApvts(patch, apvts_, preserveSequence);
        if (!preserveSequence) requestEngineReset_.store(true, std::memory_order_release);
        return;
    }

    switch (index)
    {
        case 0: // Saw bass
            setInt(swaraxt::IDs::filterEnvDepth, 45);
            setInt(swaraxt::IDs::osc1Shape, 1);
            setInt(swaraxt::IDs::osc2Shape, 1);
            setInt(swaraxt::IDs::osc2Range, -12);
            setInt(swaraxt::IDs::mixSub, 64);
            setInt(swaraxt::IDs::env2Attack, 0);
            setInt(swaraxt::IDs::env2Decay, 40);
            setInt(swaraxt::IDs::env2Sustain, 100);
            setInt(swaraxt::IDs::env2Release, 40);
            setFloat(swaraxt::IDs::filterCutoff, 600.0f);
            setFloat(swaraxt::IDs::filterResonance, 0.35f);
            setFloat(swaraxt::IDs::filterKeyTracking, 0.4f);
            break;
        case 1: // Pulse lead
            setInt(swaraxt::IDs::filterEnvDepth, 41);
            setInt(swaraxt::IDs::osc1Shape, 2);
            setInt(swaraxt::IDs::osc1Param, 64);
            setInt(swaraxt::IDs::osc2Shape, 2);
            setInt(swaraxt::IDs::osc2Param, 80);
            setInt(swaraxt::IDs::osc2Range, 0);
            setInt(swaraxt::IDs::mixBalance, 64);
            setFloat(swaraxt::IDs::filterCutoff, 3500.0f);
            setFloat(swaraxt::IDs::filterResonance, 0.25f);
            break;
        case 2: // Wavetable
            setInt(swaraxt::IDs::filterEnvDepth, 41);
            setInt(swaraxt::IDs::osc1Shape, 11);
            setInt(swaraxt::IDs::osc1Param, 40);
            setInt(swaraxt::IDs::osc2Shape, 12);
            setInt(swaraxt::IDs::osc2Param, 70);
            setFloat(swaraxt::IDs::filterCutoff, 5000.0f);
            break;
        case 3: // CZ metallic
            setInt(swaraxt::IDs::filterEnvDepth, 41);
            setInt(swaraxt::IDs::osc1Shape, 4);
            setInt(swaraxt::IDs::osc1Param, 90);
            setInt(swaraxt::IDs::osc2Shape, 5);
            setInt(swaraxt::IDs::osc2Param, 100);
            setFloat(swaraxt::IDs::filterCutoff, 7000.0f);
            setFloat(swaraxt::IDs::filterResonance, 0.4f);
            break;
        case 4: // FM
            setInt(swaraxt::IDs::filterEnvDepth, 41);
            setInt(swaraxt::IDs::osc1Shape, 10);
            setInt(swaraxt::IDs::osc1Param, 48);
            setInt(swaraxt::IDs::osc2Shape, 1);
            setInt(swaraxt::IDs::mixBalance, 20);
            setFloat(swaraxt::IDs::filterCutoff, 9000.0f);
            break;
        case 5: // Vowel
            setInt(swaraxt::IDs::filterEnvDepth, 41);
            setInt(swaraxt::IDs::osc1Shape, 24);
            setInt(swaraxt::IDs::osc1Param, 64);
            setInt(swaraxt::IDs::osc2Shape, 0);
            setInt(swaraxt::IDs::mixBalance, 0);
            setFloat(swaraxt::IDs::filterCutoff, 2500.0f);
            setFloat(swaraxt::IDs::filterResonance, 0.2f);
            break;
        case 6: // 8-bit
            setInt(swaraxt::IDs::filterEnvDepth, 41);
            setInt(swaraxt::IDs::osc1Shape, 20);
            setInt(swaraxt::IDs::osc1Param, 32);
            setInt(swaraxt::IDs::osc2Shape, 21);
            setInt(swaraxt::IDs::osc1Option, 5);
            setFloat(swaraxt::IDs::filterCutoff, 6000.0f);
            break;
        case 7: // Noise perc
            setInt(swaraxt::IDs::filterEnvDepth, 52);
            setInt(swaraxt::IDs::osc1Shape, 23);
            setInt(swaraxt::IDs::osc1Param, 80);
            setInt(swaraxt::IDs::mixNoise, 100);
            setInt(swaraxt::IDs::mixBalance, 0);
            setInt(swaraxt::IDs::env2Attack, 0);
            setInt(swaraxt::IDs::env2Decay, 30);
            setInt(swaraxt::IDs::env2Sustain, 0);
            setInt(swaraxt::IDs::env2Release, 20);
            setFloat(swaraxt::IDs::filterCutoff, 1800.0f);
            setFloat(swaraxt::IDs::filterResonance, 0.55f);
            break;
        case 8: // Sequenced
            setInt(swaraxt::IDs::filterEnvDepth, 41);
            setInt(swaraxt::IDs::osc1Shape, 1);
            setInt(swaraxt::IDs::osc2Shape, 3);
            setInt(swaraxt::IDs::seqMode, 1);
            setInt(swaraxt::IDs::arpDirection, 1);
            setInt(swaraxt::IDs::arpOctaves, 2);
            setFloat(swaraxt::IDs::filterCutoff, 2200.0f);
            setFloat(swaraxt::IDs::filterResonance, 0.45f);
            break;
        case 9: // Resonant bass
            setInt(swaraxt::IDs::filterEnvDepth, 48);
            setInt(swaraxt::IDs::osc1Shape, 1);
            setInt(swaraxt::IDs::mixSub, 80);
            setFloat(swaraxt::IDs::filterCutoff, 280.0f);
            setFloat(swaraxt::IDs::filterResonance, 0.72f);
            setFloat(swaraxt::IDs::filterKeyTracking, 0.6f);
            break;
        case 10: // Self-oscillating filter tone
            setInt(swaraxt::IDs::filterEnvDepth, 32);
            setInt(swaraxt::IDs::osc1Shape, 0);
            setInt(swaraxt::IDs::mixBalance, 0);
            setInt(swaraxt::IDs::mixNoise, 0);
            setInt(swaraxt::IDs::mixSub, 0);
            setFloat(swaraxt::IDs::filterCutoff, 880.0f);
            setFloat(swaraxt::IDs::filterResonance, 0.96f);
            setFloat(swaraxt::IDs::filterKeyTracking, 1.0f);
            break;
        default:
            break;
    }

    if (!preserveSequence) requestEngineReset_.store(true, std::memory_order_release);
}

juce::File SwaraXtAudioProcessor::userPresetDirectory() const
{
    if (presetDirectoryOverride_ != juce::File {})
        return presetDirectoryOverride_;
    return juce::File::getSpecialLocation(juce::File::userApplicationDataDirectory)
        .getChildFile("MontroneDSP")
        .getChildFile("Swara XT")
        .getChildFile("Presets");
}

void SwaraXtAudioProcessor::setUserPresetDirectoryForTests(const juce::File& directory)
{
    presetDirectoryOverride_ = directory;
}

bool SwaraXtAudioProcessor::validatePresetName(const juce::String& name, juce::String& cleanName)
{
    cleanName = name.trim();
    if (cleanName.isEmpty() || cleanName == "." || cleanName == "..")
        return false;
    if (cleanName.contains("/") || cleanName.contains("\\"))
        return false;
    if (cleanName.endsWith(".") || cleanName.endsWith(" "))
        return false;
    if (cleanName.getNumBytesAsUTF8() + sizeof(".swaraxtpreset") - 1 > 255)
        return false;
    {
        juce::String base = cleanName;
        const int dot = base.indexOfChar('.');
        if (dot >= 0)
            base = base.substring(0, dot);
        base = base.trim();
        static const char* const reserved[] = {
            "CON", "PRN", "AUX", "NUL",
            "COM1", "COM2", "COM3", "COM4", "COM5", "COM6", "COM7", "COM8", "COM9",
            "LPT1", "LPT2", "LPT3", "LPT4", "LPT5", "LPT6", "LPT7", "LPT8", "LPT9"
        };
        for (const char* r : reserved)
            if (base.equalsIgnoreCase(r))
                return false;
    }
    return juce::File::createLegalFileName(cleanName) == cleanName;
}

std::vector<SwaraXtAudioProcessor::PresetEntry> SwaraXtAudioProcessor::getPresetEntries() const
{
    std::vector<PresetEntry> entries;
    const int factoryCount = const_cast<SwaraXtAudioProcessor*>(this)->getNumPrograms();
    entries.reserve(static_cast<size_t>(factoryCount) + 16);
    for (int index = 0; index < factoryCount; ++index)
        entries.push_back({ const_cast<SwaraXtAudioProcessor*>(this)->getProgramName(index), {}, index, true });

    juce::Array<juce::File> files;
    userPresetDirectory().findChildFiles(files, juce::File::findFiles, false, "*.swaraxtpreset");
    for (const auto& file : files)
        entries.push_back({ file.getFileNameWithoutExtension(), file, -1, false });
    std::sort(entries.begin() + factoryCount, entries.end(),
              [](const PresetEntry& a, const PresetEntry& b) {
                  return a.name.compareNatural(b.name) < 0;
              });
    return entries;
}

SwaraXtAudioProcessor::PresetMetadata SwaraXtAudioProcessor::presetMetadata() const
{
    for (;;)
    {
        if ((bulkPublicationEpoch_.load(std::memory_order_acquire) & 1u) != 0u)
        {
            const juce::ScopedLock committed(committedStateLock_);
            return committedMetadata_;
        }
        const juce::ScopedTryLock publication(publicationLock_);
        if (publication.isLocked())
            return { currentProgram_, currentUserPresetName_ };
        juce::Thread::yield();
    }
}

int SwaraXtAudioProcessor::getCurrentProgram()
{
    return presetMetadata().program;
}

bool SwaraXtAudioProcessor::currentPresetIsUser() const
{
    return presetMetadata().userName.isNotEmpty();
}

juce::String SwaraXtAudioProcessor::currentPresetName() const
{
    const auto metadata = presetMetadata();
    return metadata.userName.isNotEmpty() ? metadata.userName
        : const_cast<SwaraXtAudioProcessor*>(this)->getProgramName(metadata.program);
}

bool SwaraXtAudioProcessor::decodePresetData(const void* data,
                                             int sizeInBytes,
                                             juce::ValueTree& state) const
{
    if (data == nullptr || sizeInBytes <= 0)
        return false;
    auto xml = getXmlFromBinary(data, sizeInBytes);
    if (xml == nullptr)
        return false;
    state = juce::ValueTree::fromXml(*xml);
    if (! state.isValid() || ! state.hasType(kStateRoot))
        return false;
    const int version = static_cast<int>(state.getProperty("stateVersion", 0));
    return version >= 1 && version <= kStateVersion;
}

bool SwaraXtAudioProcessor::loadPresetEntry(const PresetEntry& entry, juce::String& error)
{
    error.clear();
    if (entry.isFactory)
    {
        // Explicit UI/host-adjacent load must apply even if the factory index
        // already matches (reload factory defaults after edits).
        if (loadFactoryPreset(entry.factoryIndex))
            return true;
        error = "A preset cannot be loaded from inside another state update.";
        return false;
    }

    juce::MemoryBlock data;
    if (! entry.file.loadFileAsData(data))
    {
        error = "Could not read the preset file.";
        return false;
    }

    juce::ValueTree decoded;
    if (! decodePresetData(data.getData(), static_cast<int>(data.getSize()), decoded))
    {
        error = "The preset file is malformed or uses an unsupported schema.";
        return false;
    }

    return restoreState(decoded, true, entry.name, &error);
}

bool SwaraXtAudioProcessor::saveUserPreset(const juce::String& name,
                                           bool overwrite,
                                           juce::String& error)
{
    error.clear();
    juce::String cleanName;
    if (! validatePresetName(name, cleanName))
    {
        error = "Choose a non-empty preset name without path or filename characters.";
        return false;
    }

    const auto directory = userPresetDirectory();
    if (directory.createDirectory().failed())
    {
        error = "Could not create the user preset directory.";
        return false;
    }

    const auto target = directory.getChildFile(cleanName + ".swaraxtpreset");
    if (target.existsAsFile() && ! overwrite)
    {
        error = "A user preset with this name already exists.";
        return false;
    }

    juce::MemoryBlock data;
    uint64_t savedGeneration = 0;
    {
        const StateWriterScope writer(*this);
        if (! writer)
        {
            error = "A preset cannot be saved from inside another state update.";
            return false;
        }
        getStateInformation(data);
        savedGeneration = ++stateWriterGeneration_;
    }
    if (auto xml = getXmlFromBinary(data.getData(), static_cast<int>(data.getSize())))
    {
        xml->setAttribute("presetKind", "user");
        xml->setAttribute("presetName", cleanName);
        xml->removeAttribute("sequenceLocked");
        xml->removeAttribute("randomNoteLower");
        xml->removeAttribute("randomNoteUpper");
        copyXmlToBinary(*xml, data);
    }
#if defined(SWARAXT_ENABLE_STATE_PUBLICATION_TESTS)
    statePublicationPoint(StatePublicationPoint::userPresetFileReady);
#endif
    const auto temporaryPath = target.getParentDirectory().getNonexistentChildFile(
        "swaraxt-" + juce::Uuid().toString(), ".tmp");
#if defined(SWARAXT_ENABLE_STATE_PUBLICATION_TESTS)
    statePublicationPoint(StatePublicationPoint::userPresetTemporaryChosen);
#endif
    juce::TemporaryFile temporary(target, temporaryPath);
    {
        // Do not publish a partial payload: JUCE's replaceWithData() does not
        // propagate appendData() failure. Use an unbuffered write so a short write cannot be hidden by flush().
        juce::FileOutputStream stream(temporary.getFile(), 0);
        if (stream.failedToOpen() || ! stream.write(data.getData(), data.getSize()))
        {
            error = "Could not write the preset safely.";
            return false;
        }
        stream.flush();
        if (stream.getStatus().failed())
        {
            error = "Could not write the preset safely.";
            return false;
        }
    }
#if defined(SWARAXT_ENABLE_STATE_PUBLICATION_TESTS)
    statePublicationPoint(StatePublicationPoint::userPresetTemporaryWritten);
#endif
    if (overwrite)
    {
        if (! temporary.overwriteTargetFileWithTemporary())
        {
            error = "Could not write the preset safely.";
            return false;
        }
    }
    else
    {
        // Same-directory hard-link publication is atomic and cannot replace an
        // existing name, including a competing writer in another instance.
        std::error_code ec;
        std::filesystem::create_hard_link(
            std::filesystem::u8path(temporary.getFile().getFullPathName().toRawUTF8()),
            std::filesystem::u8path(target.getFullPathName().toRawUTF8()), ec);
        if (ec)
        {
            error = target.exists() ? "A user preset with this name already exists."
                                    : "Could not create the preset safely on this filesystem.";
            return false;
        }
    }
    {
        const StateWriterScope writer(*this);
        if (writer && stateWriterGeneration_ == savedGeneration)
            currentUserPresetName_ = cleanName;
    }
    return true;
}

void SwaraXtAudioProcessor::beginBulkPublication()
{
    jassert(stateWriterActive_);
    jassert((bulkPublicationEpoch_.load(std::memory_order_relaxed) & 1u) == 0u);
    juce::MemoryBlock fresh;
    // copyState flushes pending parameter values before mutation starts. Its
    // reentrant saves still serialize this unchanged generation under the
    // recursive publication lock; mutation callbacks use the installed blob.
    writeStateInformation(fresh);
    {
        const juce::ScopedLock lock(committedStateLock_);
        committedState_.swapWith(fresh);
        committedMetadata_ = { currentProgram_, currentUserPresetName_ };
    }
    ++stateWriterGeneration_;
    bulkPublicationEpoch_.fetch_add(1u, std::memory_order_release);
#if defined(SWARAXT_ENABLE_STATE_PUBLICATION_TESTS)
    statePublicationPoint(StatePublicationPoint::bulkEpochPublished);
#endif
}

void SwaraXtAudioProcessor::endBulkPublication() noexcept
{
    // The cache is the transaction's preimage. The next writer refreshes it;
    // completed saves serialize live state, so teardown needs no allocation.
    bulkPublicationEpoch_.fetch_add(1u, std::memory_order_release);
}

void SwaraXtAudioProcessor::getStateInformation(juce::MemoryBlock& destData)
{
  #if JUCE_ANDROID
    juce::ignoreUnused(destData);
  #else
    for (;;)
    {
        if ((bulkPublicationEpoch_.load(std::memory_order_acquire) & 1u) != 0u)
        {
            const juce::ScopedLock lock(committedStateLock_);
            destData = committedState_;
            return;
        }
#if defined(SWARAXT_ENABLE_STATE_PUBLICATION_TESTS)
        statePublicationPoint(StatePublicationPoint::stateReadBeforeLock);
#endif
        // A notification callback may hold a parameter listener lock that the
        // writer needs. Recheck the epoch instead of waiting on publication.
        const juce::ScopedTryLock publication(publicationLock_);
        if (publication.isLocked())
        {
            writeStateInformation(destData);
            return;
        }
        juce::Thread::yield();
    }
  #endif
}

void SwaraXtAudioProcessor::writeStateInformation(juce::MemoryBlock& destData)
{
  #if JUCE_ANDROID
    juce::ignoreUnused(destData);
  #else
    auto state = apvts_.copyState();
    canonicaliseLegacyModes(state);
    state.setProperty("stateVersion", kStateVersion, nullptr);
    state.setProperty("sequenceLocked", sequenceLocked(), nullptr);
    const auto range = randomNoteRange();
    state.setProperty("randomNoteLower", range.lower, nullptr);
    state.setProperty("randomNoteUpper", range.upper, nullptr);
    state.setProperty("currentProgram", currentProgram_, nullptr);
    state.setProperty("presetKind", currentUserPresetName_.isEmpty() ? "factory" : "user", nullptr);
    state.setProperty("presetName", currentPresetName(), nullptr);
    state.setProperty(kFilterQualityProperty, static_cast<int>(filterQuality()), nullptr);
    if (const auto existing = state.getChildWithName("SEQUENCE"); existing.isValid())
        state.removeChild(existing, nullptr);
    state.addChild(sequenceState_.toValueTree(), -1, nullptr);
    const auto tuningBytes = engine_.tuningBytesForState();
    if (! tuningBytes.empty())
    {
        // MemoryBlock's size-prefixed alphabet must match fromBase64Encoding on restore.
        const juce::MemoryBlock tuningBlock(tuningBytes.data(), tuningBytes.size());
        state.setProperty("tuningTable", tuningBlock.toBase64Encoding(), nullptr);
    }
    if (auto xml = state.createXml())
        copyXmlToBinary(*xml, destData);
  #endif
}

void SwaraXtAudioProcessor::setStateInformation(const void* data, int sizeInBytes)
{
    if (auto xml = getXmlFromBinary(data, sizeInBytes))
        restoreState(juce::ValueTree::fromXml(*xml), false);
    else
        lastRestoreResult_.store(RestoreResult::invalid, std::memory_order_relaxed);
}

bool SwaraXtAudioProcessor::restoreState(juce::ValueTree tree, bool fromPreset,
                                       const juce::String& presetName, juce::String* error)
{
    const auto reject = [&](RestoreResult reason, const char* message) {
        lastRestoreResult_.store(reason, std::memory_order_relaxed);
        if (error != nullptr) *error = message;
        return false;
    };
    bool mutationStarted = false;
    try
    {
        if (!tree.isValid() || !tree.hasType(kStateRoot))
            return reject(RestoreResult::invalid, "Invalid state root.");
        const int version = static_cast<int>(tree.getProperty("stateVersion", 1));
        if (version > kStateVersion || version < 1)
            return reject(RestoreResult::invalid, "Unsupported state version.");
        const StateWriterScope writer(*this);
        if (! writer)
            return reject(RestoreResult::reentrant, "A preset cannot be loaded from inside another state update.");
    #if defined(SWARAXT_ENABLE_STATE_PUBLICATION_TESTS)
        statePublicationPoint(StatePublicationPoint::restoreGateLocked);
    #endif
        const bool preserveSequence = fromPreset && sequenceLocked();
        if (preserveSequence)
        {
            // Merge the protected snapshot before APVTS publication, avoiding
            // intermediate sequence/arp changes in the engine and attachments.
            for (const auto* id : swaraxt::sequenceParameterIds)
            {
                auto child = tree.getChildWithProperty("id", id);
                if (!child.isValid())
                {
                    child = juce::ValueTree("PARAM");
                    child.setProperty("id", id, nullptr);
                    tree.addChild(child, -1, nullptr);
                }
                child.setProperty("value", apvts_.getRawParameterValue(id)->load(), nullptr);
            }
            if (const auto existing = tree.getChildWithName("SEQUENCE"); existing.isValid())
                tree.removeChild(existing, nullptr);
            tree.addChild(sequenceState_.toValueTree(), -1, nullptr);
        }
        const bool locked = fromPreset ? sequenceLocked() : static_cast<bool>(tree.getProperty("sequenceLocked", false));
        const auto range = fromPreset ? randomNoteRange() : swaraxt::SequenceState::NoteRange {
            juce::jlimit(-48, 0, static_cast<int>(tree.getProperty("randomNoteLower", -12))),
            juce::jlimit(0, 48, static_cast<int>(tree.getProperty("randomNoteUpper", 12))) };
        tree.setProperty("randomNoteLower", range.lower, nullptr);
        tree.setProperty("randomNoteUpper", range.upper, nullptr);
        tree.setProperty("sequenceLocked", locked, nullptr);
        // JUCE defaults absent ordinary parameter children during replacement.
        // Legacy states must not inherit an active Board/FX configuration.
        for (const auto* id : { swaraxt::IDs::filterModel, swaraxt::IDs::dspFxProgram,
                               swaraxt::IDs::dspFxParam1, swaraxt::IDs::dspFxParam2,
                               swaraxt::IDs::dspBoardRouting, swaraxt::IDs::postMixer,
                               swaraxt::IDs::inputConditioning, swaraxt::IDs::midiInputTuning })
        {
            if (!tree.getChildWithProperty("id", id).isValid())
            {
                const auto* parameter = apvts_.getParameter(id);
                juce::ValueTree value("PARAM");
                value.setProperty("id", id, nullptr);
                value.setProperty("value", parameter->convertFrom0to1(parameter->getDefaultValue()), nullptr);
                tree.addChild(value, -1, nullptr);
            }
        }
        // Parameter records outside the active schema are not retained in state.
        for (int index = tree.getNumChildren(); --index >= 0;)
        {
            const auto child = tree.getChild(index);
            if (child.hasType("PARAM") && apvts_.getParameter(child["id"].toString()) == nullptr)
                tree.removeChild(index, nullptr);
        }
        canonicaliseLegacyModes(tree);
        swaraxt::SwaraXtEngine::TuningReservation tuning;
        bool defaultTuning = false;
        if (! preserveSequence)
        {
            const auto encoded = tree.getProperty("tuningTable").toString();
            juce::MemoryBlock decoded;
            // Missing/old undecodable strings retain the existing legacy behavior.
            if (encoded.isNotEmpty() && encoded.containsChar('.'))
            {
                if (! decodeStoredTuning(encoded, decoded))
                    return reject(RestoreResult::invalid, "Malformed or oversized tuning encoding.");
                using Result = swaraxt::SwaraXtEngine::TuningResult;
                auto result = engine_.reserveTuning(static_cast<const uint8_t*>(decoded.getData()), decoded.getSize(), tuning);
                if (result == Result::invalid || result == Result::unsupported)
                {
                    defaultTuning = true;
                    result = engine_.reserveBuiltinTuning(0, tuning); // Existing invalid-SWT fallback, now checked.
                }
                if (result != Result::success)
                    return reject(result == Result::busy ? RestoreResult::tuningBusy : RestoreResult::tuningUnavailable,
                                  "Tuning publication is temporarily unavailable; the preset is unchanged. Please retry.");
            }
        }
        swaraxt::SequenceSnapshot sequence;
        if (!preserveSequence && !swaraxt::SequenceState::decodeValueTree(tree.getChildWithName("SEQUENCE"), sequence))
        {
            sequence = swaraxt::SequenceState::defaultSnapshot();
            const auto* pattern = apvts_.getParameter(swaraxt::IDs::arpPattern);
            juce::var value = pattern->convertFrom0to1(pattern->getDefaultValue());
            for (const auto child : tree)
                if (child.getProperty("id") == swaraxt::IDs::arpPattern)
                    value = child.getProperty("value", value); // JUCE's last duplicate wins.
            sequence.arpPattern = static_cast<uint8_t>(juce::jlimit(0, 15, static_cast<int>(std::lround(static_cast<float>(value)))));
        }
        const auto quality = decodeFilterQuality(
            tree.getProperty(kFilterQualityProperty,
                             static_cast<int>(swaraxt::FilterQuality::normal)));
        int programIndex = juce::jlimit(0,
                                       getNumPrograms() - 1,
                                       static_cast<int>(tree.getProperty("currentProgram", 0)));
        // Saved parameter state stays authoritative when factory numbering changes.
        // Names remap retained programs without substituting the current bank sound.
        if (tree.getProperty("presetKind") == "factory")
            for (int program = 0; program < getNumPrograms(); ++program)
                if (getProgramName(program) == tree.getProperty("presetName").toString())
                {
                    programIndex = program;
                    break;
                }
        const auto userName = fromPreset ? presetName
            : (tree.getProperty("presetKind") == "user" ? tree.getProperty("presetName").toString().trim()
                                                      : juce::String {});
        // Recoverable validation, allocation and admission failures precede mutation.
        // JUCE replacement/listeners must obey the supported nonthrowing mutation
        // contract. A mutation-time exception propagates; it is never reported as a
        // coherent rejection or disguised by a rollback.
        const BulkPublicationScope bulk(*this);
        mutationStarted = true;
        apvts_.replaceState(tree);
        if (!preserveSequence) sequenceState_.store(sequence, true);
        if (!fromPreset) { setSequenceLocked(locked); setRandomNoteRange(range); }
        setFilterQuality(quality);
        currentProgram_ = programIndex;
        currentUserPresetName_ = userName;
#if defined(SWARAXT_ENABLE_STATE_PUBLICATION_TESTS)
        statePublicationPoint(StatePublicationPoint::restoreBeforeTuningPublish);
#endif
        tuning.publish();
        hostSessionStateRestored_.store(! fromPreset, std::memory_order_release);
        if (!preserveSequence) requestEngineReset_.store(true, std::memory_order_release);
        lastRestoreResult_.store(defaultTuning ? RestoreResult::defaultTuning : RestoreResult::success,
                                 std::memory_order_relaxed);
        return true;
    }
    catch (const std::bad_alloc&)
    {
        if (mutationStarted) throw;
        return reject(RestoreResult::preparationFailed, "Insufficient memory to prepare the state; the preset is unchanged.");
    }
}

juce::AudioProcessor* JUCE_CALLTYPE createPluginFilter()
{
    return new SwaraXtAudioProcessor();
}
