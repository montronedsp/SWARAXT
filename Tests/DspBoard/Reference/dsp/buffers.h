// Test-only hardware queue replacement. Original DSP source stays unmodified.
#pragma once
#include "dsp/dsp.h"
#include <array>
#include <cstddef>
namespace dsp {
template<class T> struct ReferenceBuffer {
    std::array<T, kAudioBlockSize> data{};
    std::size_t cursor = 0;
    T ImmediateRead() { return data.at(cursor++); }
    void Overwrite(T value) { data.at(cursor++) = value; }
};
extern ReferenceBuffer<uint8_t> input_buffer;
extern ReferenceBuffer<uint16_t> output_buffer;
}
