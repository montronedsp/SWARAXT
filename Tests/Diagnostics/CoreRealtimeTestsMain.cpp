// Copyright 2026 MontroneDSP. SPDX-License-Identifier: GPL-3.0-or-later
#include "avrlib/random.h"
#include "shruthi/audio_out.h"
#include "shruthi/midi_dispatcher.h"
#include "shruthi/part.h"
#include "shruthi/storage.h"
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <string>

struct Runtime {
    shruthi::HostAudioRing ring;
    avrlib::Random random;
    shruthi::MidiDispatcher midi;
    shruthi::Storage storage;
    shruthi::Part part;
    Runtime() {
        random.Seed(0x21);
        ring.Init();
        part.Init(ring, random, midi, storage);
    }
};
static int render(Runtime& runtime, float* out, int block) [[clang::nonblocking]]
{
    if (block % 128 == 0) runtime.part.NoteOn(0, block % 256 == 0 ? 0 : 127, 100);
    if (block % 128 == 120) runtime.part.NoteOff(0, block % 256 < 128 ? 0 : 127);
    runtime.part.ProcessBlock();
    return runtime.ring.readBlock(out, 40, runtime.part.voice().vca(), .85f);
}
int main(int argc, char** argv)
{
    if (argc != 2) return 2;
    const std::string scenario(argv[1]);
    if (scenario != "notes" && scenario != "arp" && scenario != "modulation") return 2;
    Runtime runtime;
    if (scenario == "arp")
        runtime.part.mutable_sequencer_settings()->seq_mode = shruthi::SEQUENCER_MODE_ARP;
    if (scenario == "modulation") {
        auto& row = runtime.part.mutable_patch()->modulation_matrix.modulation[0];
        row.source = shruthi::MOD_SRC_OFFSET;
        row.destination = shruthi::MOD_DST_PWM_1;
        row.amount = 63;
    }
    float peak = 0;
    for (int block = 0; block < 2048; ++block) {
        float samples[40] {};
        const int count = render(runtime, samples, block);
        if (count != 40) return 1;
        for (float value : samples) {
            if (!std::isfinite(value)) return 1;
            peak = std::max(peak, std::abs(value));
        }
    }
    std::printf("Core RTSan scenario=%s peak=%g\n", scenario.c_str(), static_cast<double>(peak));
    return peak > 0 ? 0 : 1;
}
