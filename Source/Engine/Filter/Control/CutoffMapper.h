// Copyright 2026 MontroneDSP.
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include "Engine/Filter/Circuit/Ir3109Types.h"

#include <cmath>

namespace swaraxt {

class CutoffMapper {
 public:
    void setCalibration(const FilterCalibration& cal) noexcept { cal_ = cal; }
    void setThermal(const ThermalState& thermal) noexcept { thermal_ = thermal; }

    FilterCalibration& calibration() noexcept { return cal_; }
    const FilterCalibration& calibration() const noexcept { return cal_; }

    // Panel cutoff in Hz (10..20k), key tracking 0..1, note MIDI number, extra mod in octaves.
    double mapCutoffHz(double panelHz,
                       double keyTrack01,
                       double noteNumber,
                       double modOctaves) const noexcept
    {
        const double panel = clampFinite(panelHz, cal_.minimumPanelCutoffHz, cal_.maximumPanelCutoffHz);
        const double kt = clampFinite(keyTrack01, 0.0, 1.0);

        // A4 = MIDI 69 reference for key follow around the factory ~1 kHz point.
        constexpr double kRefNote = 69.0;
        const double keyOct = ((noteNumber - kRefNote) / 12.0) * kt * cal_.keyboardTrackingScale;
        const double totalOct = keyOct + modOctaves;
        const double hz = panel * std::pow(2.0, totalOct);
        return clampFinite(hz, cal_.minimumPanelCutoffHz, cal_.maximumPanelCutoffHz);
    }

    // Convert panel cutoff to equivalent control voltage. The panel value maps
    // to the resonant pole frequency; the no-resonance four-pole -3 dB point is
    // lower by fourPoleMinus3DbRatio().
    double controlVoltageForCutoff(double cutoffHz) const noexcept
    {
        const double f0 = cal_.nominalStageCutoffAtZeroCvHz;
        const double ratio = clampFinite(cutoffHz, 1.0, 1.0e6) / f0;
        // f(VC) = f0 * 2^(-VC / VperOct)  =>  VC = -VperOct * log2(f/f0)
        const double vc = -cal_.controlVoltsPerOctave * std::log2(ratio) + cal_.controlVoltageOffset;
        return vc;
    }

    double stageCutoffFromControlVoltage(double vc) const noexcept
    {
        const double tempScale = 1.0 + thermal_.frequencyTempcoPerDegree
                                     * (thermal_.temperatureCelsius - thermal_.referenceTemperatureCelsius);
        const double f = cal_.nominalStageCutoffAtZeroCvHz
                         * std::pow(2.0, -vc / cal_.controlVoltsPerOctave)
                         * tempScale;
        return clampFinite(f, 1.0, 100000.0);
    }

    double stageCutoffFromMusicalCutoff(double cutoffHz) const noexcept
    {
        return stageCutoffFromControlVoltage(controlVoltageForCutoff(cutoffHz));
    }

    static constexpr double fourPoleMinus3DbRatio() noexcept
    {
        return 0.43497944117144233; // sqrt(2^(1/4) - 1)
    }

    // Factory calibration ratios for F5/F4 at a fixed panel cutoff.
    static double keyTrackRatio(double keyTrack01) noexcept
    {
        // One octave between F4 (65) and F5 (77).
        return std::pow(2.0, keyTrack01);
    }

 private:
    FilterCalibration cal_ {};
    ThermalState thermal_ {};
};

}  // namespace swaraxt
