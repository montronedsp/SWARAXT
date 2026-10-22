// SPDX-FileCopyrightText: 2026 MontroneDSP
// SPDX-License-Identifier: GPL-3.0-or-later
#include <JuceHeader.h>
#include <cmath>
#include <cstdio>
#include <memory>
#include <stdexcept>
#include "FrameworkProbes.h"
#include "NativeStdio.h"

namespace {
void require(bool result, const char* message)
{
    if (!result) throw std::runtime_error(message);
    std::printf("PASS: %s\n", message);
}

void pump(int milliseconds)
{
    juce::MessageManager::getInstance()->runDispatchLoopUntil(milliseconds);
}

void checkBooleanNormalization()
{
    juce::AudioParameterBool parameter(juce::ParameterID{"ci.boolean", 1}, "CI boolean", false);
    juce::AudioProcessorParameter& api = parameter;
    for (float value : {0.0f, 0.001f, 0.425637f, 0.499f, 0.5f, 0.9f, 1.0f})
    {
        api.setValueNotifyingHost(value);
        const bool expected = value >= 0.5f;
        if (api.getValue() != (expected ? 1.0f : 0.0f) || parameter.get() != expected)
            throw std::runtime_error("noncanonical boolean parameter normalization");
    }
    std::puts("BOOLEAN_NORMALIZATION_PASS");
}

void serviceLinuxEvents()
{
   #if JUCE_LINUX
    pump(20);
   #endif
}

void render(juce::AudioPluginInstance& plugin)
{
    plugin.prepareToPlay(48000.0, 256);
    juce::AudioBuffer<float> audio(juce::jmax(2, plugin.getTotalNumOutputChannels()), 256);
    juce::MidiBuffer midi;
    double energy = 0;
    for (int block = 0; block < 128; ++block)
    {
        audio.clear();
        midi.clear();
        if (block == 0) midi.addEvent(juce::MidiMessage::noteOn(1, 60, (juce::uint8)100), 0);
        if (block == 96) midi.addEvent(juce::MidiMessage::noteOff(1, 60), 17);
        plugin.processBlock(audio, midi);
        for (int channel = 0; channel < audio.getNumChannels(); ++channel)
            for (int sample = 0; sample < audio.getNumSamples(); ++sample)
            {
                const float value = audio.getSample(channel, sample);
                if (!std::isfinite(value)) throw std::runtime_error("nonfinite plugin audio");
                energy += (double)value * value;
            }
    }
    require(energy > 1.0e-8, "actual plugin renders audible finite MIDI audio");
    plugin.releaseResources();
    serviceLinuxEvents();
}

void editor(juce::AudioPluginInstance& plugin, const juce::File& screenshot,
            [[maybe_unused]] bool prematurePeerTeardown = false)
{
    const auto closeEditor = [&plugin](juce::AudioProcessorEditor* view)
    {
        plugin.editorBeingDeleted(view);
        delete view;
    };
    std::unique_ptr<juce::AudioProcessorEditor, decltype(closeEditor)> view(
        plugin.createEditorAndMakeActive(), closeEditor);
    require(view != nullptr, "actual plugin creates editor");
    require(plugin.getActiveEditor() == view.get(), "host tracks the active plugin editor");
    view->addToDesktop(juce::ComponentPeer::windowIsTemporary);
    view->setVisible(true);
    pump(400);
    require(view->getWidth() > 100 && view->getHeight() > 100, "editor has usable dimensions");
    auto image = view->createComponentSnapshot(view->getLocalBounds());
    require(image.isValid(), "actual plugin editor snapshot exists");
    if (screenshot != juce::File())
    {
        auto stream = screenshot.createOutputStream();
        require(stream != nullptr, "snapshot output writable");
        require(juce::PNGImageFormat().writeImageToStream(image, *stream), "editor PNG saved");
    }
    view->setVisible(false);
   #if JUCE_LINUX
    // The VST3 editor must detach its foreign child before Component's base
    // destructor removes the native parent peer. Keep the broken order only
    // as an explicit negative-control mode.
    if (prematurePeerTeardown)
    {
        std::puts("DELIBERATE_PREMATURE_PEER_TEARDOWN");
        view->removeFromDesktop();
    }
   #else
    view->removeFromDesktop();
   #endif
    view.reset();
    require(plugin.getActiveEditor() == nullptr, "host clears the active editor before deletion");
    serviceLinuxEvents();
}
}

int main(int argc, char** argv)
{
    try
    {
        if (!prepareNativeOutput()) throw std::runtime_error("native stdout setup failed");
       #if JUCE_LINUX
        if (argc == 2 && juce::String(argv[1]) == "--self-test-framework")
        {
            juce::ScopedJuceInitialiser_GUI gui;
            return runNativeFrameworkProbes();
        }
       #endif
        bool prematurePeerTeardown = false;
       #if JUCE_LINUX
        prematurePeerTeardown = argc == 5 && juce::String(argv[4]) == "--premature-peer-teardown";
       #endif
        require(argc == 4 || prematurePeerTeardown, "arguments: format plugin-path screenshot-path");
        checkBooleanNormalization();
        juce::ScopedJuceInitialiser_GUI gui;
        juce::AudioPluginFormatManager manager;
        if (juce::String(argv[1]) == "VST3") manager.addFormat(std::make_unique<juce::VST3PluginFormat>());

        else throw std::runtime_error("unsupported format");
        juce::OwnedArray<juce::PluginDescription> types;
        manager.getFormat(0)->findAllTypesForFile(types, argv[2]);
        require(types.size() == 1, "exactly one actual plugin type discovered");
        juce::String error;
        auto first = manager.createPluginInstance(*types[0], 48000.0, 256, error);
        if (!first) throw std::runtime_error(error.toStdString());
        serviceLinuxEvents();
        auto second = manager.createPluginInstance(*types[0], 48000.0, 256, error);
        if (!second) throw std::runtime_error(error.toStdString());
        serviceLinuxEvents();
        require(first->getParameters().size() > 0, "actual plugin exposes parameters");
        juce::MemoryBlock state;
        first->getStateInformation(state);
        require(state.getSize() > 0, "actual plugin serializes state");
        second->setStateInformation(state.getData(), (int)state.getSize());
        serviceLinuxEvents();
        render(*first);
        render(*second);
        editor(*first, juce::File(argv[3]), prematurePeerTeardown);
        editor(*second, {});
        first.reset();
        pump(200);
        render(*second);
        editor(*second, {});
        first = manager.createPluginInstance(*types[0], 48000.0, 256, error);
        require(first != nullptr, "first instance reopens after destruction");
        serviceLinuxEvents();
        editor(*first, {});
        first.reset();
        second.reset();
        pump(200);
        std::puts("PASS: actual plugin two-instance audio/editor/state/reopen lifecycle");
        return 0;
    }
    catch (const std::exception& error)
    {
        std::fprintf(stderr, "FAIL: %s\n", error.what());
        return 1;
    }
}
