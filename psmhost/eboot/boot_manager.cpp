// ShellUI BootManager (eboot.bin, shell_ui_boot_manager.cpp), exported to managed code
// as "libSceBootManager" (Sce.Vsh.ShellUI.AppMain.NativeCall).
//
// The manager owns one kernel event flag, created on first use by 0xe3a0 as
// sceKernelCreateEventFlag(&evf, "SceShellUIBootManager", SCE_KERNEL_EVF_ATTR_MULTI, 0, nullptr).
#include <cstdint>

#include "host/boundary.h"
#include "libkernel/event_flag.h"

namespace ssn::eboot {
namespace {

kernel::EventFlag* boot_flag() {
  static kernel::EventFlag* ef = kernel::create_event_flag("SceShellUIBootManager", 0x20 /* ATTR_MULTI */, 0);
  return ef;
}

}  // namespace

void set_boot_event(uint64_t e) { kernel::set_event_flag(boot_flag(), e); }

// WaitBootEvent(e) — 0xe340: sceKernelWaitEventFlag(evf, e, AND, nullptr, nullptr).
SSN_PINVOKE("libSceBootManager", "WaitBootEvent", void, ssn_BootManager_WaitBootEvent, (uint64_t e)) {
  kernel::wait_event_flag(boot_flag(), e, kernel::kEvfWaitModeAnd, nullptr, nullptr);
}

// SetBootEvent(e) — 0xe2f0: sceKernelSetEventFlag(evf, e).
SSN_PINVOKE("libSceBootManager", "SetBootEvent", void, ssn_BootManager_SetBootEvent, (uint64_t e)) {
  kernel::set_event_flag(boot_flag(), e);
}

// GetBootEvent(e) — 0xea50: sceKernelPollEventFlag(evf, e, AND, nullptr) == 0.
SSN_PINVOKE("libSceBootManager", "GetBootEvent", int32_t, ssn_BootManager_GetBootEvent, (uint64_t e)) {
  return kernel::poll_event_flag(boot_flag(), e, kernel::kEvfWaitModeAnd, nullptr) == 0;
}

// GetBootEventFlag(out ef) — 0xea80: the manager's event flag handle (null pointer ignored).
SSN_PINVOKE("libSceBootManager", "GetBootEventFlag", void, ssn_BootManager_GetBootEventFlag, (void** ef)) {
  if (ef) *ef = boot_flag();
}

}  // namespace ssn::eboot
