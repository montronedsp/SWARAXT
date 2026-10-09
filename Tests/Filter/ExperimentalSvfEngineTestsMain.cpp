// Copyright 2026 MontroneDSP.
// SPDX-License-Identifier: GPL-3.0-or-later
// Experimental SVF through the live engine: host rates, blocks, Classic default.

#include <JuceHeader.h>

#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <string>
#include <thread>
#include <vector>

#include "Plugin/PluginProcessor.h"

namespace {

int gFailures = 0;

void expect(bool ok, const char* name)
{
    if (! ok)
    {
        std::fprintf(stderr, "FAIL: %s\n", name);
        ++gFailures;
        return;
    }
    std::printf("PASS: %s\n", name);
}

bool bufferFinite(const juce::AudioBuffer<float>& buffer)
{
    for (int ch = 0; ch < buffer.getNumChannels(); ++ch)
        for (int i = 0; i < buffer.getNumSamples(); ++i)
            if (! std::isfinite(buffer.getSample(ch, i)))
                return false;
    return true;
}

float bufferPeak(const juce::AudioBuffer<float>& buffer)
{
    float peak = 0.0f;
    for (int ch = 0; ch < buffer.getNumChannels(); ++ch)
        for (int i = 0; i < buffer.getNumSamples(); ++i)
            peak = std::max(peak, std::fabs(buffer.getSample(ch, i)));
    return peak;
}

void runMatrix(bool experimental)
{
    const double rates[] = { 44100.0, 48000.0, 88200.0, 96000.0, 176400.0, 192000.0 };
    const int blocks[] = { 1, 2, 3, 7, 16, 31, 32, 40, 41, 63, 64, 127, 128, 255, 256, 511, 512, 1023, 1024 };
    for (double rate : rates)
    {
        for (int bs : blocks)
        {
            SwaraXtAudioProcessor proc;
            proc.prepareToPlay(rate, bs);
            proc.engineForTests().setExperimentalSvfEnabledForTests(experimental);
            proc.engineForTests().setExperimentalSvfRoutingForTests(swaraxt::ShruthiSvfDualRouting::section1);
            proc.engineForTests().setExperimentalSvfModeForTests(swaraxt::ShruthiSvf2pMode::lowPass);
            proc.engineForTests().setExperimentalSvfMode2ForTests(swaraxt::ShruthiSvf2pMode::lowPass);
            juce::AudioBuffer<float> buffer(2, bs);
            float peak = 0.0f;
            const int iterations = bs < 32 ? 256 : 48;
            for (int b = 0; b < iterations; ++b)
            {
                buffer.clear();
                juce::MidiBuffer midi;
                if (b == 0)
                    midi.addEvent(juce::MidiMessage::noteOn(1, 40, static_cast<juce::uint8>(100)), 0);
                if (b == iterations / 3)
                {
                    proc.engineForTests().setExperimentalSvfRoutingForTests(
                        swaraxt::ShruthiSvfDualRouting::serial);
                    proc.engineForTests().setExperimentalSvfMode2ForTests(
                        swaraxt::ShruthiSvf2pMode::bandPass);
                    midi.addEvent(juce::MidiMessage::controllerEvent(1, 74, 90), 0);
                }
                if (b == (2 * iterations) / 3)
                {
                    proc.engineForTests().setExperimentalSvfRoutingForTests(
                        swaraxt::ShruthiSvfDualRouting::parallel);
                    proc.engineForTests().setExperimentalSvfModeForTests(
                        swaraxt::ShruthiSvf2pMode::highPass);
                    proc.engineForTests().setExperimentalSvfMode2ForTests(
                        swaraxt::ShruthiSvf2pMode::lowPass);
                    proc.engineForTests().setExperimentalSvfCalibrationForTests(
                        swaraxt::ShruthiSvfCalibration::eagleHardware);
                    midi.addEvent(juce::MidiMessage::controllerEvent(1, 71, 110), 0);
                }
                if (b == iterations - 2)
                    midi.addEvent(juce::MidiMessage::noteOff(1, 40), 0);
                proc.processBlock(buffer, midi);
                if (! bufferFinite(buffer))
                {
                    expect(false, "host matrix finite");
                    return;
                }
                peak = std::max(peak, bufferPeak(buffer));
            }
            if (experimental && rate == 48000.0 && bs == 64)
                expect(peak > 1.0e-5f, "experimental SVF produces audio at 48 kHz/64");
        }
    }
    expect(true, experimental ? "experimental host-rate/block matrix finite" : "Classic default host matrix finite");
}

void testConcurrentPublication()
{
    SwaraXtAudioProcessor proc;
    proc.prepareToPlay(48000.0, 64);
    proc.setExperimentalSvfEnabled(true);
    std::atomic<bool> stop { false };
    std::atomic<int> bad { 0 };
    std::thread audio([&] {
        juce::AudioBuffer<float> buffer(2, 64);
        for (int b = 0; b < 400 && ! stop.load(std::memory_order_relaxed); ++b)
        {
            buffer.clear();
            juce::MidiBuffer midi;
            if (b == 0)
                midi.addEvent(juce::MidiMessage::noteOn(1, 40, static_cast<juce::uint8>(100)), 0);
            proc.processBlock(buffer, midi);
            if (! bufferFinite(buffer))
                bad.fetch_add(1, std::memory_order_relaxed);
        }
    });
    std::thread gui([&] {
        for (int i = 0; i < 8000; ++i)
        {
            proc.setExperimentalSvfEnabled((i & 7) != 0);
            proc.setExperimentalSvfMode1(static_cast<swaraxt::ShruthiSvf2pMode>(i % 3));
            proc.setExperimentalSvfMode2(static_cast<swaraxt::ShruthiSvf2pMode>((i / 2) % 3));
            proc.setExperimentalSvfRouting(static_cast<swaraxt::ShruthiSvfDualRouting>(i % 3));
            proc.setExperimentalSvfCalibration(static_cast<swaraxt::ShruthiSvfCalibration>(i & 1));
        }
        stop.store(true, std::memory_order_relaxed);
    });
    gui.join();
    audio.join();
    expect(bad.load() == 0, "concurrent publication during processBlock stays finite");
}

}  // namespace

