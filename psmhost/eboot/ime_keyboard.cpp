// The IME's hardware-keyboard path — see ime_keyboard.h. Devkit-build addresses (J01736824).
//
// Reproduced for an open inline OSK session:
//   * the keyboard handle the IME opens when its UI is ready (0x121E50 -> 0x1A5BD0 -> 0x1B64D0
//     OpenKeyboard 0x1B4790 -> sceKeyboardOpen(userId, 0, 0); option REPEAT) and closes with the
//     session (0x1A5E80);
//   * per tick (0x1A2BF0 -> 0x1BFDA0 -> 0x1B5CC0 / 0x1B5D70): sceKeyboardRead of the declared
//     keyboard, the key events of 0x1AE4C0 / 0x1AE680: status 3 | modifiers << 8 | (LED & 7) << 16,
//     modifier pseudo-keys 0xE0..0xE7 (0x1AED30), key-down 0x1AEE60, key-up 0x1AF860, repeat
//     0x1B0540 (registry-free user defaults: delay 16 ticks, rate 4), the character of the layout
//     (0x1B9C50: the normal table, or the shifted one when a Shift modifier (0x22) is held, binary
//     search on the keycode) with CHARACTER_VALID cleared for the non-character keycodes of
//     0xBEDB50;
//   * the IME side: 0x1A4C30 -> 0x118540 (DOWN / REPEAT post client event 543 = keyboard), 0x118900
//     for state 0 / 2: Enter and Escape on the first key-down become the Enter / Close buttons;
//     any other first key-down enters hardware-keyboard mode (ChangeUnitForKeyboard 0x117E00:
//     state 2, client event 14 = 2, PSM event 23 {State 2 KeyboardStart}, control 0x20000);
//     printable characters pass the type filter (0x1378D0); 0x114C60 turns each key into the
//     button events the OSK unit takes (Character / Backspace / Enter / Close / caret moves /
//     Delete / Home / End / PageUp / PageDown / Shift state).
//   * leaving hardware-keyboard mode on a controller button (0x1147B0): state 0, client event
//     14 = 1, PSM event 23 {State 3 KeyboardEnd}, control 0x20001.
// Not reproduced (logged once): the keyboard type -> layout table (0xBEDE00) and per-language
// layouts (the default layout 0xCD5F78 is used), the CapsLock / NumLock keypad remaps
// (0xDC7870), AltGr tables, dead keys, the unit switch to the keyboard's language
// (0x1175F0), Shift+arrow selection, the keyboard-for-PSM / KeyboardMonitor virtual-pad path
// (the host's DualSense key table stands in when no handle is open), repeat settings from the
// user (KeyRepeatStartingTime / KeyRepeatSpeed).
#include "eboot/ime_keyboard.h"

#include <algorithm>
#include <array>
#include <mutex>
#include <string>
#include <vector>

#include "core/log.h"
#include "host/boundary.h"
#include "libSceUrlConfigResolver/sce_json.h"
#include "platform/input.h"

namespace ssn::eboot::bglayer {
std::vector<uint8_t> eboot_bytes(uint64_t va, size_t size);  // bglayer_gpu.cpp
}

namespace ssn::eboot::ime::detail {  // ime_server.cpp
std::recursive_mutex& lock();
bool ime_open();
int32_t ime_user();
void button_event(uint32_t type, int32_t id, const std::u16string& text);
void emit_value(uint32_t id, uint32_t v);
int32_t& hwkb_state();
uint32_t type();
bool filter_ok(const std::u16string& s);
void post_psm_object(urlcfg::json::Object o);
uint32_t unit_lang();
uint32_t unit_type_();
}  // namespace ssn::eboot::ime::detail

