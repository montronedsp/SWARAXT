// Copyright 2026 MontroneDSP.
// SPDX-License-Identifier: GPL-3.0-or-later
//
// Small-signal priors for the SwaraXt IR3109/AS3109-family VCF model.
//
// The realtime filter is formulated around zero volts:
//
//     v_audio = V_physical - Vref
//
// This keeps the virtual-ground bias out of the audio state and feedback
// equations. AS3109 values are used as engineering priors only; no production
// code claims calibration against measured vintage hardware.

#pragma once

#include <cmath>
#include <cstdint>

namespace swaraxt {

inline constexpr double kPi = 3.1415926535897932384626433832795;
inline constexpr double kIr3109IntegratorCapFarads = 240.0e-12;
inline constexpr double kIr3109ReferenceResistanceOhms = 68000.0;
inline constexpr double kAs3109ReferenceCutoffHz = 250.0;
inline constexpr double kOtaThermalVoltageVolts = 0.026;

// Effective attenuation from an external stage node into the OTA differential
// input. A 2.5 V audio swing becomes about 37.5 mV at the pair, so normal
// synth levels stay mostly linear while very large signals compress smoothly.
inline constexpr double kOtaDifferentialInputAttenuation = 0.015;

enum class LoadTopology : uint8_t
{
    passiveResistor,
    activeCurrentSource
};

struct Ir3109StageTraits
{
    LoadTopology loadTopology = LoadTopology::passiveResistor;
    double positiveCurrentLimitAmps = 0.0006;
    double negativeCurrentLimitAmps = 0.0006;
    double slewRateVoltsPerSecond = 25.0e6;
    double nominalOffsetVolts = 0.0;
    double frequencyScale = 1.0;
    double bufferGain = 1.0;
    double bufferOutputResistanceOhms = 100.0;
    double saturationSoftness = 1.0;
};

struct FilterCalibration
{
    double minimumPanelCutoffHz = 10.0;
    double maximumPanelCutoffHz = 20000.0;
    double nominalStageCutoffAtZeroCvHz = kAs3109ReferenceCutoffHz;
    double controlVoltsPerOctave = 0.019;
    double controlVoltageOffset = 0.0;
    double keyboardTrackingScale = 1.0;
};

struct CircuitLevelCalibration
{
    // +/-1.0 plugin full-scale maps to this peak small-signal voltage.
    double pluginFullScaleToVolts = 1.25;
    double nominalMixerOutputVrms = 0.35;
    double nominalFilterInputPeakVolts = 1.0;
    double virtualGroundVolts = 0.0;
};

struct ThermalState
{
    double temperatureCelsius = 25.0;
    double referenceTemperatureCelsius = 25.0;
    double frequencyTempcoPerDegree = 0.0033;
};

inline constexpr Ir3109StageTraits kPassiveStage1Traits {
    LoadTopology::passiveResistor,
    0.0006, 0.0006, 25.0e6, 0.0, 1.0, 1.0, 180.0, 0.96
};

inline constexpr Ir3109StageTraits kActiveStage2Traits {
    LoadTopology::activeCurrentSource,
    0.0010, 0.0008, 25.0e6, 0.0, 1.0, 1.0, 90.0, 1.05
};

inline constexpr Ir3109StageTraits kPassiveStage3Traits {
    LoadTopology::passiveResistor,
    0.0006, 0.0006, 25.0e6, 0.0, 1.0, 1.0, 180.0, 0.98
};

inline constexpr Ir3109StageTraits kActiveStage4Traits {
    LoadTopology::activeCurrentSource,
    0.0013, 0.0010, 25.0e6, 0.0, 1.0, 1.0, 70.0, 1.08
};

inline double clampFinite(double x, double lo, double hi) noexcept
{
    if (! std::isfinite(x))
        return 0.0;
    return x < lo ? lo : (x > hi ? hi : x);
}

inline float clampFinite(float x, float lo, float hi) noexcept
{
    if (! std::isfinite(x))
        return 0.0f;
    return x < lo ? lo : (x > hi ? hi : x);
}

inline double linearToDb(double linear) noexcept
{
    const double x = std::abs(linear);
    return x > 0.0 ? 20.0 * std::log10(x) : -300.0;
}

}  // namespace swaraxt
