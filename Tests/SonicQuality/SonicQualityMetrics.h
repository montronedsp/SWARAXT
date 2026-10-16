// Copyright 2026 MontroneDSP.
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include <string>
#include <vector>

namespace swaraxt::sonic {

enum class AnalysisWindow
{
    rectangular,
    hann
};

struct ToneComponent
{
    double frequencyHz = 0.0;
    double peakAmplitude = 0.0;
    double phaseRadians = 0.0;
    bool measurable = false;
};

struct LevelStats
{
    int samples = 0;
    int nonfinite = 0;
    int aboveUnity = 0;
    int aboveBoardLimit = 0;
    int aboveUnbounded = 0;
    int nearFullScale = 0;
    int longestNearFullScaleRun = 0;
    double samplePeak = 0.0;
    double rms = 0.0;
    double mean = 0.0;
    double crestDb = 0.0;
    double maxAdjacentDifference = 0.0;
    double medianAdjacentDifference = 0.0;
};

struct DistortionStats
{
    ToneComponent fundamental;
    double thd = 0.0;
    double thdN = 0.0;
    bool thdNValid = false;
    double aliasPeak = 0.0;
    double aliasFrequencyHz = 0.0;
    double harmonicPeak[4] {};
    AnalysisWindow window = AnalysisWindow::rectangular;
    bool coherent = false;
};

struct TruePeakStats
{
    double samplePeak = 0.0;
    double truePeak = 0.0;
    int factor = 4;
    int taps = 48;
};

LevelStats measureLevels(const float* samples, int count);
ToneComponent projectTone(const float* samples, int count, double sampleRate,
                          double frequencyHz, AnalysisWindow window);
DistortionStats measureDistortion(const float* samples, int count, double sampleRate,
                                  double fundamentalHz, AnalysisWindow window);
TruePeakStats measureTruePeak(const float* samples, int count);
double db(double amplitude) noexcept;
double foldFrequency(double frequencyHz, double sampleRate) noexcept;
int coherentLength(double sampleRate, double frequencyHz, int minimumSamples) noexcept;
bool nearlyIntegerCycles(double sampleRate, double frequencyHz, int count) noexcept;
int bestIntegerLag(const std::vector<float>& reference, const std::vector<float>& other,
                   int maxLag, double& correlation);
double maxAbsDifference(const std::vector<float>& a, const std::vector<float>& b, int lag);
double rmsDifference(const std::vector<float>& a, const std::vector<float>& b, int lag);
bool writeFloatWav(const std::string& path, const std::vector<float>& samples, int sampleRate);
bool writeSpectrogramPpm(const std::string& path, const std::vector<float>& samples,
                         double sampleRate, int fftSize, int hop);

// Analytic signals. Returns the number of failed checks.
int runMetricSelfTests(std::string& detail);

}  // namespace swaraxt::sonic
