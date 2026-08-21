// Copyright 2026 MontroneDSP.
// SPDX-License-Identifier: GPL-3.0-or-later

#include "Plugin/PluginProcessor.h"

#include "Plugin/PluginEditor.h"
#include "Ui/SwaraXtSkin.h"

#include <cmath>
#include <algorithm>

SwaraXtAudioProcessor::SwaraXtAudioProcessor()
    : AudioProcessor(BusesProperties().withOutput("Output", juce::AudioChannelSet::stereo(), true)),
      apvts_(*this, nullptr, kStateRoot, swaraxt::createSwaraXtParameterLayout())
{
    parameterCache_.bind(apvts_);
    engine_.bindParameters(parameterCache_);
    engine_.setFilterQuality(swaraxt::ui::UiPreferences::loadFilterQuality());
    // Preset values only — engine reset happens in prepareToPlay().
    loadFactoryPreset(0);
}

SwaraXtAudioProcessor::~SwaraXtAudioProcessor()
{
}

void SwaraXtAudioProcessor::prepareToPlay(double sampleRate, int samplesPerBlock)
{
    const double rate = sampleRate > 0.0 ? sampleRate : 44100.0;
    const int block = samplesPerBlock > 0 ? samplesPerBlock : 512;

    engine_.prepare(rate, block);

    if (! engineInitialized_)
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
    requestEngineReset_.store(false, std::memory_order_release);
    isPrepared_ = true;
}

void SwaraXtAudioProcessor::releaseResources()
{
    isPrepared_ = false;
}

void SwaraXtAudioProcessor::processBlock(juce::AudioBuffer<float>& buffer, juce::MidiBuffer& midi)
{
    juce::ScopedNoDenormals noDenormals;
    buffer.clear();

    if (! isPrepared_)
        return;

    // Consume message-thread preset/reset requests on the audio thread only.
    if (requestEngineReset_.exchange(false, std::memory_order_acq_rel))
    {
        engine_.reset();
        engine_.touchModulationRates();
    }

    // Cached atomics — safe to mirror every block without string map lookups.
    engine_.applyParameters();
    const int numSamples = buffer.getNumSamples();
    if (numSamples <= 0)
        return;

    engine_.process(midi, buffer, 0, numSamples, captureHostTransport());
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
        snapshot.hasPpqPosition = std::isfinite(*ppq) && *ppq >= 0.0;
    }
    return snapshot;
}

juce::AudioProcessorEditor* SwaraXtAudioProcessor::createEditor()
{
    return new SwaraXtAudioProcessorEditor(*this);
}

void SwaraXtAudioProcessor::setCurrentProgram(int index)
{
    const int clamped = juce::jlimit(0, getNumPrograms() - 1, index);
    // Hosts commonly re-assert the current program when an editor opens.
    // Do not reload or clear a user preset when the index is unchanged.
    if (clamped == currentProgram_)
        return;

    currentProgram_ = clamped;
    currentUserPresetName_.clear();
    loadFactoryPreset(currentProgram_);
}

