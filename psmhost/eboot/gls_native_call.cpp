// GameLiveStreamingNativeCall (eboot.bin, FW 4.03): the Game Live Streaming IPC client the
// ShellUI eboot links ("SceGameLiveStreamingIpcClient") and publishes to
// Sce.Vsh.Gls.NativeCall ({name, fn} table 0xcf1bc0).
//
// State (static initialiser 0x4D7A70): the client object qword_EE3190 with its server
// session at +200 (ymmword_EE3258, 0 = none), the GLS event flags at +160..+184
// (ymmword_EE3230, -1 = not opened) and +192 (-1), a reference count dword_EE32A0, and the
// mutex unk_EE3280.
//
// InitializeInt only creates the client when SceBootStatusFlags has SCE_BOOT_STAT_GLS
// (0x800), which the console's Game Live Streaming service sets once it runs. The host runs
// no such service (no video encoder or streaming back end), so the bit is never set and the
// firmware's own failure results follow: 0x80A00003 from InitializeInt, 0x80A00004 from the
// calls that need the session or the event flags.
#include <cstdint>
#include <mutex>

#include "host/boundary.h"
#include "libkernel/event_flag.h"

namespace ssn::eboot {
namespace {

constexpr int32_t kErrNotReady = static_cast<int32_t>(0x80A00003u);    // -2136997885
constexpr int32_t kErrNoSession = static_cast<int32_t>(0x80A00004u);   // -2136997884 / 2157969412
constexpr int32_t kErrInvalidArg = static_cast<int32_t>(0x80A00002u);  // -2136997886

struct Gls {
  std::mutex mu;                // unk_EE3280
  int32_t refs = 0;             // dword_EE32A0
  bool session = false;         // ymmword_EE3258 (+200) != 0
  bool event_flags = false;     // ymmword_EE3230 (+160..+184) != -1
};
Gls& gls() {
  static Gls g;
  return g;
}

}  // namespace

// InitializeIntNativeCall (0x4D1100): under the mutex; with a client already up the count
// is raised. Otherwise SceBootStatusFlags is opened (_1vDaenmJtyA) and polled for 0x800
// (_9lvj5DjHZiA, OR); only with the bit set and no session yet is the client created
// (0x4D7B70 1024/1024/2, callbacks 0x4D6070, session 0x4D7BE0). Failure -> 0x80A00003.
SSN_PINVOKE("GameLiveStreamingNativeCall", "InitializeIntNativeCall", int32_t, ssn_Gls_InitializeInt, ()) {
  Gls& g = gls();
  std::lock_guard<std::mutex> l(g.mu);
  if (g.refs != 0) {
    ++g.refs;
    return 0;
  }
  kernel::EventFlag* ef = kernel::open_event_flag("SceBootStatusFlags");
  if (!ef) return kErrNotReady;
  uint64_t bits = 0;
  const int32_t r = kernel::poll_event_flag(ef, 0x800, kernel::kEvfWaitModeOr, &bits);
  if (r < 0 || bits == 0 || g.session) return kErrNotReady;
  // SCE_BOOT_STAT_GLS set: the IPC client to the Game Live Streaming service would be
  // created here. Nothing on the host sets the bit.
  host::note_unimplemented("eboot GLS IPC client (0x4D7B70 / 0x4D7BE0) not reproduced");
  return kErrNotReady;
}

// TerminateIntNativeCall (0x4D12A0): count 0 -> 0x80A00003; the last reference ends the
// session (0x4D7E60).
SSN_PINVOKE("GameLiveStreamingNativeCall", "TerminateIntNativeCall", int32_t, ssn_Gls_TerminateInt, ()) {
  Gls& g = gls();
  std::lock_guard<std::mutex> l(g.mu);
  if (g.refs == 0) return kErrNotReady;
  --g.refs;
  return 0;
}

// CreateBroadcasterUIContextIntNativeCall (0x4D13A0) / CreateSpectatorContextIntNativeCall
// (0x4D1420): no session -> 0x80A00004.
SSN_PINVOKE("GameLiveStreamingNativeCall", "CreateBroadcasterUIContextIntNativeCall", int32_t,
            ssn_Gls_CreateBroadcasterUIContextInt, ()) {
  std::lock_guard<std::mutex> l(gls().mu);
  return kErrNoSession;
}
SSN_PINVOKE("GameLiveStreamingNativeCall", "CreateSpectatorContextIntNativeCall", int32_t, ssn_Gls_CreateSpectatorContextInt,
            ()) {
  std::lock_guard<std::mutex> l(gls().mu);
  return kErrNoSession;
}

// RegisterEventListenerIntNativeCall(contextId) (0x4D3290): no session -> 0x80A00004.
SSN_PINVOKE("GameLiveStreamingNativeCall", "RegisterEventListenerIntNativeCall", int32_t, ssn_Gls_RegisterEventListenerInt,
            (int32_t)) {
  std::lock_guard<std::mutex> l(gls().mu);
  return kErrNoSession;
}

// CheckEventIntNativeCall(id) (0x4D3410): with a session, polls the client's event flag
// (0x4D8320: +192 == -1 -> 0x80A00003) for bit `id` (mod 64) with mode 0x22 (OR | clear):
// bit set -> 1, SCE_KERNEL_ERROR_EBUSY (0x80020010, not set) -> 0, other errors as they
// are. No session -> 0x80A00004.
SSN_PINVOKE("GameLiveStreamingNativeCall", "CheckEventIntNativeCall", int32_t, ssn_Gls_CheckEventInt, (int32_t)) {
  return kErrNoSession;
}

// GetEventArgIntNativeCall(contextId, ref StructEventArg) (0x4D34A0): no session ->
// 0x80A00004 (null argument -> 0x80A00002 only with a session).
SSN_PINVOKE("GameLiveStreamingNativeCall", "GetEventArgIntNativeCall", int32_t, ssn_Gls_GetEventArgInt, (int32_t, void* arg)) {
  std::lock_guard<std::mutex> l(gls().mu);
  if (gls().session && !arg) return kErrInvalidArg;
  return kErrNoSession;
}

// EnableLiveStreamingIntNativeCall / EnableSystemScreenIntNativeCall /
// EnableBeRightBackIntNativeCall (contextId, bool) (0x4D18A0 / 0x4D18E0 / 0x4D1920): set or
// clear bit 0x10 / 0x20 / 0x40 of the event flag at +184; not opened (-1) -> 0x80A00004.
SSN_PINVOKE("GameLiveStreamingNativeCall", "EnableLiveStreamingIntNativeCall", int32_t, ssn_Gls_EnableLiveStreamingInt,
            (int32_t, uint8_t)) {
  return gls().event_flags ? 0 : kErrNoSession;
}
SSN_PINVOKE("GameLiveStreamingNativeCall", "EnableSystemScreenIntNativeCall", int32_t, ssn_Gls_EnableSystemScreenInt,
            (int32_t, uint8_t)) {
  return gls().event_flags ? 0 : kErrNoSession;
}
SSN_PINVOKE("GameLiveStreamingNativeCall", "EnableBeRightBackIntNativeCall", int32_t, ssn_Gls_EnableBeRightBackInt,
            (int32_t, uint8_t)) {
  return gls().event_flags ? 0 : kErrNoSession;
}

}  // namespace ssn::eboot
