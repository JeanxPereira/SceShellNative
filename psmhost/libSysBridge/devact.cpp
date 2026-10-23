// sceSblDevActGetRemainingTime — "libSceVshKernelWrapper.a" (eboot {name, fn} entry 0xcef5d0 ->
// 0x4A9920; FW 4.03 devkit build J01736824: 0xcf35f0 -> 0x4AACD0), called by
// Sce.Vsh.KernelSysWrapperSbl.DevActGetRemainingTime (the home's DebugInfoView, ReactNativeShellApp).
//
// 0x4A9920 runs the query on the eboot's worker when one is set up, else directly; the query
// (0x4AA2B0; devkit build 0x4AB660) is the devkit-activation device:
//   fd = open("/dev/devact", O_RDWR)        -> on failure printf("open error"), 0x800F0013
//   r = ioctl(fd, 0x40144401, buf[0x14]); close(fd)
//   r != 0 -> 0x800F0005; buf+0x10 != 0 -> that value; else *remaining = max(buf+4 - buf+0, 0), 0
// The PC has no /dev/devact (the host has no SBL device-activation service), so the open fails
// and the function returns what the console returns in that case; *remaining is not written.
// (The former logging stub answered 0 with 0 seconds left, which DebugInfoView shows as
// "This DevKit/TestKit is Expired".)
#include <cstdint>
#include <cstdio>

#include "host/boundary.h"

namespace ssn::sysbridge {

SSN_PINVOKE("libSceVshKernelWrapper.a", "sceSblDevActGetRemainingTime", int32_t, ssn_VshKernelWrapper_SblDevActGetRemainingTime,
            (uint32_t* /*remaining*/)) {
  std::printf("open error\n");  // the eboot's message when /dev/devact cannot be opened
  return static_cast<int32_t>(0x800F0013);
}

// Sce.Vsh.ShellUI.Settings.Sbl.SblWrapper::SblDevActHasPassCodeNative() — eboot icall 0x4EC4E0
// (devkit build 0x4EDE70), a thunk to 0x4AA530 (devkit 0x4AB8E0):
//   fd = open("/dev/devact", O_RDWR); on failure -> false
//   buf = {0, 0, status 0x800F0625}; r = ioctl(fd, 0xC0105206, buf); close(fd)
//   r == 0 -> r = buf status; return r == 0
// The host has no /dev/devact (see above), so the open fails and the answer is false.
int32_t SblDevActHasPassCodeNative() { return 0; }

// sceSblPupExpirationGetStatus — "libSceVshKernelWrapper.a" (eboot {name, fn} entry 0xcef5e0 ->
// 0x4A9AA0; devkit build 0xcf3600 -> 0x4AAE50): runs the query on the eboot's worker (job item
// run method 0x4A9DA0; devkit 0x4AB150) or directly; the query (0x4AA9A0; devkit 0x4ABD50) is, in both FW 4.03
// builds, `return 0x800F0A25` — status and time are never written. DebugInfoView then keeps its
// "trial system software" panel hidden. (No host value: this is the firmware's answer.)
SSN_PINVOKE("libSceVshKernelWrapper.a", "sceSblPupExpirationGetStatus", int32_t, ssn_VshKernelWrapper_SblPupExpirationGetStatus,
            (uint32_t* /*status*/, uint32_t* /*time*/)) {
  return static_cast<int32_t>(0x800F0A25u);
}
SSN_ICALL("Sce.Vsh.ShellUI.Settings.Sbl.SblWrapper::SblDevActHasPassCodeNative", SblDevActHasPassCodeNative);

}  // namespace ssn::sysbridge
