// Copyright 2026 MontroneDSP. SPDX-License-Identifier: GPL-3.0-or-later
#include "avrlib/random.h"
#include "shruthi/audio_out.h"
#include "shruthi/midi_dispatcher.h"
#include "shruthi/part.h"
#include "shruthi/storage.h"
#include <cstdint>
#include <cstdio>

namespace
{
int failures = 0;
void check(bool value, const char *message)
{
    if (!value)
    {
        std::fprintf(stderr, "FAIL: %s\n", message);
        ++failures;
    }
}
struct Runtime
{
    shruthi::HostAudioRing ring;
    avrlib::Random random;
    shruthi::MidiDispatcher midi;
    shruthi::Storage storage;
    shruthi::Part part;
    Runtime()
    {
        random.Seed(0x21);
        ring.Init();
        part.Init(ring, random, midi, storage);
        auto *settings = part.mutable_sequencer_settings();
        settings->seq_mode = shruthi::SEQUENCER_MODE_ARP;
        settings->seq_tempo =
            0; // External clock, including before its first tick.
        settings->arp_range = 1;
    }
};
// Independently maintained ROM pattern contract; division is the bit oracle.
constexpr uint16_t patterns[]{21845, 30583, 62965, 46517, 28527,
                              2313,  18761, 21065, 18725, 56283,
                              30431, 43281, 54553, 28609, 53505};
bool gateFor(uint16_t pattern, unsigned step)
{
    if (step >= 16)
        return false;
    for (unsigned bit = 0; bit < step; ++bit)
        pattern = static_cast<uint16_t>(pattern / 2);
    return pattern % 2 != 0;
}
void tickToNextStep(shruthi::Part &part)
{
    const auto before = part.step();
    unsigned ticks = 0;
    do
    {
        part.Clock(false);
        ++ticks;
    } while (part.step() == before && ticks < 256);
    check(ticks <= static_cast<unsigned>(part.clock_ticks_per_step()),
          "external step advances within one division");
}
} // namespace
int main()
{
    for (unsigned pattern = 0; pattern < sizeof(patterns) / sizeof(patterns[0]);
         ++pattern)
    {
        Runtime runtime;
        auto &part = runtime.part;
        auto *settings = part.mutable_sequencer_settings();
        settings->arp_pattern = static_cast<uint8_t>(pattern);
        settings->pattern_size =
            255; // Exercise all stored step bytes through real Clock.
        part.Start(false);
        check(part.step() == 255, "start retains pre-clock sentinel");
        part.NoteOn(0, 60,
                    100); // Original application reproduction: shift by 255.
        check(part.step() == 255, "pre-clock note does not move musical time");
        check(part.voice().modulation_source(shruthi::MOD_SRC_STEP) == 0,
              "sentinel has no ROM pattern gate");
        check(part.generated_note_count_for_tests() == 0,
              "sentinel generates no arpeggiator note");
        for (unsigned step = 0; step < 255; ++step)
        {
            tickToNextStep(part);
            check(static_cast<unsigned>(part.step()) == step,
                  "clock preserves stored step order");
            check(
                part.voice().modulation_source(shruthi::MOD_SRC_STEP) ==
                    (gateFor(patterns[pattern], step) ? 255 : 0),
                "all step bytes use the independent bounded pattern contract");
        }
        part.NoteOff(0, 60);
        check(part.num_notes() == 0, "held note releases after boundary sweep");
        settings->pattern_size = 16;
        part.Start(false);
        part.Clock(false); // Clock first: previously defined normal input.
        part.NoteOn(0, 60, 100);
        for (unsigned step = 1; step < 64; ++step)
        {
            tickToNextStep(part);
            check(static_cast<unsigned>(part.step()) == step % 16u,
                  "normal 16-step sequence wraps unchanged");
            check(part.voice().modulation_source(shruthi::MOD_SRC_STEP) ==
                      (gateFor(patterns[pattern], step % 16) ? 255 : 0),
                  "normal arpeggiator pattern gates remain exact");
        }
        part.NoteOff(0, 60);
        part.Stop(false);
        check(part.generated_note_count_for_tests() == 0,
              "stop releases generated notes");
    }
    std::printf("Arpeggiator boundary contracts: %d failures\n", failures);
    return failures == 0 ? 0 : 1;
}
