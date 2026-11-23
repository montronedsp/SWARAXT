// Copyright 2026 MontroneDSP.
// SPDX-License-Identifier: GPL-3.0-or-later
//
// Upper-register waveform zones for the production HD oscillator.
// Notes that previously left the supported family now use the sine pad.
// Historical output below those notes is covered by the parity grid.

#include "avrlib_hd/oscillator.h"
#include "avrlib_hd/voice.h"
#include "shruthi/resources.h"

#ifdef _WIN32
#include <windows.h>
#elif defined(__linux__)
#include <sys/mman.h>
#include <sys/wait.h>
#include <unistd.h>
#include <csignal>
#include <cerrno>
#include <sys/resource.h>
#endif

#include <cstdint>
#include <cstdio>
#include <cstring>
#include <vector>
#include <utility>

namespace {

int gFailures = 0;

void expect(bool ok, const char* message) {
  if (!ok) {
    std::fprintf(stderr, "FAIL: %s\n", message);
    ++gFailures;
  }
}

constexpr int kNotes[] = {12, 24, 60, 95, 107, 108, 109, 123, 124, 125, 126, 127};
constexpr int kParams[] = {0, 1, 64, 127};
constexpr int kSizes[] = {
    256, 256, 257, 257, 257, 257, 257, 257, 257, 257,
    257, 257, 257, 257, 257, 257, 257, 257, 257, 257,
    257, 257, 257, 257, 63, 256,
};

int rawZone(int note) {
  const int a = static_cast<uint8_t>(note - 12);
  const int swapped = static_cast<uint8_t>((a << 4) | (a >> 4));
  return swapped & 0x0f;
}

int clampedZone(int note, int maximum) {
  const int zone = rawZone(note);
  return zone > maximum ? maximum : zone;
}

int legalNote(int note, int maximum) {
  const int nibble = (note - 12) & 0x0f;
  return 12 + (clampedZone(note, maximum) << 4) + nibble;
}

bool renderEqual(uint8_t shape, int noteA, int noteB, uint8_t parameter, uint32_t increment) {
  avrlib_hd::HdOscillator a;
  avrlib_hd::HdOscillator b;
  avrlib_hd::OscillatorTables tables;
  tables.waveform_table = shruthi::waveform_table;
  tables.sine = shruthi::wav_res_sine;
  tables.bandlimited_triangle_0 = shruthi::wav_res_bandlimited_triangle_0;
  tables.fm_frequency_ratios = shruthi::lut_res_fm_frequency_ratios;
  a.set_tables(tables);
  b.set_tables(tables);
  a.set_parameter(parameter);
  b.set_parameter(parameter);
  float outA[64];
  float outB[64];
  uint8_t syncA[64];
  uint8_t syncB[64];
  a.RenderNaiveFull(shape, static_cast<uint8_t>(noteA), increment, 64, outA, nullptr, syncA);
  b.RenderNaiveFull(shape, static_cast<uint8_t>(noteB), increment, 64, outB, nullptr, syncB);
  return std::memcmp(outA, outB, sizeof(outA)) == 0;
}

void testRendererZones() {
  const struct Shape {
    const char* name;
    uint8_t id;
    int base;
    int maximum;
    bool pwm;
  } shapes[] = {
      {"saw", avrlib_hd::kRealSaw, 10, 6, false},
      {"square", avrlib_hd::kRealSquare, 3, 6, false},
      {"triangle", avrlib_hd::kRealTriangle, 17, 6, false},
      {"pwm", avrlib_hd::kRealSquare, 11, 5, true},
  };
  for (const Shape& shape : shapes) {
    for (int note : kNotes) {
      for (int parameter : kParams) {
        if (shape.pwm && parameter == 0)
          continue;
        if (!shape.pwm && shape.id == avrlib_hd::kRealSquare && parameter != 0)
          continue;
        const int maximum = shape.maximum;
        const int raw = rawZone(note);
        const int first = clampedZone(note, maximum);
        const int second = first + 1 > maximum ? maximum : first + 1;
        const int slot1 = shape.base + first;
        const int slot2 = shape.base + second;
        expect(kSizes[slot1] >= 257 && kSizes[slot2] >= 257, "selected zones are full waveforms");
        expect(shruthi::waveform_table[slot1] != shruthi::wav_res_vowel_data, "first lookup is not the vowel rom");
        expect(shruthi::waveform_table[slot2] != shruthi::wav_res_vowel_data, "second lookup is not the vowel rom");
        const bool corrected = raw > maximum;
        if (corrected) {
          expect(first == maximum, "upper register uses the final zone");
          expect(shruthi::waveform_table[slot1] == shruthi::wav_res_sine, "final zone is the sine pad");
        }
        if (!shape.pwm) {
          const int reference = legalNote(note, maximum);
          if (!renderEqual(shape.id, note, reference, static_cast<uint8_t>(parameter), 0x010000u)) {
            std::fprintf(stderr, "FAIL: %s note %d param %d does not match the supported zone\n",
                         shape.name, note, parameter);
            ++gFailures;
          }
        }
      }
    }
  }
}

#ifdef _WIN32

struct GuardWave {
  uint8_t* block = nullptr;
  uint8_t* data = nullptr;
};

GuardWave guard(int bytes) {
  const size_t page = 4096;
  GuardWave wave;
  wave.block = static_cast<uint8_t*>(VirtualAlloc(nullptr, page * 2, MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE));
  DWORD previous = 0;
  VirtualProtect(wave.block + page, page, PAGE_NOACCESS, &previous);
  wave.data = wave.block + page - static_cast<size_t>(bytes);
  std::memset(wave.data, 128, static_cast<size_t>(bytes));
  return wave;
}

LONG CALLBACK onGuardFault(EXCEPTION_POINTERS* info) {
  if (info->ExceptionRecord->ExceptionCode == EXCEPTION_ACCESS_VIOLATION) {
    std::fprintf(stderr, "FAIL: guard-page read at %p\n",
                 reinterpret_cast<void*>(info->ExceptionRecord->ExceptionInformation[1]));
    ExitProcess(2);
  }
  return EXCEPTION_CONTINUE_SEARCH;
}

void renderGuarded(const uint8_t* const* table, uint8_t shape, int note, uint8_t parameter) {
  avrlib_hd::OscillatorTables tables;
  tables.waveform_table = table;
  tables.sine = table[2];
  tables.bandlimited_triangle_0 = table[17];
  avrlib_hd::HdOscillator osc;
  osc.set_tables(tables);
  osc.set_parameter(parameter);
  float out[320];
  uint8_t syncOut[320];
  osc.RenderNaiveFull(shape, static_cast<uint8_t>(note), 0x010000u, 320, out, nullptr, syncOut);
  for (float sample : out)
    expect(sample >= -1.0f && sample <= 1.0f, "guard-page render stays in range");
}

void testGuardPage() {
  AddVectoredExceptionHandler(1, onGuardFault);
  GuardWave full = guard(257);
  GuardWave blocked = guard(63);
  const uint8_t* table[26];
  for (int i = 0; i < 26; ++i)
    table[i] = full.data;
  table[24] = blocked.data;
  for (int note : {124, 125, 126, 127})
    renderGuarded(table, avrlib_hd::kRealTriangle, note, 0);
  table[24] = full.data;
  table[17] = blocked.data;
  table[18] = blocked.data;
  for (int note : {124, 125, 126, 127}) {
    renderGuarded(table, avrlib_hd::kRealSaw, note, 0);
    renderGuarded(table, avrlib_hd::kRealSquare, note, 1);
    renderGuarded(table, avrlib_hd::kRealSquare, note, 127);
  }
  for (int note : {108, 109, 123, 124, 127})
    renderGuarded(table, avrlib_hd::kRealSquare, note, 64);
  table[17] = full.data;
  table[18] = full.data;
  table[10] = blocked.data;
  for (int note : {124, 125, 126, 127})
    renderGuarded(table, avrlib_hd::kRealSquare, note, 0);
  std::printf("guard-page upper-register renders completed\n");
}

#elif defined(__linux__)

class GuardWave {
 public:
  explicit GuardWave(size_t bytes) {
    const long size = sysconf(_SC_PAGESIZE);
    if (size <= 0 || bytes > static_cast<size_t>(size)) {
      expect(false, "valid system page size");
      return;
    }
    page_ = static_cast<size_t>(size);
    void* allocation = mmap(nullptr, page_ * 2, PROT_READ | PROT_WRITE,
                            MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    if (allocation == MAP_FAILED) {
      expect(false, "guard allocation succeeds");
      return;
    }
    block_ = static_cast<uint8_t*>(allocation);
    if (mprotect(block_ + page_, page_, PROT_NONE) != 0) {
      expect(false, "guard protection succeeds");
      return;
    }
    data = block_ + page_ - bytes;
    std::memset(data, 128, bytes);
  }
  ~GuardWave() {
    if (block_ != nullptr)
      expect(munmap(block_, page_ * 2) == 0, "guard mapping released");
  }
  GuardWave(const GuardWave&) = delete;
  GuardWave& operator=(const GuardWave&) = delete;
  uint8_t* data = nullptr;
 private:
  uint8_t* block_ = nullptr;
  size_t page_ = 0;
};

template <typename Operation>
void checkChild(Operation operation, bool expectFault) {
  const pid_t pid = fork();
  if (pid < 0) {
    expect(false, "guard child creation succeeds");
    return;
  }
  if (pid == 0) {
    const rlimit noCore {0, 0};
    if (setrlimit(RLIMIT_CORE, &noCore) != 0)
      _exit(3);
    // The deliberate protection probe must terminate through the kernel signal,
    // including when a sanitizer otherwise installs its own signal handler.
    std::signal(SIGSEGV, SIG_DFL);
    alarm(5);
    const int before = gFailures;
    operation();
    _exit(gFailures == before ? 0 : 1);
  }
  int status = 0;
  pid_t waited;
  do { waited = waitpid(pid, &status, 0); } while (waited < 0 && errno == EINTR);
  expect(waited == pid, "guard child reaped");
  if (waited != pid)
    return;
  const bool valid = expectFault ? WIFSIGNALED(status) && WTERMSIG(status) == SIGSEGV
                                : WIFEXITED(status) && WEXITSTATUS(status) == 0;
  if (!valid)
    std::fprintf(stderr, "guard child status=%d expected=%s\n", status,
                 expectFault ? "SIGSEGV" : "exit 0");
  expect(valid, expectFault ? "protected access raises exactly SIGSEGV"
                           : "production guard render exits successfully");
}

void renderGuarded(const uint8_t* const* table, uint8_t shape, int note, uint8_t parameter) {
  avrlib_hd::OscillatorTables tables;
  tables.waveform_table = table;
  tables.sine = table[2];
  tables.bandlimited_triangle_0 = table[17];
  avrlib_hd::HdOscillator osc;
  osc.set_tables(tables);
  osc.set_parameter(parameter);
  float out[320];
  uint8_t syncOut[320];
  osc.RenderNaiveFull(shape, static_cast<uint8_t>(note), 0x010000u, 320, out, nullptr, syncOut);
  for (float sample : out)
    expect(sample >= -1.0f && sample <= 1.0f, "guard-page render stays in range");
}

void testGuardPage() {
  GuardWave full(257), blocked(63);
  if (full.data == nullptr || blocked.data == nullptr)
    return; // Allocation/protection errors have already failed the test.
  // Independent negative controls prove the first byte beyond each ROM extent
  // is inaccessible; the preceding byte remains readable.
  for (const auto& probe : {std::make_pair(full.data, 257), std::make_pair(blocked.data, 63)}) {
    checkChild([&] { volatile uint8_t value = probe.first[probe.second - 1]; (void)value; }, false);
    checkChild([&] { volatile uint8_t value = probe.first[probe.second]; (void)value; }, true);
  }
  const uint8_t* table[26];
  for (auto& entry : table) entry = full.data;
  auto render = [&](uint8_t shape, int note, uint8_t parameter) {
    checkChild([&] { renderGuarded(table, shape, note, parameter); }, false);
  };
  table[24] = blocked.data;
  for (int note : {124, 125, 126, 127}) render(avrlib_hd::kRealTriangle, note, 0);
  table[24] = full.data;
  table[17] = table[18] = blocked.data;
  for (int note : {124, 125, 126, 127}) {
    render(avrlib_hd::kRealSaw, note, 0);
    render(avrlib_hd::kRealSquare, note, 1);
    render(avrlib_hd::kRealSquare, note, 127);
  }
  for (int note : {108, 109, 123, 124, 127}) render(avrlib_hd::kRealSquare, note, 64);
  table[17] = table[18] = full.data;
  table[10] = blocked.data;
  for (int note : {124, 125, 126, 127}) render(avrlib_hd::kRealSquare, note, 0);
  std::printf("Linux guard-page controls and upper-register renders completed\n");
}

#endif

avrlib_hd::OscillatorTables voiceTables() {
  avrlib_hd::OscillatorTables tables;
  tables.waveform_table = shruthi::waveform_table;
  tables.sine = shruthi::wav_res_sine;
  tables.waves = shruthi::wav_res_waves;
  tables.wavetables = shruthi::wav_res_wavetables;
  tables.fm_frequency_ratios = shruthi::lut_res_fm_frequency_ratios;
  tables.vowel_data = shruthi::wav_res_vowel_data;
  tables.formant_sine = shruthi::wav_res_formant_sine;
  tables.formant_square = shruthi::wav_res_formant_square;
  tables.bandlimited_triangle_0 = shruthi::wav_res_bandlimited_triangle_0;
  return tables;
}

struct VoiceRun {
  std::vector<uint8_t> osc1;
  std::vector<uint8_t> osc2;
};

VoiceRun play(uint8_t shape1, uint8_t shape2, int note, int range1, int range2,
              uint8_t param1, uint8_t param2, bool bend) {
  const auto osc = voiceTables();
  avrlib_hd::EnvelopeTables envelopes;
  envelopes.portamento_increments = shruthi::lut_res_env_portamento_increments;
  envelopes.env_expo = shruthi::wav_res_env_expo;
  avrlib_hd::HdVoiceTables voice;
  voice.oscillator_increments = shruthi::lut_res_oscillator_increments;
  voice.portamento_increments = shruthi::lut_res_env_portamento_increments;
  voice.distortion = shruthi::wav_res_distortion;
  avrlib_hd::Random random;
  random.Seed(0x21);
  avrlib_hd::HdVoice engine;
  engine.Init(&random);
  engine.set_tables(&voice);
  engine.set_envelope_tables(envelopes);
  for (int i = 0; i < 2; ++i)
    engine.mutable_oscillator(i)->set_tables(osc);
  engine.set_user_wavetable(shruthi::wav_res_waves, avrlib_hd::HdOscillator::kUserWavetableSize);
  avrlib_hd::HdVoicePatch patch;
  patch.osc[0].shape = shape1;
  patch.osc[1].shape = shape2;
  patch.osc[0].parameter = param1;
  patch.osc[1].parameter = param2;
  patch.osc[0].range = static_cast<int8_t>(range1);
  patch.osc[1].range = static_cast<int8_t>(range2);
  patch.env[1].sustain = 127;
  if (bend) {
    patch.mod[0].source = avrlib_hd::kModSourcePitchBend;
    patch.mod[0].destination = avrlib_hd::kModDestVco1_2Coarse;
    patch.mod[0].amount = 63;
    patch.mod[1].source = avrlib_hd::kModSourceLfo1;
    patch.mod[1].destination = avrlib_hd::kModDestVco1;
    patch.mod[1].amount = 63;
    engine.set_modulation_source(avrlib_hd::kModSourceLfo1, 255);
    engine.PitchBend(16383);
  }
  engine.NoteOn(static_cast<uint16_t>(note * 128), 100, 0, true);
  engine.ProcessBlock(patch, {});
  engine.NoteOn(static_cast<uint16_t>(note * 128), 100, 0, true);
  engine.ProcessBlock(patch, {});
  VoiceRun run;
  run.osc1.assign(engine.osc1_buffer(), engine.osc1_buffer() + avrlib_hd::kAudioBlockSize);
  run.osc2.assign(engine.osc2_buffer(), engine.osc2_buffer() + avrlib_hd::kAudioBlockSize);
  return run;
}

bool same(const std::vector<uint8_t>& a, const std::vector<uint8_t>& b) {
  return a.size() == b.size() && std::memcmp(a.data(), b.data(), a.size()) == 0;
}

void testVoice() {
  const uint8_t shapes[] = {avrlib_hd::kRealSaw, avrlib_hd::kRealSquare, avrlib_hd::kRealTriangle};
  for (uint8_t shape : shapes) {
    for (uint8_t parameter : {uint8_t{0}, uint8_t{1}, uint8_t{64}, uint8_t{127}}) {
      if (shape != avrlib_hd::kRealSquare && parameter != 0)
        continue;
      const auto direct = play(shape, shape, 124, 0, 0, parameter, parameter, false);
      const auto folded = play(shape, shape, 100, 48, 48, parameter, parameter, false);
      expect(same(direct.osc1, folded.osc1), "OSC1 range +48 matches note 124");
      expect(same(direct.osc2, folded.osc2), "OSC2 range +48 matches note 124");
      const auto top = play(shape, shape, 127, 0, 0, parameter, parameter, false);
      expect(!top.osc1.empty() && !top.osc2.empty(), "both oscillators render MIDI 127");
    }
  }
  const auto saw = play(avrlib_hd::kRealSaw, avrlib_hd::kRealFm, 60, 0, 0, 0, 0, false);
  const auto switched = play(avrlib_hd::kRealTriangle, avrlib_hd::kRealSaw, 127, 0, 0, 0, 0, false);
  expect(!saw.osc1.empty() && !switched.osc1.empty(), "model change still renders");
  const auto bent = play(avrlib_hd::kRealTriangle, avrlib_hd::kRealSquare, 123, 0, 0, 0, 64, true);
  expect(bent.osc1.size() == static_cast<size_t>(avrlib_hd::kAudioBlockSize), "pitch bend at the upper boundary renders");
  const auto again = play(avrlib_hd::kRealTriangle, avrlib_hd::kRealSquare, 123, 0, 0, 0, 64, true);
  expect(same(bent.osc1, again.osc1) && same(bent.osc2, again.osc2), "retriggers at the boundary repeat");
}

}  // namespace

int main() {
  testRendererZones();
  testGuardPage();
  testVoice();
  if (gFailures != 0) {
    std::fprintf(stderr, "%d upper-register checks failed\n", gFailures);
    return 1;
  }
  std::printf("AvrlibHdUpperRegisterOscillatorTests passed\n");
  return 0;
}
