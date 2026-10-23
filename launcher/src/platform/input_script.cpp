#include "platform/input_script.h"

#include <windows.h>

#include <algorithm>
#include <cctype>
#include <cstdlib>
#include <sstream>
#include <vector>

#include "core/log.h"
#include "platform/input.h"

namespace ssn::platform::input_script {
namespace {

// Button name -> the keyboard key that drives it (input.cpp kKeyButtons, docs/input.md).
struct Name {
  const char* name;
  uint32_t vk;
};
constexpr Name kNames[] = {
    {"cross", VK_RETURN}, {"circle", VK_BACK}, {"square", 'C'},      {"triangle", 'V'},   {"up", VK_UP},
    {"down", VK_DOWN},    {"left", VK_LEFT},   {"right", VK_RIGHT},  {"l1", 'Q'},         {"r1", 'E'},
    {"l2", '1'},          {"r2", '3'},         {"l3", 'F'},          {"r3", 'G'},         {"options", VK_TAB},
    {"create", VK_F1},    {"share", VK_F1},    {"ps", VK_HOME},      {"touchpad", 'T'},   {"mute", 'M'},
    {"lup", 'W'},         {"ldown", 'S'},      {"lleft", 'A'},       {"lright", 'D'},     {"rup", 'I'},
    {"rdown", 'K'},       {"rleft", 'J'},      {"rright", 'L'},
};
constexpr uint64_t kDefaultHold = 42;  // ~700 ms at 60 Hz
constexpr uint64_t kTextHold = 4;      // a typed character: held 4 frames ...
constexpr uint64_t kTextStep = 10;     // ... and the next one 10 frames later

// USB keyboard keys by name (HID usages, keyboard page 0x07).
struct UsbName {
  const char* name;
  uint16_t usage;
};
constexpr UsbName kUsbNames[] = {
    {"enter", 0x28},  {"escape", 0x29}, {"backspace", 0x2A}, {"tab", 0x2B},   {"space", 0x2C},   {"delete", 0x4C},
    {"home", 0x4A},   {"end", 0x4D},    {"pageup", 0x4B},    {"pagedown", 0x4E}, {"right", 0x4F}, {"left", 0x50},
    {"down", 0x51},   {"up", 0x52},     {"shift", 0xE1},     {"ctrl", 0xE0},  {"alt", 0xE2},
};

struct Event {
  uint64_t frame;
  uint32_t vk;
  bool down;
  std::string name;
  uint16_t usage = 0;  // non-zero: a USB keyboard key (input::inject_usb_key)
  char16_t ch = 0;     // non-zero: a typed character (resolved in the firmware layout when it fires)
};
std::vector<Event> g_events;  // sorted by frame

}  // namespace
}  // namespace ssn::platform::input_script

namespace ssn::eboot::ime_keyboard {
// The firmware's default keyboard layout, inverted (ime_keyboard.cpp).
bool usage_for_character(char16_t c, uint16_t* usage, bool* shift);
}

