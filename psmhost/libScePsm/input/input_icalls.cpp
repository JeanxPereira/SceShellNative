// Input icalls of libScePsm.sprx (FW 4.03): Sce.PlayStation.Core.Input.{GamePad, Motion,
// RemoteControl, Touch}, Sce.PlayStation.Orbis.Input.{InputManager, DebugTools} and the
// pad-related Sce.PlayStation.Orbis.Environment.SystemParameters entries. Each body is
// the decompiled function at the address given beside it (libScePsm_dyn.elf.i64).
#include <cstdint>
#include <cstring>

#include <mono/metadata/object.h>

#include "host/boundary.h"
#include "libScePsm/error.h"
#include "libScePsm/input/pss_input.h"
#include "platform/input.h"

namespace ssn::psm::input {
namespace {

constexpr int32_t kErrArg = static_cast<int32_t>(0x80B30001u);
constexpr int32_t kErrNull = static_cast<int32_t>(0x80B30002u);
constexpr int32_t kErrRange = static_cast<int32_t>(0x80B30003u);

// PSS button bits -> Sce.PlayStation.Core.Input.GamePadButtons (GetDataNative 0x2dad80 and
// GetSystemButtonsStateNative 0x2dca80 use the same chain of tests).
uint32_t to_gamepad_buttons(uint32_t v) {
  uint32_t b = 0;
  if (v & 0x4) b |= 0x1;              // Left
  if (v & 0x1) b |= 0x2;              // Up
  if (v & 0x8) b |= 0x4;              // Right
  if (v & 0x2) b |= 0x8;              // Down
  if (v & 0x40) b |= 0x10;            // Square
  if (v & 0x80) b |= 0x20;            // Triangle
  if (v & 0x20) b |= 0x40;            // Circle
  if (v & 0x10) b |= 0x80;            // Cross
  if (v & 0x100) b |= 0x100;          // Start (Options)
  if (v & 0x200) b |= 0x200;          // Select
  if (v & 0x400) b |= 0x400;          // L
  if (v & 0x800) b |= 0x1000;         // L2
  if (v & 0x1000) b |= 0x4000;        // L3
  if (v & 0x2000) b |= 0x800;         // R
  if (v & 0x4000) b |= 0x2000;        // R2
  if (v & 0x8000) b |= 0x8000;        // R3
  if (v & 0x100000) b |= 0x40000;     // TouchPad
  if (v & 0x200000) b |= 0x90000;     // Enter | Move ("or byte [rbx+6], 9")
  if (v & 0x400000) b |= 0x10000;     // Enter
  if (v & 0x800000) b |= 0x20000;     // Back
  if (v & 0x80000000u) b |= 0x80000000u;  // Intercepted
  return b;
}

// Enter/Back from Cross/Circle by dword_53FC88 (2 = circle is enter).
uint32_t with_enter_back(uint32_t v) {
  uint32_t enter_back;
  if (pad_button_meaning() == 2) enter_back = ((v << 10) & 0x20000) | ((v << 10) & 0x10000);
  else enter_back = ((v << 9) & 0x10000) | ((v & 0x40) << 11);
  return v | enter_back;
}

void default_controller_info(uint8_t* info) {
  const int64_t id = -1;
  const uint64_t q = 0xFFFFFFFFull;
  std::memcpy(info, &id, 8);
  std::memcpy(info + 8, &q, 8);
  info[16] = info[17] = 0;
  std::memset(info + 20, 0, 8);
}

// Device index resolution shared by the readers: behaviour 0/1/16/17 (bit test 0x30003)
// only answer index 0 with the merged device; 2 (EachDevice) answers accepted slots.
// Returns an error, or 0 with `out` = slot (negative: nothing to read).
int32_t resolve_index(InputManager& im, uint32_t index, int32_t& out) {
  std::lock_guard lk(im.mu);
  const uint32_t b = im.behavior;
  if (b > 17) return kErrArg;
  if ((0x30003u >> b) & 1) {
    if (index != 0) return kErrRange;
    out = im.active;
    return 0;
  }
  if (b != 2) return kErrArg;
  if (index > 23) return kErrRange;
  out = slot_accepted(im, index) ? static_cast<int32_t>(index) : -1;
  return 0;
}

// ---- Sce.PlayStation.Core.Input.GamePad -------------------------------------------

// GetDataNative(int, ref GamePadData) — 0x2dad80. GamePadData: Skip(+0) Buttons(+4)
// ButtonsPrev(+8) ButtonsDown(+12) ButtonsUp(+16) AnalogLeftX/Y RightX/Y (+20..+32).
int32_t GamePadGetData(int32_t device_index, uint8_t* d) {
  if (!d) return kErrNull;
  std::memset(d, 0, 36);
  ensure_started();
  if (!config().gamepad) return 0;
  InputManager& im = manager();
  const uint32_t v8 = static_cast<uint32_t>(device_index);
  int32_t v10 = device_index;
  if (v8 != 0) {
    if (!im.alive) return kErrInputSystem;
    const int32_t err = resolve_index(im, v8, v10);
    if (err) return err;
    if (v10 < 0) return 0;
  } else {
    // Merged read: find the device that produced new input this frame.
    {
      std::lock_guard lk(im.mu);
      if (!slot_accepted(im, static_cast<uint32_t>(im.active))) im.active = -1;
      uint64_t pad[24] = {}, remote[24] = {};
      bool act[24] = {};
      for (uint32_t s = 0; s < 24; ++s) {
        if (!im.slots[s].connected) continue;
        PssPad p{};
        if (s <= 7) read_pss_pad(s, &p);  // slots 8..23: VirtualPadManager pads (none created)
        uint64_t v18 = p.buttons & 0xF3FFFF;
        if (s <= 7 && pss_touch_count(s) != 0) v18 |= 0x400000000ull;  // a touch Down/Move
        pad[s] = v18;
      }
      for (uint32_t s = 0; s < 8; ++s) {
        if (!im.slots[s].connected) continue;
        uint64_t r[2];
        if (read_pss_remote(s, r) == 0) remote[s] = r[0];
      }
      for (uint32_t s = 0; s < 24; ++s)
        act[s] = ((remote[s] & ~im.slots[s].prev_remote) | (pad[s] & ~im.slots[s].prev_pad)) != 0;
      const uint32_t b = im.behavior;
      if (im.bypass || b == 0 || b == 16 || b == 17) {
        for (uint32_t i = 0; i < 24; ++i)
          if (slot_accepted(im, i) && act[i]) im.active = static_cast<int32_t>(i);
      } else if (b == 1) {
        for (uint32_t k = 0; k < 24; ++k) {
          if (slot_accepted(im, k)) {
            if (act[k]) im.active = static_cast<int32_t>(k);
          } else if (im.slots[k].user_id > 0 && act[k]) {
            im.invalid_op = true;
            im.invalid_user = im.slots[k].user_id;
          }
        }
      }
      for (uint32_t s = 0; s < 24; ++s) {
        im.slots[s].prev_pad = pad[s];
        im.slots[s].prev_remote = remote[s];
      }
    }
    if (!im.alive) return kErrInputSystem;
    std::lock_guard lk(im.mu);
    const uint32_t b = im.behavior;
    if (b > 17) return kErrArg;
    if ((0x30003u >> b) & 1) v10 = im.active;
    else if (b == 2) v10 = (slot_accepted(im, 0) ? 1 : 0) - 1;
    else return kErrArg;
    if (v10 < 0) return 0;
  }

  uint32_t buttons = 0;
  if (v10 <= 7) {
    PssPad p{};
    if (read_pss_pad(static_cast<uint32_t>(v10), &p) != 0) return kErrInputSystem;
    std::memcpy(d + 20, &p.lx, 16);
    buttons = to_gamepad_buttons(p.buttons);
  }
  // v10 > 7: VirtualPadManager slot (+2168 table); none exist on the host.
  const uint32_t v84 = with_enter_back(buttons);
  if (buttons & 0xC0) buttons = v84;
  std::memcpy(d + 4, &buttons, 4);
  const uint32_t prev = g_prev_gamepad[v8 < 24 ? v8 : 0];
  const uint32_t down = v84 & ~prev, up = prev & ~v84;
  std::memcpy(d + 8, &prev, 4);
  std::memcpy(d + 12, &down, 4);
  std::memcpy(d + 16, &up, 4);
  g_prev_gamepad[v8 < 24 ? v8 : 0] = v84;
  return 0;
}
SSN_ICALL("Sce.PlayStation.Core.Input.GamePad::GetDataNative(int,Sce.PlayStation.Core.Input.GamePadData&)", GamePadGetData);

// ---- Motion / RemoteControl / Touch -----------------------------------------------

// Motion::GetDataNative(int, ref MotionData) — 0x2dba00: clears the 44-byte struct; with
// Motion input off (byte 0x54107A) the result is 0 and the data stays zero.
int32_t MotionGetData(int32_t, uint8_t* d) {
  if (!d) return kErrNull;
  std::memset(d, 0, 44);
  ensure_started();
  if (!config().motion) return 0;
  host::note_unimplemented("libScePsm Motion::GetDataNative with Motion input enabled");
  return 0;
}
SSN_ICALL("Sce.PlayStation.Core.Input.Motion::GetDataNative(int,Sce.PlayStation.Core.Input.MotionData&)", MotionGetData);

// Motion::ResetOrientationNative(int) — 0x2dbbe0: 0 while Motion input is off.
int32_t MotionResetOrientation(int32_t) {
  ensure_started();
  if (!config().motion) return 0;
  host::note_unimplemented("libScePsm Motion::ResetOrientationNative with Motion input enabled");
  return 0;
}
SSN_ICALL("Sce.PlayStation.Core.Input.Motion::ResetOrientationNative(int)", MotionResetOrientation);

// RemoteControl::GetDataNative(int, ref RemoteControlData) — 0x2dbcd0: clears the
// 40-byte struct; 0 while RemoteControl input (HIBYTE word_54107A) is off.
int32_t RemoteControlGetData(int32_t, uint8_t* d) {
  if (!d) return kErrNull;
  std::memset(d, 0, 40);
  ensure_started();
  if (!config().remote) return 0;
  host::note_unimplemented("libScePsm RemoteControl::GetDataNative with RemoteControl input enabled");
  return 0;
}
SSN_ICALL("Sce.PlayStation.Core.Input.RemoteControl::GetDataNative(int,Sce.PlayStation.Core.Input.RemoteControlData&)",
          RemoteControlGetData);

// Touch::GetDataNative(int, TouchData[], ref int) — 0x2da930: null -> 0x80B30002; empty
// array -> 0x80B30003; clears the array (32 bytes per TouchData) and the count; 0 while
// Touch input (LOBYTE word_541078) is off.
int32_t TouchGetData(int32_t, MonoArray* arr, int32_t* num) {
  if (!arr || !num) return kErrNull;
  const auto n = static_cast<int32_t>(mono_array_length(arr));
  if (n <= 0) return kErrRange;
  char* p = mono_array_addr_with_size(arr, 32, 0);
  if (!p) return static_cast<int32_t>(0x80B30007u);
  *num = 0;
  std::memset(p, 0, 32 * static_cast<size_t>(n));
  ensure_started();
  if (!config().touch) return 0;
  host::note_unimplemented("libScePsm Touch::GetDataNative with Touch input enabled");
  return 0;
}
SSN_ICALL("Sce.PlayStation.Core.Input.Touch::GetDataNative(int,Sce.PlayStation.Core.Input.TouchData[],int&)", TouchGetData);

// ---- Sce.PlayStation.Orbis.Input.InputManager -------------------------------------

// SetBypassSwitchNative(bool) — 0x2dc760.
int32_t SetBypassSwitch(uint8_t enable) {
  ensure_started();
  InputManager& im = manager();
  if (!im.alive) return kErrInputSystem;
  std::lock_guard lk(im.mu);
  im.bypass = enable != 0;
  if (enable) {
    im.invalid_op = false;
    im.invalid_user = -1;
  }
  return 0;
}
SSN_ICALL("Sce.PlayStation.Orbis.Input.InputManager::SetBypassSwitchNative(bool)", SetBypassSwitch);

// GetBypassSwitchNative(ref bool) — 0x2dc7c0.
int32_t GetBypassSwitch(uint8_t* enable) {
  ensure_started();
  InputManager& im = manager();
  if (!im.alive) return kErrInputSystem;
  if (!enable) return kErrNull;
  std::lock_guard lk(im.mu);
  *enable = im.bypass ? 1 : 0;
  return 0;
}
SSN_ICALL("Sce.PlayStation.Orbis.Input.InputManager::GetBypassSwitchNative(bool&)", GetBypassSwitch);

// SetDeviceIndexBehaviorNative(DeviceIndexBehavior, long, ref bool) — 0x2dc820 -> sub_1FD1D0.
int32_t SetDeviceIndexBehavior(uint32_t mode, int64_t param, uint8_t* updated) {
  ensure_started();
  return set_device_index_behavior(manager(), mode, param, updated);
}
SSN_ICALL("Sce.PlayStation.Orbis.Input.InputManager::SetDeviceIndexBehaviorNative(Sce.PlayStation.Orbis.Input.DeviceIndexBehavior,long,bool&)",
          SetDeviceIndexBehavior);

// GetDeviceIndexBehaviorNative(ref DeviceIndexBehavior, ref long) — 0x2dc870.
int32_t GetDeviceIndexBehavior(uint32_t* mode, int64_t* param) {
  ensure_started();
  InputManager& im = manager();
  if (!im.alive) return kErrInputSystem;
  if (!mode || !param) return kErrNull;
  std::lock_guard lk(im.mu);
  *mode = im.behavior;
  *param = im.behavior == 16 ? im.param_device : (im.behavior == 1 ? static_cast<int64_t>(im.param_user) : 0);
  return 0;
}
SSN_ICALL("Sce.PlayStation.Orbis.Input.InputManager::GetDeviceIndexBehaviorNative(Sce.PlayStation.Orbis.Input.DeviceIndexBehavior&,long&)",
          GetDeviceIndexBehavior);

// GetControllerInfoNative(int, bool isFilteredIndex, ref ControllerInfo, ref int handle)
// — 0x2dc900.
int32_t GetControllerInfo(int32_t device_index, uint8_t filtered, uint8_t* info, int32_t* handle) {
  ensure_started();
  InputManager& im = manager();
  if (!im.alive) return kErrInputSystem;
  if (!info || !handle) return kErrNull;
  default_controller_info(info);
  *handle = -1;
  const uint32_t idx = static_cast<uint32_t>(device_index);
  int32_t slot;
  if (filtered) {
    const int32_t err = resolve_index(im, idx, slot);
    if (err) return err;
    if (slot < 0) return 0;
  } else {
    if (idx > 7) return kErrRange;
    slot = device_index;
  }
  fill_controller_info(im, static_cast<uint32_t>(slot), info);
  std::lock_guard lk(im.mu);
  *handle = im.slots[slot].handle;
  return 0;
}
SSN_ICALL("Sce.PlayStation.Orbis.Input.InputManager::GetControllerInfoNative(int,bool,Sce.PlayStation.Orbis.Input.ControllerInfo&,int&)",
          GetControllerInfo);

// GetSystemButtonsStateNative(int, ref ControllerInfo, ref SystemButtons, ref GamePadButtons)
// — 0x2dca80. SystemButtons (11 bytes): Ps, Share, Eject, UniversalMute, PushToTalk,
// PartnerButton, AudioVolumeUp, AudioVolumeDown, AudioBalanceChat, AudioBalanceGame, EnterTuning.
int32_t GetSystemButtonsState(int32_t device_index, uint8_t* info, uint8_t* sys, uint32_t* normal) {
  ensure_started();
  InputManager& im = manager();
  if (!im.alive) return kErrInputSystem;
  if (!info || !sys || !normal) return kErrNull;
  default_controller_info(info);
  std::memset(sys, 0, 11);
  *normal = 0;
  const uint32_t idx = static_cast<uint32_t>(device_index);
  if (idx > 23) return kErrRange;
  fill_controller_info(im, idx, info);
  if (!info[16]) return 0;
  int32_t connection;
  std::memcpy(&connection, info + 20, 4);
  if (idx <= 7 && connection != 2) {
    PssPad p{};
    if (read_pss_pad(idx, &p) == 0) {
      const uint32_t v = p.buttons;
      sys[0] = (v & 0x40000) != 0;      // Ps
      sys[1] = (v & 0x80000) != 0;      // Share
      sys[3] = (v >> 24) & 1;           // UniversalMute
      sys[4] = (v & 0x2000000) != 0;    // PushToTalk
      sys[6] = (v & 0x4000000) != 0;    // AudioVolumeUp
      sys[7] = (v & 0x8000000) != 0;    // AudioVolumeDown
      sys[8] = (v & 0x10000000) != 0;   // AudioBalanceChat
      sys[9] = (v & 0x20000000) != 0;   // AudioBalanceGame
      sys[10] = (v & 0x40000000) != 0;  // EnterTuning
      *normal = to_gamepad_buttons(v);
    }
    uint64_t r[2];
    if (read_pss_remote(idx, r) == 0) {
      sys[2] = (r[0] & 0x4000) != 0;           // Eject
      sys[5] = static_cast<uint8_t>(r[1]);     // PartnerButton
    }
  }
  // idx 8..23: VirtualPadManager slots (none exist on the host).
  if (*normal & 0xC0) *normal = with_enter_back(*normal);
  return 0;
}
SSN_ICALL("Sce.PlayStation.Orbis.Input.InputManager::GetSystemButtonsStateNative(int,Sce.PlayStation.Orbis.Input.ControllerInfo&,Sce.PlayStation.Orbis.Input.SystemButtons&,Sce.PlayStation.Core.Input.GamePadButtons&)",
          GetSystemButtonsState);

// GetInvalidOperationByOtherUsersNative(ref bool, ref int) — 0x2dcf90: reports and clears.
int32_t GetInvalidOperationByOtherUsers(uint8_t* does, int32_t* user) {
  ensure_started();
  InputManager& im = manager();
  if (!im.alive) return kErrInputSystem;
  if (!does || !user) return kErrNull;
  std::lock_guard lk(im.mu);
  if (im.bypass) {
    *does = 0;
    *user = -1;
  } else {
    *does = im.invalid_op ? 1 : 0;
    *user = im.invalid_op ? im.invalid_user : -1;
  }
  im.invalid_op = false;
  im.invalid_user = -1;
  return 0;
}
SSN_ICALL("Sce.PlayStation.Orbis.Input.InputManager::GetInvalidOperationByOtherUsersNative(bool&,int&)",
          GetInvalidOperationByOtherUsers);

// GetControllerFocusStatusNative(ref ControllerFocusStatus) — 0x2dd030: Unknown (2) unless
// device slot 7 is connected, then bit 31 of its PSS buttons (0 Focused, 1 Intercepted).
int32_t GetControllerFocusStatus(uint32_t* status) {
  ensure_started();
  InputManager& im = manager();
  if (!im.alive) return kErrInputSystem;
  if (!status) return kErrNull;
  *status = 2;
  bool slot7;
  {
    std::lock_guard lk(im.mu);
    slot7 = im.slots[7].connected;
  }
  if (slot7) {
    PssPad p{};
    read_pss_pad(7, &p);
    *status = p.buttons >> 31;
  }
  return 0;
}
SSN_ICALL("Sce.PlayStation.Orbis.Input.InputManager::GetControllerFocusStatusNative(Sce.PlayStation.Orbis.Input.ControllerFocusStatus&)",
          GetControllerFocusStatus);

// SetVibration(int handle, ref PadVibrationParam) — 0x2dd0d0 -> scePadSetVibration.
int32_t SetVibration(int32_t handle, const uint8_t* param) {
  ensure_started();
  if (!param) return kErrNull;
  if (handle != kHostPadHandle) return kScePadErrorInvalidHandle;
  platform::input::set_vibration(param[0], param[1]);
  return 0;
}
SSN_ICALL("Sce.PlayStation.Orbis.Input.InputManager::SetVibration(int,Sce.PlayStation.Orbis.Input.PadVibrationParam&)",
          SetVibration);

// DebugTools::scePadReadStateNative(int handle, ref uint buttons) — 0x2dd110:
// scePadReadState(handle, &data); buttons = data.buttons; returns the libScePad result.
int32_t DebugPadReadState(int32_t handle, uint32_t* buttons) {
  if (!buttons) return kErrNull;
  ensure_started();
  uint32_t b = 0;
  const int32_t r = pad_read_state_buttons(handle, &b);
  *buttons = b;
  return r;
}
SSN_ICALL("Sce.PlayStation.Orbis.Input.DebugTools::scePadReadStateNative(int,uint&)", DebugPadReadState);

// ---- Sce.PlayStation.Orbis.Environment.SystemParameters (pad button meaning) -------

// GetSystemPadButtonMeaning(out int) — 0x2d4860.
int32_t GetSystemPadButtonMeaning(int32_t* value) {
  const int32_t m = pad_button_meaning();
  *value = m == 1 ? 1 : (m == 2 ? 2 : 0);
  return 0;
}
SSN_ICALL("Sce.PlayStation.Orbis.Environment.SystemParameters::GetSystemPadButtonMeaning(int&)", GetSystemPadButtonMeaning);

// SetSystemPadButtonMeaning(int) — 0x2d4880.
int32_t SetSystemPadButtonMeaning(int32_t value) {
  set_pad_button_meaning(value == 1 ? 1 : (value == 2 ? 2 : 0));
  return 0;
}
SSN_ICALL("Sce.PlayStation.Orbis.Environment.SystemParameters::SetSystemPadButtonMeaning(int)", SetSystemPadButtonMeaning);

// IsValidUserIdNative(int) — 0x2d48a0.
MonoBoolean IsValidUserId(int32_t user) {
  const uint32_t u = static_cast<uint32_t>(user);
  return ((u & 0xF0000000u) == 0x20000000u || (u & 0xD0000000u) == 0x10000000u) ? 1 : 0;
}
SSN_ICALL("Sce.PlayStation.Orbis.Environment.SystemParameters::IsValidUserIdNative", IsValidUserId);

}  // namespace
}  // namespace ssn::psm::input
