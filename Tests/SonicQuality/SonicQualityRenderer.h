// Copyright 2026 MontroneDSP.
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include "Engine/Filter/FilterQuality.h"

#include <cstdint>
#include <string>
#include <vector>

namespace swaraxt::sonic {

struct BoardTone
{
    double frequencyHz = 1000.0;
    float amplitude = 0.1f;
    float cutoffHz = 20000.0f;
    float resonance = 0.0f;
    swaraxt::FilterQuality quality = swaraxt::FilterQuality::normal;
    int warmupSamples = 4096;
    int captureSamples = 2048;
    bool instrumentPath = true;
    int oversampleOverride = 0;
    float vcaTarget = 1.0f;
};

struct BoardRender
{
    std::vector<float> samples;
    double sampleRate = 0.0;
    int latencySamples = 0;
    int oversample = 1;
    bool finite = true;
};

enum class FxStimulus
{
    sine,
    saw,
    impulse,
    noise
};

struct FxInsert
{
    int effectChoice = 0;
    std::uint8_t cv1 = 0;
    std::uint8_t cv2 = 0;
    double frequencyHz = 1000.0;
    float amplitude = 0.25f;
    int blocks = 80;
    FxStimulus stimulus = FxStimulus::sine;
    bool applyPostDac = true;
};

struct FxRender
{
    std::vector<float> samples;
    double sampleRate = 0.0;
    bool finite = true;
};

enum class CutoffLaw
{
    hold,
    linear,
    logarithmic,
    jump
};

struct PluginTone
{
    double hostRate = 48000.0;
    int blockSize = 64;
    int totalSamples = 12000;
    int note = 69;
    int noteOffSample = -1;
    swaraxt::FilterQuality quality = swaraxt::FilterQuality::normal;
    float cutoffHz = 20000.0f;
    float resonance = 0.0f;
    float cutoffEndHz = 20000.0f;
    CutoffLaw cutoffLaw = CutoffLaw::hold;
    int fxChoice = 0;
    int fxParam1 = 0;
    int fxParam2 = 0;
    float master = 0.85f;
    float postMixerDb = 0.0f;
    int filterEnv = 0;
    int filterLfo = 0;
    int lfoRate = 16;
    bool matrixCutoff = false;
    int osc2Shape = 0;
    int mixSub = 0;
    int mixNoise = 0;
};

struct PluginRender
{
    std::vector<float> left;
    std::vector<float> right;
    int reportedLatency = 0;
    int expectedLatency = 0;
    double renderSeconds = 0.0;
    bool prepared = false;
    bool rightMatchesLeft = true;
    bool finite = true;
};

struct ReprepareRender
{
    int latencyBefore = 0;
    int latencyAfter = 0;
    int expectedBefore = 0;
    int expectedAfter = 0;
    bool finite = true;
    bool audibleBefore = false;
    bool audibleAfter = false;
};

BoardRender renderBoardTone(const BoardTone& tone);
FxRender renderClassicFxInsert(const FxInsert& insert);
PluginRender renderPluginTone(const PluginTone& tone);
ReprepareRender renderPluginReprepare();
double schematicOpenFilterGain(double frequencyHz) noexcept;
double coupledOpenFilterGain(double frequencyHz) noexcept;
float requestedCutoff(const PluginTone& tone, int sample) noexcept;
const char* qualityName(swaraxt::FilterQuality quality) noexcept;
const char* effectName(int choice) noexcept;
bool effectCanBeTransparent(int choice) noexcept;

}  // namespace swaraxt::sonic
