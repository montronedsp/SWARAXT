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

#ifndef SHRUTHI_PART_H_
#define SHRUTHI_PART_H_

#include "shruthi/shruthi.h"

#include "shruthi/envelope.h"
#include "shruthi/lfo.h"
#include "shruthi/note_stack.h"
#include "shruthi/patch.h"
#include "shruthi/sequencer_settings.h"
#include "shruthi/system_settings.h"
#include "shruthi/voice.h"
#include "shruthi/voice_allocator.h"
#include "shruthi/clock.h"

namespace shruthi {

class HostAudioRing;
class MidiDispatcher;
class Storage;
}  // namespace shruthi

namespace avrlib {
class Random;
}  // namespace avrlib

namespace shruthi {

class Part {
  friend class Voice;

 public:
  Part() { }
  void Init(HostAudioRing& audio_out,
            avrlib::Random& random,
            MidiDispatcher& midi_dispatcher,
            Storage& storage);

  // Forwarded to the controller.
  void NoteOn(uint8_t channel, uint8_t note, uint8_t velocity);
  void NoteOff(uint8_t channel, uint8_t note);

  // Handled.
  void ControlChange(uint8_t controller, uint8_t value);
  void PitchBend(uint16_t pitch_bend) {
    voice_.PitchBend(pitch_bend);
  }
  void Aftertouch(uint8_t /*note*/, uint8_t velocity) {
    voice_.Aftertouch(velocity);
  }
  void Aftertouch(uint8_t velocity) {
    voice_.Aftertouch(velocity);
  }
  void AllSoundOff();
  void ResetAllControllers();
  void AllNotesOff();
  void OmniModeOff(uint8_t channel);
  void OmniModeOn();
  void Reset();
  void Clock(bool internal);
  void Start(bool internal);
  void Stop(bool internal);
  void AlignExternalClock(uint64_t ticksBeforeNextClock);
  void SetHostLfoSync(uint8_t index, bool enabled, uint16_t phaseIncrement);
  void SetGatePercent(uint8_t percent);
  uint8_t clock_ticks_per_step() const { return step_duration(); }
  uint16_t lfo_phase_increment_for_tests(uint8_t index) const {
    return index < kNumLfos ? lfo_[index].phase_increment_for_tests() : 0;
  }
  uint16_t lfo_phase_for_tests(uint8_t index) const {
    return index < kNumLfos ? lfo_[index].phase_for_tests() : 0;
  }
  uint8_t lfo_shape_for_tests(uint8_t index) const {
    return index < kNumLfos ? lfo_[index].shape_for_tests() : 0;
  }
  uint8_t generated_note_count_for_tests() const { return generated_notes_.size(); }
  uint16_t gate_counter_for_tests() const { return arp_seq_gate_length_counter_; }

  void ProcessBlock();
  void ProcessControlBlock();
  void SetName(uint8_t* name);
  void SetSequenceStep(uint8_t index, uint8_t data_a, uint8_t data_b);
  void SetPatternRotation(uint8_t rotation);
  void SetPatternLength(uint8_t length);

  // Patch manipulation stuff.
  void SetParameter(uint8_t index, uint8_t offset, uint8_t value,
      bool user_initiated);
  void SetScaledParameter(uint8_t index, uint8_t value,
      bool user_initiated);
  uint8_t GetParameter(uint8_t offset) const;
  void ResetPatch();
  void ResetSequencerSettings();
  void ResetSequence();
  void ResetSystemSettings();
  // Variables dependent on parameters (increments) are recomputed in
  // SetParameter when the related parameter is modified. Sometimes, the patch
  // is modified all at once without any call to SetParameter (for example when
  // loading a patch from the EEPROM)... so in this case we need to recompute
  // all the related variables. This is also a good occasion to dump by SysEx
  // the patch to polychained units.
  void Touch(bool cascade);
  void TriggerLfos() {
    for (uint8_t i = 0; i < kNumLfos; ++i) {
      lfo_[i].Trigger();
    }
  }

  inline bool running() const { return arp_seq_running_; }
  inline uint8_t step() const { return arp_seq_step_; }

  inline const Patch& patch() const { return patch_; }
  inline const SequencerSettings& sequencer_settings() const {
    return sequencer_settings_;
  }
  inline const SystemSettings& system_settings() const {
    return system_settings_;
  }
  inline Patch* mutable_patch() { return &patch_; }
  inline SequencerSettings* mutable_sequencer_settings() {
    return &sequencer_settings_;
  }
  inline SystemSettings* mutable_system_settings() {
    return &system_settings_;
  }

  // These variables are sent to I/O pins, and are made accessible here.
  inline uint8_t modulation_source(uint8_t /*i*/, uint8_t j) const {
    return voice_.modulation_source(j);
  }

  const Voice& voice() const { return voice_; }
  Voice* mutable_voice() { return &voice_; }

  inline bool dirty() {
    bool value = dirty_;
    dirty_ = false;
    return value;
  }

