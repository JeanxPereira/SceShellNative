// Sce.PlayStation.Core.Runtime.NativeThread (Sce.PlayStation.Core.dll) — P/Invokes into
// "Sce.PlayStation.Core", which libScePsm.sprx serves from its {name, fn} table (0x4a54f0):
//   Runtime_GetThreadPriority -> 0x300f40, Runtime_SetThreadPriority -> 0x300f90.
//
// Runtime_SetThreadPriority(int prio) — 0x300f90: scePthreadSetprio(scePthreadSelf(), prio)
// (W0Hpm2X0uPE -> libkernel pthread_setprio 0xc4d0: stores the priority in the pthread (+152)
// and sets it in the kernel scheduler (rtprio_thread); a kernel error comes back as
// 0x80020000 | errno). The only caller in the shell is ReactNative.PUI's
// SingleBackgroundMessageQueueThread (priority 615, inside the PS5 range 256..767).
//
// HOST POLICY (thread scheduling): the PC has no PS5 kernel scheduler; the host does not map
// PS5 priorities onto Windows thread priorities. The call answers what it answers on the console
// for a valid priority — 0 — without changing how Windows schedules the thread.
#include <cstdint>

#include "host/boundary.h"

namespace ssn::psm {

SSN_PINVOKE("Sce.PlayStation.Core", "Runtime_SetThreadPriority", int32_t, ssn_PsmCore_Runtime_SetThreadPriority,
            (int32_t prio)) {
  (void)prio;
  static const bool noted = (host::note_unimplemented(
                                 "HOST POLICY thread scheduling: PS5 thread priorities / schedule policies are not applied to Windows threads (calls succeed)"),
                             true);
  (void)noted;
  return 0;
}

}  // namespace ssn::psm
