// Copyright 2026 MontroneDSP.
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include <string>

namespace swaraxt::sonic {

// PASS: a stated invariant or a self-test with an analytic tolerance held.
// FAIL: that invariant or self-test did not hold. CTest stops on FAIL.
// OBSERVED: a sonic measurement with no approved acceptance limit.
// NEEDS REVIEW: the measurement is outside an informal screening bound,
// or no independent reference exists and the value should be inspected.
// NOT APPLICABLE: the scenario has no meaningful equivalent (for example a
// 12-bit effect that cannot be dry).
enum class Classification
{
    pass,
    fail,
    observed,
    needsReview,
    notApplicable
};

inline const char* classificationName(Classification value) noexcept
{
    switch (value)
    {
        case Classification::pass: return "PASS";
        case Classification::fail: return "FAIL";
        case Classification::observed: return "OBSERVED";
        case Classification::needsReview: return "NEEDS REVIEW";
        case Classification::notApplicable: return "NOT APPLICABLE";
    }
    return "NEEDS REVIEW";
}

inline Classification worse(Classification a, Classification b) noexcept
{
    auto rank = [](Classification value) {
        switch (value)
        {
            case Classification::fail: return 4;
            case Classification::needsReview: return 3;
            case Classification::observed: return 2;
            case Classification::notApplicable: return 1;
            case Classification::pass: return 0;
        }
        return 3;
    };
    return rank(a) >= rank(b) ? a : b;
}

// Production filter output is clamped to this magnitude. Exceeding it on the
// direct instrument path means the clamp contract broke.
inline constexpr float kBoardSafetyLimit = 8.0f;

// Samples this far above the clamp are treated as unbounded, including after
// later gain stages. Musical peaks above full scale stay characterization.
inline constexpr float kUnboundedLimit = 100.0f;

// Qualified by SwaraXTIR3109BoardTests at 1 kHz, amplitude 1e-4, resonance 0:
// small-signal gain versus the schematic OTA estimate.
inline constexpr double kSchematicGainTolerance = 0.015;
inline constexpr float kSchematicCheckAmplitude = 1.0e-4f;
inline constexpr double kSchematicCheckFrequencyHz = 1000.0;

// Windowed-sinc true peak is checked against a pure sinusoid to this error.
inline constexpr double kTruePeakTolerance = 0.02;

inline constexpr double kPi = 3.14159265358979323846;

struct Measurement
{
    std::string testCase;
    std::string family;
    std::string sourceClass;
    std::string input;
    std::string parameters;
    std::string quality;
    double hostRate = 0.0;
    int blockSize = 0;
    std::string commit;
    std::string reference;
    std::string observed;
    std::string tolerance;
    Classification result = Classification::observed;
    std::string notes;
};

}  // namespace swaraxt::sonic
