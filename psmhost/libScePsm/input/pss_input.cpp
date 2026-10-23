// libScePsm.sprx (FW 4.03) input stack below the icalls — see pss_input.h.
#include "libScePsm/input/pss_input.h"

#include <algorithm>
#include <chrono>
#include <cstring>
#include <thread>

#include "core/log.h"
#include "host/boundary.h"
#include "libSceMbus/mbus.h"
#include "platform/input.h"

namespace ssn::psm::input {

uint32_t g_prev_gamepad[24];
uint64_t g_prev_remote[24];

namespace {

constexpr int32_t kPssErrorInvalidArg = static_cast<int32_t>(0x80B3F003u);  // sub_181900 / sub_1844B0

// ---- PSS state tables --------------------------------------------------------------
std::mutex g_pss_mu;
PssPad g_pss_pad[8];            // 0x4fd2a0
uint64_t g_pss_remote[8][2];    // 0x525750 (16 bytes per device)
// dword_4FBE90 (PSM suspended) and byte_4FC2C0 (set by event 14 "all devices reset",
// cleared by event 15): both gate the state writers. Neither event is raised on the host.
constexpr bool kSuspended = false;
constexpr bool kDevicesBlocked = false;

int32_t g_pad_button_meaning = 0;  // dword_53FC88

// ---- the "orbis-controller" device object (sub_188F50 ctor, 744 bytes) -------------
struct Device {
  bool initialized = false;   // +32
  bool started = false;       // +36 (vtable +40 sub_1897A0)
  bool closed = false;        // +40
  uint8_t slot = 0;           // +44
  uint32_t enabled = 0;       // +16 feature mask (vtable +56 sub_189810 ORs in)
  uint32_t caps = 0;          // +20 features of the model (dword_3D8B10[type-2], else 0xDD)
  uint32_t button_state = 0;  // +268 pressed PSS buttons (edge detection)
  float lstick[2] = {};       // +0x110, +0x114
  float rstick[2] = {};       // +0x118, +0x11C
  uint8_t deadzone[2] = {};   // +376 / +377 (scePadGetControllerInformation stickInfo)
  // info block +128 (64 bytes) copied into the InputManager slot by sub_1FD520
  uint64_t device_id = 0;     // +168
  int32_t user_id = -1;       // +176
  int32_t type = 0;           // +180
  int32_t handle = -1;        // +184
  int32_t connection = 0;     // +188
  uint32_t capability = 0;    // +160
};
Device g_device[8];
std::recursive_mutex g_dev_mu;  // device +24 critical section

// dword_3D8B10 — features of each internal model, indexed by type - 2.
constexpr uint32_t kModelFeatures[11] = {0x19, 0xdd, 0xdd, 0xdd, 0x401, 0xdd, 0x59, 0x99, 0xdd, 0x401, 0x401};
// dword_40FC20 — internal model (type - 1) -> Sce.PlayStation.Orbis.Input.HardwareType.
constexpr int32_t kHardwareType[13] = {1, 2, 3, 5, 0, 4, 6, 7, 8, 9, 10, 11, 12};

// ---- sub_181B00: PSS button callback (vtable slot +40) ------------------------------
void pss_button(uint8_t slot, uint32_t bit, bool pressed) {
  if (slot > 7 || kSuspended || kDevicesBlocked) return;
  std::lock_guard lk(g_pss_mu);
  PssPad& p = g_pss_pad[slot];
  if (!pressed && !((p.buttons >> bit) & 1)) return;
  p.slot = slot;
  if (pressed) p.buttons |= 1u << bit;
  else p.buttons &= ~(1u << bit);
  // sub_17C270(2, pressed ? 20 : 19, {slot, bit, mask}) posts a PSM input event; the
  // only listener registered for category 2 (sub_2DBFE0) ignores ids 19..22.
}

// ---- sub_181940 / sub_181A20: stick callbacks (vtable +24 / +32) -------------------
void pss_stick(uint8_t slot, bool right, float x, float y) {
  if (kSuspended || slot > 7 || kDevicesBlocked) return;
  x = std::max(-1.0f, std::min(1.0f, x));  // vminss 1.0 (dword_35015C), vmaxss -1.0 (dword_3501C0)
  y = std::max(-1.0f, std::min(1.0f, y));
  std::lock_guard lk(g_pss_mu);
  PssPad& p = g_pss_pad[slot];
  p.slot = slot;
  if (right) { p.rx = x; p.ry = y; }
  else { p.lx = x; p.ly = y; }
  // sub_17C490(2, 21 / 22, ...) — event post, see pss_button.
}

// One stick axis of sub_189BA0: dead zone `dz` from scePadGetControllerInformation.
float stick_axis(uint8_t raw, uint8_t dz) {
  int lo = 0x80 - dz;
  if (lo < 0) lo = 0;
  if (lo > raw) return static_cast<float>(raw) / static_cast<float>(static_cast<uint8_t>(lo)) + -1.0f;
  const uint8_t hi = dz < 0x7F ? static_cast<uint8_t>(dz | 0x80) : 0xFF;
  if (hi < raw) return static_cast<float>(raw - hi) / static_cast<float>(hi ^ 0xFF);
  return 0.0f;
}

// sub_189BA0 table: ScePadData.buttons bit -> PSS button index, in the order the
// converter tests them (state bit = 1 << index).
struct ButtonMap {
  uint32_t sce_mask;
  uint8_t index;
};
constexpr ButtonMap kButtonTable[] = {
    {0x00010000, 18}, {0x00000001, 19}, {0x00000008, 8},  {0x00000010, 0},  {0x00000040, 1},
    {0x00000080, 2},  {0x00000020, 3},  {0x00000400, 10}, {0x00000100, 11}, {0x00000002, 12},
    {0x00000800, 13}, {0x00000200, 14}, {0x00000004, 15}, {0x00004000, 4},  {0x00002000, 5},
    {0x00008000, 6},  {0x00001000, 7},  {0x00100000, 20}, {0x00020000, 21}, {0x00200000, 24},
    {0x00800000, 25},
};
// ScePadData +0x58 (extension byte) bits 0..4 -> indices 26..30 — the host DualSense
// has no extension unit, so these are always released; then bit 31 (+0x00 sign).

void edge(Device& d, uint32_t index, bool down) {
  const uint32_t bit = 1u << index;
  if (down == ((d.button_state & bit) != 0)) return;
  d.button_state = down ? (d.button_state | bit) : (d.button_state & ~bit);
  pss_button(d.slot, index, down);
}

void convert(Device& d, const platform::input::PadData& pd) {
  // sub_189BA0: buttons (feature 1).
  if ((d.caps & 1) && (d.enabled & 1)) {
    for (const ButtonMap& m : kButtonTable) edge(d, m.index, (pd.buttons & m.sce_mask) != 0);
    for (uint32_t i = 0; i < 5; ++i) edge(d, 26 + i, false);  // +0x58 extension bits
    edge(d, 31, (pd.buttons & 0x80000000u) != 0);
    // Enter/Back from +0x5C/+0x5D (indices 22/23) only for feature 0x400 (remote models).
  } else {
    d.button_state = 0;
  }
  // Left stick (feature 0x40) and right stick (feature 0x80).
  if ((d.caps & 0x40) && (d.enabled & 0x40)) {
    const bool was = d.lstick[0] != 0.0f || d.lstick[1] != 0.0f;
    d.lstick[0] = stick_axis(pd.lx, d.deadzone[0]);
    d.lstick[1] = stick_axis(pd.ly, d.deadzone[0]);
    if (was || d.lstick[0] != 0.0f || d.lstick[1] != 0.0f) pss_stick(d.slot, false, d.lstick[0], d.lstick[1]);
  }
  if ((d.caps & 0x80) && (d.enabled & 0x80)) {
    const bool was = d.rstick[0] != 0.0f || d.rstick[1] != 0.0f;
    d.rstick[0] = stick_axis(pd.rx, d.deadzone[1]);
    d.rstick[1] = stick_axis(pd.ry, d.deadzone[1]);
    if (was || d.rstick[0] != 0.0f || d.rstick[1] != 0.0f) pss_stick(d.slot, true, d.rstick[0], d.rstick[1]);
  }
  // sub_1888F0 then feeds the motion callback (features 0x10, state at 0x5255e0, read only
  // by Motion.GetData) and sub_18A8D0 (touch, feature 4) / sub_18AC00 (remote, feature
  // 0x400). The host DualSense has no motion sensors or touch points to report.
}

// sub_1887F0 / sub_1888F0: one poll of every opened device.
void poll() {
  std::lock_guard lk(g_dev_mu);
  for (Device& d : g_device) {
    if (!d.initialized || !d.started || d.closed) continue;
    platform::input::PadData pd;
    if (d.handle >= 0) pd = platform::input::read();
    else { pd.buttons = 0; pd.lx = pd.ly = pd.rx = pd.ry = 0x80; }  // 0x80808080 at +4
    convert(d, pd);
  }
}

// "ScePsP:InputCtrl" (sub_186AC0): woken by the vblank event queue (42 ms timeout);
// reads the pads when at least 12 ms passed since the previous read.
void input_ctrl_thread() {
  using clock = std::chrono::steady_clock;
  auto last = clock::now();
  for (;;) {
    std::this_thread::sleep_for(std::chrono::microseconds(16667));  // display vblank
    const auto now = clock::now();
    const auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(now - last).count();
    if (ms >= 100) log::warn("input", "[ScePsP:InputCtrl] LOST PAD DATA: {}msec", ms);
    else if (ms < 12) continue;
    poll();
    last = now;
  }
}

// sub_188DD0 + sub_1891F0: a device found by the controller scanner is opened.
void open_device(uint8_t slot, int32_t owner) {
  Device& d = g_device[slot];
  d = Device{};
  d.initialized = true;
  d.slot = slot;
  // Scanner entry (sub_186E70): Mbus type 22 -> internal model 10 (DUALSENSE), bus local;
  // the user is the record's owner (+200).
  d.device_id = kHostDeviceId;
  d.user_id = owner;
  d.type = 10;
  d.connection = 0;
  d.handle = kHostPadHandle;  // scePadDeviceOpen(deviceId, dword_3D8ADC[type-1] = 0x10, 0)
  d.caps = (d.type - 2 >= 0 && d.type - 2 <= 10) ? kModelFeatures[d.type - 2] : 0xDD;
  // scePadGetControllerInformation(handle): stick dead zones (+8) — the host reports 0
  // (its XInput source is already dead-zoned in platform/input.cpp).
  d.deadzone[0] = d.deadzone[1] = 0;
  // scePadGetCapability(deviceId) -> InputDeviceType bits (sub_1891F0).
  const uint32_t c = kHostCapability;
  uint32_t v43 = ((((16 * c) & 0x10) + 4 * (c & 2)) | ((c << 9) & 0x800) | ((c << 9) & 0x1000)) + 4 * ((c & 0x10) != 0);
  v43 |= ((c << 8) & 0x2000) | ((c << 8) & 0x4000) | ((c << 8) & 0x18000);
  switch (d.type) {
    case 2: d.capability = v43 | 1; break;
    case 6: case 11: case 12: d.capability = v43 | 0x401; break;
    case 8: d.capability = v43 | 0x41; break;
    case 9: d.capability = v43 | 0x81; break;
    default: d.capability = v43 | 0xC1; break;
  }
}

// sub_1FD520: copy a device's info block into its InputManager slot.
void manager_refresh_slot(uint8_t slot) {
  InputManager& im = manager();
  std::lock_guard lk(im.mu);
  Slot& s = im.slots[slot];
  const Device& d = g_device[slot];
  if (!d.initialized || d.closed) {
    s = Slot{};
    return;
  }
  s.connected = true;
  s.capability = d.capability;
  s.device_id = d.device_id;
  s.user_id = d.user_id;
  s.type = d.type;
  s.handle = d.handle;
  s.connection = d.connection;
}

// sub_2DBFE0 event 23 (device connected).
void on_device_connected(uint8_t slot) {
  uint32_t v9 = config().gamepad ? 221 : 0;
  if (config().motion) v9 |= 0x10;
  if (config().remote) v9 += 1024;
  if (v9 == 0) return;
  Device& d = g_device[slot];
  if (!(d.enabled & v9)) {               // sub_17B670 -> vtable +72 sub_189830
    d.started = true;                    // sub_17B280 -> sub_183640 -> vtable +40 sub_1897A0
    d.enabled |= v9;                     // sub_17B5B0 -> vtable +56 sub_189810
    manager_refresh_slot(slot);          // sub_1FD520
  }
  g_prev_gamepad[slot] = 0;
  g_prev_remote[slot] = 0;
}

// "ScePsP:CtrlScanner" pass (sub_186F90): the bus is read with
// sceMbusGetDeviceInfoByCondition; a device not yet in a slot is opened in the first free
// slot (Open Pad, then the scanner's "connected" callback a2 -> sub_183970 -> PSM input
// event 23); for a slot whose device is still present the owner (+200 of the record) is
// compared with the slot's and, when it changed, written to the device (+176) and the
// device's info callback (+352, sub_17B4D0) posts PSM input event 28, whose listener
// (sub_2DBFE0) refreshes the InputManager slot (sub_1FD520). The host bus holds at most
// the one DualSense (libSceMbus model), so only slot 0 is ever used.
void scan() {
  uint8_t cond[256];
  mbus::init_condition(cond, sizeof cond);
  const uint64_t id = kHostDeviceId;
  std::memcpy(cond, &id, 8);
  uint8_t rec[mbus::kRecordSize];
  int32_t total = 0, gen = 0;
  const bool present = mbus::get_device_info_by_condition(cond, 0, rec, 1, &total, &gen, mbus::kRecordSize) > 0;
  int32_t owner = -1;
  if (present) std::memcpy(&owner, rec + 200, 4);
  bool opened = false, owner_changed = false;
  {
    std::lock_guard lk(g_dev_mu);
    Device& d = g_device[0];
    if (present && !d.initialized) {
      open_device(0, owner);
      opened = true;
    } else if (present && d.user_id != owner) {
      d.user_id = owner;
      owner_changed = true;
    }
  }
  if (opened) {
    {
      std::lock_guard lk(g_dev_mu);
      on_device_connected(0);
    }
    log::info("input", "DualSense (host keyboard/XInput) on slot 0: deviceId=0x{:x} handle={} user={}", kHostDeviceId,
              kHostPadHandle, owner);
  }
  if (owner_changed) {
    manager_refresh_slot(0);
    log::info("input", "DualSense slot 0: owner -> 0x{:x}", static_cast<uint32_t>(owner));
  }
}

// sub_188AF0: scan, then wait for the next sceMbus event (puHrnP8V_dY, blocking) and scan
// again, for every event id.
void scanner_thread(int32_t handle) {
  for (;;) {
    scan();
    mbus::Event ev;
    mbus::receive_event(handle, &ev, true);
  }
}

}  // namespace

int32_t read_pss_pad(uint32_t slot, PssPad* out) {
  if (slot > 7 || !out) return kPssErrorInvalidArg;
  std::lock_guard lk(g_pss_mu);
  *out = g_pss_pad[slot];
  return 0;
}

int32_t read_pss_remote(uint32_t slot, uint64_t out[2]) {
  if (!out) return kPssErrorInvalidArg;
  out[0] = out[1] = 0;
  if (slot > 7) return kPssErrorInvalidArg;
  std::lock_guard lk(g_pss_mu);
  out[0] = g_pss_remote[slot][0];
  out[1] = g_pss_remote[slot][1];
  return 0;
}

uint32_t pss_touch_count(uint32_t) { return 0; }

InputManager& manager() {
  static InputManager* im = [] {
    auto* m = new InputManager();
    m->alive = true;
    return m;
  }();
  return *im;
}

bool slot_accepted(InputManager& im, uint32_t index) {
  std::lock_guard lk(im.mu);
  if (index > 23 || !im.slots[index].connected) return false;
  const Slot& s = im.slots[index];
  if (im.bypass) return s.user_id > 0 || s.connection != 2;
  switch (im.behavior) {
    case 0:
    case 2: return s.user_id > 0;
    case 1: {
      if (s.user_id == 255 || s.user_id == im.param_user) return true;
      const uint32_t t = static_cast<uint32_t>(s.type - 6);
      return t < 7 && ((97u >> t) & 1);
    }
    case 16: return s.device_id == static_cast<uint64_t>(im.param_device);
    case 17: return s.user_id > 0 || s.connection != 2;
    default: return false;
  }
}

void fill_controller_info(InputManager& im, uint32_t index, uint8_t* info) {
  if (index > 23) {
    const int64_t id = -1;
    std::memcpy(info, &id, 8);
    const uint64_t q = 0xFFFFFFFFull;
    std::memcpy(info + 8, &q, 8);
    info[16] = info[17] = 0;
    std::memset(info + 20, 0, 8);
    return;
  }
  std::lock_guard lk(im.mu);
  const Slot& s = im.slots[index];
  std::memcpy(info, &s.device_id, 8);
  std::memcpy(info + 8, &s.user_id, 4);
  const uint32_t t = static_cast<uint32_t>(s.type - 1);
  const int32_t hw = t <= 12 ? kHardwareType[t] : 0;
  std::memcpy(info + 12, &hw, 4);
  info[16] = s.connected ? 1 : 0;
  info[17] = s.connection != 0 ? 1 : 0;
  const int32_t conn = s.connection == 2 ? 2 : (s.connection == 1 ? 1 : 0);
  std::memcpy(info + 20, &conn, 4);
  const uint32_t cap = s.capability & 0x1FCDF;
  std::memcpy(info + 24, &cap, 4);
}

int32_t set_device_index_behavior(InputManager& im, uint32_t mode, int64_t param, uint8_t* updated) {
  if (!im.alive) return kErrInputSystem;
  if (!updated) return static_cast<int32_t>(0x80B30002u);
  *updated = 0;
  int64_t device = param;
  int32_t user = -1;
  switch (mode) {
    case 0:
    case 2:
    case 17:
      if (param != 0) return static_cast<int32_t>(0x80B30001u);
      device = -1;
      break;
    case 1:
      if ((static_cast<uint64_t>(param) >> 32) != 0 || static_cast<int32_t>(param) <= 0) return static_cast<int32_t>(0x80B30001u);
      user = static_cast<int32_t>(param);
      device = -1;
      break;
    case 16:
      break;
    default:
      return static_cast<int32_t>(0x80B30001u);
  }
  std::lock_guard lk(im.mu);
  if (im.behavior != mode || im.param_device != device || im.param_user != user) {
    im.invalid_op = false;
    im.invalid_user = -1;
    *updated = 1;
  }
  im.behavior = mode;
  im.param_device = device;
  im.param_user = user;
  return 0;
}

const Config& config() {
  // PsmInitParam +8..+0xB: defaults set by the PsmInitParam constructor (libScePsm export
  // FqHN0elWA6E, not resolved in the database); the ShellUI eboot (sub_E500) does not
  // override them. Host: GamePad on (the PUI reads every key through GamePad.GetData);
  // Touch/RemoteControl off (with this device their results are the same either way);
  // Motion off (the host has no sensor data to give).
  static const Config c{true, false, false, false};
  return c;
}

void ensure_started() {
  static std::once_flag once;
  std::call_once(once, [] {
    host::note_unimplemented("libScePsm PsmInitParam input flags / pad-button-meaning defaults (FqHN0elWA6E) not traced: GamePad on, Touch/Motion/RemoteControl off");
    manager();  // sub_1FCF90
    // A pad that is already on at boot (--pad-on-at-boot) is on the bus before the scanner's
    // first pass; one turned on later reaches the bus through the power listener (mbus).
    if (platform::input::powered()) mbus::attach_host_pad();
    // Controller scanner ("ScePsP:CtrlScanner", sub_1832D0 -> sub_188AF0 / sub_186F90).
    const int32_t handle = mbus::create_event_handle();
    std::thread(scanner_thread, handle).detach();
    std::thread(input_ctrl_thread).detach();
  });
}

int32_t pad_button_meaning() { return g_pad_button_meaning; }
void set_pad_button_meaning(int32_t v) { g_pad_button_meaning = v; }

int32_t pad_read_state_buttons(int32_t handle, uint32_t* buttons) {
  if (handle != kHostPadHandle) return kScePadErrorInvalidHandle;
  *buttons = platform::input::read().buttons;
  return 0;
}

}  // namespace ssn::psm::input
