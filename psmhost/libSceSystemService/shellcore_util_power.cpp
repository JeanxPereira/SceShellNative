// sceShellCoreUtil* exports of libSceSystemService (FW 4.03) that work on the kernel objects
// SceShellCore shares with its clients, reached through "libSceShellCoreUtilWrapper"
// (eboot table 0xcf4ad0 -> the imports).
//
// The library's init (0xF080) opens, besides its IPMI client:
//   qword_440A0 "SceShellCoreUtilAppFocus", qword_440A8 "SceShellCoreUtilCtrlFocus",
//   qword_440B0 "SceShellCoreUtilPowerControl" (event flags, created by SceShellCore's
//   ShellCoreUtil init 0x61C580; see system/system_objects.cpp), and
//   qword_45218 = the shared memory "/SceShellCoreUtil" (0x4000 bytes, mapped read-only,
//   accepted when its first qword is 0x6ED81EDE6DF17259). SceShellCore creates and fills that
//   block in 0x620AE0 (see shellcore_util_shm() below).
// "SceShellCoreUtilDevIdxBehavior" (qword_440C0) is opened on first use under unk_46230.
#include <cstdint>
#include <cstring>
#include <mutex>

#include "host/boundary.h"
#include "libSceRegMgr/regmgr.h"
#include "libkernel/event_flag.h"