const juce::String SwaraXtAudioProcessor::getProgramName(int index)
{
    static const char* names[] = {
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
    if (index >= 0 && index < 11)
        return names[index];
    return {};
}

void SwaraXtAudioProcessor::loadFactoryPreset(int index)
{
    index = juce::jlimit(0, 10, index);
    currentProgram_ = index;

    // Start from APVTS defaults (Shruthi-aligned init), then specialize.
    for (auto* param : getParameters())
    {
        if (auto* ranged = dynamic_cast<juce::RangedAudioParameter*>(param))
            ranged->setValueNotifyingHost(ranged->getDefaultValue());
    }

    auto setInt = [this](const char* id, int value) {
        if (auto* p = dynamic_cast<juce::AudioParameterInt*>(apvts_.getParameter(id)))
            *p = value;
    };
    auto setFloat = [this](const char* id, float value) {
        if (auto* p = dynamic_cast<juce::AudioParameterFloat*>(apvts_.getParameter(id)))
            *p = value;
    };

    switch (index)
    {
        case 0: // Saw bass
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
            setFloat(swaraxt::IDs::filterEnvAmount, 0.55f);
            setFloat(swaraxt::IDs::filterKeyTracking, 0.4f);
            break;
        case 1: // Pulse lead
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
            setInt(swaraxt::IDs::osc1Shape, 11);
            setInt(swaraxt::IDs::osc1Param, 40);
            setInt(swaraxt::IDs::osc2Shape, 12);
            setInt(swaraxt::IDs::osc2Param, 70);
            setFloat(swaraxt::IDs::filterCutoff, 5000.0f);
            break;
        case 3: // CZ metallic
            setInt(swaraxt::IDs::osc1Shape, 4);
            setInt(swaraxt::IDs::osc1Param, 90);
            setInt(swaraxt::IDs::osc2Shape, 5);
            setInt(swaraxt::IDs::osc2Param, 100);
            setFloat(swaraxt::IDs::filterCutoff, 7000.0f);
            setFloat(swaraxt::IDs::filterResonance, 0.4f);
            break;
        case 4: // FM
            setInt(swaraxt::IDs::osc1Shape, 10);
            setInt(swaraxt::IDs::osc1Param, 48);
            setInt(swaraxt::IDs::osc2Shape, 1);
            setInt(swaraxt::IDs::mixBalance, 20);
            setFloat(swaraxt::IDs::filterCutoff, 9000.0f);
            break;
        case 5: // Vowel
            setInt(swaraxt::IDs::osc1Shape, 24);
            setInt(swaraxt::IDs::osc1Param, 64);
            setInt(swaraxt::IDs::osc2Shape, 0);
            setInt(swaraxt::IDs::mixBalance, 0);
            setFloat(swaraxt::IDs::filterCutoff, 2500.0f);
            setFloat(swaraxt::IDs::filterResonance, 0.2f);
            break;
        case 6: // 8-bit
            setInt(swaraxt::IDs::osc1Shape, 20);
            setInt(swaraxt::IDs::osc1Param, 32);
            setInt(swaraxt::IDs::osc2Shape, 21);
            setInt(swaraxt::IDs::osc1Option, 5);
            setFloat(swaraxt::IDs::filterCutoff, 6000.0f);
            break;
        case 7: // Noise perc
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
            setFloat(swaraxt::IDs::filterEnvAmount, 0.8f);
            break;
        case 8: // Sequenced
            setInt(swaraxt::IDs::osc1Shape, 1);
            setInt(swaraxt::IDs::osc2Shape, 3);
            setInt(swaraxt::IDs::seqMode, 1);
            setInt(swaraxt::IDs::arpDirection, 1);
            setInt(swaraxt::IDs::arpOctaves, 2);
            setFloat(swaraxt::IDs::filterCutoff, 2200.0f);
            setFloat(swaraxt::IDs::filterResonance, 0.45f);
            break;
        case 9: // Resonant bass
            setInt(swaraxt::IDs::osc1Shape, 1);
            setInt(swaraxt::IDs::mixSub, 80);
            setFloat(swaraxt::IDs::filterCutoff, 280.0f);
            setFloat(swaraxt::IDs::filterResonance, 0.72f);
            setFloat(swaraxt::IDs::filterEnvAmount, 0.65f);
            setFloat(swaraxt::IDs::filterKeyTracking, 0.6f);
            break;
        case 10: // Self-oscillating filter tone
            setInt(swaraxt::IDs::osc1Shape, 0);
            setInt(swaraxt::IDs::mixBalance, 0);
            setInt(swaraxt::IDs::mixNoise, 0);
            setInt(swaraxt::IDs::mixSub, 0);
            setFloat(swaraxt::IDs::filterCutoff, 880.0f);
            setFloat(swaraxt::IDs::filterResonance, 0.96f);
            setFloat(swaraxt::IDs::filterKeyTracking, 1.0f);
            setFloat(swaraxt::IDs::filterEnvAmount, 0.0f);
            break;
        default:
            break;
    }

    requestEngineReset_.store(true, std::memory_order_release);
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
    if (cleanName.isEmpty() || cleanName == "." || cleanName == ".."
        || cleanName.contains("/") || cleanName.contains("\\"))
        return false;
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

juce::String SwaraXtAudioProcessor::currentPresetName() const
{
    if (currentUserPresetName_.isNotEmpty())
        return currentUserPresetName_;
    return const_cast<SwaraXtAudioProcessor*>(this)->getProgramName(currentProgram_);
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
        currentUserPresetName_.clear();
        currentProgram_ = juce::jlimit(0, getNumPrograms() - 1, entry.factoryIndex);
        loadFactoryPreset(currentProgram_);
        return true;
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

    setStateInformation(data.getData(), static_cast<int>(data.getSize()));
    currentUserPresetName_ = entry.name;
    return true;
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

    const auto previousPresetName = currentUserPresetName_;
    currentUserPresetName_ = cleanName;
    juce::MemoryBlock data;
    getStateInformation(data);
    juce::TemporaryFile temporary(target);
    if (! temporary.getFile().replaceWithData(data.getData(), data.getSize())
        || ! temporary.overwriteTargetFileWithTemporary())
    {
        currentUserPresetName_ = previousPresetName;
        error = "Could not write the preset safely.";
        return false;
    }
    return true;
}

void SwaraXtAudioProcessor::getStateInformation(juce::MemoryBlock& destData)
{
  #if JUCE_ANDROID
    juce::ignoreUnused(destData);
  #else
    auto state = apvts_.copyState();
    state.setProperty("stateVersion", kStateVersion, nullptr);
    state.setProperty("currentProgram", currentProgram_, nullptr);
    state.setProperty("presetKind", currentUserPresetName_.isEmpty() ? "factory" : "user", nullptr);
    state.setProperty("presetName", currentPresetName(), nullptr);
    if (auto xml = state.createXml())
        copyXmlToBinary(*xml, destData);
  #endif
}

void SwaraXtAudioProcessor::setStateInformation(const void* data, int sizeInBytes)
{
    if (auto xml = getXmlFromBinary(data, sizeInBytes))
    {
        auto tree = juce::ValueTree::fromXml(*xml);
        if (tree.isValid() && tree.hasType(kStateRoot))
        {
            const int version = static_cast<int>(tree.getProperty("stateVersion", 1));
            if (version > kStateVersion || version < 1)
                return;

            apvts_.replaceState(tree);
            currentProgram_ = juce::jlimit(0,
                                           getNumPrograms() - 1,
                                           static_cast<int>(tree.getProperty("currentProgram", 0)));
            currentUserPresetName_ = tree.getProperty("presetKind") == "user"
                ? tree.getProperty("presetName").toString().trim()
                : juce::String {};
            requestEngineReset_.store(true, std::memory_order_release);
        }
    }
}

juce::AudioProcessor* JUCE_CALLTYPE createPluginFilter()
{
    return new SwaraXtAudioProcessor();
}
