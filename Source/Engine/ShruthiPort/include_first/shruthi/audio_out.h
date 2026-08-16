// Copyright 2026 MontroneDSP.
// SPDX-License-Identifier: GPL-3.0-or-later
// Host audio ring buffer replacing Shruthi PWM audio output.

#ifndef SWARAXT_SHRUTHI_AUDIO_OUT_H_
#define SWARAXT_SHRUTHI_AUDIO_OUT_H_

#include <algorithm>
#include <cstdint>

#include "shruthi/shruthi.h"

namespace shruthi {

// Enlarged power-of-two ring so multiple internal ProcessBlock writes cannot
// wrap unread host-drain pointers under irregular host block sizes.
static constexpr int kHostAudioRingSize = 512;

class HostAudioRing {
 public:
    enum {
        buffer_size = kHostAudioRingSize,
        data_size = 1
    };
    typedef uint8_t Value;

    void Init();
    void Overwrite(uint8_t sample);
    uint16_t writable() const;
    uint16_t writable_block() const;
    uint16_t readable() const;
    uint8_t ImmediateRead();

    int readBlockRaw(float* destination, int maxSamples);
    int readBlock(float* destination,
                  int maxSamples,
                  uint8_t vca,
                  float headroom);

private:
    uint8_t ring_[kHostAudioRingSize] {};
    uint16_t readPtr_ = 0;
    uint16_t writePtr_ = 0;
};

}  // namespace shruthi

#endif  // SWARAXT_SHRUTHI_AUDIO_OUT_H_
