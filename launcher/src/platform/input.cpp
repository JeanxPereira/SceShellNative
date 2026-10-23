#include "platform/input.h"

#include <windows.h>

#include <array>
#include <atomic>
#include <mutex>

#include "core/log.h"
#include "platform/window.h"

namespace ssn::platform::input {
namespace {

// ---- Keyboard -> DualSense (host decision; documented in docs/input.md) ----------
struct KeyButton {
  uint32_t vk;
  uint32_t button;
};
constexpr KeyButton kKeyButtons[] = {
    {VK_UP, kPadUp},          {VK_DOWN, kPadDown},        {VK_LEFT, kPadLeft},       {VK_RIGHT, kPadRight},
    {VK_RETURN, kPadCross},   {'Z', kPadCross},           {VK_BACK, kPadCircle},     {VK_ESCAPE, kPadCircle},
    {'X', kPadCircle},        {'C', kPadSquare},          {'V', kPadTriangle},       {'Q', kPadL1},
    {'E', kPadR1},            {'1', kPadL2},              {'3', kPadR2},             {'F', kPadL3},
    {'G', kPadR3},            {VK_TAB, kPadOptions},      {VK_F1, kPadCreate},       {VK_HOME, kPadPs},
    {'T', kPadTouchPad},      {'M', kPadMute},
};
// Stick directions: WASD = left stick, IJKL = right stick (full deflection).
enum StickKey { kLUp, kLDown, kLLeft, kLRight, kRUp, kRDown, kRLeft, kRRight, kStickKeys };
constexpr uint32_t kStickVk[kStickKeys] = {'W', 'S', 'A', 'D', 'I', 'K', 'J', 'L'};

std::mutex g_mu;
uint32_t g_key_buttons = 0;
std::array<bool, kStickKeys> g_stick{};
std::array<uint32_t, 256> g_key_down{};  // vk -> pressed (keeps overlapping keys exact)
// Keys pressed by the input script (--input-script). Kept apart from the window's keys so
// the focus rule below (which drops window keys) never releases them.
std::array<uint32_t, 256> g_script_down{};

// Power state (see input.h). g_swallow_ps keeps the press that turned the pad on out of
// the button data until the PS button is released.
std::atomic<bool> g_powered{false};
bool g_swallow_ps = false;
void (*g_power_on_listener)() = nullptr;
std::atomic<int> g_host_apps{0};  // applications started by the shell, still running

// Recomputes the buttons/sticks from the window's and the script's keys (under g_mu).
// Returns true when this change turned the pad on.
bool recompute() {
  uint32_t buttons = 0;
  for (const auto& kb : kKeyButtons)
    if (g_key_down[kb.vk] || g_script_down[kb.vk]) buttons |= kb.button;
  bool turned_on = false;
  if (!g_powered.load() && (buttons & kPadPs)) {
    g_powered = true;
    g_swallow_ps = true;
    turned_on = true;
  }
  if (g_swallow_ps) {
    if (buttons & kPadPs) buttons &= ~kPadPs;
    else g_swallow_ps = false;
  }
  const bool on = g_powered.load();
  g_key_buttons = on ? buttons : 0;  // an off pad reports nothing
  for (int i = 0; i < kStickKeys; ++i) g_stick[i] = on && (g_key_down[kStickVk[i]] || g_script_down[kStickVk[i]]);
  return turned_on;
}

void notify_power_on() {
  if (g_power_on_listener) g_power_on_listener();
}

uint8_t axis(bool neg, bool pos) { return neg == pos ? 0x80 : (neg ? 0x00 : 0xFF); }

bool window_has_focus() {
  HWND h = main_window().hwnd();
  return h && GetForegroundWindow() == h;
}

// ---- XInput (loaded at run time: no import library needed) ------------------------
struct XGamepad {
  WORD wButtons;
  BYTE bLeftTrigger, bRightTrigger;
  SHORT sThumbLX, sThumbLY, sThumbRX, sThumbRY;
};
struct XState {
  DWORD dwPacketNumber;
  XGamepad Gamepad;
};
struct XVibration {
  WORD wLeftMotorSpeed, wRightMotorSpeed;
};
using XInputGetStateFn = DWORD(WINAPI*)(DWORD, XState*);
using XInputSetStateFn = DWORD(WINAPI*)(DWORD, XVibration*);

struct XInputApi {
  XInputGetStateFn get = nullptr;
  XInputSetStateFn set = nullptr;
  XInputApi() {
    for (const wchar_t* dll : {L"xinput1_4.dll", L"xinput9_1_0.dll"}) {
      if (HMODULE m = LoadLibraryW(dll)) {
        get = reinterpret_cast<XInputGetStateFn>(GetProcAddress(m, "XInputGetState"));
        set = reinterpret_cast<XInputSetStateFn>(GetProcAddress(m, "XInputSetState"));
        if (get) return;
      }
    }
  }
};
XInputApi& xinput() {
  static XInputApi api;
  return api;
}

// XInput button bits (XINPUT_GAMEPAD_*) -> DualSense.
constexpr std::pair<WORD, uint32_t> kXButtons[] = {
    {0x0001, kPadUp},   {0x0002, kPadDown},    {0x0004, kPadLeft},   {0x0008, kPadRight},  {0x0010, kPadOptions},
    {0x0020, kPadCreate}, {0x0040, kPadL3},    {0x0080, kPadR3},     {0x0100, kPadL1},     {0x0200, kPadR1},
    {0x1000, kPadCross}, {0x2000, kPadCircle}, {0x4000, kPadSquare}, {0x8000, kPadTriangle},
};
constexpr int kLeftDeadzone = 7849;   // XINPUT_GAMEPAD_LEFT_THUMB_DEADZONE
constexpr int kRightDeadzone = 8689;  // XINPUT_GAMEPAD_RIGHT_THUMB_DEADZONE
constexpr BYTE kTriggerThreshold = 30;  // XINPUT_GAMEPAD_TRIGGER_THRESHOLD

// XInput thumb (-32768..32767, up positive) -> DualSense stick byte (0..255, up = 0).
uint8_t thumb(SHORT v, int deadzone, bool invert) {
  int x = v;
  if (x > -deadzone && x < deadzone) x = 0;
  if (invert) x = -x - 1;
  int b = (x + 32768) >> 8;
  if (x == 0 || x == -1) b = 0x80;
  return static_cast<uint8_t>(b < 0 ? 0 : (b > 255 ? 255 : b));
}

uint8_t merge_axis(uint8_t a, uint8_t b) {
  // Whichever source is deflected further from the centre wins.
  int da = a - 0x80, db = b - 0x80;
  return (db * db > da * da) ? b : a;
}

// ---- Declared USB keyboard ------------------------------------------------------------
// HOST VALUE: PS/2 scan code set 1 (what Windows reports in WM_KEYDOWN lParam) -> USB HID
// usage (HID Usage Tables, keyboard page 0x07). A position mapping: the firmware applies the
// layout (eboot 0x1B9C50 with the user's keyboard type), as for a USB keyboard on the console.
uint16_t hid_usage(uint32_t scan, bool ext) {
  static const uint8_t kSet1[0x59] = {
      0,    0x29, 0x1E, 0x1F, 0x20, 0x21, 0x22, 0x23, 0x24, 0x25, 0x26, 0x27, 0x2D, 0x2E, 0x2A, 0x2B,  // 00-0F
      0x14, 0x1A, 0x08, 0x15, 0x17, 0x1C, 0x18, 0x0C, 0x12, 0x13, 0x2F, 0x30, 0x28, 0xE0, 0x04, 0x16,  // 10-1F
      0x07, 0x09, 0x0A, 0x0B, 0x0D, 0x0E, 0x0F, 0x33, 0x34, 0x35, 0xE1, 0x31, 0x1D, 0x1B, 0x06, 0x19,  // 20-2F
      0x05, 0x11, 0x10, 0x36, 0x37, 0x38, 0xE5, 0x55, 0xE2, 0x2C, 0x39, 0x3A, 0x3B, 0x3C, 0x3D, 0x3E,  // 30-3F
      0x3F, 0x40, 0x41, 0x42, 0x43, 0x53, 0x47, 0x5F, 0x60, 0x61, 0x56, 0x5C, 0x5D, 0x5E, 0x57, 0x59,  // 40-4F
      0x5A, 0x5B, 0x62, 0x63, 0,    0,    0x64, 0x44, 0x45};                                         // 50-58
  if (!ext) return scan < sizeof(kSet1) ? kSet1[scan] : 0;
  switch (scan) {
    case 0x1C: return 0x58;  // keypad Enter
    case 0x1D: return 0xE4;  // right Ctrl
    case 0x35: return 0x54;  // keypad /
    case 0x37: return 0x46;  // PrintScreen
    case 0x38: return 0xE6;  // right Alt
    case 0x47: return 0x4A;  // Home
    case 0x48: return 0x52;  // Up
    case 0x49: return 0x4B;  // PageUp
    case 0x4B: return 0x50;  // Left
    case 0x4D: return 0x4F;  // Right
    case 0x4F: return 0x4D;  // End
    case 0x50: return 0x51;  // Down
    case 0x51: return 0x4E;  // PageDown
    case 0x52: return 0x49;  // Insert
    case 0x53: return 0x4C;  // Delete
    case 0x5B: return 0xE3;  // left GUI
    case 0x5C: return 0xE7;  // right GUI
    case 0x5D: return 0x65;  // Application
    default: return 0;
  }
}

std::atomic<bool> g_kb_open{false};
std::array<uint8_t, 256> g_kb_window{};  // usage -> held (window keys)
std::array<uint8_t, 256> g_kb_script{};  // usage -> held (script keys)
std::array<uint16_t, 256> g_vk_usage{};  // vk -> usage it was routed to (0: went to the pad)

}  // namespace

void set_powered_at_boot(bool on) { g_powered = on; }
bool powered() { return g_powered.load(); }
void set_power_on_listener(void (*fn)()) { g_power_on_listener = fn; }
void set_host_app_running(bool running) { g_host_apps += running ? 1 : -1; }

void on_key(uint32_t vk, bool down) {
  if (vk >= 256) return;
  bool turned_on;
  {
    std::lock_guard lk(g_mu);
    g_key_down[vk] = down;
    turned_on = recompute();
  }
  if (turned_on) notify_power_on();
}

void inject_key(uint32_t vk, bool down) {
  if (vk >= 256) return;
  bool turned_on;
  {
    std::lock_guard lk(g_mu);
    g_script_down[vk] = down;
    turned_on = recompute();
  }
  if (turned_on) notify_power_on();
}

void keyboard_set_open(bool open) {
  static const bool noted = (log::info("input", "HOST VALUE: the PC keyboard is also one USB keyboard (connected, HID usages from "
                                                "the scan codes); keys go to it while the firmware holds a keyboard handle"),
                             true);
  (void)noted;
  std::lock_guard lk(g_mu);
  if (g_kb_open.exchange(open) == open) return;
  log::info("input", "USB keyboard handle {}", open ? "opened (keys -> keyboard)" : "closed (keys -> DualSense)");
}

bool keyboard_is_open() { return g_kb_open.load(); }

KeyboardData keyboard_read() {
  KeyboardData d;
  std::lock_guard lk(g_mu);
  for (int u = 4; u < 256 && d.count < 16; ++u) {
    if (!g_kb_window[u] && !g_kb_script[u]) continue;
    if (u >= 0xE0 && u <= 0xE7) {
      d.modifiers |= static_cast<uint8_t>(1u << (u - 0xE0));
      continue;
    }
    d.keys[d.count++] = static_cast<uint16_t>(u);
  }
  // HOST VALUE: the LEDs follow the PC's lock-key toggles.
  if (GetKeyState(VK_NUMLOCK) & 1) d.led |= 1;
  if (GetKeyState(VK_CAPITAL) & 1) d.led |= 2;
  if (GetKeyState(VK_SCROLL) & 1) d.led |= 4;
  return d;
}

void on_key_scan(uint32_t vk, uint32_t scan, bool extended, bool down) {
  if (vk >= 256) return;
  {
    std::lock_guard lk(g_mu);
    if (down && g_kb_open.load() && !g_key_down[vk]) {
      // A key pressed while the keyboard is open belongs to the keyboard until released.
      const uint16_t u = hid_usage(scan, extended);
      if (u) {
        g_vk_usage[vk] = u;
        g_kb_window[u] = 1;
        return;
      }
    }
    if (!down && g_vk_usage[vk]) {
      g_kb_window[g_vk_usage[vk]] = 0;
      g_vk_usage[vk] = 0;
      return;
    }
    if (g_vk_usage[vk]) return;  // auto-repeat of a keyboard key
  }
  on_key(vk, down);
}

void inject_usb_key(uint16_t usage, bool down) {
  if (usage >= 256) return;
  std::lock_guard lk(g_mu);
  g_kb_script[usage] = down ? 1 : 0;
}

PadData read() {
  PadData d;
  d.connected = g_powered.load();
  if (!d.connected) return d;  // off: no report (the PSS layer only reads opened pads)
  {
    std::lock_guard lk(g_mu);
    if (!window_has_focus()) {
      // Keys released while another window had the focus never reach us: drop them.
      g_key_down.fill(0);
      g_kb_window.fill(0);
      g_vk_usage.fill(0);
      recompute();
    }
    d.buttons = g_key_buttons;
    d.lx = axis(g_stick[kLLeft], g_stick[kLRight]);
    d.ly = axis(g_stick[kLUp], g_stick[kLDown]);
    d.rx = axis(g_stick[kRLeft], g_stick[kRRight]);
    d.ry = axis(g_stick[kRUp], g_stick[kRDown]);
    if (d.buttons & kPadL2) d.l2 = 0xFF;
    if (d.buttons & kPadR2) d.r2 = 0xFF;
  }
  auto& x = xinput();
  XState s{};
  // While an application runs in front, its window has the pad.
  const bool app_in_front = g_host_apps.load() > 0 && !window_has_focus();
  if (!app_in_front && x.get && x.get(0, &s) == ERROR_SUCCESS) {
    const XGamepad& g = s.Gamepad;
    for (const auto& [xb, pb] : kXButtons)
      if (g.wButtons & xb) d.buttons |= pb;
    if (g.bLeftTrigger > kTriggerThreshold) d.buttons |= kPadL2;
    if (g.bRightTrigger > kTriggerThreshold) d.buttons |= kPadR2;
    d.l2 = g.bLeftTrigger > d.l2 ? g.bLeftTrigger : d.l2;
    d.r2 = g.bRightTrigger > d.r2 ? g.bRightTrigger : d.r2;
    d.lx = merge_axis(d.lx, thumb(g.sThumbLX, kLeftDeadzone, false));
    d.ly = merge_axis(d.ly, thumb(g.sThumbLY, kLeftDeadzone, true));
    d.rx = merge_axis(d.rx, thumb(g.sThumbRX, kRightDeadzone, false));
    d.ry = merge_axis(d.ry, thumb(g.sThumbRY, kRightDeadzone, true));
  }
  return d;
}

void set_vibration(uint8_t large_motor, uint8_t small_motor) {
  auto& x = xinput();
  if (!x.set) return;
  XVibration v{static_cast<WORD>(large_motor * 257), static_cast<WORD>(small_motor * 257)};
  x.set(0, &v);
}

}  // namespace ssn::platform::input