void writeWavFloat32(const std::filesystem::path& path, const std::vector<float>& samples, double rate)
{
    std::filesystem::create_directories(path.parent_path());
    const auto count = static_cast<std::uint32_t>(samples.size());
    const std::uint32_t dataBytes = count * 4;
    std::ofstream out(path, std::ios::binary);
    out.write("RIFF", 4);
    const std::uint32_t riffSize = 36 + dataBytes;
    out.write(reinterpret_cast<const char*>(&riffSize), 4);
    out.write("WAVEfmt ", 8);
    const std::uint32_t fmtSize = 16;
    const std::uint16_t audioFormat = 3, channels = 1, bits = 32;
    const std::uint32_t byteRate = static_cast<std::uint32_t>(rate) * 4;
    const std::uint16_t blockAlign = 4;
    out.write(reinterpret_cast<const char*>(&fmtSize), 4);
    out.write(reinterpret_cast<const char*>(&audioFormat), 2);
    out.write(reinterpret_cast<const char*>(&channels), 2);
    const auto sr = static_cast<std::uint32_t>(rate);
    out.write(reinterpret_cast<const char*>(&sr), 4);
    out.write(reinterpret_cast<const char*>(&byteRate), 4);
    out.write(reinterpret_cast<const char*>(&blockAlign), 2);
    out.write(reinterpret_cast<const char*>(&bits), 2);
    out.write("data", 4);
    out.write(reinterpret_cast<const char*>(&dataBytes), 4);
    out.write(reinterpret_cast<const char*>(samples.data()), static_cast<std::streamsize>(dataBytes));
}

void writeWav16(const std::filesystem::path& path, const std::vector<float>& samples, double rate)
{
    std::filesystem::create_directories(path.parent_path());
    const auto count = static_cast<std::uint32_t>(samples.size());
    const std::uint32_t dataBytes = count * 2;
    std::ofstream out(path, std::ios::binary);
    out.write("RIFF", 4);
    const std::uint32_t riffSize = 36 + dataBytes;
    out.write(reinterpret_cast<const char*>(&riffSize), 4);
    out.write("WAVEfmt ", 8);
    const std::uint32_t fmtSize = 16;
    const std::uint16_t audioFormat = 1, channels = 1, bits = 16;
    const std::uint32_t byteRate = static_cast<std::uint32_t>(rate) * 2;
    const std::uint16_t blockAlign = 2;
    out.write(reinterpret_cast<const char*>(&fmtSize), 4);
    out.write(reinterpret_cast<const char*>(&audioFormat), 2);
    out.write(reinterpret_cast<const char*>(&channels), 2);
    const auto sr = static_cast<std::uint32_t>(rate);
    out.write(reinterpret_cast<const char*>(&sr), 4);
    out.write(reinterpret_cast<const char*>(&byteRate), 4);
    out.write(reinterpret_cast<const char*>(&blockAlign), 2);
    out.write(reinterpret_cast<const char*>(&bits), 2);
    out.write("data", 4);
    out.write(reinterpret_cast<const char*>(&dataBytes), 4);
    for (float s : samples)
    {
        const float clipped = std::max(-1.0f, std::min(1.0f, s));
        const auto pcm = static_cast<std::int16_t>(clipped * 32767.0f);
        out.write(reinterpret_cast<const char*>(&pcm), 2);
    }
}

