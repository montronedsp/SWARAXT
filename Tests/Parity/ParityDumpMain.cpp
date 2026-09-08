// Copyright 2026 MontroneDSP.
// SPDX-License-Identifier: GPL-3.0-or-later
// External classic+ vs main parity dump. Not a CTest target.

#include <JuceHeader.h>

#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <string>
#include <vector>

#include "Engine/SwaraXtEngine.h"
#include "Plugin/PluginProcessor.h"
#include "Plugin/SwaraXtParameterLayout.h"

#if !SWARAXT_ENABLE_SHRUTHI_DEBUG_TAPS
#error SWARAXT_ENABLE_SHRUTHI_DEBUG_TAPS must be enabled for parity dumps.
#endif

namespace {

struct LayerAccum {
    std::vector<float> osc1;
    std::vector<float> osc2;
    std::vector<float> mixer;
    std::vector<float> vcaTarget;
    std::vector<float> vcaGain;
    std::vector<float> filter;
    std::vector<float> postVca;
    std::vector<uint8_t> env1;
    std::vector<uint8_t> env2;
    std::vector<uint8_t> lfo1;
    std::vector<uint8_t> lfo2;
    std::vector<int16_t> vcaByte;
};

void tapSink(void* context, const swaraxt::SwaraXtEngine::DebugBlockCapture& capture)
{
    auto* layers = static_cast<LayerAccum*>(context);
    const int n = capture.samples;
    layers->osc1.insert(layers->osc1.end(), capture.rawOsc1, capture.rawOsc1 + n);
    layers->osc2.insert(layers->osc2.end(), capture.rawOsc2, capture.rawOsc2 + n);
    layers->mixer.insert(layers->mixer.end(), capture.postShruthiMixer, capture.postShruthiMixer + n);
    layers->vcaTarget.insert(layers->vcaTarget.end(), capture.vcaTarget, capture.vcaTarget + n);
    layers->vcaGain.insert(layers->vcaGain.end(), capture.vcaGain, capture.vcaGain + n);
    layers->filter.insert(layers->filter.end(), capture.filterOutput, capture.filterOutput + n);
    layers->postVca.insert(layers->postVca.end(), capture.postVca, capture.postVca + n);
    layers->env1.push_back(capture.env1);
    layers->env2.push_back(capture.env2);
    layers->lfo1.push_back(capture.lfo1);
    layers->lfo2.push_back(capture.lfo2);
    layers->vcaByte.push_back(capture.vca);
}

void setChoice(SwaraXtAudioProcessor& proc, const char* id, float index)
{
    if (auto* param = proc.getApvts().getParameter(id))
        param->setValueNotifyingHost(param->convertTo0to1(index));
}

void setFloat(SwaraXtAudioProcessor& proc, const char* id, float value)
{
    if (auto* param = proc.getApvts().getParameter(id))
        param->setValueNotifyingHost(param->convertTo0to1(value));
}

void writeF32(const juce::File& file, const std::vector<float>& data)
{
    file.getParentDirectory().createDirectory();
    std::ofstream out(file.getFullPathName().toStdString(), std::ios::binary);
    out.write(reinterpret_cast<const char*>(data.data()),
              static_cast<std::streamsize>(data.size() * sizeof(float)));
}

template <typename T>
void writeRaw(const juce::File& file, const std::vector<T>& data)
{
    file.getParentDirectory().createDirectory();
    std::ofstream out(file.getFullPathName().toStdString(), std::ios::binary);
    out.write(reinterpret_cast<const char*>(data.data()),
              static_cast<std::streamsize>(data.size() * sizeof(T)));
}

uint64_t hashBits(const std::vector<float>& data)
{
    uint64_t hash = 1469598103934665603ull;
    for (float sample : data)
    {
        uint32_t bits = 0;
        std::memcpy(&bits, &sample, sizeof(bits));
        hash ^= bits;
        hash *= 1099511628211ull;
    }
    return hash;
}

std::vector<float> renderNote(SwaraXtAudioProcessor& proc,
                              LayerAccum* layers,
                              double sampleRate,
                              int blockSize,
                              int midiNote,
                              int onBlocks,
                              int offBlocks)
{
    proc.prepareToPlay(sampleRate, blockSize);
    if (layers != nullptr)
        proc.engineForTests().setDebugTapSink(layers, &tapSink);

    std::vector<float> interleaved;
    juce::AudioBuffer<float> buffer(2, blockSize);
    const int totalBlocks = onBlocks + offBlocks;
    for (int block = 0; block < totalBlocks; ++block)
    {
        buffer.clear();
        juce::MidiBuffer midi;
        if (block == 0)
            midi.addEvent(juce::MidiMessage::noteOn(1, midiNote, static_cast<juce::uint8>(100)), 0);
        if (block == onBlocks)
            midi.addEvent(juce::MidiMessage::noteOff(1, midiNote), 0);
        proc.processBlock(buffer, midi);
        for (int i = 0; i < blockSize; ++i)
        {
            interleaved.push_back(buffer.getSample(0, i));
            interleaved.push_back(buffer.getSample(1, i));
        }
    }

    if (layers != nullptr)
        proc.engineForTests().setDebugTapSink(nullptr, nullptr);
    proc.releaseResources();
    return interleaved;
}

void dumpLayers(const juce::File& dir, const juce::String& name, const LayerAccum& layers)
{
    writeF32(dir.getChildFile(name + ".osc1.f32"), layers.osc1);
    writeF32(dir.getChildFile(name + ".osc2.f32"), layers.osc2);
    writeF32(dir.getChildFile(name + ".mixer.f32"), layers.mixer);
    writeF32(dir.getChildFile(name + ".vcaTarget.f32"), layers.vcaTarget);
    writeF32(dir.getChildFile(name + ".vcaGain.f32"), layers.vcaGain);
    writeF32(dir.getChildFile(name + ".filter.f32"), layers.filter);
    writeF32(dir.getChildFile(name + ".postVca.f32"), layers.postVca);
    writeRaw(dir.getChildFile(name + ".env1.u8"), layers.env1);
    writeRaw(dir.getChildFile(name + ".env2.u8"), layers.env2);
    writeRaw(dir.getChildFile(name + ".lfo1.u8"), layers.lfo1);
    writeRaw(dir.getChildFile(name + ".lfo2.u8"), layers.lfo2);
    writeRaw(dir.getChildFile(name + ".vcaByte.i16"), layers.vcaByte);
}

void dumpCase(const juce::File& dir,
              const juce::String& name,
              SwaraXtAudioProcessor& proc,
              double sampleRate,
              int blockSize,
              int midiNote,
              double onSeconds,
              double offSeconds,
              bool layers)
{
    LayerAccum accum;
    const int onBlocks = juce::jmax(1, static_cast<int>(std::lround(onSeconds * sampleRate / blockSize)));
    const int offBlocks = juce::jmax(1, static_cast<int>(std::lround(offSeconds * sampleRate / blockSize)));
    const auto audio = renderNote(proc, layers ? &accum : nullptr,
                                  sampleRate, blockSize, midiNote, onBlocks, offBlocks);
    writeF32(dir.getChildFile(name + ".f32"), audio);
    if (layers)
        dumpLayers(dir, name, accum);

    std::printf("%s samples=%zu hash=%016llx layers=%d sr=%.1f bs=%d note=%d\n",
                name.toRawUTF8(),
                audio.size(),
                static_cast<unsigned long long>(hashBits(audio)),
                layers ? 1 : 0,
                sampleRate,
                blockSize,
                midiNote);
}

}  // namespace

