// Copyright 2026 MontroneDSP.
// SPDX-License-Identifier: GPL-3.0-or-later
// In-memory patch storage (no EEPROM / external flash).

#ifndef SWARAXT_SHRUTHI_STORAGE_H_
#define SWARAXT_SHRUTHI_STORAGE_H_

#include <string.h>

#include "avrlib/base.h"
#include "shruthi/hardware_config.h"
#include "shruthi/patch.h"
#include "shruthi/sequencer_settings.h"
#include "shruthi/shruthi.h"
#include "shruthi/system_settings.h"

namespace shruthi {

class Part;

enum SysExReceptionState {
    RECEIVING_HEADER = 0,
    RECEIVING_OLD_HEADER = 1,
    RECEIVING_COMMAND = 2,
    RECEIVING_DATA = 3,
    RECEIVING_FOOTER = 4,
    RECEPTION_OK = 5,
    RECEPTION_ERROR = 6,
};

template<typename T>
class StorageConfiguration {};

template<>
class StorageConfiguration<Patch> {
 public:
    enum {
        num_internal = 16,
        num_external = 64,
        offset_internal = 16,
        offset_external = 0,
        size = PATCH_SIZE,
        sysex_object_id = 0x01,
        undo_buffer_offset = 0,
    };
};

template<>
class StorageConfiguration<SequencerSettings> {
 public:
    enum {
        num_internal = 16,
        num_external = 64,
        offset_internal = StorageConfiguration<Patch>::offset_internal
            + StorageConfiguration<Patch>::num_internal * PATCH_SIZE,
        offset_external = StorageConfiguration<Patch>::offset_external
            + StorageConfiguration<Patch>::num_external * PATCH_SIZE,
        size = sizeof(SequenceStep) * kNumSteps,
        sysex_object_id = 0x02,
        undo_buffer_offset = PATCH_SIZE,
    };
};

class Storage {
 public:
    Storage() = default;

    void Init(Part* part);

    inline uint8_t sysex_rx_state() const { return sysex_rx_state_; }

    template<typename T>
    uint16_t size() const
    {
        return static_cast<uint16_t>(StorageConfiguration<T>::num_internal
            + StorageConfiguration<T>::num_external * num_accessible_banks_);
    }

    uint16_t addressable_space_size() const
    {
        return static_cast<uint16_t>(kInternalEepromSize + num_accessible_banks_ * kBankSize);
    }

    uint8_t num_accessible_banks() const { return num_accessible_banks_; }

    template<typename T>
    void SysExDump(T* ptr)
    {
        (void) ptr;
    }

    void SysExBulkDump() {}
    void SysExReceive(uint8_t) {}

    template<typename T>
    void Backup(T* ptr)
    {
        ptr->PrepareForWrite();
        memcpy(undo_buffer_ + StorageConfiguration<T>::undo_buffer_offset,
               ptr->saved_data(),
               StorageConfiguration<T>::size);
    }

    template<typename T>
    void Restore(T* ptr)
    {
        AcceptData(ptr, undo_buffer_ + StorageConfiguration<T>::undo_buffer_offset);
    }

    void WritePatch(uint16_t slot);
    void WriteSequence(uint16_t slot);
    void LoadPatch(uint16_t slot);
    void LoadSequence(uint16_t slot);

    template<typename T>
    void LoadPatchName(uint8_t* destination, uint16_t slot)
    {
        LoadBytes<T>(destination, 68, 8, slot);
    }

    template<typename T>
    uint8_t AcceptData(T* ptr, uint8_t* data)
    {
        const uint8_t success = ptr->CheckBuffer(data);
        if (success)
        {
            memcpy(ptr->saved_data(), data, StorageConfiguration<T>::size);
            ptr->Update();
        }
        return success;
    }

 private:
    template<typename T>
    void Write(T* ptr, uint16_t slot)
    {
        ptr->PrepareForWrite();
        memcpy(slotData<T>(slot), ptr->saved_data(), StorageConfiguration<T>::size);
    }

    template<typename T>
    void Load(T* ptr, uint16_t slot)
    {
        memcpy(load_buffer_, slotData<T>(slot), StorageConfiguration<T>::size);
        AcceptData(ptr, load_buffer_);
    }

    template<typename T>
    void LoadBytes(uint8_t* destination,
                   uint16_t offset,
                   uint16_t byteCount,
                   uint16_t slot)
    {
        memcpy(destination, slotData<T>(slot) + offset, byteCount);
    }

    template<typename T>
    uint8_t* slotData(uint16_t slot)
    {
        return memory_ + StorageConfiguration<T>::offset_external
            + slot * StorageConfiguration<T>::size;
    }

    Part* part_ = nullptr;
    uint8_t undo_buffer_[128] {};
    uint8_t load_buffer_[sizeof(Patch)] {};
    uint8_t sysex_rx_state_ = RECEPTION_ERROR;
    uint8_t num_accessible_banks_ = 1;
    uint8_t memory_[16384] {};

    DISALLOW_COPY_AND_ASSIGN(Storage);
};

}  // namespace shruthi

#endif  // SWARAXT_SHRUTHI_STORAGE_H_
