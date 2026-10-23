// Sce.PlayStation.Orbis.Framework.PsmFramework natives (libScePsm, FW 4.03), registered by
// the PSM initialiser (lWlBrUu77Kg, libScePsm 0x2c9460).
#include <cstdint>

#include "host/boundary.h"
#include "libScePsm/runtime/event_hub.h"

namespace ssn::psm {
namespace {

// GetEnableNativeMethod(out int) — 0x2d2fa0: *out = byte_5409C8, result 0. The initialiser
// copies that byte from PsmInitParam +251 (0x2cb5d2); the PsmInitParam constructor
// (FqHN0elWA6E, 0x2c9200) stores 0x01010101 at +248, so +251 = 1, and the ShellUI eboot's
// InitializeFramework (sub_E500) does not change it.
int32_t GetEnableNativeMethod(int32_t* enable) {
  *enable = 1;
  return 0;
}
SSN_ICALL("Sce.PlayStation.Orbis.Framework.PsmFramework/Native::GetEnableNativeMethod(int&)", GetEnableNativeMethod);

// Update() — 0x2d2fb0: under the framework lock, calls every update callback that libScePsm
// components registered in the list at xmmword_540980. The one libScePsm itself registers is
// the event hub's (sub_2D2630, by TGsos8ZCV8c 0x2d2880): dispatch the posted events.
void Update() { events::pump(); }
SSN_ICALL("Sce.PlayStation.Orbis.Framework.PsmFramework/Native::Update()", Update);

// GetSysIdtableCount() — 0x2d3040: the kernel's count of the process's id-table entries
// (import 2YsHtbvCrgs). PUI forces a GC above 3996. HOST VALUE: the host process has no PS5
// kernel id table; 0.
int32_t GetSysIdtableCount() {
  static const bool noted_ = (host::note_unimplemented("HOST VALUE libScePsm GetSysIdtableCount: no PS5 kernel id table (0)"), true);
  (void)noted_;
  return 0;
}
SSN_ICALL("Sce.PlayStation.Orbis.Framework.PsmFramework/Native::GetSysIdtableCount()", GetSysIdtableCount);

// IsReadyForShutdown(out int) — 0x2d3790: 1 unless a registered shutdown-readiness callback
// says otherwise; the host registers none.
void IsReadyForShutdown(int32_t* ready) { *ready = 1; }
SSN_ICALL("Sce.PlayStation.Orbis.Framework.PsmFramework/Native::IsReadyForShutdown(int&)", IsReadyForShutdown);

// StartShutdown() — 0x2d3750: calls the registered shutdown callbacks; none on the host.
void StartShutdown() {}
SSN_ICALL("Sce.PlayStation.Orbis.Framework.PsmFramework/Native::StartShutdown()", StartShutdown);

}  // namespace
}  // namespace ssn::psm
