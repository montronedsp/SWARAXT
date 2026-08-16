// Copyright 2009 Emilie Gillet.
//
// Author: Emilie Gillet (emilie.o.gillet@gmail.com)
// Host adaptation for Swara XT by MontroneDSP; based on Shruthi commit 56bfe78.
//
// This program is free software: you can redistribute it and/or modify
// it under the terms of the GNU General Public License as published by
// the Free Software Foundation, either version 3 of the License, or
// (at your option) any later version.
// This program is distributed in the hope that it will be useful,
// but WITHOUT ANY WARRANTY; without even the implied warranty of
// MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
// GNU General Public License for more details.
// You should have received a copy of the GNU General Public License
// along with this program.  If not, see <http://www.gnu.org/licenses/>.

#ifndef SHRUTHI_VOICE_H_
#define SHRUTHI_VOICE_H_

#include "shruthi/shruthi.h"

#include "midi/midi.h"
#include "avrlib/random.h"
#include "shruthi/audio_out.h"
#include "shruthi/envelope.h"
#include "shruthi/oscillator.h"
#include "shruthi/patch.h"
#include "shruthi/sub_oscillator.h"
#include "shruthi/transient_generator.h"
#include "shruthi/voice_allocator.h"

namespace shruthi {

class Part;

// Used for MIDI -> oscillator increment conversion.
static const int16_t kLowestNote = 0 * 128;
static const int16_t kHighestNote = 128 * 128;
static const int16_t kOctave = 12 * 128;
static const int16_t kPitchTableStart = 116 * 128;

static const uint8_t kNumLfos = 2;
static const uint8_t kNumEnvelopes = 2;
static const uint8_t kNumOscillators = 2;

class Voice {
 public:
  Voice() { }
  void Init(Part* part, HostAudioRing* audio_out, avrlib::Random* random);

  // Called whenever a new note is played, manually or through the arpeggiator.
  void NoteOn(
      uint16_t note,
      uint8_t velocity,
      uint8_t portamento,
      bool trigger);

  // Move this voice to the release stage.
  void NoteOff();

  // Move this voice to the release stage.
  void Kill() { TriggerEnvelope(DEAD); }

  void ProcessBlock();
  void ProcessControlBlock();

  // Called whenever a write to the CV analog outputs has to be made.
  inline uint8_t cutoff() const {
    return modulation_destinations_[MOD_DST_FILTER_CUTOFF];
  }
  inline uint8_t vca() const {
    return modulation_destinations_[MOD_DST_VCA];
  }
  inline uint8_t resonance() const {
    return modulation_destinations_[MOD_DST_FILTER_RESONANCE];
  }
  inline uint8_t cv_1() const {
    return modulation_destinations_[MOD_DST_CV_1];
  }
  inline uint8_t cv_2() const {
    return modulation_destinations_[MOD_DST_CV_2];
  }
  inline uint8_t modulation_source(uint8_t i) const {
    return modulation_sources_[i];
  }
  bool amplitude_envelope_dead() const { return envelope_[1].dead(); }
  uint8_t modulation_destination(uint8_t i) const {
    return modulation_destinations_[i];
  }

  inline void set_modulation_source(uint8_t i, uint8_t value) {
    modulation_sources_[i] = value;
  }

  inline void set_volume(uint8_t volume) {
    volume_ = volume;
  }

  inline void set_bass_note(int16_t bass_note) {
    pitch_bass_note_ = bass_note;
  }

  Envelope* mutable_envelope(uint8_t i) { return &envelope_[i]; }
  void RefreshEnvelopeRatesFromPatch();
  void TriggerEnvelope(uint8_t stage);
  void TriggerEnvelope(uint8_t index, uint8_t stage);

  void ControlChange(uint8_t controller, uint8_t value);
  void Aftertouch(uint8_t value) {
    modulation_sources_[MOD_SRC_AFTERTOUCH] = value << 1;
  }
  void PitchBend(uint16_t value);
  void ResetAllControllers();

#if SWARAXT_ENABLE_SHRUTHI_DEBUG_TAPS
  const uint8_t* debug_osc1_buffer() const { return debug_osc1_buffer_; }
  const uint8_t* debug_osc2_buffer() const { return debug_osc2_buffer_; }
  int16_t debug_destination14(uint8_t i) const { return dst_[i]; }
  uint32_t debug_process_block_count() const { return debug_process_block_count_; }
#endif

 private:
  inline void LoadSources() __attribute__((always_inline));
  inline void ProcessModulationMatrix() __attribute__((always_inline));
  inline void UpdateDestinations() __attribute__((always_inline));
  inline void RenderOscillators() __attribute__((always_inline));

  // Envelope generators.
  Envelope envelope_[kNumEnvelopes];
  uint8_t disable_envelope_auto_retriggering_[kNumEnvelopes] {};
  uint8_t gate_ = 0;
  int16_t dst_[kNumModulationDestinations] {};

  // Counters/phases for the pitch envelope generator (portamento).
  // Pitches are stored on 14 bits, the 7 highest bits are the MIDI note value,
  // the 7 lowest bits are used for fine-tuning.
  int16_t pitch_increment_ = 0;
  int16_t pitch_target_ = 0;
  int16_t pitch_value_ = 0;
  int16_t pitch_bass_note_ = 0;

  // The voice-specific modulation sources are from MOD_SRC_ENV_1 to
  // MOD_SRC_GATE.
  uint8_t modulation_sources_[kNumModulationSources] {};

  // Value of all the stuff controlled by the modulators, scaled to the value
  // they will be used for. MOD_DST_FILTER_RESONANCE is the last entry
  // in the modulation destinations enum.
  int8_t modulation_destinations_[kNumModulationDestinations] {};

  uint8_t buffer_[kAudioBlockSize] {};
  uint8_t osc2_buffer_[kAudioBlockSize] {};
  uint8_t sync_state_[kAudioBlockSize] {};
  uint8_t no_sync_[kAudioBlockSize] {};
  uint8_t dummy_sync_state_[kAudioBlockSize] {};
  uint8_t trigger_count_ = 0;

#if SWARAXT_ENABLE_SHRUTHI_DEBUG_TAPS
  uint8_t debug_osc1_buffer_[kAudioBlockSize] {};
  uint8_t debug_osc2_buffer_[kAudioBlockSize] {};
  uint32_t debug_process_block_count_ = 0;
#endif

  uint8_t volume_ = 0;
  Part* part_ = nullptr;
  HostAudioRing* audio_out_ = nullptr;
  avrlib::Random* random_ = nullptr;
  Oscillator osc_1_;
  Oscillator osc_2_;
  SubOscillator sub_osc_;
  TransientGenerator transient_generator_;
  uint8_t user_wavetable_[kUserWavetableSize + 1] {};

  DISALLOW_COPY_AND_ASSIGN(Voice);
};

}  // namespace shruthi

#endif // SHRUTHI_VOICE_H_
