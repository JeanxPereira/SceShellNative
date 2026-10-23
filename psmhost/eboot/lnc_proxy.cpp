// LncProxy (eboot.bin) — "libSceLncUtil" P/Invokes of Sce.Vsh.LncUtil ({name, fn} table at
// 0xcc4780: UpdateEvent, GetEvent, LaunchAppParamInit).
#include <cstdint>
#include <cstring>
#include <mutex>

#include "host/boundary.h"
#include "shellcore/lnc_service.h"

namespace ssn::eboot {
namespace {
std::mutex g_lock;
shellcore::LncEvent g_event{};  // 0xeba600 (5736 bytes)
int32_t g_last = 0;             // 0xebbc68
}  // namespace

// LncProxy::UpdateEvent — 0x8da0: the first call (byte_EBA5FC == 1) clears the flag and
// calls nENvUAsAKdY (libSceSystemService 0x11330: drops a count at dword_46248 and, at
// zero, clears bit 19 of the event flag qword_440B0 — not reproduced); then every call does
// g_last = sceLncUtilGetEventForShellUI(&g_event) (ZucoOmNsb7w, libSceSystemService
// 0x6470 -> 0x9000), which pops one event ShellCore queued for ShellUI. The host's
// ShellCore launch service answers it (psmhost/shellcore/lnc_service.cpp).
SSN_PINVOKE("libSceLncUtil", "LncProxy::UpdateEvent", int32_t, ssn_LncProxy_UpdateEvent, ()) {
  std::lock_guard<std::mutex> lk(g_lock);
  g_last = shellcore::get_event_for_shellui(&g_event);
  return g_last;
}

// LncProxy::GetEvent(ref Event) — 0x8dd0: null -> 0x80940005; copies the 5736-byte
// event record and returns the last UpdateEvent result.
SSN_PINVOKE("libSceLncUtil", "LncProxy::GetEvent", int32_t, ssn_LncProxy_GetEvent, (void* e)) {
  if (!e) return static_cast<int32_t>(0x80940005u);
  std::lock_guard<std::mutex> lk(g_lock);
  std::memcpy(e, &g_event, sizeof g_event);
  return g_last;
}

// LncProxy::LaunchAppParamInit(ref LaunchAppParam) — 0x8e00: null -> nothing; otherwise the
// first 16 bytes from xmmword_B89B60 {size 0x28, userId -1, appAttr 0, enableCrashReport 1},
// checkFlag (+16) and contextId (+24) zeroed, isSpeculativeLaunch (+32) = 0.
SSN_PINVOKE("libSceLncUtil", "LncProxy::LaunchAppParamInit", void, ssn_LncProxy_LaunchAppParamInit, (void* p)) {
  if (!p) return;
  auto* b = static_cast<uint8_t*>(p);
  static constexpr uint8_t kInit[16] = {0x28, 0, 0, 0, 0xff, 0xff, 0xff, 0xff, 0, 0, 0, 0, 1, 0, 0, 0};
  std::memcpy(b, kInit, sizeof kInit);
  std::memset(b + 16, 0, 16);
  b[32] = 0;
}

}  // namespace ssn::eboot
