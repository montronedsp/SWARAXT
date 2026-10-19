// Copyright 2026 MontroneDSP. SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
#include <JuceHeader.h>
#include <cmath>
#include <cstdio>
#include <set>
#include <string>

namespace swaraxt::test {
// Independently maintained host contract, revision 1: legacy prefix (86),
// board append (7), MIDI Out append (1), Input Tune append (1).
// Public baseline: 8c5ea737; reviewed appends af089b0 and adcf3bb.
// Literal IDs/domains are intentional. Never generate this fixture from the
// live layout. Intentional schema revisions require reviewing this contract.
struct ParameterContract {
    const char* id;
    float minimum, maximum, defaultValue, interval;
    int version = 1;
    bool logarithmic = false;
    bool halfStepRoundsUp = false;
};
inline constexpr ParameterContract parameterContractV1[] {
    {"master.level", 0, 1, .85f, .001f},
    {"filter_cutoff", 10, 20000, 8000, 0, 1, true},
    {"filter_resonance", 0, 1, .15f, 0},
    {"filter_key_tracking", 0, 1, .5f, 0},
    {"filter.shruthi_env", 0, 63, 32, 1}, {"filter.shruthi_lfo", 0, 63, 0, 1},
    {"osc1.shape", 0, 34, 1, 1}, {"osc1.parameter", 0, 127, 0, 1},
    {"osc1.range", -48, 48, 0, 1}, {"osc1.option", 0, 13, 0, 1},
    {"osc2.shape", 0, 34, 1, 1}, {"osc2.parameter", 0, 127, 16, 1},
    {"osc2.range", -48, 48, -12, 1}, {"osc2.option", 0, 127, 12, 1},
    {"mix.balance", 0, 127, 32, 1}, {"mix.sub", 0, 127, 0, 1},
    {"mix.noise", 0, 127, 0, 1}, {"mix.subShape", 0, 10, 0, 1},
    {"env1.attack", 0, 127, 0, 1}, {"env1.decay", 0, 127, 50, 1},
    {"env1.sustain", 0, 127, 20, 1}, {"env1.release", 0, 127, 60, 1},
    {"env2.attack", 0, 127, 0, 1}, {"env2.decay", 0, 127, 40, 1},
    {"env2.sustain", 0, 127, 90, 1}, {"env2.release", 0, 127, 30, 1},
    {"lfo1.waveform", 0, 20, 1, 1}, {"lfo1.rate", 0, 127, 80, 1},
    {"lfo1.attack", 0, 127, 0, 1}, {"lfo1.retrigger", 0, 3, 0, 1},
    {"lfo1.sync", 0, 1, 0, 1}, {"lfo1.division", 0, 7, 3, 1},
    {"lfo2.waveform", 0, 20, 1, 1}, {"lfo2.rate", 0, 127, 3, 1},
    {"lfo2.attack", 0, 127, 0, 1}, {"lfo2.retrigger", 0, 3, 0, 1},
    {"lfo2.sync", 0, 1, 0, 1}, {"lfo2.division", 0, 7, 3, 1},
    {"mod.row1.source", 0, 31, 0, 1}, {"mod.row1.destination", 0, 26, 4, 1}, {"mod.row1.amount", -63, 63, 0, 1},
    {"mod.row2.source", 0, 31, 19, 1}, {"mod.row2.destination", 0, 26, 5, 1}, {"mod.row2.amount", -63, 63, 0, 1},
    {"mod.row3.source", 0, 31, 19, 1}, {"mod.row3.destination", 0, 26, 2, 1}, {"mod.row3.amount", -63, 63, 0, 1},
    {"mod.row4.source", 0, 31, 0, 1}, {"mod.row4.destination", 0, 26, 3, 1}, {"mod.row4.amount", -63, 63, 0, 1},
    {"mod.row5.source", 0, 31, 1, 1}, {"mod.row5.destination", 0, 26, 8, 1}, {"mod.row5.amount", -63, 63, 0, 1},
    {"mod.row6.source", 0, 31, 2, 1}, {"mod.row6.destination", 0, 26, 8, 1}, {"mod.row6.amount", -63, 63, 0, 1},
    {"mod.row7.source", 0, 31, 21, 1}, {"mod.row7.destination", 0, 26, 2, 1}, {"mod.row7.amount", -63, 63, 0, 1},
    {"mod.row8.source", 0, 31, 21, 1}, {"mod.row8.destination", 0, 26, 3, 1}, {"mod.row8.amount", -63, 63, 0, 1},
    {"mod.row9.source", 0, 31, 20, 1}, {"mod.row9.destination", 0, 26, 1, 1}, {"mod.row9.amount", -63, 63, 63, 1},
    {"mod.row10.source", 0, 31, 21, 1}, {"mod.row10.destination", 0, 26, 1, 1}, {"mod.row10.amount", -63, 63, 16, 1},
    {"mod.row11.source", 0, 31, 8, 1}, {"mod.row11.destination", 0, 26, 6, 1}, {"mod.row11.amount", -63, 63, 32, 1},
    {"mod.row12.source", 0, 31, 0, 1}, {"mod.row12.destination", 0, 26, 6, 1}, {"mod.row12.amount", -63, 63, 16, 1},
    {"perf.portamento", 0, 127, 0, 1}, {"perf.legato", 0, 1, 0, 1}, {"midi.channel", 0, 16, 0, 1},
    {"seq.mode", 0, 2, 0, 1}, {"seq.tempo", 40, 240, 120, 1},
    {"seq.swing", 0, 127, 0, 1}, {"seq.clockMode", 0, 1, 0, 1}, {"seq.gate", 5, 100, 50, 1},
    {"arp.direction", 0, 4, 0, 1}, {"arp.pattern", 0, 7, 1, 1},
    {"arp.octaves", 1, 4, 1, 1}, {"arp.gate", 0, 11, 7, 1},
    {"filter.model", 0, 1, 0, 1, 2}, {"dsp.fx", 0, 16, 0, 1, 2},
    {"dsp.param1", 0, 127, 64, 1, 2}, {"dsp.param2", 0, 63, 0, 1, 2},
    {"dsp.route", 0, 4, 0, 1, 2}, {"mix.postMixer", -18, 0, 0, .1f},
    {"mix.inputConditioning", 0, 1, 1, 1},
    // AudioParameterBool rounds its midpoint up; legacy integer/choice
    // parameters instead preserve JUCE's nearest-even integer tie contract.
    {"seq.midiOut", 0, 1, 0, 1, 1, false, true}, {"midi.inputTuning", 0, 2, 0, 1},
};
static_assert(std::size(parameterContractV1) == 86 + 7 + 1 + 1);

template <typename Check>
void checkParameterContract(const juce::AudioProcessor& processor, Check check)
{
    const auto& parameters = processor.getParameters();
    check(parameters.size() == static_cast<int>(std::size(parameterContractV1)),
          "parameter contract v1 count");
    std::set<std::string> ids;
    for (int index = 0; index < parameters.size(); ++index) {
        const auto* p = dynamic_cast<const juce::RangedAudioParameter*>(parameters[index]);
        check(p != nullptr, "parameter retains ranged identity");
        if (p == nullptr) continue;
        check(ids.insert(p->paramID.toStdString()).second, "no duplicate parameter IDs");
        if (index >= static_cast<int>(std::size(parameterContractV1))) continue;
        const auto& expected = parameterContractV1[index];
        const auto field = [&](bool valid, const char* label, double actual, double historical) {
            if (!valid)
                std::fprintf(stderr, "parameter=%s %s actual=%.9g expected=%.9g\n",
                             expected.id, label, actual, historical);
            check(valid, label);
        };
        check(p->paramID == expected.id, expected.id);
        check(p->getVersionHint() == expected.version, "historical version hint");
        const auto& range = p->getNormalisableRange();
        check(range.start == expected.minimum && range.end == expected.maximum,
              "historical parameter endpoints");
        check(std::abs(range.interval - expected.interval) < 1.0e-6f, "historical parameter step");
        const float normalizedDefault = expected.logarithmic
            ? std::log(expected.defaultValue / expected.minimum) / std::log(expected.maximum / expected.minimum)
            : (expected.defaultValue - expected.minimum) / (expected.maximum - expected.minimum);
        field(std::abs(p->getDefaultValue() - normalizedDefault) < 1.0e-6f,
              "historical normalized default", p->getDefaultValue(), normalizedDefault);
        for (float unit : {0.0f, .25f, .5f, .75f, 1.0f}) {
            float physical = expected.logarithmic
                ? expected.minimum * std::pow(expected.maximum / expected.minimum, unit)
                : expected.minimum + unit * (expected.maximum - expected.minimum);
            if (expected.interval == 1 && !expected.halfStepRoundsUp) {
                // Independent mathematical oracle, not the JUCE bit-cast
                // rounding implementation. Signed half steps round to even.
                const float lower = std::floor(physical);
                const float fraction = physical - lower;
                physical = lower + ((fraction > .5f ||
                    (fraction == .5f && static_cast<int>(lower) % 2 != 0)) ? 1.0f : 0.0f);
            }
            else if (expected.interval > 0)
                physical = expected.minimum + expected.interval * std::floor((physical - expected.minimum) / expected.interval + .5f);
            const float actual = p->convertFrom0to1(unit);
            const float tolerance = std::max(1.0f, std::abs(physical)) * 1.0e-6f;
            field(expected.interval == 1 ? actual == physical : std::abs(actual - physical) <= tolerance,
                  "historical normalized domain", actual, physical);
            const float inverse = expected.logarithmic
                ? std::log(physical / expected.minimum) / std::log(expected.maximum / expected.minimum)
                : (physical - expected.minimum) / (expected.maximum - expected.minimum);
            field(std::abs(p->convertTo0to1(physical) - inverse) <= 1.0e-6f,
                  "historical inverse normalization", p->convertTo0to1(physical), inverse);
        }
    }
}
} // namespace swaraxt::test
