// Copyright 2026 MontroneDSP.
// SPDX-License-Identifier: GPL-3.0-or-later

#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <thread>

#include "avrlib/op.h"
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

uint8_t vcaMixBalanceDefined(int8_t amount)
{
    return static_cast<uint8_t>(static_cast<int16_t>(amount) * 4);
}

uint8_t vcaMixBalanceAvrReference(int8_t amount)
{
    const int scaled = static_cast<int>(amount) * 4;
    return static_cast<uint8_t>(scaled & 0xff);
}

void testVcaMixBalanceConversion()
{
    expectTrue(vcaMixBalanceDefined(63) == 252, "VCA mix balance 63 -> 252");
    expectTrue(vcaMixBalanceDefined(1) == 4, "VCA mix balance 1 -> 4");
    expectTrue(vcaMixBalanceDefined(-1) == 252, "VCA mix balance -1 -> 252");
    expectTrue(vcaMixBalanceDefined(-64) == 0, "VCA mix balance -64 -> 0");
    expectTrue(vcaMixBalanceDefined(static_cast<int8_t>(-128)) == 0,
               "VCA mix balance -128 -> 0");

    for (int i = -128; i <= 127; ++i)
    {
        const int8_t amount = static_cast<int8_t>(i);
        expectTrue(vcaMixBalanceDefined(amount) == vcaMixBalanceAvrReference(amount),
                   "defined VCA mix balance matches AVR modulo-256 reference");
    }

    ShruthiRuntime runtime;
    runtime.init();
    auto* patch = runtime.part.mutable_patch();
    for (int row = 0; row < shruthi::kModulationMatrixSize; ++row)
    {
        patch->modulation_matrix.modulation[row].source = shruthi::MOD_SRC_OFFSET;
        patch->modulation_matrix.modulation[row].destination = shruthi::MOD_DST_VCA;
        patch->modulation_matrix.modulation[row].amount = 0;
    }
    patch->modulation_matrix.modulation[0].source = shruthi::MOD_SRC_OFFSET;
    patch->modulation_matrix.modulation[0].destination = shruthi::MOD_DST_VCA;
    patch->modulation_matrix.modulation[0].amount = static_cast<int8_t>(-128);
    runtime.part.mutable_voice()->ProcessControlBlock();
    const uint8_t inverted = static_cast<uint8_t>(255 - 255);
    const uint8_t mixed = avrlib::U8Mix(255, inverted, vcaMixBalanceDefined(static_cast<int8_t>(-128)));
    const uint8_t expectedVca = avrlib::U8U8MulShift8(255, mixed);
    const uint8_t vca = runtime.part.voice().vca();
    expectTrue(vca == expectedVca, "VCA amount -128 uses defined mix-balance wrap");
    std::printf("vca-mix-balance amount=-128 vca=%u expected=%u\n",
                static_cast<unsigned>(vca), static_cast<unsigned>(expectedVca));
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
    testVcaMixBalanceConversion();
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