void renderEvidence()
{
    const double rate = 48000.0;
    const int bs = 64;
    const auto dir = std::filesystem::current_path() / "artifacts" / "svf2p-evidence";
    const swaraxt::ShruthiSvf2pMode modes[] = {
        swaraxt::ShruthiSvf2pMode::lowPass,
        swaraxt::ShruthiSvf2pMode::lowPass,
        swaraxt::ShruthiSvf2pMode::lowPass,
        swaraxt::ShruthiSvf2pMode::bandPass,
        swaraxt::ShruthiSvf2pMode::highPass
    };
    const char* names[] = {
        "48k_lp_low_res.wav",
        "48k_lp_high_res.wav",
        "48k_lp_selfosc.wav",
        "48k_bp.wav",
        "48k_hp.wav"
    };
    const float resonances[] = { 0.05f, 0.7f, 1.0f, 0.35f, 0.2f };
    for (int n = 0; n < 5; ++n)
    {
        SwaraXtAudioProcessor proc;
        proc.prepareToPlay(rate, bs);
        proc.engineForTests().setExperimentalSvfEnabledForTests(true);
        proc.engineForTests().setExperimentalSvfModeForTests(modes[n]);
        if (auto* resParam = proc.getApvts().getParameter("filter_resonance"))
            resParam->setValueNotifyingHost(resParam->convertTo0to1(resonances[n]));
        std::vector<float> audio;
        audio.reserve(static_cast<size_t>(rate));
        juce::AudioBuffer<float> buffer(2, bs);
        const int blocks = static_cast<int>(rate) / bs;
        for (int b = 0; b < blocks; ++b)
        {
            buffer.clear();
            juce::MidiBuffer midi;
            if (b == 0)
                midi.addEvent(juce::MidiMessage::noteOn(1, 36, static_cast<juce::uint8>(110)), 0);
            if (b == blocks - 8)
                midi.addEvent(juce::MidiMessage::noteOff(1, 36), 0);
            proc.processBlock(buffer, midi);
            for (int i = 0; i < bs; ++i)
                audio.push_back(buffer.getSample(0, i));
        }
        writeWav16(dir / names[n], audio, rate);
        float peak = 0.0f;
        for (float s : audio)
            peak = std::max(peak, std::fabs(s));
        std::printf("wrote %s peak=%.4f\n", names[n], peak);
    }
    expect(true, "48 kHz experimental evidence wavs written");
}

std::vector<float> renderHostPatch(swaraxt::ShruthiSvfDualRouting routing,
                                   swaraxt::ShruthiSvf2pMode mode1,
                                   swaraxt::ShruthiSvf2pMode mode2,
                                   float resonance,
                                   double rate, int bs,
                                   swaraxt::ShruthiSvfCalibration calibration = swaraxt::ShruthiSvfCalibration::paperApproved)
{
    SwaraXtAudioProcessor proc;
    proc.prepareToPlay(rate, bs);
    proc.prepareToPlay(rate, bs);
    proc.engineForTests().setExperimentalSvfEnabledForTests(true);
    proc.engineForTests().setExperimentalSvfCalibrationForTests(calibration);
    proc.engineForTests().setExperimentalSvfRoutingForTests(routing);
    proc.engineForTests().setExperimentalSvfModeForTests(mode1);
    proc.engineForTests().setExperimentalSvfMode2ForTests(mode2);
    if (auto* resParam = proc.getApvts().getParameter("filter_resonance"))
        resParam->setValueNotifyingHost(resParam->convertTo0to1(resonance));
    std::vector<float> audio;
    audio.reserve(static_cast<size_t>(rate));
    juce::AudioBuffer<float> buffer(2, bs);
    const int blocks = static_cast<int>(rate) / bs;
    for (int b = 0; b < blocks; ++b)
    {
        buffer.clear();
        juce::MidiBuffer midi;
        if (b == 0)
            midi.addEvent(juce::MidiMessage::noteOn(1, 36, static_cast<juce::uint8>(110)), 0);
        if (b == blocks - 8)
            midi.addEvent(juce::MidiMessage::noteOff(1, 36), 0);
        proc.processBlock(buffer, midi);
        for (int i = 0; i < bs; ++i)
            audio.push_back(buffer.getSample(0, i));
    }
    int invalid = 0;
    float peak = 0.0f;
    for (float s : audio)
    {
        if (! std::isfinite(s))
            ++invalid;
        peak = std::max(peak, std::fabs(s));
    }
    expect(invalid == 0, "prototype 2 render finite");
    (void) peak;
    return audio;
}

