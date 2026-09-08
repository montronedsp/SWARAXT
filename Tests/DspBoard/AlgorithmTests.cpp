// Copyright 2026 MontroneDSP. SPDX-License-Identifier: GPL-3.0-or-later
#include "Engine/DspBoard/BoardFilter.h"
#include "Engine/DspBoard/BoardEffects.h"
#include "Engine/DspBoard/BoardResources.h"
#include "dsp/fx_engine.h"
#include "dsp/buffers.h"
#include "dsp/resources.h"
#include <cmath>
#include <iostream>
#include <stdexcept>
#include <string>

namespace dsp {
ReferenceBuffer<uint8_t> input_buffer;
ReferenceBuffer<uint16_t> output_buffer;
}
namespace {
using namespace swaraxt::board;
void require(bool ok, const char* label) { if (!ok) throw std::runtime_error(label); }
std::int32_t floorDivide(std::int64_t numerator, std::int64_t denominator)
{
    const auto quotient = numerator / denominator;
    return static_cast<std::int32_t>(quotient - (numerator < 0 && numerator % denominator != 0 ? 1 : 0));
}
void arithmeticTests()
{
    using namespace arithmetic;
    for (int a = -32768; a <= 32767; ++a)
    {
        require(s16(a) == a, "signed16 decode");
        for (int b = 0; b < 256; ++b)
        {
            const auto sa = static_cast<std::int16_t>(a);
            const auto ub = static_cast<std::uint8_t>(b);
            const auto sb = static_cast<std::int8_t>(b - 128);
            require(S16U8MulShift8(sa, ub) == floorDivide(static_cast<std::int64_t>(a) * b, 256), "signed16 x unsigned8 high");
            require(S16S8MulShift8(sa, sb) == floorDivide(static_cast<std::int64_t>(a) * (b - 128), 256), "signed16 x signed8 high");
            const auto u16 = static_cast<std::uint16_t>((b << 8) | (a & 255));
            require(S16U16MulShift16(sa, u16) == floorDivide(static_cast<std::int64_t>(a) * u16, 65536), "signed16 x unsigned16 high");
        }
        for (unsigned shiftCount = 0; shiftCount <= 16; ++shiftCount)
            require(shift(a, shiftCount) == floorDivide(a, std::int64_t{1} << shiftCount), "arithmetic shift");
        require(S16ClipS8(static_cast<std::int16_t>(a)) == std::clamp(a, -128, 127), "clip8");
    }
    for (int a = 0; a < 256; ++a)
        for (int b = 0; b < 256; ++b)
        {
            const auto ua = static_cast<std::uint8_t>(a), ub = static_cast<std::uint8_t>(b);
            require(U8U8MulShift8(ua, ub) == (a * b) / 256, "unsigned8 high");
            require(S8U8MulShift8(s8(a), ub) == floorDivide((a < 128 ? a : a - 256) * b, 256), "signed8 high");
            for (int c = 0; c < 256; ++c)
            {
                const auto balance = static_cast<std::uint8_t>(c);
                const auto sum = a * (255 - c) + b * c;
                require(U8MixU16(ua, ub, balance) == sum, "interpolation full");
                require(U8Mix(ua, ub, balance) == sum / 256, "interpolation high");
            }
        }
    for (unsigned code = 0; code < 256; ++code)
        require(fromAdc(static_cast<std::uint8_t>(code)) == (static_cast<int>(code) - 128) * 16, "ADC expansion");
    for (int code = -10; code < 30; ++code)
        require(choiceFromEffect(effectFromChoice(code)) == (code >= 1 && code <= 16 ? code : 0), "Off/source code mapping");
    require(phaseAdd(0xffffffu, 1) == 0, "24bit wrap");
    std::cout << "Arithmetic exhaustive 8-bit/16x8, stratified 16x16: PASS\n";
}
template<class T, std::size_t N>
void table(const T (&ported)[N], const T* original, const char* name)
{
    std::uint64_t hash = 14695981039346656037ull;
    for (std::size_t i = 0; i < N; ++i)
    {
        require(ported[i] == original[i], name);
        for (std::size_t byte = 0; byte < sizeof(T); ++byte)
        { hash ^= (ported[i] >> (byte * 8)) & 255u; hash *= 1099511628211ull; }
    }
    std::cout << name << " size=" << N << " FNV1aLE=" << std::hex << hash << std::dec << " PASS\n";
}
void resourceTests()
{
#define CHECK_TABLE(name) table(resources::name, dsp::name, #name)
    CHECK_TABLE(waveform_res_resonance_response); CHECK_TABLE(waveform_res_sine);
    CHECK_TABLE(lut_res_distortion); CHECK_TABLE(lut_res_fold); CHECK_TABLE(lut_res_integrator_gain);
    CHECK_TABLE(lut_res_comb_delays); CHECK_TABLE(lut_res_phase_increment);
    CHECK_TABLE(lut_res_delay_duration); CHECK_TABLE(lut_res_delay_decimation); CHECK_TABLE(lut_res_delay_filter_gain);
    CHECK_TABLE(lut_res_delay_phase_scaling); CHECK_TABLE(lut_res_tap_delay_duration);
    CHECK_TABLE(lut_res_tap_delay_decimation); CHECK_TABLE(lut_res_tap_delay_filter_gain); CHECK_TABLE(lut_res_pitch_ratio);
#undef CHECK_TABLE
}
void parityTests()
{
    unsigned cases = 0;
    for (std::uint8_t route = 0; route < 5; ++route)
        for (std::uint8_t fx = 0; fx < 16; ++fx)
            for (unsigned history = 0; history < 6; ++history)
            {
                BoardFilter filter;
                BoardEffects effects;
                dsp::FxEngine::Init();
                std::uint32_t random = 0x21;
                for (int block = 0; block < 300; ++block)
                {
                    const auto cv1 = static_cast<std::uint8_t>(history == 0 ? 0 : history == 1 ? 255 : history == 2 ? 128 : (block / 13 * 19));
                    const auto cv2 = static_cast<std::uint8_t>(history == 0 ? 0 : history == 1 ? 255 : history == 2 ? 96 : (block / 50 % 2 ? 210 : 32));
                    const auto cutoff = static_cast<std::uint8_t>(history < 3 ? 254 : block * 3);
                    const auto resonance = static_cast<std::uint8_t>(history == 1 ? 255 : history == 2 ? 128 : block / 2);
                    const auto vca = static_cast<std::uint8_t>(history == 0 ? 0 : history == 1 ? 255 : block < 220 ? 254 : 0);
                    const auto tempo = static_cast<std::uint8_t>(history < 3 ? 120 : block);
                    dsp::FxEngine::set_mode(route); dsp::FxEngine::set_fx_program(fx);
                    dsp::FxEngine::set_cv(dsp::CV_1, cv1); dsp::FxEngine::set_cv(dsp::CV_2, cv2);
                    dsp::FxEngine::set_cv(dsp::CV_CUTOFF, cutoff); dsp::FxEngine::set_cv(dsp::CV_RESONANCE, resonance);
                    dsp::FxEngine::set_cv(dsp::CV_VCA, vca); dsp::FxEngine::set_tempo(tempo);
                    dsp::input_buffer.cursor = dsp::output_buffer.cursor = 0;
                    Block samples{};
                    for (std::size_t i = 0; i < blockSize; ++i)
                    {
                        random = random * 1664525u + 1013904223u;
                        const auto input = static_cast<std::uint8_t>(history == 0 || block >= 220 ? 128 : history == 1 ? (i & 1 ? 255 : 0) : random >> 24);
                        dsp::input_buffer.data[i] = input;
                        samples[i] = arithmetic::fromAdc(input);
                    }
                    dsp::FxEngine::ProcessBlock();
                    if (route < 2) filter.process(samples, cutoff, resonance, (route & 1) != 0);
                    BoardFilter::dca(samples, vca);
                    effects.process(samples, static_cast<Effect>(fx), cv1, cv2, tempo);
                    if (route == 2 || route == 3) filter.process(samples, cutoff, resonance, (route & 1) != 0);
                    for (std::size_t i = 0; i < blockSize; ++i)
                    {
                        const auto expected = static_cast<int>(dsp::output_buffer.data[i]) - 2048;
                        if (samples[i] != expected)
                        {
                            std::cerr << "route=" << int(route) << " fx=" << int(fx) << " history=" << history << " block=" << block << " sample=" << i << " actual=" << samples[i] << " expected=" << expected << '\n';
                            throw std::runtime_error("source sample parity");
                        }
                    }
                }
                ++cases;
            }
    std::cout << "Filter/DCA/16 FX/5 routes/control histories: " << cases << " PASS, max diff=0\n";
}
}
int main()
{
    try { arithmeticTests(); resourceTests(); parityTests(); }
    catch (const std::exception& e) { std::cerr << "FAIL: " << e.what() << '\n'; return 1; }
    return 0;
}
