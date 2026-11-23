// Copyright 2026 MontroneDSP.
// SPDX-License-Identifier: GPL-3.0-or-later

#ifndef SWARAXT_SHRUTHI_EDITOR_H_
#define SWARAXT_SHRUTHI_EDITOR_H_

#include "avrlib/base.h"
#include "shruthi/patch.h"
#include "shruthi/shruthi.h"

namespace avrlib {
class Event {};
}  // namespace avrlib

namespace shruthi {

class Editor {
 public:
    Editor() {}
    static void Init() {}
    static void Refresh() {}
    static uint8_t OnNoteOn(uint8_t, uint16_t) { return 0; }
    static void set_current_patch_number(uint16_t) {}
    static uint16_t current_patch_number() { return 0; }

 private:
    DISALLOW_COPY_AND_ASSIGN(Editor);
};

}  // namespace shruthi

#endif  // SWARAXT_SHRUTHI_EDITOR_H_
