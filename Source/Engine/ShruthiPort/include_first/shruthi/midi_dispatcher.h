// Copyright 2026 MontroneDSP.
// SPDX-License-Identifier: GPL-3.0-or-later

#ifndef SWARAXT_SHRUTHI_MIDI_DISPATCHER_H_
#define SWARAXT_SHRUTHI_MIDI_DISPATCHER_H_

#include "avrlib/base.h"
#include "midi/midi.h"
#include "shruthi/editor.h"
#include "shruthi/parameter.h"
#include "shruthi/part.h"
#include "shruthi/storage.h"

namespace shruthi {

class MidiDispatcher : public midi::MidiDevice {
 public:
    MidiDispatcher() = default;

    void Init(Part* part, Storage* storage)
    {
        part_ = part;
        storage_ = storage;
        current_bank_ = 0;
        Flush();
    }

    inline void NoteOn(uint8_t channel, uint8_t note, uint8_t velocity)
    {
        (void) channel;
        if (part_ != nullptr && ! Editor::OnNoteOn(note, velocity))
            part_->NoteOn(channel, note, velocity);
    }

    inline void NoteOff(uint8_t channel, uint8_t note, uint8_t velocity)
    {
        (void) velocity;
        if (part_ != nullptr)
            part_->NoteOff(channel, note);
    }

    inline void ControlChange(uint8_t channel, uint8_t controller, uint8_t value)
    {
        (void) channel;
        if (controller == midi::kBankMsb)
            current_bank_ = value;
        else if (part_ != nullptr)
            part_->ControlChange(controller, value);
    }

    inline void PitchBend(uint8_t channel, uint16_t pitch_bend)
    {
        (void) channel;
        if (part_ != nullptr)
            part_->PitchBend(pitch_bend);
    }

    void Aftertouch(uint8_t channel, uint8_t note, uint8_t velocity)
    {
        (void) channel;
        if (part_ != nullptr)
            part_->Aftertouch(note, velocity);
    }

    void Aftertouch(uint8_t channel, uint8_t velocity)
    {
        (void) channel;
        if (part_ != nullptr)
            part_->Aftertouch(velocity);
    }

    void AllSoundOff(uint8_t channel)
    {
        (void) channel;
        if (part_ != nullptr)
            part_->AllSoundOff();
    }

    void ResetAllControllers(uint8_t channel)
    {
        (void) channel;
        if (part_ != nullptr)
            part_->ResetAllControllers();
    }

    void AllNotesOff(uint8_t channel)
    {
        (void) channel;
        if (part_ != nullptr)
            part_->AllNotesOff();
    }

    void OmniModeOff(uint8_t channel)
    {
        if (part_ != nullptr)
            part_->OmniModeOff(channel);
    }

    void OmniModeOn(uint8_t channel)
    {
        (void) channel;
        if (part_ != nullptr)
            part_->OmniModeOn();
    }

    void ProgramChange(uint8_t channel, uint8_t program)
    {
        // MIDI Program Change is not part of the SWARA XT preset model.
        // Plugin programs are owned by the processor/host program API and GUI.
        (void) channel;
        (void) program;
    }

    void Reset()
    {
        if (part_ != nullptr)
            part_->Reset();
    }

    void Clock()
    {
        if (part_ != nullptr && ! part_->internal_clock())
            part_->Clock(false);
    }

    void Start()
    {
        if (part_ != nullptr && ! part_->internal_clock())
            part_->Start(false);
    }

    void Continue()
    {
        if (part_ != nullptr && ! part_->internal_clock())
            part_->Start(false);
    }

    void Stop()
    {
        if (part_ != nullptr && ! part_->internal_clock())
            part_->Stop(false);
    }

    void SysExStart() {}
    void SysExByte(uint8_t) {}
    void SysExEnd() {}

    uint8_t CheckChannel(uint8_t channel) const
    {
        if (part_ == nullptr)
            return 0;
        const SystemSettings& settings = part_->system_settings();
        return settings.midi_channel == 0
            || settings.midi_channel == static_cast<uint8_t>(channel + 1);
    }

    void RawMidiData(uint8_t, uint8_t*, uint8_t, uint8_t) {}
    void RawByte(uint8_t) {}

    uint8_t readable() const
    {
        return static_cast<uint8_t>((outputWrite_ - outputRead_) & (kOutputBufferSize - 1));
    }

    uint8_t ImmediateRead()
    {
        const uint8_t result = output_[outputRead_];
        outputRead_ = static_cast<uint8_t>((outputRead_ + 1) & (kOutputBufferSize - 1));
        return result;
    }

    inline void OnInternalNoteOff(uint8_t) {}
    inline void OnInternalNoteOn(uint8_t, uint8_t) {}
    inline void ForwardNoteOn(uint8_t, uint8_t, uint8_t) {}
    inline void ForwardNoteOff(uint8_t, uint8_t) {}
    inline void OnStart() {}
    inline void OnStop() {}
    inline void OnClock() {}
    inline void OnProgramChange(uint16_t) {}
    inline void OnEdit(uint8_t, uint8_t, uint8_t) {}

    void Flush()
    {
        running_status_ = 0;
        outputRead_ = outputWrite_;
    }

    void Send3(uint8_t a, uint8_t b, uint8_t c);

    uint8_t channel() const
    {
        if (part_ == nullptr)
            return 0;
        return part_->system_settings().midi_channel == 0
            ? 0
            : static_cast<uint8_t>(part_->system_settings().midi_channel - 1);
    }

 private:
    static constexpr uint8_t kOutputBufferSize = 128;

    void Send(uint8_t status, uint8_t* data, uint8_t size);
    void SendNow(uint8_t byte);
    void writeOutput(uint8_t byte);

    Part* part_ = nullptr;
    Storage* storage_ = nullptr;
    uint8_t output_[kOutputBufferSize] {};
    uint8_t outputRead_ = 0;
    uint8_t outputWrite_ = 0;
    uint8_t current_bank_ = 0;
    uint8_t running_status_ = 0;

    DISALLOW_COPY_AND_ASSIGN(MidiDispatcher);
};

}  // namespace shruthi

#endif  // SWARAXT_SHRUTHI_MIDI_DISPATCHER_H_
