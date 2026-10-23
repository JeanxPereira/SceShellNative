// libScePsm.sprx (FW 4.03) input stack, below the Mono icalls:
//
//   libScePad (physical boundary: ScePadData, device identity)       <- host device
//     -> "ScePsP:InputCtrl" poll loop           sub_186AC0 / sub_1887F0
//     -> orbis-controller converter             sub_1888F0 / sub_189BA0
//     -> PSS per-device state tables            0x4fd2a0 pad, 0x525750 remote, 0x5260b0 touch
//     -> "ScePssCtrlSelector" InputManager      sub_1FCF90 object (qword_53F680)
//     -> icalls (input_icalls.cpp)
//
// Everything here is reproduced from the decompiled libScePsm code (addresses beside
// each piece); the host only supplies the device (platform/input.h) and its identity.
#pragma once
#include <cstdint>
#include <mutex>

namespace ssn::psm::input {

// ---- physical boundary: identity of the host's DualSense --------------------------
// Values the console's Mbus/libScePad would report for the connected pad. The ids are
// host-chosen (no hardware); the model and capability are those of a DualSense.
constexpr uint64_t kHostDeviceId = 0x0000000000010001ull;  // Mbus device id (host-chosen)
constexpr int32_t kHostPadHandle = 1;                       // scePadDeviceOpen result (host-chosen)
constexpr int32_t kHostUserId = -1;  // Mbus owner: none (no user logged in on the host)
// SCE_HID_CONTROLLER_CAPABILITY_BOND (Sce.Vsh.SysBridge.Pad.SceHidControllerCapability).
constexpr uint32_t kHostCapability = 0x80003FB7u;
constexpr int32_t kScePadErrorInvalidHandle = static_cast<int32_t>(0x80920003u);  // SCE_PAD_ERROR_INVALID_HANDLE

// PSM error results used by the input icalls (0x80B3xxxx: SCE_PSM_ERROR_*).
constexpr int32_t kErrInputSystem = static_cast<int32_t>(0x80B30025u);  // input not enabled / no InputManager

// ---- PSS per-device pad state (0x4fd2a0, 24 bytes x 8) -----------------------------
struct PssPad {
  uint8_t slot;
  uint8_t pad_[3];
  uint32_t buttons;  // bit i = PSS button index i (see kButtonTable in pss_input.cpp)
  float lx, ly, rx, ry;
};
static_assert(sizeof(PssPad) == 24);

// sub_181900(slot, out) — copies the 24-byte entry; 0x80B3F003 for slot > 7.
int32_t read_pss_pad(uint32_t slot, PssPad* out);
// sub_1844B0(slot, out) — remote-control state (16 bytes at 0x525750): buttons (u64) +
// partner byte. Only remote-type devices (capability 0x400) ever write it.
int32_t read_pss_remote(uint32_t slot, uint64_t out[2]);
// sub_184DB0(slot, ...) — number of touch points (only the count is consumed here).
uint32_t pss_touch_count(uint32_t slot);

// ---- InputManager ("ScePssCtrlSelector", sub_1FCF90, 4344 bytes) -------------------
struct Slot {                // +8 + 88*i
  bool connected = false;    // +8
  uint32_t capability = 0;   // +48 (info +32)
  uint64_t device_id = 0;    // +56 (info +40)
  int32_t user_id = -1;      // +64 (info +48)
  int32_t type = 0;          // +68 (info +52) internal model, 10 = DUALSENSE
  int32_t handle = 0;        // +72 (info +56)
  int32_t connection = 0;    // +76 (info +60) 0 local, 1 remote play, 2 share play
  uint64_t prev_pad = 0;     // +80
  uint64_t prev_remote = 0;  // +88
};

struct InputManager {
  std::recursive_mutex mu;     // +2152 "ScePssCtrlSelector"
  bool alive = false;          // +0
  int32_t active = -1;         // +4  device picked by the merge behaviours
  Slot slots[24];              // 0..7 physical, 8..23 virtual pads (VirtualPadManager)
  bool bypass = false;         // +2120
  uint32_t behavior = 0;       // +2124 DeviceIndexBehavior
  int64_t param_device = -1;   // +2128
  int32_t param_user = -1;     // +2136
  bool invalid_op = false;     // +2140
  int32_t invalid_user = -1;   // +2144
};

// sub_1FCF90: the InputManager singleton (created on first use; never null).
InputManager& manager();
// sub_1FD430: does `index` take part under the current behaviour?
bool slot_accepted(InputManager& im, uint32_t index);
// sub_1FD360: ControllerInfo (Sce.PlayStation.Orbis.Input, 28 bytes) for a slot.
void fill_controller_info(InputManager& im, uint32_t index, uint8_t* info);
// sub_1FD1D0: SetDeviceIndexBehaviorNative body.
int32_t set_device_index_behavior(InputManager& im, uint32_t mode, int64_t param, uint8_t* updated);

// ---- PSM init state (sub_2DBE90) ---------------------------------------------------
struct Config {
  // PsmInitParam bytes +8..+0xB copied to word_540A28/2A: GamePad, Touch, Motion,
  // RemoteControl input enabled; byte_54107C..F mirror them while the pad device is up.
  bool gamepad, touch, motion, remote;
};
const Config& config();
// Starts the stack once (PSM init + controller scanner + InputCtrl thread).
void ensure_started();

// dword_53FC88 — SystemPadButtonMeaning (0 conform, 1 cross is enter, 2 circle is enter).
int32_t pad_button_meaning();
void set_pad_button_meaning(int32_t v);

// Per-device-index previous GamePad buttons (word_541078+8, 24 x u32) and
// RemoteControl buttons (word_541078+104, 24 x u64), owned by the icalls.
extern uint32_t g_prev_gamepad[24];
extern uint64_t g_prev_remote[24];

// The host pad as libScePad sees it: scePadReadState buttons for a handle.
int32_t pad_read_state_buttons(int32_t handle, uint32_t* buttons);

}  // namespace ssn::psm::input
