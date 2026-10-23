// SceShellCore launcher service, ShellUI event queue (FW 4.03).
//
// ShellUI's LncUtil client (libSceSystemService, lnc_util_ipc_client.cpp):
//   initialize (0x6b40): IPMI client "SceLncIpcClient" connecting to the server
//     "SceLncService" with one 5736-byte message queue; then getAppStatus for its own appId.
//   getEventForShellUI (0x9000): client not initialised -> 0x80940001; null buffer ->
//     0x80940005; otherwise IpmiClient::tryGetMessage(queue 0, buf, &size, 5736)
//     (libSceIpmi vtable +56, 0xdf0 -> 0x4a20: ipmimgr call 0x252 through libkernel
//     Hk7iHmGxB18, errno -> SCE error by D4yla3vx4tY (libkernel_sys 0x16130: 0x80020000 + errno)).
//     0xdf0 passes the status through unless it is 0x80020002/0x8002000C/0x8002000E/0x80020059.
//   Kernel side (the FW 7.00 kernel image; no FW 4.03 kernel is in hand): op 0x252 ->
//     0xFFFFFFFF80CFB4B0 -> getMessage 0xFFFFFFFF80CFAB30 with a zero wait flag -> the
//     non-blocking receive 0xFFFFFFFF805E1A30, which answers EAGAIN (35) when the channel's
//     head (+136) is empty; getMessage returns it unchanged. ShellUI therefore reads 0x80020023.
// The managed EventMonitor.Update only reads an event when UpdateEvent returns 0, so any
// non-zero status means "no event".
#include "shellcore/lnc_service.h"

#include <deque>
#include <mutex>

#include "host/boundary.h"

namespace ssn::shellcore {
namespace {

// IPMI tryGetMessage on an empty queue: EAGAIN (kernel 0xFFFFFFFF805E1A30) as SCE_KERNEL_ERROR_EAGAIN.
constexpr int32_t kQueueEmpty = static_cast<int32_t>(0x80020023u);

std::mutex g_lock;
std::deque<LncEvent> g_queue;

}  // namespace

void push_event_for_shellui(const LncEvent& e) {
  std::lock_guard<std::mutex> lk(g_lock);
  g_queue.push_back(e);
}

int32_t get_event_for_shellui(LncEvent* out) {
  if (!out) return static_cast<int32_t>(0x80940005u);  // LNC_CHECK "event" (0x9000, line 496)
  std::lock_guard<std::mutex> lk(g_lock);
  if (g_queue.empty()) return kQueueEmpty;
  *out = g_queue.front();
  g_queue.pop_front();
  return 0;
}

// sceLncUtilGetGpuCrashFullDumpAppStatus(status) — libSceSystemService 0x6650 -> 0xA560:
// null -> 0x80940005; client not initialised -> 0x80940001; otherwise IPMI 0x3002F with a
// 1320-byte output -> SceShellCore 0x4866F0 (system processes only) -> lnc_manager 0x43EBC0:
// the first running application whose GPU-crash full-dump flag (+1280) is set fills the
// status; with none, 0x80940004. The host runs no applications, so none has crashed.
SSN_PINVOKE("libSceLncUtilWrapper", "sceLncUtilGetGpuCrashFullDumpAppStatus", int32_t,
            ssn_LncUtil_GetGpuCrashFullDumpAppStatus, (void* status)) {
  if (!status) return static_cast<int32_t>(0x80940005u);
  return static_cast<int32_t>(0x80940004u);
}

}  // namespace ssn::shellcore
