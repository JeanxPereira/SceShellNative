// sceShellCoreUtilGetUIStatus / sceShellCoreUtilSetUIStatus (libSceSystemService, FW 4.03),
// reached through "libSceShellCoreUtilWrapper" (eboot table 0xcf4ad0 -> the imports).
//
// The UI status is the event flag "SceShellCoreUtilUIStatus" that SceShellCore creates
// (0x61C580: attr 0x120, initial pattern 0x20001; see system/system_objects.cpp). The library
// opens it on first use (qword_440B8, under unk_46230).
#include <cstdint>
#include <mutex>

#include "host/boundary.h"
#include "libkernel/event_flag.h"

namespace ssn::systemservice {
namespace {

kernel::EventFlag* ui_status_flag(int32_t* err) {
  static std::mutex m;
  static kernel::EventFlag* ef = nullptr;
  std::lock_guard<std::mutex> lk(m);
  if (!ef) ef = kernel::open_event_flag("SceShellCoreUtilUIStatus");
  *err = ef ? 0 : kernel::kErrorEsrch;
  return ef;
}

}  // namespace

// fORZmlh1TQo (0x12330): sceKernelPollEventFlag(flag, ~0, OR, status). EBUSY (no bit set) is
// success without a result; other errors are returned.
SSN_PINVOKE("libSceShellCoreUtilWrapper", "sceShellCoreUtilGetUIStatus", int32_t, ssn_ShellCoreUtil_GetUIStatus, (uint64_t* status)) {
  int32_t err = 0;
  kernel::EventFlag* ef = ui_status_flag(&err);
  if (!ef) return err;
  const int32_t r = kernel::poll_event_flag(ef, ~0ull, kernel::kEvfWaitModeOr, status);
  if (r == kernel::kErrorEbusy) return 0;
  return r < 0 ? r : 0;
}

// atiUTsTFJ3k (0x123D0): clear (status | ~mask), then set (status & mask). Returns 0.
SSN_PINVOKE("libSceShellCoreUtilWrapper", "sceShellCoreUtilSetUIStatus", int32_t, ssn_ShellCoreUtil_SetUIStatus,
            (uint64_t status, uint64_t mask)) {
  int32_t err = 0;
  kernel::EventFlag* ef = ui_status_flag(&err);
  if (!ef) return err;
  kernel::clear_event_flag(ef, status | ~mask);
  kernel::set_event_flag(ef, status & mask);
  return 0;
}

}  // namespace ssn::systemservice
