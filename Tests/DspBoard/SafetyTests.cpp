// Copyright 2026 MontroneDSP. SPDX-License-Identifier: GPL-3.0-or-later
#include "Engine/DspBoard/BoardProcessor.h"
#include "Engine/SampleRate/HostResampler.h"
#include "Engine/SampleRate/PolyphaseFirResampler.h"
#include <iostream>
#include <stdexcept>

namespace {
using namespace swaraxt::board;
void require(bool ok, const char* message) { if (!ok) throw std::runtime_error(message); }

void endpointMatrix()
{
    double peak = 0;
    std::uint64_t count = 0;
    for (int effect = 0; effect < 17; ++effect)
    for (int route = 0; route < 5; ++route)
    for (int corner = 0; corner < 32; ++corner)
    for (int input = 0; input < 8; ++input)
    {
        BoardControl c;
        c.model = Model::dspBoard; c.effect = effectFromChoice(effect); c.route = static_cast<Route>(route);
        c.cutoff = (corner & 1) ? 254 : 0;
        c.resonance = (corner & 2) ? 254 : 0;
        c.cv1 = (corner & 4) ? 254 : 0;
        c.cv2 = (corner & 8) ? 254 : 0;
        c.dca = (corner & 16) ? 254 : 0;
        BoardProcessor processor;
        processor.reset(c.effect);
        std::uint32_t random = 21;
        for (int block = 0; block < 12; ++block)
        {
            FloatBlock samples{};
            for (std::size_t i = 0; i < blockSize; ++i)
            {
                random = random * 1664525u + 1013904223u;
                const auto n = static_cast<double>(block * blockSize + i);
                samples[i] = input == 0 ? 0.f : input == 1 ? .25f : input == 2 ? -.25f
                    : input == 3 ? (block == 0 && i == 0 ? 1.f : 0.f)
                    : input == 4 ? 1.f : input == 5 ? -1.f
                    : input == 6 ? (i & 1 ? 1.f : -1.f)
                    : static_cast<float>(.5 * std::sin(6.283185307179586 * 440 * n / sampleRate)
                        + (static_cast<int>(random >> 24) - 128) / 256.);
            }
            processor.processBoard(samples, c);
            for (const float value : samples)
            {
                require(std::isfinite(value) && std::abs(value) < 8.f, "endpoint matrix finite/headroom");
                if (c.effect == Effect::looper && c.cv2 >= 128)
                    require(value == 0, "empty replay must be silent before host DC protection");
                peak = std::max(peak, std::abs(double(value)));
                ++count;
            }
        }
    }
    std::cout << "Endpoint matrix: samples=" << count << " native peak=" << peak << '\n';
}

void hostBoundary()
{
    // Use the same 256-tap/256-phase converter and blocker as production.
    swaraxt::PolyphaseFirResampler<256, 256, true> converter;
    for (const double rate : { 44100., 48000., 88200., 96000., 176400., 192000. })
    for (int input = 0; input < 7; ++input)
    {
        double peak = 0, maxMean = 0, distortionPreDc = 0, distortionPostDc = 0;
        for (int effect = 0; effect < 17; ++effect)
        for (int route = 0; route < 5; ++route)
        {
            BoardControl c;
            c.model = Model::dspBoard; c.effect = effectFromChoice(effect); c.route = static_cast<Route>(route);
            c.cv1 = c.cv2 = c.cutoff = c.dca = 254;
            c.resonance = 0;
            BoardProcessor processor;
            processor.reset(c.effect);
            converter.reset(); converter.setStep(sampleRate, rate);
            swaraxt::DcBlocker dc; dc.prepare(rate);
            double sum = 0, preSum = 0;
            const int total = static_cast<int>(rate * 1.25);
            const int window = static_cast<int>(rate * .25);
            std::uint64_t nativeIndex = 0;
            std::uint32_t random = 21;
            for (int i = 0; i < total; ++i)
            {
                while (converter.size() < converter.queueTargetSize())
                {
                    FloatBlock samples{};
                    for (auto& value : samples)
                    {
                        random = random * 1664525u + 1013904223u;
                        value = input == 0 ? 0.f : input == 1 ? 1.f : input == 2 ? -1.f
                            : input == 3 ? (nativeIndex == 0 ? 1.f : 0.f)
                            : input == 4 ? (nativeIndex & 1 ? 1.f : -1.f)
                            : input == 5 ? static_cast<float>(static_cast<int>(random >> 24) - 128) / 128.f
                            : static_cast<float>(std::sin(6.283185307179586 * 440 * static_cast<double>(nativeIndex) / sampleRate));
                        ++nativeIndex;
                    }
                    processor.processBoard(samples, c);
                    for (const float value : samples) converter.push(value);
                }
                const float pre = converter.readInterpolated();
                const float out = dc.process(pre);
                require(std::isfinite(out) && std::abs(out) < 8.f, "host output finite/headroom");
                if (c.effect == Effect::looper) require(out == 0, "empty loop host output");
                if (i >= total - window) { sum += out; preSum += pre; }
                peak = std::max(peak, std::abs(double(out)));
            }
            const double mean = std::abs(sum / window);
            require(mean < (input == 0 ? .001 : .1), "persistent host DC / low-frequency window bound");
            maxMean = std::max(maxMean, mean);
            if (c.effect == Effect::distortion && c.route == Route::fxOnly)
            {
                distortionPreDc = preSum / window;
                distortionPostDc = sum / window;
            }
        }
        std::cout << "Host rate=" << rate << " input=" << input << " all FX/routes: peak=" << peak
                  << " max final mean=" << maxMean << " distortion preDC=" << distortionPreDc
                  << " postDC=" << distortionPostDc << '\n';
        for (const float value : { -1.f, 1.f })
        {
            swaraxt::DcBlocker dc; dc.prepare(rate);
            float last = 0;
            for (int i = 0; i < static_cast<int>(rate); ++i) last = dc.process(value);
            require(std::abs(last) < 1.e-7f, "shared blocker constant-input decay");
            dc.reset(); require(dc.process(value) == value, "shared blocker deterministic reset");
        }
    }
    BoardEffects effect;
    Block silence{};
    effect.process(silence, Effect::distortion, 254, 254, 120);
    require(silence[0] != 0, "preserve original distortion's internal silence offset");
    std::cout << "Source distortion silence integer code=" << silence[0] << '\n';
}

void oscillationLoopAndFeedback()
{
    swaraxt::PolyphaseFirResampler<256, 256, true> converter;
    for (const double rate : { 44100., 48000., 88200., 96000., 176400., 192000. })
    {
        converter.reset(); converter.setStep(sampleRate, rate);
        swaraxt::DcBlocker dc; dc.prepare(rate);

        BoardControl osc;
        osc.model = Model::dspBoard; osc.effect = Effect::off; osc.route = Route::lowPassLast;
        osc.cutoff = 80; osc.resonance = 254; osc.dca = 254;
        BoardProcessor processor; processor.reset(osc.effect);
        double peak = 0, windowSum = 0;
        const int total = static_cast<int>(rate * 0.6);
        const int window = static_cast<int>(rate * 0.1);
        std::uint64_t nativeIndex = 0;
        for (int i = 0; i < total; ++i)
        {
            while (converter.size() < converter.queueTargetSize())
            {
                FloatBlock samples{};
                if (nativeIndex < 200)
                    samples.fill(0.25f);
                processor.processBoard(samples, osc);
                for (const float value : samples) converter.push(value);
                ++nativeIndex;
            }
            const float out = dc.process(converter.readInterpolated());
            require(std::isfinite(out) && std::abs(out) < 8.f, "self-oscillation host finite");
            peak = std::max(peak, std::abs(double(out)));
            if (i >= total - window) windowSum += out;
        }
        require(std::abs(windowSum / window) < 0.1, "self-oscillation final host DC bound");

        BoardControl comb;
        comb.model = Model::dspBoard; comb.effect = Effect::combPositive; comb.route = Route::fxOnly;
        comb.cv2 = 254; comb.dca = 254;
        BoardProcessor combProc; combProc.reset(comb.effect);
        converter.reset(); converter.setStep(sampleRate, rate);
        dc.prepare(rate); dc.reset();
        peak = 0; windowSum = 0; nativeIndex = 0;
        for (int i = 0; i < total; ++i)
        {
            while (converter.size() < converter.queueTargetSize())
            {
                FloatBlock samples{};
                if (nativeIndex < 40) samples[0] = 1.f;
                combProc.processBoard(samples, comb);
                for (const float value : samples) converter.push(value);
                ++nativeIndex;
            }
            const float out = dc.process(converter.readInterpolated());
            require(std::isfinite(out) && std::abs(out) < 8.f, "high-feedback comb host finite");
            peak = std::max(peak, std::abs(double(out)));
            if (i >= total - window) windowSum += out;
        }
        require(std::abs(windowSum / window) < 0.1, "high-feedback comb final host DC bound");

        BoardControl loop;
        loop.model = Model::dspBoard; loop.effect = Effect::looper; loop.route = Route::fxOnly;
        loop.cv2 = 0; loop.dca = 254; loop.cv1 = 128;
        BoardProcessor loopProc; loopProc.reset(loop.effect);
        for (int block = 0; block < 8; ++block)
        {
            FloatBlock samples{};
            samples.fill(0.25f);
            loopProc.processBoard(samples, loop);
        }
        require(loopProc.hasValidLoop(), "recorded loop is valid");
        loop.cv2 = 200;
        converter.reset(); converter.setStep(sampleRate, rate);
        dc.prepare(rate); dc.reset();
        peak = 0; windowSum = 0;
        for (int i = 0; i < total; ++i)
        {
            while (converter.size() < converter.queueTargetSize())
            {
                FloatBlock samples{};
                loopProc.processBoard(samples, loop);
                for (const float value : samples) converter.push(value);
            }
            const float out = dc.process(converter.readInterpolated());
            require(std::isfinite(out) && std::abs(out) < 8.f, "recorded loop host finite");
            peak = std::max(peak, std::abs(double(out)));
            if (i >= total - window) windowSum += out;
        }
        require(std::abs(windowSum / window) < 0.1, "recorded loop final host DC bound");
        std::cout << "Oscillation/comb/loop host rate=" << rate << " peak=" << peak
                  << " final mean=" << (windowSum / window) << '\n';
    }
}
}
int main()
{
    try { endpointMatrix(); hostBoundary(); oscillationLoopAndFeedback(); }
    catch (const std::exception& error) { std::cerr << "FAIL: " << error.what() << '\n'; return 1; }
    return 0;
}
