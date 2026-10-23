// Kernel objects that other processes of the console create before ShellUI starts
// and that ShellUI opens by name.
#include "core/log.h"
#include "host/boundary.h"
#include "libkernel/event_flag.h"

namespace ssn::system {

void create_system_objects() {
  // "SceBootStatusFlags": system boot-status event flag (opened by
  // Sce.Vsh.ShellUI.AppMain.BootEvf.Initialize; on the console it belongs to the system
  // core process). ShellUI only sets bits in it (0x20000, 0x80000, 0x1000000). Its
  // initial pattern on the console is not traced: the host starts it at 0.
  kernel::create_event_flag("SceBootStatusFlags", 0x20 /* ATTR_MULTI */, 0);

  // "SceAutoMounterUIM2Evf": SceShellCore's AutoMounter error-dialog launcher
  // (initializeErrorDialogLauncher, SceShellCore 0x29C1E0) creates it with attributes
  // 0x10 | 0x100 and pattern 0, then registers callbackM2Mount (0x29D250) for M.2 mount
  // events. ShellUI's BootManager.GetM2SystemModalDialogTypeFlags waits (OR, no timeout)
  // for any of bits 0..7: callbackM2Mount sets bit 0 (1) when the M.2 (media type 0x1002)
  // mount result is 0 or one of 0x80F40007 / 0x80F40034 / 0x80F40036 ("not target", no
  // dialog), and bits 1..7 for the error dialogs (0x29D450).
  // HOST VALUE: the host has no M.2 SSD; the flag is created with bit 0 already set, i.e.
  // the state after the AutoMounter has reported the M.2 slot with a "not target" result
  // (which result code the console reports for an empty slot is not traced).
  kernel::create_event_flag("SceAutoMounterUIM2Evf", 0x10 | 0x100, 1);

  // "SceShellCoreUtilUIStatus": SceShellCore's UI status flag (ShellCoreUtil init 0x61C580:
  // attr 0x120, initial pattern 0x20001); ShellUI sets it through sceShellCoreUtilSetUIStatus.
  kernel::create_event_flag("SceShellCoreUtilUIStatus", 0x120, 0x20001);

  // The other event flags of that init (0x61C580), all attr 0x120:
  //   "SceShellCoreUtilPowerControl": initial pattern = the "OnScreenProfiler" debug option
  //     byte (0x1A7663F, .bss, listed at 0x19A9440 in the debug-option table that only the
  //     debug-option reader 0x760510 sets) << 23. The host has no debug options: 0.
  //   "SceShellCoreUtilAppFocus" / "SceShellCoreUtilCtrlFocus": all bits set.
  //   "SceShellCoreUtilDevIdxBehavior": 0.
  kernel::create_event_flag("SceShellCoreUtilPowerControl", 0x120, 0);
  kernel::create_event_flag("SceShellCoreUtilAppFocus", 0x120, ~0ull);
  kernel::create_event_flag("SceShellCoreUtilCtrlFocus", 0x120, ~0ull);
  kernel::create_event_flag("SceShellCoreUtilDevIdxBehavior", 0x120, 0);

  // "SceLoginMgrServerHmdConnect": SceShellCore's LoginMgr server (constructors 0x491C36 /
  // 0x492FD6) creates it with attr 0x112 and pattern 0; the high dword is the user id of the
  // VR headset's user (sceShellCoreUtilGetUserIdOfMorpheusUser). No headset on the host.
  kernel::create_event_flag("SceLoginMgrServerHmdConnect", 0x112, 0);

  // SceShellCore's SystemStateMgr: "SceSystemStateMgrInfo" (0x6C5BC0: attr 0x120, pattern
  // 0x10000000A = trigger code 1 | state 10 INITIALIZING) and "SceSystemStateMgrStatus"
  // (0x6BE700: attr 0x120, pattern 0). Its init (0x6BE700) then sets the boot trigger code and
  // state from 0x6C8350 (0x6BEC70 / 0x6BED70), and, the state not being 500
  // (MAIN_ON_STANDBY), sets Status bit 4. 0x6C8350 maps sceKernelIccGetPowerUpCause:
  // 0x200 -> 4, 0x40000 -> 2, 0x80000 -> 3, any other cause -> 1, state 1000 (WORKING); when
  // the ICC call fails, trigger code 1 and state 1000 as well.
  // HOST VALUE: no ICC (power-up cause) on the PC; the failed-call path: code 1, WORKING.
  kernel::create_event_flag("SceSystemStateMgrInfo", 0x120, (1ull << 32) | 1000);
  kernel::create_event_flag("SceSystemStateMgrStatus", 0x120, 4);
  host::note_unimplemented(
      "HOST VALUE SceSystemStateMgr: no ICC power-up cause; trigger code 1, state WORKING (failed-call path of 0x6C8350)");
}

}  // namespace ssn::system
