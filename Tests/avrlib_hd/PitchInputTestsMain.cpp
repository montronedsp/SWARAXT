// Copyright 2026 MontroneDSP.
// SPDX-License-Identifier: GPL-3.0-or-later

#include <JuceHeader.h>
#include <array>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <cstdio>
#include <cstdlib>
#include <limits>
#include "Plugin/PluginProcessor.h"
#include "Plugin/ShruthiFactoryPresetData.h"
#include "Plugin/ShruthiFactoryPresets.h"
#include "avrlib_hd/voice.h"
#include "shruthi/resources.h"
#include "shruthi/parameter.h"

namespace {
constexpr int tableSize = 97;
uint64_t cases = 0;
uint64_t audioHash = UINT64_C(14695981039346656037);
void require(bool condition, const char* message) {
    if (!condition) { std::fprintf(stderr, "FAIL: %s case=%llu\n", message,
        static_cast<unsigned long long>(cases)); std::exit(1); }
}
int floorDivide(int value, int divisor) {
    return value >= 0 ? value / divisor : -((-value + divisor - 1) / divisor);
}
struct Lookup { int index, fraction, shifts; uint32_t increment; };
Lookup reference(int pitch, const std::array<uint16_t, tableSize>& table) {
    while (pitch >= 16384) pitch -= 1536;
    int relative = pitch - 14848;
    require(relative >= -32768 && relative <= 32767, "reference subtraction representable");
    int shifts = 0;
    while (relative < 0) { relative += 1536; ++shifts; }
    const int index = relative / 16, fraction = (relative % 16) * 16;
    require(index >= 0 && index + 1 < static_cast<int>(table.size()), "both interpolation indices in actual table extent");
    const uint32_t interpolated = table[static_cast<size_t>(index)]
        + (static_cast<uint32_t>(table[static_cast<size_t>(index + 1)] - table[static_cast<size_t>(index)])
           * static_cast<uint32_t>(fraction)) / 256u;
    return {index, fraction, shifts, (interpolated * 256u) >> shifts};
}
std::array<uint16_t, tableSize> pitchTable() {
    std::array<uint16_t, tableSize> result {};
    std::copy_n(shruthi::lut_res_oscillator_increments, result.size(), result.begin());
    require(result.front() == 11104 && result.back() == 22209, "authoritative pitch ROM endpoints");
    return result;
}
avrlib_hd::OscillatorTables oscillatorTables() {
    avrlib_hd::OscillatorTables t;
    t.waveform_table = shruthi::waveform_table; t.sine = shruthi::wav_res_sine;
    t.waves = shruthi::wav_res_waves; t.wavetables = shruthi::wav_res_wavetables;
    t.fm_frequency_ratios = shruthi::lut_res_fm_frequency_ratios;
    t.vowel_data = shruthi::wav_res_vowel_data; t.formant_sine = shruthi::wav_res_formant_sine;
    t.formant_square = shruthi::wav_res_formant_square;
    t.bandlimited_triangle_0 = shruthi::wav_res_bandlimited_triangle_0;
    return t;
}
void lookupDomain() {
    const auto table = pitchTable();
    int minIndex = 96, maxIndex = 0;
    for (int pitch = -11967; pitch <= 28474; ++pitch) {
        const auto lookup = reference(pitch, table);
        minIndex = std::min(minIndex, lookup.index); maxIndex = std::max(maxIndex, lookup.index);
        ++cases;
    }
    require(minIndex == 0 && maxIndex == 95, "complete valid pitch interval covers exactly indices 0..95");
    // Reproduce the old injected conversion numerically, never dereference it.
    const int injectedPitch = -18967;
    const int wrappedRelative = injectedPitch - 14848 + 65536;
    require(wrappedRelative / 16 == 1982 && wrappedRelative / 16 + 1 >= tableSize,
            "labelled raw -128 injection would escape the table without input admission");

    // A separately allocated exact-size array gives ASan independent red zones.
    // Its lifetime encloses all voice processing; the same ROM values are used.
    const auto bounded = std::make_unique<std::array<uint16_t, tableSize>>(table);
    avrlib_hd::HdVoiceTables voiceTables;
    voiceTables.oscillator_increments = bounded->data();
    voiceTables.portamento_increments = shruthi::lut_res_env_portamento_increments;
    voiceTables.distortion = shruthi::wav_res_distortion;
    avrlib_hd::EnvelopeTables envelopes;
    envelopes.portamento_increments = shruthi::lut_res_env_portamento_increments;
    envelopes.env_expo = shruthi::wav_res_env_expo;
    avrlib_hd::Random random;
    avrlib_hd::HdVoice voice;
    voice.Init(&random); voice.set_tables(&voiceTables); voice.set_envelope_tables(envelopes);
    for (int i = 0; i < 2; ++i) voice.mutable_oscillator(i)->set_tables(oscillatorTables());
    voice.set_user_wavetable(shruthi::wav_res_waves, avrlib_hd::HdOscillator::kUserWavetableSize);
    avrlib_hd::HdVoicePatch patch;
    patch.osc[0].shape = patch.osc[1].shape = avrlib_hd::kRealSaw;
    patch.env[1].sustain = 127;
    patch.mix_balance = 32;
    avrlib_hd::HdVoiceSystemSettings sys;
    const uint8_t destinations[] = { avrlib_hd::kModDestVco1_2Coarse, avrlib_hd::kModDestVco1_2Fine,
                                     avrlib_hd::kModDestVco1, avrlib_hd::kModDestVco2 };
    for (int row = 0; row < 8; ++row) {
        patch.mod[row].source = avrlib_hd::kModSourcePitchBend;
        patch.mod[row].destination = destinations[row / 2]; patch.mod[row].amount = 63;
    }
    for (int note = 1; note <= 127; ++note) for (int range = -48; range <= 48; ++range)
        for (int octave = -2; octave <= 2; ++octave) for (int tuning : {-127, 0, 127})
            for (uint16_t bend : {uint16_t{0}, uint16_t{8192}, uint16_t{16383}})
                for (bool duo : {false, true}) {
                    patch.osc[0].range = patch.osc[1].range = static_cast<int8_t>(range);
                    patch.osc[0].option = duo ? avrlib_hd::kMixOpDuo : avrlib_hd::kMixOpSum;
                    patch.osc[1].option = 127;
                    sys.octave = static_cast<int8_t>(octave); sys.master_tuning = static_cast<int8_t>(tuning);
                    voice.NoteOn(static_cast<uint16_t>(note * 128), 127, 0, true);
                    voice.set_bass_note(static_cast<int16_t>((128 - note) * 128));
                    voice.PitchBend(bend); voice.ProcessBlock(patch, sys);
                    for (int osc = 0; osc < 2; ++osc) {
                        const int base = duo && osc == 0 ? (128 - note) * 128 : note * 128;
                        const int pitch = base + floorDivide(voice.debug_destination14(destinations[0]) - 8192, 16)
                            + floorDivide(voice.debug_destination14(destinations[1]) - 8192, 128) + tuning
                            + (range + octave * 12) * 128 + (osc == 1 ? 127 : 0)
                            + floorDivide(voice.debug_destination14(destinations[osc + 2]) - 8192, 4);
                        const auto expected = reference(pitch, table);
                        require(voice.oscillator(osc)->phase_increment_for_tests() == expected.increment / 256u,
                                "real voice increment matches bounded independent lookup");
                    }
                    for (int s = 0; s < avrlib_hd::kAudioBlockSize; ++s)
                        audioHash = (audioHash ^ voice.output_buffer()[s]) * UINT64_C(1099511628211);
                    ++cases;
                }
    std::printf("Pitch reference indices=%d..%d interpolation reads=0..96 voice cases=%llu hash=%016llx\n",
        minIndex, maxIndex, static_cast<unsigned long long>(cases), static_cast<unsigned long long>(audioHash));
}
void set(SwaraXtAudioProcessor& p, const char* id, float value) {
    auto* parameter = p.getApvts().getParameter(id); require(parameter != nullptr, "parameter exists");
    parameter->setValueNotifyingHost(parameter->convertTo0to1(value));
}
void exactParameter(SwaraXtAudioProcessor& p, const char* id, int expected, const char* message) {
    const float actual = p.getApvts().getRawParameterValue(id)->load();
    const float referenceValue = static_cast<float>(expected);
    require(std::memcmp(&actual, &referenceValue, sizeof(actual)) == 0, message);
}
void process(SwaraXtAudioProcessor& p, int note = 1, int count = 12, int block = 256) {
    juce::AudioBuffer<float> audio(2, block); juce::MidiBuffer midi;
    const int latency = p.getLatencySamples();
    for (int n = 0; n < count; ++n) {
        midi.clear(); if (n == 0) midi.addEvent(juce::MidiMessage::noteOn(1, note, static_cast<juce::uint8>(127)), 0);
        p.processBlock(audio, midi);
        for (int ch = 0; ch < 2; ++ch) for (int s = 0; s < block; ++s) {
            const float sample = audio.getSample(ch, s);
            require(std::isfinite(sample), "admitted pitch generates finite plugin audio");
        }
    }
    require(latency == p.getLatencySamples(), "pitch inputs preserve latency");
    for (int i = 0; i < 2; ++i) require(p.engineForTests().shruthiPart().patch().osc[i].range >= -48
        && p.engineForTests().shruthiPart().patch().osc[i].range <= 48, "actual engine patch range admitted");
    ++cases;
}
void restoredAdmission() {
    SwaraXtAudioProcessor p; p.prepareToPlay(48000, 256);
    for (const char* id : {swaraxt::IDs::osc1Range, swaraxt::IDs::osc2Range}) {
        for (int value = -128; value <= 127; ++value) {
            set(p, id, static_cast<float>(value));
            exactParameter(p, id, std::clamp(value, -48, 48), "automation admission preserves every valid value");
        }
        for (float normalized : {-1.0f, 2.0f, -std::numeric_limits<float>::infinity(),
                                  std::numeric_limits<float>::infinity(), std::numeric_limits<float>::quiet_NaN()}) {
            p.getApvts().getParameter(id)->setValueNotifyingHost(normalized);
            const float admitted = p.getApvts().getRawParameterValue(id)->load();
            require(std::isfinite(admitted) && admitted >= -48 && admitted <= 48,
                    "malformed normalized automation remains in the pitch domain");
            process(p);
        }
        // External state is XML text. Inject non-finite spellings directly so
        // fixture creation does not ask JUCE's numeric serializer to cast NaN.
        for (const char* value : {"-1e300", "-128", "-49", "-48", "0", "48", "49", "127", "1e300",
                                  "-inf", "inf", "nan"}) {
            auto tree = p.getApvts().copyState(); tree.setProperty("stateVersion", 1, nullptr);
            tree.getChildWithProperty("id", id).setProperty("value", value, nullptr);
            juce::MemoryBlock binary; const auto xml = tree.createXml();
            juce::AudioProcessor::copyXmlToBinary(*xml, binary);
            p.setStateInformation(binary.getData(), static_cast<int>(binary.getSize()));
            const float admitted = p.getApvts().getRawParameterValue(id)->load();
            require(std::isfinite(admitted) && admitted >= -48 && admitted <= 48, "malformed state remains in pitch domain");
            process(p);
        }
    }
    for (const auto& record : swaraxt::kShruthiFactoryPresets) {
        shruthi::Patch patch;
        require(swaraxt::ShruthiFactoryPresets::decodePatch(record.bytes.data(), record.bytes.size(), patch), "legacy factory decode");
        swaraxt::ShruthiFactoryPresets::applyPatchToApvts(patch, p.getApvts()); process(p, 127);
    }
    for (int value = -128; value <= 127; ++value) {
        auto bytes = swaraxt::kShruthiFactoryPresets[0].bytes;
        bytes[2] = bytes[6] = static_cast<uint8_t>(value); bytes[91] = '!';
        shruthi::Patch patch;
        require(swaraxt::ShruthiFactoryPresets::decodePatch(bytes.data(), bytes.size(), patch), "range bytes accepted by legacy decoder");
        swaraxt::ShruthiFactoryPresets::applyPatchToApvts(patch, p.getApvts());
        exactParameter(p, swaraxt::IDs::osc1Range, std::clamp(value, -48, 48),
                       "legacy decode reaches the existing APVTS range boundary"); process(p);
    }
    SwaraXtAudioProcessor factory;
    factory.prepareToPlay(48000, 256);
    for (int program = 0; program < factory.getNumPrograms(); ++program) {
        factory.setCurrentProgram(program);
        require(factory.getCurrentProgram() == program, "factory program selection executed");
        process(factory);
    }
    std::printf("Automation, malformed host state, 40 legacy presets, all256 range bytes and factory programs: PASS\n");
}
void admittedAudio() {
    for (int mode = 0; mode < 3; ++mode) {
        SwaraXtAudioProcessor malformed, bounded;
        for (auto* p : {&malformed, &bounded}) {
            p->prepareToPlay(48000, 256);
            set(*p, swaraxt::IDs::osc1Range, -48); set(*p, swaraxt::IDs::osc2Range, 48);
            set(*p, swaraxt::IDs::perfGlide, 127); set(*p, swaraxt::IDs::perfLegato, 1);
            p->setExperimentalSvfEnabled(mode != 0);
            p->setExperimentalSvfCalibration(static_cast<swaraxt::ShruthiSvfCalibration>(mode == 2));
        }
        set(malformed, swaraxt::IDs::osc1Range, -128); set(malformed, swaraxt::IDs::osc2Range, 127);
        juce::AudioBuffer<float> a(2, 256), b(2, 256); juce::MidiBuffer am, bm;
        for (int block = 0; block < 512; ++block) {
            am.clear(); bm.clear();
            if (block % 64 == 0) {
                const auto message = juce::MidiMessage::noteOn(1, block & 64 ? 127 : 1, static_cast<juce::uint8>(127));
                am.addEvent(message, 0); bm.addEvent(message, 0);
            }
            malformed.processBlock(a, am); bounded.processBlock(b, bm);
            for (int channel = 0; channel < 2; ++channel)
                require(std::memcmp(a.getReadPointer(channel), b.getReadPointer(channel), 256 * sizeof(float)) == 0,
                        "invalid range admission equals the published endpoint audio bit-for-bit");
        }
        ++cases;
    }
    std::printf("Invalid range admission vs published endpoints: 1536 paired plugin blocks bit-exact, Classic/Paper/Eagle\n");
}
void midiAdmission() {
    SwaraXtAudioProcessor p; p.prepareToPlay(48000, 256);
    auto& part = p.engineForTests().shruthiPart();
    juce::AudioBuffer<float> audio(2, 256); juce::MidiBuffer midi;
    for (int raw = 0; raw < 256; ++raw) {
        midi.clear(); midi.addEvent(juce::MidiMessage::controllerEvent(1, 22, raw & 127), 0);
        midi.addEvent(juce::MidiMessage::controllerEvent(1, 26, raw & 127), 0); p.processBlock(audio, midi);
        require(part.patch().osc[0].range >= -24 && part.patch().osc[0].range <= 24, "CC range scaled by firmware table");
    }
    const std::array<int, 4> offsets {{shruthi::PRM_OSC_RANGE_1, shruthi::PRM_OSC_RANGE_2,
                                     shruthi::PRM_SYS_OCTAVE, shruthi::PRM_SYS_MASTER_TUNING}};
    for (const auto offset : offsets) {
        for (int raw = 0; raw < 256; ++raw) {
            midi.clear(); p.processBlock(audio, midi);
            const uint8_t old = part.GetParameter(static_cast<uint8_t>(offset));
            midi.clear(); midi.addEvent(juce::MidiMessage::controllerEvent(1, 99, offset >> 7), 0);
            midi.addEvent(juce::MidiMessage::controllerEvent(1, 98, offset & 127), 1);
            midi.addEvent(juce::MidiMessage::controllerEvent(1, 6, raw >> 7), 2);
            midi.addEvent(juce::MidiMessage::controllerEvent(1, 38, raw & 127), 3); p.processBlock(audio, midi);
            const auto& parameter = shruthi::ParameterManager::parameter(shruthi::ParameterManager::at_offset(static_cast<uint8_t>(offset)));
            const uint8_t code = static_cast<uint8_t>(raw);
            const bool valid = parameter.Clamp(code) == code;
            require(part.GetParameter(static_cast<uint8_t>(offset)) == (valid ? code : old), "real NRPN validates signed bytes before assignment");
            ++cases;
        }
    }
    // System raga is reachable via NRPN; prove tuned notes remain inside14bits.
    int minPitch = 16383, maxPitch = 0;
    for (int scale = 1; scale <= 32; ++scale) for (int note = 0; note <= 127; ++note) {
        const int shift = shruthi::ResourcesManager::Lookup<int16_t, uint8_t>(
            static_cast<shruthi::ResourceId>(LUT_RES_SCALE_JUST + scale - 1), static_cast<uint8_t>(note % 12));
        if (shift == 32767) continue;
        const int pitch = note * 128 + shift;
        require(pitch >= 0 && pitch <= 16383, "every playable raga pitch in14bit domain");
        minPitch = std::min(minPitch, pitch); maxPitch = std::max(maxPitch, pitch);
    }
    std::printf("All CC/NRPN byte values: PASS; all32 raga tables x128 notes pitch=%d..%d\n", minPitch, maxPitch);
    for (double rate : {8000.,22050.,44100.,48000.,88200.,96000.,176400.,192000.,384000.})
        for (int block : {1,64,256,1024}) {
            SwaraXtAudioProcessor matrix; matrix.prepareToPlay(rate, block);
            set(matrix, swaraxt::IDs::osc1Range, -128); set(matrix, swaraxt::IDs::osc2Range, 127);
            set(matrix, swaraxt::IDs::osc1Option, shruthi::OP_DUO);
            set(matrix, swaraxt::IDs::perfGlide, 127); set(matrix, swaraxt::IDs::perfLegato, 1);
            process(matrix, 1, 16, block); process(matrix, 127, 16, block);
        }
}
} // namespace
int main() {
    juce::ScopedJuceInitialiser_GUI init;
    lookupDomain(); restoredAdmission(); midiAdmission(); admittedAudio();
    std::printf("Pitch input qualification: PASS cases=%llu\n", static_cast<unsigned long long>(cases));
    return 0;
}
