// Copyright 2026 MontroneDSP. SPDX-License-Identifier: GPL-3.0-or-later
#include <JuceHeader.h>
#include "Plugin/PluginProcessor.h"
#include "Engine/Filter/Smr4InputCoupling.h"
#include "Engine/DspBoard/BoardInputModel.h"
#include "Engine/DspBoard/BoardResponse.h"
#include <algorithm>
#include <cmath>
#include <complex>
#include <iostream>
#include <stdexcept>
#include <vector>

namespace {
using namespace swaraxt;
void require(bool ok, const char* why) { if (!ok) throw std::runtime_error(why); }
void set(SwaraXtAudioProcessor& p, const char* id, float value)
{
    auto* parameter = p.getApvts().getParameter(id);
    require(parameter != nullptr, "parameter exists");
    parameter->setValueNotifyingHost(parameter->convertTo0to1(value));
}
float get(const SwaraXtAudioProcessor& p, const char* id)
{
    return p.getApvts().getRawParameterValue(id)->load();
}
struct Capture {
    std::vector<float> mixer, filter, host;
};
void sink(void* context, const SwaraXtEngine::DebugBlockCapture& block)
{
    auto* capture = static_cast<Capture*>(context);
    for (int i = 0; i < block.samples; ++i)
    {
        capture->mixer.push_back(block.postShruthiMixer[i]);
        capture->filter.push_back(block.filterOutput[i]);
    }
}
void process(SwaraXtAudioProcessor& p, int samples, bool note)
{
    juce::AudioBuffer<float> audio(2, samples);
    juce::MidiBuffer midi;
    if (note) midi.addEvent(juce::MidiMessage::noteOn(1, 60, juce::uint8(100)), 0);
    p.processBlock(audio, midi);
}
std::vector<float> renderHost(SwaraXtAudioProcessor& p, int blocks, int blockSize, bool noteOnFirst)
{
    std::vector<float> host;
    for (int i = 0; i < blocks; ++i)
    {
        juce::AudioBuffer<float> audio(2, blockSize);
        juce::MidiBuffer midi;
        if (noteOnFirst && i == 0)
            midi.addEvent(juce::MidiMessage::noteOn(1, 69, juce::uint8(110)), 0);
        p.processBlock(audio, midi);
        host.insert(host.end(), audio.getReadPointer(0), audio.getReadPointer(0) + blockSize);
        for (int s = 0; s < blockSize; ++s)
            require(std::isfinite(audio.getSample(0, s)) && std::abs(audio.getSample(0, s)) < 8.f,
                    "finite host");
    }
    return host;
}
double rms(const std::vector<float>& x, int skip)
{
    double sum = 0;
    int n = 0;
    for (int i = skip; i < static_cast<int>(x.size()); ++i)
    {
        sum += static_cast<double>(x[static_cast<size_t>(i)]) * x[static_cast<size_t>(i)];
        ++n;
    }
    return n > 0 ? std::sqrt(sum / n) : 0.0;
}
double mean(const std::vector<float>& x, int skip)
{
    double sum = 0;
    int n = 0;
    for (int i = skip; i < static_cast<int>(x.size()); ++i)
    {
        sum += x[static_cast<size_t>(i)];
        ++n;
    }
    return n > 0 ? sum / n : 0.0;
}

void parameterTests()
{
    SwaraXtAudioProcessor p;
    auto* post = dynamic_cast<juce::AudioParameterFloat*>(p.getApvts().getParameter(IDs::postMixer));
    auto* cond = dynamic_cast<juce::AudioParameterChoice*>(p.getApvts().getParameter(IDs::inputConditioning));
    require(post != nullptr && cond != nullptr, "new parameters exist");
    require(post->getNormalisableRange().start == -18.0f && post->getNormalisableRange().end == 0.0f,
            "Post Mixer range -18..0");
    require(post->get() == 0.0f, "default Post Mixer 0 dB");
    require(post->convertFrom0to1(1.0f) == 0.0f, "Post Mixer cannot boost");
    require(post->convertFrom0to1(0.0f) == -18.0f, "Post Mixer min is -18 dB");
    require(cond->getIndex() == 0 && cond->choices[0] == "RAW" && cond->choices[1] == "HARDWARE",
            "Input Conditioning default RAW");
    const float six = std::pow(10.0f, -6.0f / 20.0f);
    const float twelve = std::pow(10.0f, -12.0f / 20.0f);
    const float eighteen = std::pow(10.0f, -18.0f / 20.0f);
    require(std::abs(six - 0.501187f) < 1.0e-5f, "-6 dB linear");
    require(std::abs(twelve - 0.251189f) < 1.0e-5f, "-12 dB linear");
    require(std::abs(eighteen - 0.125893f) < 1.0e-5f, "-18 dB linear");
    std::cout << "Post Mixer/Input Conditioning parameter domain PASS\n";
}

void classicPostMixerAndConditioning()
{
    SwaraXtAudioProcessor unity;
    set(unity, IDs::postMixer, 0.0f);
    set(unity, IDs::inputConditioning, 0.0f);
    unity.prepareToPlay(48000, 256);
    const auto a = renderHost(unity, 40, 256, true);

    SwaraXtAudioProcessor six;
    set(six, IDs::postMixer, -6.0f);
    set(six, IDs::inputConditioning, 0.0f);
    six.prepareToPlay(48000, 256);
    const auto b = renderHost(six, 40, 256, true);
    const double ratio = rms(b, 2048) / std::max(1.0e-9, rms(a, 2048));
    require(std::abs(ratio - std::pow(10.0, -6.0 / 20.0)) < 0.08, "Classic -6 dB Post Mixer ratio");

    SwaraXtAudioProcessor min;
    set(min, IDs::postMixer, -18.0f);
    min.prepareToPlay(48000, 256);
    const auto c = renderHost(min, 40, 256, true);
    const double minRatio = rms(c, 2048) / std::max(1.0e-9, rms(a, 2048));
    require(std::abs(minRatio - std::pow(10.0, -18.0 / 20.0)) < 0.08, "Classic -18 dB Post Mixer ratio");

    SwaraXtAudioProcessor hardware;
    set(hardware, IDs::postMixer, 0.0f);
    set(hardware, IDs::inputConditioning, 1.0f);
    Capture cap;
    hardware.engineForTests().setDebugTapSink(&cap, sink);
    hardware.prepareToPlay(48000, 256);
    const auto hwHost = renderHost(hardware, 40, 256, true);
    require(mean(hwHost, static_cast<int>(hwHost.size()) - 2048) < 0.05, "Classic HARDWARE host DC");
    require(cap.filter.size() > 100 && cap.mixer.size() == cap.filter.size(), "Classic taps recorded");
    // SMR4 coupling is a ~0.34 Hz HPF, not a 16 kHz Chebyshev. Mixer and
    // pre-VCA filter remain close at musical frequencies after settling.
    double mixerRms = 0, filterRms = 0;
    const int skip = static_cast<int>(cap.mixer.size() / 4);
    for (int i = skip; i < static_cast<int>(cap.mixer.size()); ++i)
    {
        mixerRms += cap.mixer[static_cast<size_t>(i)] * cap.mixer[static_cast<size_t>(i)];
        filterRms += cap.filter[static_cast<size_t>(i)] * cap.filter[static_cast<size_t>(i)];
    }
    mixerRms = std::sqrt(mixerRms / std::max(1, static_cast<int>(cap.mixer.size()) - skip));
    filterRms = std::sqrt(filterRms / std::max(1, static_cast<int>(cap.filter.size()) - skip));
    require(mixerRms > 1.0e-4 && filterRms / mixerRms > 0.2 && filterRms / mixerRms < 5.0,
            "Classic HARDWARE is not a steep DSP-Board Chebyshev");
    std::cout << "Classic Post Mixer/HARDWARE PASS ratio-6=" << ratio
              << " filter/mixer=" << (filterRms / mixerRms) << '\n';
}

void boardPostMixerAndConditioning()
{
    SwaraXtAudioProcessor unity;
    set(unity, IDs::filterModel, 1);
    set(unity, IDs::postMixer, 0.0f);
    set(unity, IDs::inputConditioning, 0.0f);
    unity.prepareToPlay(48000, 256);
    const auto raw = renderHost(unity, 48, 256, true);

    SwaraXtAudioProcessor att;
    set(att, IDs::filterModel, 1);
    set(att, IDs::postMixer, -12.0f);
    set(att, IDs::inputConditioning, 0.0f);
    att.prepareToPlay(48000, 256);
    const auto quiet = renderHost(att, 48, 256, true);
    const double ratio = rms(quiet, 2048) / std::max(1.0e-9, rms(raw, 2048));
    require(std::abs(ratio - std::pow(10.0, -12.0 / 20.0)) < 0.12, "Board -12 dB Post Mixer ratio");

    SwaraXtAudioProcessor hardware;
    set(hardware, IDs::filterModel, 1);
    set(hardware, IDs::postMixer, 0.0f);
    set(hardware, IDs::inputConditioning, 1.0f);
    hardware.prepareToPlay(48000, 256);
    const auto hw = renderHost(hardware, 48, 256, true);
    require(rms(hw, 2048) > 1.0e-5, "Board HARDWARE still produces audio");
    require(std::abs(rms(hw, 2048) - rms(raw, 2048)) > 1.0e-4,
            "Board RAW and HARDWARE are not the same path");
    std::cout << "Board Post Mixer/RAW vs HARDWARE PASS ratio-12=" << ratio
              << " rawRms=" << rms(raw, 2048) << " hwRms=" << rms(hw, 2048) << '\n';
}

void smr4CouplingAndBoardAnalog()
{
    require(std::abs(Smr4InputCoupling::kCutoffHz - 0.338627) < 1.0e-4, "SMR4 C=4.7u R=100k cutoff");
    Smr4InputCoupling coupling;
    coupling.prepare(20000000.0 / 510.0);
    double dc = 0;
    for (int i = 0; i < 200000; ++i)
        dc = coupling.process(1.0f);
    require(std::abs(dc) < 0.01, "SMR4 input coupling rejects DC");
    coupling.reset();
    const double w = 2.0 * 3.14159265358979323846 * 1000.0 / (20000000.0 / 510.0);
    double peak = 0;
    for (int i = 0; i < 8000; ++i)
        peak = std::max(peak, std::abs(static_cast<double>(
            coupling.process(static_cast<float>(std::sin(w * i))))));
    require(peak > 0.85, "SMR4 coupling is near-unity at 1 kHz");

    const double analog16k = std::abs( [] {
        using namespace board;
        const auto z = std::exp(std::complex<double>(0.0, -2 * 3.14159265358979323846 * 18000.0 / sampleRate));
        std::complex<double> h(1, 0);
        for (const auto& c : inputSos)
            h *= (c[0] + c[1] * z + c[2] * z * z) / (1.0 + c[4] * z + c[5] * z * z);
        return h;
    }());
    const double analog1k = std::abs( [] {
        using namespace board;
        const auto z = std::exp(std::complex<double>(0.0, -2 * 3.14159265358979323846 * 1000.0 / sampleRate));
        std::complex<double> h(1, 0);
        for (const auto& c : inputSos)
            h *= (c[0] + c[1] * z + c[2] * z * z) / (1.0 + c[4] * z + c[5] * z * z);
        return h;
    }());
    require(analog16k / analog1k < 0.25, "DSP Board HARDWARE SOS rejects 16 kHz");
    std::cout << "SMR4 coupling and Board SOS character PASS 16k/1k=" << (analog16k / analog1k) << '\n';
}

void stateAndSwitching()
{
    SwaraXtAudioProcessor p;
    set(p, IDs::postMixer, -9.0f);
    set(p, IDs::inputConditioning, 1.0f);
    p.prepareToPlay(48000, 256);
    juce::MemoryBlock bytes;
    p.getStateInformation(bytes);
    auto xml = juce::AudioProcessor::getXmlFromBinary(bytes.getData(), static_cast<int>(bytes.getSize()));
    auto tree = juce::ValueTree::fromXml(*xml);
    tree.removeChild(tree.getChildWithProperty("id", IDs::postMixer), nullptr);
    tree.removeChild(tree.getChildWithProperty("id", IDs::inputConditioning), nullptr);
    juce::MemoryBlock old;
    juce::AudioProcessor::copyXmlToBinary(*tree.createXml(), old);
    SwaraXtAudioProcessor restored;
    restored.setStateInformation(old.getData(), static_cast<int>(old.getSize()));
    require(std::abs(get(restored, IDs::postMixer)) < 1.0e-4f, "old state resolves Post Mixer 0 dB");
    require(get(restored, IDs::inputConditioning) == 0.0f, "old state resolves RAW");

    SwaraXtAudioProcessor live;
    live.prepareToPlay(48000, 64);
    for (int i = 0; i < 20; ++i)
    {
        set(live, IDs::inputConditioning, float(i % 2));
        set(live, IDs::postMixer, i % 2 ? -6.0f : 0.0f);
        set(live, IDs::filterModel, float(i % 2));
        process(live, 64, i == 0);
    }
    std::cout << "State restore and RAW/HARDWARE switching PASS\n";
}

void rates()
{
    for (double rate : { 44100.0, 48000.0, 88200.0, 96000.0, 176400.0, 192000.0 })
    {
        SwaraXtAudioProcessor p;
        set(p, IDs::inputConditioning, 1.0f);
        p.prepareToPlay(rate, 128);
        auto host = renderHost(p, 12, 128, true);
        require(rms(host, 256) >= 0.0, "hardware mode renders at host rate");
    }
    std::cout << "Host-rate HARDWARE render PASS\n";
}
}

int main()
{
    juce::ScopedJuceInitialiser_GUI gui;
    try
    {
        parameterTests();
        smr4CouplingAndBoardAnalog();
        classicPostMixerAndConditioning();
        boardPostMixerAndConditioning();
        stateAndSwitching();
        rates();
    }
    catch (const std::exception& e)
    {
        std::cerr << "FAIL: " << e.what() << '\n';
        return 1;
    }
    std::cout << "SwaraXt conditioning tests PASSED\n";
    return 0;
}
