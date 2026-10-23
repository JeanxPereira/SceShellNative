// Host input device: the PC keyboard (and the first XInput pad, when one is attached)
// presented as one DualSense, at the physical-device boundary of the firmware's input
// stack — the data libScePad hands its callers (ScePadData: SCE_PAD_BUTTON_* bits,
// 8-bit sticks/triggers, connection) and the device's identity (scePadGetCapability,
// scePadGetControllerInformation). Everything above that boundary is the firmware's
// own code, reproduced in psmhost/libScePsm/input and psmhost/libSysBridge.
//
// The only host decision is which PC key / XInput control drives which DualSense
// control; the table is in input.cpp and docs/input.md.
#pragma once
#include <cstdint>

namespace ssn::platform::input {

// SCE_PAD_BUTTON_* bits as libScePad reports them in ScePadData.buttons (the bit each
// one occupies is confirmed by libScePsm's converter, sub_189BA0 — see docs/input.md).
enum PadButton : uint32_t {
  kPadCreate = 0x00000001,     // SHARE / CREATE (libScePsm: system button "Share")
  kPadL3 = 0x00000002,
  kPadR3 = 0x00000004,
  kPadOptions = 0x00000008,
  kPadUp = 0x00000010,
  kPadRight = 0x00000020,
  kPadDown = 0x00000040,
  kPadLeft = 0x00000080,
  kPadL2 = 0x00000100,
  kPadR2 = 0x00000200,
  kPadL1 = 0x00000400,
  kPadR1 = 0x00000800,
  kPadTriangle = 0x00001000,
  kPadCircle = 0x00002000,
  kPadCross = 0x00004000,
  kPadSquare = 0x00008000,
  kPadPs = 0x00010000,         // libScePsm: system button "Ps"
  kPadTouchPad = 0x00100000,
  kPadMute = 0x00200000,       // libScePsm: system button "UniversalMute"
};

// The subset of ScePadData the firmware's PSM converter reads (sub_189BA0 / sub_1888F0):
// buttons (+0x00), sticks (+0x04..+0x07), and the connection state.
struct PadData {
  uint32_t buttons = 0;
  uint8_t lx = 0x80, ly = 0x80, rx = 0x80, ry = 0x80;  // 0..255, 0x80 = centre, up/left = 0
  uint8_t l2 = 0, r2 = 0;                              // analog triggers
  bool connected = true;
};

// Power of the emulated DualSense. A DualSense is off while the console boots and is only
// turned on by its PS button; the press that turns it on connects the controller (the
// console sees a device being added) and is not reported as button data. The host pad
// starts off unless `on` is set (--pad-on-at-boot: the controller was already on, e.g. the
// console was woken with it). The keyboard's PS key (Home) or the input script's `ps`
// turns it on; XInput does not expose the Guide button, so an XInput pad cannot.
void set_powered_at_boot(bool on);
bool powered();
// Called once, on the thread that delivered the PS press, when the pad turns on.
void set_power_on_listener(void (*fn)());

// Window keyboard messages (called on the thread that pumps the window).
void on_key(uint32_t vk, bool down);

// Scripted key events (--input-script, platform/input_script.h): the same key table as
// the window keyboard, but independent of window focus (headless runs have no window).
void inject_key(uint32_t vk, bool down);

// ---- Declared USB keyboard (HOST VALUE; docs/input.md "USB keyboard") --------------------
// The PC keyboard is also presented as one USB keyboard, at the boundary of the firmware's
// keyboard library (libSceKeyboard, statically linked in the ShellUI eboot): the data a
// sceKeyboardRead returns (SceKeyboardData: HID modifier byte, LED bits, up to 16 held HID
// usages, connected). While a firmware keyboard handle is open (the IME opens the user's
// keyboard when an OSK session is ready, eboot 0x121E50), window key presses go to this
// keyboard instead of the DualSense key table; with no handle open they drive the DualSense.
struct KeyboardData {
  bool connected = true;
  uint8_t modifiers = 0;  // HID: 0x01 LCtrl 0x02 LShift 0x04 LAlt 0x08 LGui 0x10 RCtrl 0x20 RShift 0x40 RAlt 0x80 RGui
  uint8_t led = 0;        // bit0 NumLock, bit1 CapsLock, bit2 ScrollLock
  uint8_t count = 0;
  uint16_t keys[16] = {};  // HID usages (modifiers excluded)
};
void keyboard_set_open(bool open);
bool keyboard_is_open();
KeyboardData keyboard_read();
// Window key with its PS/2 set-1 scan code (lParam bits 16..24); routed to the USB keyboard
// while a handle is open, to on_key otherwise.
void on_key_scan(uint32_t vk, uint32_t scan, bool extended, bool down);
// Scripted USB keyboard keys (--input-script text / key events): HID usage, held or released.
void inject_usb_key(uint16_t usage, bool down);

// Current state of the emulated DualSense (any thread). Polls XInput.
PadData read();

// scePadSetVibration: drives the XInput pad's motors when one is attached.
void set_vibration(uint8_t large_motor, uint8_t small_motor);

}  // namespace ssn::platform::input