int main(int argc, char* argv[])
{
    const juce::File outDir = argc > 1
        ? juce::File(juce::String::fromUTF8(argv[1]))
        : juce::File::getCurrentWorkingDirectory().getChildFile("parity-dump");
    outDir.createDirectory();
    juce::ScopedJuceInitialiser_GUI juceInit;

    {
        SwaraXtAudioProcessor proc;
        dumpCase(outDir, "classic_raw_n60", proc, 48000.0, 64, 60, 1.0, 0.35, true);
    }
    {
        SwaraXtAudioProcessor proc;
        dumpCase(outDir, "classic_raw_n24", proc, 48000.0, 64, 24, 0.6, 0.25, true);
    }
    {
        SwaraXtAudioProcessor proc;
        dumpCase(outDir, "classic_raw_n96", proc, 48000.0, 64, 96, 0.6, 0.25, true);
    }
    {
        SwaraXtAudioProcessor proc;
        setChoice(proc, swaraxt::IDs::inputConditioning, 1.0f);
        dumpCase(outDir, "classic_hw_n60", proc, 48000.0, 64, 60, 1.0, 0.35, true);
    }
    {
        SwaraXtAudioProcessor proc;
        setFloat(proc, swaraxt::IDs::postMixer, -6.0f);
        dumpCase(outDir, "classic_pm6_n60", proc, 48000.0, 64, 60, 0.5, 0.2, false);
    }
    {
        SwaraXtAudioProcessor proc;
        setFloat(proc, swaraxt::IDs::postMixer, -12.0f);
        dumpCase(outDir, "classic_pm12_n60", proc, 48000.0, 64, 60, 0.5, 0.2, false);
    }
    {
        SwaraXtAudioProcessor proc;
        setFloat(proc, swaraxt::IDs::postMixer, -18.0f);
        dumpCase(outDir, "classic_pm18_n60", proc, 48000.0, 64, 60, 0.5, 0.2, false);
    }
    {
        SwaraXtAudioProcessor proc;
        setChoice(proc, swaraxt::IDs::filterModel, 1.0f);
        dumpCase(outDir, "board_raw_n60", proc, 48000.0, 64, 60, 1.0, 0.5, true);
    }
    {
        SwaraXtAudioProcessor proc;
        setChoice(proc, swaraxt::IDs::filterModel, 1.0f);
        setChoice(proc, swaraxt::IDs::inputConditioning, 1.0f);
        dumpCase(outDir, "board_hw_n60", proc, 48000.0, 64, 60, 1.0, 0.5, true);
    }
    {
        SwaraXtAudioProcessor proc;
        dumpCase(outDir, "classic_raw_long", proc, 48000.0, 64, 60, 8.0, 0.5, true);
    }

    const double rates[] = { 44100.0, 48000.0, 88200.0, 96000.0, 176400.0, 192000.0 };
    for (double rate : rates)
    {
        SwaraXtAudioProcessor proc;
        dumpCase(outDir, juce::String::formatted("classic_raw_sr_%d", static_cast<int>(rate)),
                 proc, rate, 64, 60, 0.4, 0.15, false);
    }

    const int blocks[] = { 1, 16, 32, 64, 127, 128, 256, 511, 512, 1024 };
    for (int block : blocks)
    {
        SwaraXtAudioProcessor proc;
        dumpCase(outDir, juce::String::formatted("classic_raw_bs_%d", block),
                 proc, 48000.0, block, 60, 0.25, 0.1, false);
    }

    SwaraXtAudioProcessor programs;
    const int presetCount = programs.getNumPrograms();
    for (int index = 0; index < presetCount; ++index)
    {
        SwaraXtAudioProcessor proc;
        proc.setCurrentProgram(index);
        const juce::String name = juce::String("preset_")
            + juce::String(index).paddedLeft('0', 2) + "_"
            + juce::File::createLegalFileName(proc.getProgramName(index));
        dumpCase(outDir, name, proc, 48000.0, 128, 48, 0.35, 0.15, false);
    }

    std::printf("preset_count=%d out=%s\n", presetCount, outDir.getFullPathName().toRawUTF8());
    return 0;
}