void renderPrototype2Evidence()
{
    const double rate = 48000.0;
    const int bs = 64;
    const auto dir = std::filesystem::current_path() / "artifacts" / "svf2p-prototype2-evidence";
    struct Job
    {
        const char* name;
        swaraxt::ShruthiSvfDualRouting routing;
        swaraxt::ShruthiSvf2pMode mode1;
        swaraxt::ShruthiSvf2pMode mode2;
        float resonance;
    };
    const Job jobs[] = {
        { "s1_lp_low_res.wav", swaraxt::ShruthiSvfDualRouting::section1,
          swaraxt::ShruthiSvf2pMode::lowPass, swaraxt::ShruthiSvf2pMode::lowPass, 0.05f },
        { "s1_lp_high_res.wav", swaraxt::ShruthiSvfDualRouting::section1,
          swaraxt::ShruthiSvf2pMode::lowPass, swaraxt::ShruthiSvf2pMode::lowPass, 0.7f },
        { "s1_lp_selfosc.wav", swaraxt::ShruthiSvfDualRouting::section1,
          swaraxt::ShruthiSvf2pMode::lowPass, swaraxt::ShruthiSvf2pMode::lowPass, 1.0f },
        { "s2_lp.wav", swaraxt::ShruthiSvfDualRouting::serial,
          swaraxt::ShruthiSvf2pMode::lowPass, swaraxt::ShruthiSvf2pMode::lowPass, 0.05f },
        { "s2_bp.wav", swaraxt::ShruthiSvfDualRouting::serial,
          swaraxt::ShruthiSvf2pMode::lowPass, swaraxt::ShruthiSvf2pMode::bandPass, 0.2f },
        { "s2_hp.wav", swaraxt::ShruthiSvfDualRouting::serial,
          swaraxt::ShruthiSvf2pMode::lowPass, swaraxt::ShruthiSvf2pMode::highPass, 0.2f },
        { "s2_high_res.wav", swaraxt::ShruthiSvfDualRouting::serial,
          swaraxt::ShruthiSvf2pMode::lowPass, swaraxt::ShruthiSvf2pMode::lowPass, 0.7f },
        { "serial_lp_lp.wav", swaraxt::ShruthiSvfDualRouting::serial,
          swaraxt::ShruthiSvf2pMode::lowPass, swaraxt::ShruthiSvf2pMode::lowPass, 0.35f },
        { "serial_lp_bp.wav", swaraxt::ShruthiSvfDualRouting::serial,
          swaraxt::ShruthiSvf2pMode::lowPass, swaraxt::ShruthiSvf2pMode::bandPass, 0.35f },
        { "serial_bp_lp.wav", swaraxt::ShruthiSvfDualRouting::serial,
          swaraxt::ShruthiSvf2pMode::bandPass, swaraxt::ShruthiSvf2pMode::lowPass, 0.35f },
        { "serial_hp_lp.wav", swaraxt::ShruthiSvfDualRouting::serial,
          swaraxt::ShruthiSvf2pMode::highPass, swaraxt::ShruthiSvf2pMode::lowPass, 0.35f },
        { "parallel_lp_hp.wav", swaraxt::ShruthiSvfDualRouting::parallel,
          swaraxt::ShruthiSvf2pMode::lowPass, swaraxt::ShruthiSvf2pMode::highPass, 0.35f },
        { "parallel_lp_bp.wav", swaraxt::ShruthiSvfDualRouting::parallel,
          swaraxt::ShruthiSvf2pMode::lowPass, swaraxt::ShruthiSvf2pMode::bandPass, 0.35f },
        { "parallel_bp_hp.wav", swaraxt::ShruthiSvfDualRouting::parallel,
          swaraxt::ShruthiSvf2pMode::bandPass, swaraxt::ShruthiSvf2pMode::highPass, 0.35f },
    };
    for (const auto& job : jobs)
    {
        const auto audio = renderHostPatch(job.routing, job.mode1, job.mode2, job.resonance, rate, bs);
        writeWavFloat32(dir / job.name, audio, rate);
        float peak = 0.0f;
        for (float s : audio)
            peak = std::max(peak, std::fabs(s));
        std::printf("wrote prototype2 %s peak=%.4f\n", job.name, peak);
    }
    expect(true, "prototype 2 evidence wavs written");
}

