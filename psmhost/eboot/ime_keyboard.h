// The IME's hardware-keyboard path (eboot.bin FW 4.03 devkit J01736824): the keyboard manager
// ("SceImeKbdMgr", init 0x1A7AE0 / 0x1B46F0) over the statically linked libSceKeyboard, its key
// event generation (0x1AE4C0 / 0x1AE680, characters 0x1B9C50, repeat 0x1B0540), and the IME side
// (event delivery 0x1A4C30 -> 0x10B130 -> 0x118540, processing 0x1186F0 -> 0x118900, key -> button
// events 0x114C60, hardware-keyboard mode 0x117E00 / 0x1147B0). The keyboard device is the host's
// declared USB keyboard (platform/input.h).
#pragma once
#include <cstdint>

namespace ssn::eboot::ime_keyboard {

void initialize();                                // 0x1A7AE0
void update();                                    // per SceImeUpdate tick: 0x1A2BF0 then 0x10B160
void on_ime_ready(int32_t user, uint32_t option); // 0x121E50: the IME opens the user's keyboard
void on_ime_close();                              // 0x1A5E80
void leave_hwkb_mode();                           // 0x1147B0
// The key of the default layout (0xCD5F78) that types `c` (k < 0x53: the main block, keypad
// excluded), with Shift when it is in the shifted table. For the input script's text events.
bool usage_for_character(char16_t c, uint16_t* usage, bool* shift);

}  // namespace ssn::eboot::ime_keyboard