namespace ssn::platform::input_script {
namespace {
size_t g_next = 0;

std::string trim(std::string s) {
  auto sp = [](unsigned char c) { return std::isspace(c) != 0; };
  s.erase(s.begin(), std::find_if_not(s.begin(), s.end(), sp));
  s.erase(std::find_if_not(s.rbegin(), s.rend(), sp).base(), s.end());
  return s;
}

bool number(const std::string& s, uint64_t& out) {
  if (s.empty() || !std::all_of(s.begin(), s.end(), [](unsigned char c) { return std::isdigit(c); })) return false;
  out = std::strtoull(s.c_str(), nullptr, 10);
  return true;
}

}  // namespace

bool load(const std::string& script, std::string& error) {
  g_events.clear();
  g_next = 0;
  std::stringstream ss(script);
  std::string item;
  while (std::getline(ss, item, ';')) {
    item = trim(item);
    if (item.empty()) continue;
    std::vector<std::string> parts;
    std::stringstream is(item);
    for (std::string p; std::getline(is, p, ':');) parts.push_back(trim(p));
    // f<frame>:text:<characters>  — typed on the USB keyboard, one character every kTextStep frames
    // f<frame>:key:<name>[:<hold>] — one USB keyboard key (enter, backspace, left, ...)
    if (parts.size() >= 3 && (parts[1] == "text" || parts[1] == "key")) {
      uint64_t frame = 0;
      if (parts[0].size() < 2 || (parts[0][0] != 'f' && parts[0][0] != 'F') || !number(parts[0].substr(1), frame)) {
        error = "bad event '" + item + "'";
        return false;
      }
      if (parts[1] == "text") {
        std::string text = item.substr(item.find(':', item.find(':') + 1) + 1);  // the rest, colons included
        for (unsigned char ch : text) {
          if (ch < 0x20 || ch >= 0x7F) {
            error = "text: only printable ASCII is typed ('" + item + "')";
            return false;
          }
          const std::string nm = std::string("text '") + static_cast<char>(ch) + "'";
          g_events.push_back({frame, 0, true, nm, 0, static_cast<char16_t>(ch)});
          g_events.push_back({frame + kTextHold, 0, false, nm, 0, static_cast<char16_t>(ch)});
          frame += kTextStep;
        }
        continue;
      }
      std::string name = parts[2];
      std::transform(name.begin(), name.end(), name.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
      const auto it = std::find_if(std::begin(kUsbNames), std::end(kUsbNames), [&](const UsbName& n) { return name == n.name; });
      uint64_t hold = kTextHold;
      if (it == std::end(kUsbNames) || (parts.size() == 4 && (!number(parts[3], hold) || hold == 0)) || parts.size() > 4) {
        error = "bad key event '" + item + "'";
        return false;
      }
      g_events.push_back({frame, 0, true, "key " + name, it->usage});
      g_events.push_back({frame + hold, 0, false, "key " + name, it->usage});
      continue;
    }
    uint64_t frame = 0, hold = kDefaultHold;
    if (parts.size() < 2 || parts.size() > 3 || parts[0].size() < 2 || (parts[0][0] != 'f' && parts[0][0] != 'F') ||
        !number(parts[0].substr(1), frame) || (parts.size() == 3 && (!number(parts[2], hold) || hold == 0))) {
      error = "bad event '" + item + "' (expected f<frame>:<button>[:<hold frames>])";
      return false;
    }
    std::string name = parts[1];
    std::transform(name.begin(), name.end(), name.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    const auto it = std::find_if(std::begin(kNames), std::end(kNames), [&](const Name& n) { return name == n.name; });
    if (it == std::end(kNames)) {
      error = "unknown button '" + parts[1] + "' in '" + item + "'";
      return false;
    }
    g_events.push_back({frame, it->vk, true, name});
    g_events.push_back({frame + hold, it->vk, false, name});
  }
  // Releases before presses on the same frame, so back-to-back presses of one key re-press.
  std::stable_sort(g_events.begin(), g_events.end(),
                   [](const Event& a, const Event& b) { return a.frame != b.frame ? a.frame < b.frame : (!a.down && b.down); });
  return true;
}

void tick(uint64_t frame) {
  while (g_next < g_events.size() && g_events[g_next].frame <= frame) {
    const Event& e = g_events[g_next++];
    if (e.ch) {
      // The key (and Shift) that type the character in the firmware's default layout.
      static uint16_t typed_usage[128] = {};
      static bool typed_shift[128] = {};
      if (e.down) {
        uint16_t usage = 0;
        bool shift = false;
        if (!ssn::eboot::ime_keyboard::usage_for_character(e.ch, &usage, &shift)) {
          log::warn("input-script", "frame {}: {}: no key types it in the keyboard layout", frame, e.name);
          continue;
        }
        typed_usage[e.ch] = usage;
        typed_shift[e.ch] = shift;
        if (shift) input::inject_usb_key(0xE1, true);
        input::inject_usb_key(usage, true);
      } else if (typed_usage[e.ch]) {
        input::inject_usb_key(typed_usage[e.ch], false);
        if (typed_shift[e.ch]) input::inject_usb_key(0xE1, false);
        typed_usage[e.ch] = 0;
      }
      log::info("input-script", "frame {}: {} {}", frame, e.name, e.down ? "down" : "up");
      continue;
    }
    if (e.usage) {
      static uint32_t usb_held[256] = {};
      if (e.down ? usb_held[e.usage]++ == 0 : (usb_held[e.usage] && --usb_held[e.usage] == 0)) input::inject_usb_key(e.usage, e.down);
      log::info("input-script", "frame {}: {} {}", frame, e.name, e.down ? "down" : "up");
      continue;
    }
    // Overlapping holds of one button keep it down until the last one ends.
    static uint32_t held[256] = {};
    if (e.down ? held[e.vk]++ == 0 : (held[e.vk] && --held[e.vk] == 0)) input::inject_key(e.vk, e.down);
    log::info("input-script", "frame {}: {} {}", frame, e.name, e.down ? "down" : "up");
  }
}

}  // namespace ssn::platform::input_script
