// libSceRemotePlayClientIpc (FW 4.03) as ReactNative.Modules.Vsh's RemotePlayClientModule
// reaches it.
//
// RemotePlayClientModule.GetClientStatus allocates a RemotePlayClientStatus with
// Marshal.AllocHGlobal, calls SceRemotePlayClientGetClientStatus and, for a non-negative
// result, reads the structure back (Marshal.PtrToStructure, strings included). The P/Invoke
// target "libReactNative.Modules.Vsh.sprx!sceRemotePlayClientGetClientStatus" is the
// export ux9VESJUgh4 (libReactNative.Modules.Vsh.sprx 0x4450), a thunk to the
// libSceRemotePlayClientIpc export irNARl_D5xU (0xaa0):
//   client = singleton (0x1150);
//   if client initialised (0x470) and its IPC connection is complete (0xf40: the three
//   handles at +24/+32/+40 all non-zero) -> fill *status over IPC (0x1b0) and return 0
//   (or the IPC's negative result);
//   otherwise -> 0x8122101D, *status untouched.
//
// The connection is made by sceRemotePlayClientInitialize / WaitConnectionAsync, whose
// server (the Remote Play client session service) the host does not run, so the client
// is never connected and the binary's answer is 0x8122101D. The managed caller then
// rejects the promise ("SceRemotePlayClientGetClientStatus(memHead) failed") instead of
// decoding uninitialised memory.
#include <cstdint>

#include "host/boundary.h"

namespace ssn::remote_play_client {

constexpr int32_t kErrNotConnected = static_cast<int32_t>(0x8122101Du);  // irNARl_D5xU default result

SSN_PINVOKE("libReactNative.Modules.Vsh.sprx", "sceRemotePlayClientGetClientStatus", int32_t,
            ssn_RemotePlayClient_GetClientStatus, (void* status)) {
  (void)status;
  static const bool noted = (host::note_unimplemented(
                                 "Remote Play client session service not provided: client never connected (GetClientStatus -> 0x8122101D)"),
                             true);
  (void)noted;
  return kErrNotConnected;
}

// Sce.Vsh.RemotePlayClientIpcWrapper: "libSceRemotePlayClientIpcWrapper!
// sceRemotePlayClientIpcWrapperIsStreamingOnFg" — the eboot's {name, fn} entry (0xd04010)
// jumps to the libSceRemotePlayClientIpc export JGKtr0+B87A (0xbf0): the same client
// singleton (0x1150); connected (0x13E0 / 0xF40) -> IPMI method 0x12340004 (0x290) and its
// result; otherwise 0x8122101D. The managed IsStreamingOnFg() is `result > 0`. The client is
// never connected on the host (see above), so: 0x8122101D (not streaming).
SSN_PINVOKE("libSceRemotePlayClientIpcWrapper", "sceRemotePlayClientIpcWrapperIsStreamingOnFg", int32_t,
            ssn_RemotePlayClientIpcWrapper_IsStreamingOnFg, ()) {
  return kErrNotConnected;
}

}  // namespace ssn::remote_play_client