  inline uint8_t fx_control_byte() const {
    return (patch_.filter_1_mode_ << 4) | patch_.filter_2_mode_;
  }

  inline uint8_t pvk_routing_byte() const {
    uint8_t byte = 0;
    if (patch_.filter_1_mode_ == FILTER_MODE_LP) {
      byte |= 4;
    }
    if (!voice().cv_1()) {
      byte |= 2;
    }
    if (!voice().cv_2()) {
      byte |= 1;
    }
    return byte;
  }

  inline uint8_t sp_routing_byte() const {
    return patch_.filter_1_mode_;
  }

  uint8_t four_pole_routing_byte();
  uint8_t blinky_eyes();

  inline uint8_t svf_routing_byte() {
    uint8_t byte = 0;
    uint8_t filter_1_mode = patch_.filter_1_mode_;
    if (filter_1_mode >= 3) {
      filter_1_mode -= 3;
    }
    if (filter_1_mode == FILTER_MODE_BP) {
      byte = 2;
    } else if (filter_1_mode == FILTER_MODE_HP) {
      byte = 4;
    }
    if (patch_.filter_2_mode_ >= FILTER_MODE_SERIAL_LP &&
        patch_.filter_2_mode_ <= FILTER_MODE_SERIAL_HP) {
      byte |= 1;  // Do not mix filter 1 to the output.
    }

    uint8_t filter_2_mode = patch_.filter_2_mode_;
    if (filter_2_mode >= 3) {
      filter_2_mode -= 3;
    }
    if (filter_2_mode == FILTER_MODE_BP) {
      byte |= (2 << 3);
    } else if (filter_2_mode == FILTER_MODE_HP) {
      byte |= (4 << 3);
    }
    if (patch_.filter_2_mode_ < FILTER_MODE_SERIAL_LP) {
      byte |= (1 << 3);  // Do not mix filter 1 to the output.
    }
    return byte | blinky_eyes();
  }

  inline bool internal_clock() { return sequencer_settings_.internal_clock(); }
  inline bool latched() { return ignore_note_off_messages_; }
  inline void Latch() {
    ignore_note_off_messages_ = true;
    release_latched_keys_on_next_note_on_ = true;
  }
  inline uint8_t num_notes() const {
    return pressed_keys_.size();
  }

  inline void Unlatch() {
    ignore_note_off_messages_ = false;
    release_latched_keys_on_next_note_on_ = true;
  }

 private:
  void ProcessBlockInternal(bool render_audio);
  // Called when patch updates can affect LFO or envelope timing.
  void UpdateModulationRates();
  void UpdateLfoRate(uint8_t i);
  void StopSequencerArpeggiatorNotes();
  void ReleaseLatchedNotes();
  void InternalNoteOn(uint8_t note, uint8_t velocity);
  void InternalNoteOff(uint8_t note);
  void ClockArpeggiator();
  void ClockSequencer();
  void NextStep();

  uint16_t Tune(uint8_t note);
  uint8_t step_duration() const;
  uint8_t* parameter_byte(uint8_t offset);
  const uint8_t* parameter_byte(uint8_t offset) const;

  Patch patch_ {};
  SequencerSettings sequencer_settings_ {};
  SystemSettings system_settings_ {};
  bool dirty_ = false;

  uint8_t nrpn_parameter_number_ = 0;
  uint8_t nrpn_parameter_number_msb_ = 0;
  uint8_t data_entry_msb_ = 0;

  Voice voice_;
  NoteStack mono_allocator_;
  NoteStack pressed_keys_;
  NoteStack generated_notes_;
  VoiceAllocator poly_allocator_;
  bool release_latched_keys_on_next_note_on_ = false;
  bool ignore_note_off_messages_ = false;

  Lfo lfo_[kNumLfos];
  uint8_t previous_lfo_fm_[kNumLfos] {};
  uint16_t lfo_step_[kNumLfos] {};
  uint16_t lfo_limit_[kNumLfos] {};
  uint16_t lfo_increment_[kNumLfos] {};
  bool host_lfo_sync_[kNumLfos] {};
  uint16_t host_lfo_increment_[kNumLfos] {};

  // Sequencer stuff
  bool arp_seq_running_ = false;
  uint8_t arp_seq_prescaler_ = 0;
  uint8_t arp_seq_step_ = 0;
  int8_t arp_note_ = 0;
  int8_t arp_octave_ = 0;
  int8_t arp_direction_ = 1;
  int8_t arp_previous_note_ = 0;
  uint16_t arp_seq_gate_length_counter_ = 0;
  int8_t swing_amount_ = 0;
  uint8_t internal_clock_blank_ticks_ = 0;
  int8_t seq_transposition_ = 0;
  uint8_t gate_percent_ = 50;

  shruthi::Clock clock_;
  MidiDispatcher* midi_dispatcher_ = nullptr;
  Storage* storage_ = nullptr;
  avrlib::Random* random_ = nullptr;

  DISALLOW_COPY_AND_ASSIGN(Part);
};

}  // namespace shruthi

#endif // SHRUTHI_PART_H_
