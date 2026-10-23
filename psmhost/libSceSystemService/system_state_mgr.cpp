// sceSystemStateMgr* exports of libSceSystemService (FW 4.03) that read SceShellCore's system
// state, reached through "libSceSystemStateMgrWrapper" (eboot {name, fn} table -> the imports).
//
// The library's SystemStateMgr init (0x19910) opens the event flags "SceSystemStateMgrInfo"
// (qword_440E8) and "SceSystemStateMgrStatus" (qword_440F0), which SceShellCore's
// SystemStateMgr creates (see system/system_objects.cpp for their boot pattern).
// "SceSystemStateMgrInfo" holds the state in bits 0..15 and the trigger code in bits 32..47.
#include <chrono>
#include <cstdint>
#include <thread>

#include "host/boundary.h"
#include "libkernel/event_flag.h"

namespace ssn::systemservice {
namespace {

kernel::EventFlag* state_info() {  // qword_440E8
  static kernel::EventFlag* ef = kernel::open_event_flag("SceSystemStateMgrInfo");
  return ef;
}

// The library's loop: poll(flag, mask, OR) into a zeroed word, sleeping 1 ms
// (sceKernelUsleep(1000)) while the masked field reads 0.
uint64_t poll_field(uint64_t mask) {
  for (;;) {
    uint64_t v = 0;
    kernel::poll_event_flag(state_info(), mask, kernel::kEvfWaitModeOr, &v);
    if (v & mask) return v;
    if (!state_info()) return 0;  // the host creates the flag at boot; guards a missing flag only
    std::this_thread::sleep_for(std::chrono::milliseconds(1));
  }
}

}  // namespace

// ze0ky5Q1yE8 (0x19DF0): the state (bits 0..15), waiting while it is 0.
SSN_PINVOKE("libSceSystemStateMgrWrapper", "sceSystemStateMgrGetCurrentState", int32_t, ssn_SystemStateMgr_GetCurrentState, ()) {
  return static_cast<int32_t>(poll_field(0xFFFFull) & 0xFFFFFFFFu);
}

// wlxvESTUplk (0x19EA0): the trigger code (bits 32..47), waiting while it is 0.
SSN_PINVOKE("libSceSystemStateMgrWrapper", "sceSystemStateMgrGetTriggerCode", int32_t, ssn_SystemStateMgr_GetTriggerCode, ()) {
  return static_cast<int32_t>(poll_field(0xFFFF00000000ull) >> 32);
}

// gPx1b36zyMY (0x19FB0; devkit build 0x1A040) — eboot {name, fn} entry 0xcf5240 (devkit 0xcf9260)
// -> the import: sceKernelSetEventFlag("SceSystemStateMgrStatus", 1), its result returned.
// SceShellCore's SystemStateMgr consumes the bit (the CEC One Touch Play request to the TV); that
// side (SceShellCore's SystemStateMgr and the HDMI CEC link) is not reproduced, so the bit stays set.
SSN_PINVOKE("libSceSystemStateMgrWrapper", "sceSystemStateMgrSendCecOneTouchPlayCommand", int32_t,
            ssn_SystemStateMgr_SendCecOneTouchPlayCommand, ()) {
  static kernel::EventFlag* status = kernel::open_event_flag("SceSystemStateMgrStatus");  // qword_440F0
  return kernel::set_event_flag(status, 1);
}

}  // namespace ssn::systemservice
