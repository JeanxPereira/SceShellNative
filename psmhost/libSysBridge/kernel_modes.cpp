// Kernel mode / QA-flag queries of libkernel (FW 4.03) and the kernel sysctls behind them
// (kernel handlers cited from the 7.00 kernel, the one in hand; see docs/registry.md).
//
// HOST VALUE (process identity): several handlers answer only to a caller whose ucred
// carries the system attribute (SCE attributes at ucred+96, bit 62: kernel 0xFFFFFFFF8080A280)
// and fail otherwise. The attribute comes from the process's signed SELF auth info, which a
// decrypted dump does not keep. The host answers as the console does for the system shell:
// the attribute is set.
#include <cstdint>
#include <cstring>
#include <mutex>

#include "host/boundary.h"
#include "libSceRegMgr/regmgr.h"

namespace ssn::sysbridge {
namespace {

void system_attr_note() {
  static const bool noted =
      (host::note_unimplemented("HOST VALUE ShellUI ucred system attribute (ucred+96 bit 62) set: the SELF auth info is not in the dump"),
       true);
  (void)noted;
}

// libkernel D4yla3vx4tY (0x16130): errno -> SCE kernel error (errno + 0x80020000; 0 stays 0).
int32_t errno_to_sce(int32_t e) { return e == 0 ? 0 : static_cast<int32_t>(static_cast<uint32_t>(e) + 0x80020000u); }

// Kernel handler of the machdep.rcmgr_* sysctls the Settings QA-menu checks read
// (rcmgr_debug_menu 0xFFFFFFFF80E34180, rcmgr_debug_menu_mini 0xFFFFFFFF80E34320,
// rcmgr_utoken_store_mode 0xFFFFFFFF80E34A10, rcmgr_utoken_np_env_switching
// 0xFFFFFFFF80E34D10): for a caller with the system attribute, SYSCTL_OUT of an int 0;
// otherwise the error of 0xFFFFFFFF804C15A0. The libkernel readers (sysctlbyname, 4 bytes)
// return the sysctl's result when it fails, else the value: 0.
int32_t rcmgr_flag() {
  system_attr_note();
  return 0;
}

}  // namespace

// sceKernelHasNeoMode — "libSceVshKernelWrapper.a" (eboot table 0xcef750 -> import
// rNRtm1uioyY, libkernel_sys 0x1f9c0): returns 1.
SSN_PINVOKE("libSceVshKernelWrapper.a", "sceKernelHasNeoMode", int32_t, ssn_VshKernelWrapper_HasNeoMode, ()) { return 1; }

// sceKernelGetBackupRestoreMode — "libSysBridgeKernel" table 0x78820 -> QtLhuYZf9jg
// (libkernel_sys 0x20ba0): sysctlbyname("kern.backup_restore_mode", mode, 4); 0, or the
// errno converted (0x16130). Kernel handler 0xFFFFFFFF8074E020: a cached int
// (dword_FFFFFFFF820C5EF0, -1 at boot) loaded on first use from the registry key
// 0x020C0000 (SYSTEM_backup_mode, sceRegMgrGetInt 0xFFFFFFFF80CA2050; a failure is the
// handler's result and leaves the cache unloaded), then copied out.
SSN_PINVOKE("libSysBridgeKernel", "sceKernelGetBackupRestoreMode", int32_t, ssn_SysBridgeKernel_GetBackupRestoreMode,
            (int32_t* mode)) {
  static std::mutex m;
  static int32_t cached = -1;
  std::lock_guard<std::mutex> lk(m);
  if (cached < 0) {
    int32_t v = 0;
    const int32_t r = regmgr::get_int(0x020C0000u, &v);
    if (r != 0) return errno_to_sce(r);
    cached = v;
  }
  if (mode) std::memcpy(mode, &cached, 4);
  return 0;
}

// sysKernelGetManufacturingMode — table 0x78800 -> 6jj29MbyzuI (libkernel_sys 0x1e590):
// sysctlbyname("machdep.curr_manumode", &v, 4); returns v | -(sysctl failed). Kernel handler
// 0xFFFFFFFF80E33A20: with the system attribute, SYSCTL_OUT of an int 0; otherwise EPERM.
SSN_PINVOKE("libSysBridgeKernel", "sysKernelGetManufacturingMode", int32_t, ssn_SysBridgeKernel_GetManufacturingMode, ()) {
  system_attr_note();
  return 0;
}

// Sce.Vsh.ShellUI.Settings.Sbl.SblWrapper icalls (eboot): each a thunk to the libkernel
// export reading one machdep.rcmgr_* sysctl:
//   SblRcMgrIsAllowDebugMenuForSettingsNative        0x4ebbf0 -> opMiSfTJyJo (0x1ec60) rcmgr_debug_menu
//   SblRcMgrIsAllowMinimumDebugMenuForSettingsNative 0x4ebc00 -> JVYt1hlHSvo (0x1ed20) rcmgr_debug_menu_mini
//   SblRcMgrIsAllowNpEnvSwitchingNative              0x4ebc30 -> XuZD9HV0p4 (0x1f140) rcmgr_utoken_np_env_switching
//   SblRcMgrIsStoreModeNative                        0x4ebc40 -> 0NyJTCJWuuk (0x1efc0) rcmgr_utoken_store_mode
int32_t SblRcMgrIsAllowDebugMenuForSettingsNative() { return rcmgr_flag(); }
int32_t SblRcMgrIsAllowMinimumDebugMenuForSettingsNative() { return rcmgr_flag(); }
int32_t SblRcMgrIsAllowNpEnvSwitchingNative() { return rcmgr_flag(); }
int32_t SblRcMgrIsStoreModeNative() { return rcmgr_flag(); }
SSN_ICALL("Sce.Vsh.ShellUI.Settings.Sbl.SblWrapper::SblRcMgrIsAllowDebugMenuForSettingsNative", SblRcMgrIsAllowDebugMenuForSettingsNative);
SSN_ICALL("Sce.Vsh.ShellUI.Settings.Sbl.SblWrapper::SblRcMgrIsAllowMinimumDebugMenuForSettingsNative",
          SblRcMgrIsAllowMinimumDebugMenuForSettingsNative);
SSN_ICALL("Sce.Vsh.ShellUI.Settings.Sbl.SblWrapper::SblRcMgrIsAllowNpEnvSwitchingNative", SblRcMgrIsAllowNpEnvSwitchingNative);
SSN_ICALL("Sce.Vsh.ShellUI.Settings.Sbl.SblWrapper::SblRcMgrIsStoreModeNative", SblRcMgrIsStoreModeNative);

}  // namespace ssn::sysbridge
