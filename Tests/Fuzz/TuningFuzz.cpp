// Copyright 2026 MontroneDSP. SPDX-License-Identifier: GPL-3.0-or-later
#include "Tuning/TuningTable.h"
#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <cmath>
#include <string>
#include <vector>

extern "C" int LLVMFuzzerTestOneInput(const uint8_t* data, size_t size)
{
    if (size > 4096) return 0;
    using namespace swaraxt::tuning;
    TuningSnapshot snapshot;
    ParseError error;
    auto roundtrip = [&] {
        std::vector<uint8_t> bytes;
        if (!writeTuningTable(snapshot, bytes, error)) std::abort();
        TuningSnapshot restored;
        if (!readTuningTable(bytes.data(), bytes.size(), restored, error)) std::abort();
        for (int note = 0; note < kMidiNotes; ++note) {
            if (snapshot.mapped(note) != restored.mapped(note)) std::abort();
            // SWT accepts a bounded cached-frequency error; the writer rebuilds
            // that cache. Canonicalization need not preserve incoming cache bits.
            const double expected = restored.hzForMidi(note);
            if (std::abs(snapshot.hzForMidi(note) - expected)
                > 1.0e-9 * std::max(1.0, expected)) std::abort();
        }
        std::vector<uint8_t> canonical;
        if (!writeTuningTable(restored, canonical, error) || canonical != bytes) std::abort();
    };
    if (readTuningTable(data, size, snapshot, error)) roundtrip();
    const std::string text(reinterpret_cast<const char*>(data), size);
    if (parseScalaAndKeyboard(text.c_str(), nullptr, snapshot, error)) roundtrip();
    // A fixed independent scale drives arbitrary KBM parsing as well.
    constexpr const char* scale = "fuzz scale\n2\n700.0\n1200.0\n";
    if (parseScalaAndKeyboard(scale, text.c_str(), snapshot, error)) roundtrip();
    return 0;
}