namespace ssn::eboot::ime_keyboard {
namespace {
namespace d = ime::detail;
namespace json = urlcfg::json;

void note(const std::string& s) { host::note_unimplemented("eboot IME keyboard: " + s); }

// The default layout (0xCD5F78; J01736823 0xCD1F58): {u8 keycode, u8, u16 character} entries.
struct Layout {
  std::vector<std::pair<uint8_t, uint16_t>> normal, shifted;
  std::vector<uint16_t> non_char;  // 0xBEDB50: 41 u16 keycodes
  bool loaded = false;
};
const Layout& layout() {
  static const Layout l = [] {
    Layout r;
    auto table = [](uint64_t va, size_t n) {
      std::vector<std::pair<uint8_t, uint16_t>> t;
      const std::vector<uint8_t> b = bglayer::eboot_bytes(va, n * 4);
      for (size_t i = 0; i + 4 <= b.size(); i += 4) t.emplace_back(b[i], static_cast<uint16_t>(b[i + 2] | (b[i + 3] << 8)));
      return t;
    };
    r.normal = table(0xBE9970, 0x4A);   // layout +0 / +8 (devkit 0xBEE4B0)
    r.shifted = table(0xBE9AA0, 0x4A);  // layout +16 / +24 (devkit 0xBEE5E0)
    const std::vector<uint8_t> nc = bglayer::eboot_bytes(0xBE9010, 41 * 2);
    for (size_t i = 0; i + 2 <= nc.size(); i += 2) r.non_char.push_back(static_cast<uint16_t>(nc[i] | (nc[i + 1] << 8)));
    r.loaded = !r.normal.empty() && !r.shifted.empty() && r.non_char.size() == 41;
    if (!r.loaded) log::error("ime", "keyboard layout tables not read from the eboot");
    return r;
  }();
  return l;
}

// 0x1B9C50: character of a keycode (binary search of the sorted table).
uint16_t character(uint16_t keycode, uint8_t modifiers, uint32_t* status) {
  const Layout& l = layout();
  const auto& t = (modifiers & 0x22) ? l.shifted : l.normal;
  auto it = std::lower_bound(t.begin(), t.end(), keycode, [](const auto& e, uint16_t k) { return e.first < k; });
  uint16_t ch = (it != t.end() && it->first == keycode) ? it->second : 0;
  if (std::find(l.non_char.begin(), l.non_char.end(), keycode) != l.non_char.end() || ch == 0) {
    *status &= ~2u;
    ch = 0;
  }
  return ch;
}

struct KeyEvent {
  uint32_t id;  // 257 down, 258 up, 259 repeat
  uint16_t keycode, character;
  uint32_t status;
};

struct State {
  bool open = false;
  int32_t user = -1;
  uint32_t option = 0;  // keyboard option: bit 0 REPEAT
  std::array<uint8_t, 256> held{};
  uint16_t last_down = 0;
  uint64_t last_down_tick = 0;
  uint64_t tick = 0;
  int shift_state = 0;  // framework +9144
} g;

constexpr uint64_t kRepeatDelay = 16, kRepeatRate = 4;  // 0x1B0540 defaults when the user has none

json::Value unit_id_json() {
  return json::Value::make_object({{"language", json::Value::make_uint(d::unit_lang())},
                                   {"type", json::Value::make_int(d::unit_type_())},
                                   {"inputMethod", json::Value::make_uint(1)},
                                   {"pluginType", json::Value::make_uint(0)}});
}

void button(int32_t control) { d::button_event(0, control, u""); }

// 0x114C60 for the default branch (LABEL_246 onwards) with the BasicLatin unit.
void key_to_buttons(const KeyEvent& e) {
  const uint8_t mods = static_cast<uint8_t>(e.status >> 8);
  if (g.shift_state == 1 && !(mods & 0x22)) {
    g.shift_state = 0;
    button(131097);  // IME_KEY_SHIFT_OFF
  } else if (g.shift_state == 0 && (mods & 0x22)) {
    g.shift_state = 1;
    button(65538);  // Shift
  }
  if ((e.id | 2) != 0x103) return;  // up: nothing more
  if ((mods & 0x22) && e.keycode >= 0x4A && e.keycode <= 0x52 && e.keycode != 0x4C) {
    note("Shift + navigation key (range selection 131074, 0x114C60) not reproduced");
    return;
  }
  if (e.character == 0) {
    switch (e.keycode) {
      case 0x29: button(65539); return;                                         // Escape: Close
      case 0x4A: button((mods & 0x11) ? 131083 : 131081); return;               // Home (Ctrl: Top)
      case 0x4B: button(131079); return;                                        // PageUp
      case 0x4C: button(131077); return;                                        // Delete
      case 0x4D: button((mods & 0x11) ? 131084 : 131082); return;               // End (Ctrl: Bottom)
      case 0x4E: button(131080); return;                                        // PageDown
      case 0x4F: button(65548); return;                                         // Right
      case 0x50: button(65547); return;                                         // Left
      case 0x51: button(65550); return;                                         // Down
      case 0x52: button(65549); return;                                         // Up
      default: return;
    }
  }
  if (e.character == 8) {
    button((mods & 0x22) ? 131078 : 65537);  // Backspace (Shift: delete all)
    return;
  }
  if (e.character == 10) {
    button(65544);  // Enter (multiline + modifier: LineBreak, not reachable with this field)
    return;
  }
  if (mods & 0x99) return;  // Ctrl / GUI shortcuts (LABEL_340)
  if (mods & 0x44) {
    note("Alt + character (0x1A78C0) not reproduced");
    return;
  }
  d::button_event(1, 65584, std::u16string(1, static_cast<char16_t>(e.character)));  // Character / KeyInput
}

// 0x118540 + 0x118900 (state 0 / 2).
void deliver(const KeyEvent& e) {
  if (e.id == 257 || e.id == 259) d::emit_value(543, 5);  // ReceiveUserAction: DeviceType.Keyboard
  if (!d::ime_open()) return;
  int32_t& hw = d::hwkb_state();
  if (hw == 0 && e.id == 257) {
    if (e.character == 10 || e.keycode == 0x29) {  // Enter / Escape
      button(e.character == 10 ? 65544 : 65539);
      return;
    }
    // ChangeUnitForKeyboard 0x117E00
    hw = 2;
    d::emit_value(14, 2);
    d::post_psm_object({{"EventId", json::Value::make_int(23)}, {"State", json::Value::make_uint(2)}, {"unitId", unit_id_json()}});
    button(131072);  // IME_KEY_HWKB_MODE_IN
    static const bool n = (note("keyboard-language unit switch (0x1175F0) not reproduced: the open unit stays"), true);
    (void)n;
  }
  if ((e.id | 2) == 0x103 && e.character >= 0x20) {
    if (!d::filter_ok(std::u16string(1, static_cast<char16_t>(e.character)))) return;  // 0x1378D0 rejected
  }
  key_to_buttons(e);
}

}  // namespace

bool usage_for_character(char16_t c, uint16_t* usage, bool* shift) {
  const Layout& l = layout();
  for (const auto& [k, ch] : l.normal)
    if (ch == c && std::find(l.non_char.begin(), l.non_char.end(), k) == l.non_char.end() && k < 0x53) {
      *usage = k;
      *shift = false;
      return true;
    }
  for (const auto& [k, ch] : l.shifted)
    if (ch == c && std::find(l.non_char.begin(), l.non_char.end(), k) == l.non_char.end() && k < 0x53) {
      *usage = k;
      *shift = true;
      return true;
    }
  return false;
}

void initialize() {
  static const bool n = (note("keyboard type -> layout (0xBEDE00), CapsLock / NumLock keypad remaps, AltGr / dead keys, "
                              "user repeat settings and the KeyboardForPsm virtual pad not reproduced (default layout 0xCD5F78)"),
                         true);
  (void)n;
}

void on_ime_ready(int32_t user, uint32_t option) {
  if (g.open) return;
  g = State{};
  g.open = true;
  g.user = user;
  g.option = (option & 0x80) ? 193 : 1;  // 0x121E50: REPEAT, or 0xC1
  layout();
  platform::input::keyboard_set_open(true);
  log::info("ime", "sceKeyboardOpen(user {}, 0, 0): keyboard handle opened (option {})", user, g.option);
}

void on_ime_close() {
  if (!g.open) return;
  g.open = false;
  platform::input::keyboard_set_open(false);
  d::hwkb_state() = 0;
  log::info("ime", "keyboard handle closed");
}

void leave_hwkb_mode() {  // 0x1147B0
  int32_t& hw = d::hwkb_state();
  if (hw != 2) return;
  hw = 0;
  d::emit_value(14, 1);
  d::post_psm_object({{"EventId", json::Value::make_int(23)}, {"State", json::Value::make_uint(3)}, {"unitId", unit_id_json()}});
  button(131073);  // IME_KEY_HWKB_MODE_OUT
}

void update() {
  if (!g.open) return;
  ++g.tick;
  const platform::input::KeyboardData kd = platform::input::keyboard_read();
  std::array<uint8_t, 256> now{};
  for (int i = 0; i < 8; ++i)
    if (kd.modifiers & (1 << i)) now[0xE0 + i] = 1;  // 0x1AED30
  for (int i = 0; i < kd.count; ++i) now[kd.keys[i]] = 1;
  const uint32_t base = 3u | (static_cast<uint32_t>(kd.modifiers) << 8) | (static_cast<uint32_t>(kd.led & 7) << 16);
  std::vector<KeyEvent> evs;
  auto make = [&](uint32_t id, uint16_t k) {
    KeyEvent e{id, k, 0, base};
    e.character = character(k, kd.modifiers, &e.status);
    return e;
  };
  for (int k = 4; k < 256; ++k)  // key-up 0x1AF860
    if (g.held[k] && !now[k]) evs.push_back(make(258, static_cast<uint16_t>(k)));
  for (int k = 4; k < 256; ++k)  // key-down 0x1AEE60
    if (now[k] && !g.held[k]) {
      evs.push_back(make(257, static_cast<uint16_t>(k)));
      if (k < 0xE0) {
        g.last_down = static_cast<uint16_t>(k);
        g.last_down_tick = g.tick;
      }
    }
  if ((g.option & 1) && g.last_down && now[g.last_down] && g.held[g.last_down]) {  // repeat 0x1B0540
    const uint64_t el = g.tick - g.last_down_tick;
    if (el > kRepeatDelay && (el - kRepeatDelay) % kRepeatRate == 0) evs.push_back(make(259, g.last_down));
  }
  g.held = now;
  for (const KeyEvent& e : evs) deliver(e);
}

}  // namespace ssn::eboot::ime_keyboard
