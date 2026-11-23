// Copyright 2026 MontroneDSP.
// SPDX-License-Identifier: GPL-3.0-or-later

#include "shruthi/audio_out.h"

namespace shruthi {

void HostAudioRing::Init()
{
    readPtr_ = 0;
    writePtr_ = 0;
}

uint16_t HostAudioRing::writable() const
{
    return static_cast<uint16_t>((readPtr_ - writePtr_ - 1u) & (buffer_size - 1));
}

uint16_t HostAudioRing::writable_block() const
{
    return writable() >= kAudioBlockSize ? static_cast<uint16_t>(kAudioBlockSize) : 0;
}

void HostAudioRing::Overwrite(uint8_t sample)
{
    const uint16_t w = writePtr_;
    ring_[w & (buffer_size - 1)] = sample;
    writePtr_ = static_cast<uint16_t>((w + 1u) & (buffer_size - 1));
}

uint16_t HostAudioRing::readable() const
{
    return static_cast<uint16_t>((writePtr_ - readPtr_) & (buffer_size - 1));
}

uint8_t HostAudioRing::ImmediateRead()
{
    const uint16_t r = readPtr_;
    const uint8_t sample = ring_[r & (buffer_size - 1)];
    readPtr_ = static_cast<uint16_t>((r + 1u) & (buffer_size - 1));
    return sample;
}

int HostAudioRing::readBlockRaw(float* destination, int maxSamples)
{
    const int available = static_cast<int>(readable());
    const int count = std::min(maxSamples, available);

    for (int i = 0; i < count; ++i)
    {
        const uint8_t sample = ImmediateRead();
        destination[i] = (static_cast<float>(sample) - 128.0f) / 128.0f;
    }

    return count;
}

int HostAudioRing::readBlock(float* destination,
                             int maxSamples,
                             uint8_t vca,
                             float headroom)
{
    const float vcaGain = static_cast<float>(vca) / 255.0f;
    const int count = readBlockRaw(destination, maxSamples);
    for (int i = 0; i < count; ++i)
        destination[i] *= vcaGain * headroom;
    return count;
}

}  // namespace shruthi
