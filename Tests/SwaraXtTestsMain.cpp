// Copyright 2026 MontroneDSP.
// SPDX-License-Identifier: GPL-3.0-or-later

#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstdio>
#include <thread>

#include "avrlib/random.h"
#include "shruthi/audio_out.h"
#include "shruthi/midi_dispatcher.h"
#include "shruthi/part.h"
#include "shruthi/storage.h"

namespace {

std::atomic<int> failures { 0 };

struct ShruthiRuntime {
    shruthi::HostAudioRing ring;
    avrlib::Random random;
    shruthi::MidiDispatcher midi;
    shruthi::Storage storage;
    shruthi::Part part;

    void init()
    {
        random.Seed(0x21);
        ring.Init();
        part.Init(ring, random, midi, storage);
        part.ProcessBlock();
        ring.Init();
    }
};

void expectTrue(bool value, const char* message)
{
    if (! value)
    {
        std::fprintf(stderr, "FAIL: %s\n", message);
        failures.fetch_add(1, std::memory_order_relaxed);
    }
}

float renderPeak(ShruthiRuntime& runtime, int blocks)
{
    float peak = 0.0f;
    for (int block = 0; block < blocks; ++block)
    {
        runtime.part.ProcessBlock();
        float temp[40];
        const int n = runtime.ring.readBlock(
            temp, 40, runtime.part.voice().vca(), 0.85f);
        for (int i = 0; i < n; ++i)
        {
            expectTrue(std::isfinite(temp[i]), "finite sample");
            peak = std::max(peak, std::abs(temp[i]));
        }
    }
    return peak;
}

void testSingleRuntimeEnergy()
{
    ShruthiRuntime runtime;
    runtime.init();
    runtime.part.NoteOn(0, 48, 100);
    const float peak = renderPeak(runtime, 16);
    expectTrue(peak > 0.01f, "note energy");
    std::printf("single-runtime peak=%f\n", peak);
}

void testIndependentPatchAndAudioState()
{
    ShruthiRuntime a;
    ShruthiRuntime b;
    a.init();
    b.init();

    a.part.mutable_patch()->osc[0].shape = shruthi::WAVEFORM_SAW;
    b.part.mutable_patch()->osc[0].shape = shruthi::WAVEFORM_TRIANGLE;
    expectTrue(a.part.patch().osc[0].shape == shruthi::WAVEFORM_SAW, "A patch shape");
    expectTrue(b.part.patch().osc[0].shape == shruthi::WAVEFORM_TRIANGLE, "B patch shape");

    a.part.NoteOn(0, 48, 100);
    b.part.NoteOn(0, 72, 100);
    const float peakA = renderPeak(a, 8);
    a.part.NoteOff(0, 48);
    const float peakB = renderPeak(b, 12);

    expectTrue(peakA > 0.01f, "A audio ring energy");
    expectTrue(peakB > 0.01f, "B continues after A release");
    expectTrue(a.part.patch().osc[0].shape != b.part.patch().osc[0].shape, "patches remain independent");
}

void testIndependentRandomState()
{
    ShruthiRuntime a;
    ShruthiRuntime b;
    a.init();
    b.init();

    expectTrue(a.random.state() == b.random.state(), "same seed state");
    const uint8_t byteA = a.random.GetByte();
    expectTrue(byteA != 0 || a.random.state() != b.random.state(), "A random advances");
    expectTrue(a.random.state() != b.random.state(), "B random unchanged");

    b.random.GetByte();
    expectTrue(a.random.state() == b.random.state(), "same seed reproducibility");
}

void testConcurrentRuntimes()
{
    float peakA = 0.0f;
    float peakB = 0.0f;

    std::thread threadA([&] {
        ShruthiRuntime runtime;
        runtime.init();
        runtime.part.NoteOn(0, 50, 100);
        peakA = renderPeak(runtime, 32);
    });

    std::thread threadB([&] {
        ShruthiRuntime runtime;
        runtime.init();
        runtime.part.NoteOn(0, 67, 100);
        peakB = renderPeak(runtime, 32);
    });

    threadA.join();
    threadB.join();

    expectTrue(peakA > 0.01f, "concurrent A energy");
    expectTrue(peakB > 0.01f, "concurrent B energy");
}

}  // namespace

int main()
{
    std::printf("SwaraXtTests start\n");
    std::fflush(stdout);

    expectTrue(std::abs((20000000.0 / 510.0) - 39215.686) < 0.01, "internal rate");
    expectTrue(shruthi::kAudioBlockSize == 40, "block size");

    testSingleRuntimeEnergy();
    testIndependentPatchAndAudioState();
    testIndependentRandomState();
    testConcurrentRuntimes();

    FILE* log = nullptr;
#if defined(_MSC_VER)
    fopen_s(&log, "artifacts/unit_test_log.txt", "w");
#else
    log = std::fopen("artifacts/unit_test_log.txt", "w");
#endif
    if (log != nullptr)
    {
        std::fprintf(log, "failures=%d\n", failures.load(std::memory_order_relaxed));
        std::fprintf(log, failures.load(std::memory_order_relaxed) == 0 ? "PASSED\n" : "FAILED\n");
        std::fclose(log);
    }

    std::printf(failures.load(std::memory_order_relaxed) == 0
                    ? "SwaraXtTests: all passed\n"
                    : "SwaraXtTests: failed\n");
    return failures.load(std::memory_order_relaxed) == 0 ? 0 : 1;
}