namespace ssn::systemservice {
namespace {

// The "/SceShellCoreUtil" block as SceShellCore 0x620AE0 initialises it (shm_open O_RDWR|O_CREAT
// 0644, ftruncate 0x4000, mmap RW, zero-fill from +8):
//   +0  magic 0x6ED81EDE6DF17259     +17 = 1
//   +19 = 1 when registry 0x02860100 (SYSTEM_SPECIFIC_idu_mode) reads 1
//   +24 = -1                          +28 = registry 0x02820D00 when it can be read
//   +116 = -1
// Later writers in SceShellCore (thermal monitoring sets +120, the M.2 storage temperature
// danger byte, ...) are not reproduced: the host has no M.2 SSD and no thermal sensors, so
// those bytes keep their initial value 0.
struct ShellCoreUtilShm {
  uint8_t bytes[0x4000];
};

const uint8_t* shellcore_util_shm() {
  static ShellCoreUtilShm* shm = [] {
    auto* s = new ShellCoreUtilShm();
    std::memset(s->bytes, 0, sizeof(s->bytes));
    const uint64_t magic = 0x6ED81EDE6DF17259ull;
    std::memcpy(s->bytes, &magic, 8);
    s->bytes[17] = 1;
    const int32_t minus1 = -1;
    std::memcpy(s->bytes + 24, &minus1, 4);
    int32_t v = 0;
    if (regmgr::get_int(0x02860100u, &v) == 0 && v == 1) s->bytes[19] = 1;
    v = 0;
    if (regmgr::get_int(0x02820D00u, &v) == 0) std::memcpy(s->bytes + 28, &v, 4);
    std::memcpy(s->bytes + 116, &minus1, 4);
    return s;
  }();
  return shm->bytes;
}

kernel::EventFlag* power_control() {  // qword_440B0
  static kernel::EventFlag* ef = kernel::open_event_flag("SceShellCoreUtilPowerControl");
  return ef;
}

// sceKernelPollEventFlag(flag, bits, OR, &v) with v = 0 beforehand: v & bits.
bool power_control_bit(uint64_t bits) {
  uint64_t v = 0;
  kernel::poll_event_flag(power_control(), bits, kernel::kEvfWaitModeOr, &v);
  return (v & bits) != 0;
}

std::mutex g_power_lock;  // unk_46240 ("SceShellCoreUtilPowerLock")
int32_t g_power_lock_count = 0;  // dword_46248

std::mutex g_dev_idx_lock;  // unk_46230
kernel::EventFlag* g_dev_idx = nullptr;  // qword_440C0
bool g_dev_idx_opened = false;
uint64_t g_dev_idx_value = 0;  // qword_49260

}  // namespace

// vzWoetyaUuA (0x114F0): PowerControl bit 0x1000000.
SSN_PINVOKE("libSceShellCoreUtilWrapper", "sceShellCoreUtilIsTemperatureDanger", uint8_t,
            ssn_ShellCoreUtil_IsTemperatureDanger, ()) {
  return power_control_bit(0x1000000ull) ? 1 : 0;
}

// qwGVQS60KyE (0x11550): shared block mapped and its byte +120 non-zero.
SSN_PINVOKE("libSceShellCoreUtilWrapper", "sceShellCoreUtilIsM2StorageTemperatureDanger", uint8_t,
            ssn_ShellCoreUtil_IsM2StorageTemperatureDanger, ()) {
  return shellcore_util_shm()[120] != 0 ? 1 : 0;
}

// fBjY1lY7sbs (0x10320): byte +19 of the shared block.
SSN_PINVOKE("libSceShellCoreUtilWrapper", "sceShellCoreUtilIsIDUState", uint8_t, ssn_ShellCoreUtil_IsIDUState, ()) {
  return shellcore_util_shm()[19];
}

// TJp3kdSGsIw (0x105E0): set or clear PowerControl bit 0x4000000.
SSN_PINVOKE("libSceShellCoreUtilWrapper", "sceShellCoreUtilSetImposeStatusFlag", void,
            ssn_ShellCoreUtil_SetImposeStatusFlag, (uint8_t flag)) {
  if (flag)
    kernel::set_event_flag(power_control(), 0x4000000ull);
  else
    kernel::clear_event_flag(power_control(), ~0x4000000ull);
}

// R013D1VIETQ (0x10640): set PowerControl bit 0x2000000.
SSN_PINVOKE("libSceShellCoreUtilWrapper", "sceShellCoreUtilSignalUserInput", void, ssn_ShellCoreUtil_SignalUserInput, ()) {
  kernel::set_event_flag(power_control(), 0x2000000ull);
}

// lcp9E77DAB4 (0x10590): set PowerControl bit 0x40000.
SSN_PINVOKE("libSceShellCoreUtilWrapper", "sceShellCoreUtilTurnOffScreenSaver", void, ssn_ShellCoreUtil_TurnOffScreenSaver, ()) {
  kernel::set_event_flag(power_control(), 0x40000ull);
}

// mpeGML7ulA8 (0x13170): poll(PowerControl, 0x1000000000000, OR) == 0.
SSN_PINVOKE("libSceShellCoreUtilWrapper", "sceShellCoreUtilIsExternalStorageAppMoveInProgress", uint8_t,
            ssn_ShellCoreUtil_IsExternalStorageAppMoveInProgress, ()) {
  return kernel::poll_event_flag(power_control(), 0x1000000000000ull, kernel::kEvfWaitModeOr, nullptr) == 0 ? 1 : 0;
}

// 9plZCCRm9x4 (0x112E0): under the power lock, ++count; when the old count was >= 0, set
// PowerControl bit 0x80000. Returns the new count.
SSN_PINVOKE("libSceShellCoreUtilWrapper", "sceShellCoreUtilEnterPowerLockSection", int32_t,
            ssn_ShellCoreUtil_EnterPowerLockSection, ()) {
  std::lock_guard l(g_power_lock);
  const int32_t old = g_power_lock_count;
  const int32_t now = ++g_power_lock_count;
  if (old >= 0) kernel::set_event_flag(power_control(), 0x80000ull);
  return now;
}

// nENvUAsAKdY (0x11330): count 0 -> 0x80E30008; otherwise --count, and at 0 clear bit 0x80000.
SSN_PINVOKE("libSceShellCoreUtilWrapper", "sceShellCoreUtilLeavePowerLockSection", int32_t,
            ssn_ShellCoreUtil_LeavePowerLockSection, ()) {
  std::lock_guard l(g_power_lock);
  if (g_power_lock_count == 0) return static_cast<int32_t>(0x80E30008u);
  const int32_t now = --g_power_lock_count;
  if (now == 0) kernel::clear_event_flag(power_control(), ~0x80000ull);
  return now;
}

// l96YlUEtMPk (0x127C0): on first use opens "SceShellCoreUtilDevIdxBehavior" (its error is
// returned) and polls its pattern into the cache with byte 2 cleared. The value
// behavior | (scene << 8 & 0xFFFF) | (userId << 32), when it differs from the cache, is cached
// and written: clear(0), then set(value | 0xFF0000). Returns 0.
SSN_PINVOKE("libSceShellCoreUtilWrapper", "sceShellCoreUtilSetDeviceIndexBehavior", int32_t,
            ssn_ShellCoreUtil_SetDeviceIndexBehavior, (uint8_t behavior, uint32_t user_id, int16_t scene)) {
  std::lock_guard l(g_dev_idx_lock);
  if (!g_dev_idx_opened) {
    g_dev_idx = kernel::open_event_flag("SceShellCoreUtilDevIdxBehavior");
    // The firmware returns sceKernelOpenEventFlag's error; the host creates the flag at boot
    // (system/system_objects.cpp), so this only guards a missing flag.
    if (!g_dev_idx) return kernel::kErrorEsrch;
    g_dev_idx_opened = true;
    g_dev_idx_value = 0;
    kernel::poll_event_flag(g_dev_idx, ~0ull, kernel::kEvfWaitModeOr, &g_dev_idx_value);
    g_dev_idx_value &= ~0xFF0000ull;
  }
  const uint64_t v = (static_cast<uint64_t>(user_id) << 32) | static_cast<uint16_t>(scene << 8) | behavior;
  if (g_dev_idx_value != v) {
    g_dev_idx_value = v;
    kernel::clear_event_flag(g_dev_idx, 0);
    kernel::set_event_flag(g_dev_idx, v | 0xFF0000ull);
  }
  return 0;
}

// plK52OfeEIc (0x13AF0): opens "SceLoginMgrServerHmdConnect" on first use (-1 when that
// fails), polls its whole pattern and returns the high dword when it is > 0, else -1.
SSN_PINVOKE("libSceShellCoreUtilWrapper", "sceShellCoreUtilGetUserIdOfMorpheusUser", int32_t,
            ssn_ShellCoreUtil_GetUserIdOfMorpheusUser, ()) {
  static std::mutex m;
  static kernel::EventFlag* ef = nullptr;
  {
    std::lock_guard l(m);
    if (!ef) ef = kernel::open_event_flag("SceLoginMgrServerHmdConnect");  // retried while it fails
    if (!ef) return -1;
  }
  uint64_t v = 0;
  kernel::poll_event_flag(ef, ~0ull, kernel::kEvfWaitModeOr, &v);
  const int32_t hi = static_cast<int32_t>(v >> 32);
  return hi > 0 ? hi : -1;
}

// "BgmPlayStatusCheck" sceGetBgmPlayStatusNative — libScePsm {name, fn} entry 0x4A35B0 -> 0x2DA8C0,
// a thunk to the libSceSystemService import SYqaqLuQU6w (0x24E0 -> 0x141A0, same address in the
// devkit build): the shared block is mapped and its byte +16 is non-zero. SceShellCore writes +16
// in 0x61E700 (devkit 0x62A160) from the SystemStateMgr tick 0x6C23D0 (devkit 0x6CDF00): true while
// bit 8 of its state event flag was seen set less than 1000 ms ago. Nothing on the host sets that
// bit (the SystemStateMgr state machine is not reproduced), so the byte keeps its initial 0 and
// the answer is false, as on a console where it was never set.
SSN_PINVOKE("BgmPlayStatusCheck", "sceGetBgmPlayStatusNative", int32_t, ssn_BgmPlayStatusCheck_GetBgmPlayStatus, ()) {
  return shellcore_util_shm()[16] != 0 ? 1 : 0;
}

}  // namespace ssn::systemservice
