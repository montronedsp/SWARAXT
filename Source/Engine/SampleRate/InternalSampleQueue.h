// Copyright 2026 MontroneDSP.
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include <algorithm>
#include <array>
#include <cmath>

namespace swaraxt {

class InternalSampleQueue {
 public:
    static constexpr int kCapacity = 16384;

    void reset() noexcept;
    void setStep(double internalRate, double hostRate) noexcept;
    void push(float sample) noexcept;
    int minimumReadableSize() const noexcept;
    // How full the caller should keep the queue. It must exceed
    // minimumReadableSize(), otherwise the producer never refills and the
    // reader stalls on a permanently minimal queue.
    int queueTargetSize() const noexcept;
    float readInterpolated() noexcept;
    int size() const noexcept { return size_; }

 private:
    float at(int index) const noexcept;

    std::array<float, kCapacity> data_{};
    int readIndex_ = 0;
    int writeIndex_ = 0;
    int size_ = 0;
    double fraction_ = 0.0;
    double step_ = 1.0;
};

inline void InternalSampleQueue::reset() noexcept
{
    data_.fill(0.0f);
    readIndex_ = 0;
    writeIndex_ = 0;
    size_ = 0;
    fraction_ = 0.0;
}

inline void InternalSampleQueue::setStep(double internalRate, double hostRate) noexcept
{
    const double safeHost = hostRate > 1.0 ? hostRate : 44100.0;
    const double safeInternal = internalRate > 1.0 ? internalRate : safeHost;
    step_ = safeInternal / safeHost;
    if (! std::isfinite(step_) || step_ <= 0.0)
        step_ = 1.0;
    // Keep fractional phase in range after rate changes.
    if (! std::isfinite(fraction_) || fraction_ < 0.0)
        fraction_ = 0.0;
    while (fraction_ >= 1.0)
        fraction_ -= 1.0;
}

inline void InternalSampleQueue::push(float sample) noexcept
{
    if (size_ >= kCapacity - 4)
    {
        readIndex_ = (readIndex_ + 1) % kCapacity;
        --size_;
    }

    data_[static_cast<size_t>(writeIndex_)] = std::isfinite(sample) ? sample : 0.0f;
    writeIndex_ = (writeIndex_ + 1) % kCapacity;
    ++size_;
}

inline float InternalSampleQueue::at(int index) const noexcept
{
    const int position = (readIndex_ + index) % kCapacity;
    return data_[static_cast<size_t>(position)];
}

inline int InternalSampleQueue::minimumReadableSize() const noexcept
{
    if (! std::isfinite(fraction_) || fraction_ <= 0.0)
        return 4;

    return 4 + static_cast<int>(std::floor(fraction_));
}

inline int InternalSampleQueue::queueTargetSize() const noexcept
{
    return std::max(8, minimumReadableSize());
}

inline float InternalSampleQueue::readInterpolated() noexcept
{
    if (size_ < minimumReadableSize())
        return 0.0f;

    // Advance whole-sample phase. Bound iterations by how far fraction can run
    // in one host sample (step) plus a small safety margin — never unbounded.
    const int maxAdvances = 2 + static_cast<int>(std::ceil(step_)) + 8;
    int advances = 0;
    while (fraction_ >= 1.0 && size_ > 4 && advances < maxAdvances)
    {
        fraction_ -= 1.0;
        readIndex_ = (readIndex_ + 1) % kCapacity;
        --size_;
        ++advances;
    }
    if (fraction_ >= 1.0)
        return 0.0f;
    if (size_ < 4)
        return 0.0f;

    const float y0 = at(0);
    const float y1 = at(1);
    const float y2 = at(2);
    const float y3 = at(3);
    const float t = static_cast<float>(fraction_);

    const float c0 = y1;
    const float c1 = 0.5f * (y2 - y0);
    const float c2 = y0 - 2.5f * y1 + 2.0f * y2 - 0.5f * y3;
    const float c3 = 0.5f * (y3 - y0) + 1.5f * (y1 - y2);
    const float sample = ((c3 * t + c2) * t + c1) * t + c0;

    fraction_ += step_;
    return std::isfinite(sample) ? sample : 0.0f;
}

}  // namespace swaraxt
