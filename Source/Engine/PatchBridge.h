// Copyright 2026 MontroneDSP.
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

namespace shruthi {
class Part;
}  // namespace shruthi

namespace swaraxt {

struct ParameterCache;

class PatchBridge {
 public:
    void bind(shruthi::Part& part) noexcept
    {
        part_ = &part;
        invalidateArpRuntimeSync();
    }

    void applyCacheToEngine(const ParameterCache& cache);

    /** Call after Part::Init so arp_direction_ is force-synced on next apply. */
    void invalidateArpRuntimeSync() noexcept { arpRuntimeNeedsForceSync_ = true; }

 private:
    shruthi::Part* part_ = nullptr;
    bool arpRuntimeNeedsForceSync_ = true;
};

}  // namespace swaraxt