void renderPrototype3AbEvidence()
{
    const double rate = 48000.0;
    const int bs = 64;
    const auto dir = std::filesystem::current_path() / "artifacts" / "svf2p-prototype3-evidence";
    struct Job
    {
        const char* stem;
        swaraxt::ShruthiSvfDualRouting routing;
        swaraxt::ShruthiSvf2pMode mode1;
        swaraxt::ShruthiSvf2pMode mode2;
        float resonance;
    };
    const Job jobs[] = {
        { "s1_lp_low_res", swaraxt::ShruthiSvfDualRouting::section1,
          swaraxt::ShruthiSvf2pMode::lowPass, swaraxt::ShruthiSvf2pMode::lowPass, 0.05f },
        { "s1_lp_med_res", swaraxt::ShruthiSvfDualRouting::section1,
          swaraxt::ShruthiSvf2pMode::lowPass, swaraxt::ShruthiSvf2pMode::lowPass, 0.4f },
        { "s1_lp_high_res", swaraxt::ShruthiSvfDualRouting::section1,
          swaraxt::ShruthiSvf2pMode::lowPass, swaraxt::ShruthiSvf2pMode::lowPass, 0.7f },
        { "s1_lp_near_threshold", swaraxt::ShruthiSvfDualRouting::section1,
          swaraxt::ShruthiSvf2pMode::lowPass, swaraxt::ShruthiSvf2pMode::lowPass, 0.84f },
        { "s1_lp_selfosc", swaraxt::ShruthiSvfDualRouting::section1,
          swaraxt::ShruthiSvf2pMode::lowPass, swaraxt::ShruthiSvf2pMode::lowPass, 1.0f },
        { "s1_bp", swaraxt::ShruthiSvfDualRouting::section1,
          swaraxt::ShruthiSvf2pMode::bandPass, swaraxt::ShruthiSvf2pMode::lowPass, 0.35f },
        { "s1_hp", swaraxt::ShruthiSvfDualRouting::section1,
          swaraxt::ShruthiSvf2pMode::highPass, swaraxt::ShruthiSvf2pMode::lowPass, 0.2f },
        { "s1_cutoff_sweep", swaraxt::ShruthiSvfDualRouting::section1,
          swaraxt::ShruthiSvf2pMode::lowPass, swaraxt::ShruthiSvf2pMode::lowPass, 0.35f },
        { "s1_res_sweep", swaraxt::ShruthiSvfDualRouting::section1,
          swaraxt::ShruthiSvf2pMode::lowPass, swaraxt::ShruthiSvf2pMode::lowPass, 0.05f },
        { "serial_lp_lp", swaraxt::ShruthiSvfDualRouting::serial,
          swaraxt::ShruthiSvf2pMode::lowPass, swaraxt::ShruthiSvf2pMode::lowPass, 0.35f },
        { "serial_lp_bp", swaraxt::ShruthiSvfDualRouting::serial,
          swaraxt::ShruthiSvf2pMode::lowPass, swaraxt::ShruthiSvf2pMode::bandPass, 0.35f },
        { "serial_bp_lp", swaraxt::ShruthiSvfDualRouting::serial,
          swaraxt::ShruthiSvf2pMode::bandPass, swaraxt::ShruthiSvf2pMode::lowPass, 0.35f },
        { "serial_hp_lp", swaraxt::ShruthiSvfDualRouting::serial,
          swaraxt::ShruthiSvf2pMode::highPass, swaraxt::ShruthiSvf2pMode::lowPass, 0.35f },
        { "parallel_lp_hp", swaraxt::ShruthiSvfDualRouting::parallel,
          swaraxt::ShruthiSvf2pMode::lowPass, swaraxt::ShruthiSvf2pMode::highPass, 0.35f },
        { "parallel_lp_bp", swaraxt::ShruthiSvfDualRouting::parallel,
          swaraxt::ShruthiSvf2pMode::lowPass, swaraxt::ShruthiSvf2pMode::bandPass, 0.35f },
        { "parallel_bp_hp", swaraxt::ShruthiSvfDualRouting::parallel,
          swaraxt::ShruthiSvf2pMode::bandPass, swaraxt::ShruthiSvf2pMode::highPass, 0.35f },
    };
    for (const auto& job : jobs)
    {
        for (int which = 0; which < 2; ++which)
        {
            const auto cal = which == 0
                ? swaraxt::ShruthiSvfCalibration::paperApproved
                : swaraxt::ShruthiSvfCalibration::eagleHardware;
            SwaraXtAudioProcessor proc;
            proc.prepareToPlay(rate, bs);
            proc.engineForTests().setExperimentalSvfEnabledForTests(true);
            proc.engineForTests().setExperimentalSvfCalibrationForTests(cal);
            proc.engineForTests().setExperimentalSvfRoutingForTests(job.routing);
            proc.engineForTests().setExperimentalSvfModeForTests(job.mode1);
            proc.engineForTests().setExperimentalSvfMode2ForTests(job.mode2);
            if (auto* resParam = proc.getApvts().getParameter("filter_resonance"))
                resParam->setValueNotifyingHost(resParam->convertTo0to1(job.resonance));
            std::vector<float> audio;
            audio.reserve(static_cast<size_t>(rate));
            juce::AudioBuffer<float> buffer(2, bs);
            const int blocks = static_cast<int>(rate) / bs;
            for (int b = 0; b < blocks; ++b)
            {
                buffer.clear();
                juce::MidiBuffer midi;
                if (b == 0)
                    midi.addEvent(juce::MidiMessage::noteOn(1, 36, static_cast<juce::uint8>(110)), 0);
                if (std::strcmp(job.stem, "s1_cutoff_sweep") == 0)
                    midi.addEvent(juce::MidiMessage::controllerEvent(1, 74, static_cast<juce::uint8>(b % 128)), 0);
                if (std::strcmp(job.stem, "s1_res_sweep") == 0)
                    midi.addEvent(juce::MidiMessage::controllerEvent(1, 71, static_cast<juce::uint8>(b % 128)), 0);
                if (b == blocks - 8)
                    midi.addEvent(juce::MidiMessage::noteOff(1, 36), 0);
                proc.processBlock(buffer, midi);
                for (int i = 0; i < bs; ++i)
                    audio.push_back(buffer.getSample(0, i));
            }
            const char* prefix = which == 0 ? "approved_" : "hardware_";
            const auto path = dir / (std::string(prefix) + job.stem + ".wav");
            writeWavFloat32(path, audio, rate);
            double peak = 0.0, energy = 0.0;
            int invalid = 0;
            for (float s : audio)
            {
                if (! std::isfinite(s))
                    ++invalid;
                peak = std::max(peak, static_cast<double>(std::fabs(s)));
                energy += static_cast<double>(s) * static_cast<double>(s);
            }
            expect(invalid == 0, "prototype 3 A/B render finite");
            std::printf("wrote %s peak=%.4f rms=%.4f\n", path.filename().string().c_str(),
                        peak, std::sqrt(energy / static_cast<double>(audio.size())));
        }
    }
    expect(true, "prototype 3 A/B evidence wavs written");
}

int main()
{
    expect(! SwaraXtAudioProcessor().experimentalSvfSelection().enabled,
           "new instance defaults to Classic");
    expect(SwaraXtAudioProcessor().experimentalSvfSelection().routing
               == swaraxt::ShruthiSvfDualRouting::section1,
           "experimental Dual defaults to Section 1 reference routing");
    expect(SwaraXtAudioProcessor().experimentalSvfSelection().calibration
               == swaraxt::ShruthiSvfCalibration::paperApproved,
           "experimental Dual defaults to Approved/Paper calibration");
    runMatrix(false);
    runMatrix(true);
    testConcurrentPublication();
    renderEvidence();
    renderPrototype2Evidence();
    renderPrototype3AbEvidence();
    if (gFailures != 0)
    {
        std::printf("%d failure(s)\n", gFailures);
        return 1;
    }
    std::printf("Experimental SVF engine tests passed\n");
    return 0;
}
