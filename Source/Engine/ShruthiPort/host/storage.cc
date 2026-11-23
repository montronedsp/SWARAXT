// Copyright 2026 MontroneDSP.
// SPDX-License-Identifier: GPL-3.0-or-later

#include "shruthi/storage.h"

#include "shruthi/part.h"

namespace shruthi {

void Storage::Init(Part* part)
{
    part_ = part;
    sysex_rx_state_ = RECEPTION_ERROR;
    num_accessible_banks_ = 1;
}

void Storage::WritePatch(uint16_t slot)
{
    if (part_ != nullptr)
        Write(part_->mutable_patch(), slot);
}

void Storage::WriteSequence(uint16_t slot)
{
    if (part_ != nullptr)
        Write(part_->mutable_sequencer_settings(), slot);
}

void Storage::LoadPatch(uint16_t slot)
{
    if (part_ != nullptr)
    {
        Load(part_->mutable_patch(), slot);
        part_->Touch(false);
    }
}

void Storage::LoadSequence(uint16_t slot)
{
    if (part_ != nullptr)
    {
        Load(part_->mutable_sequencer_settings(), slot);
        part_->Touch(false);
    }
}

}  // namespace shruthi
