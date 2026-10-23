// NpPreorderMgr (eboot.bin, FW 4.03): the preorder client the ShellUI eboot links and publishes
// to managed code as "NpPreorderMgrApi" ({name, fn} table 0xcf0800: scePreorderMgrIntInit
// 0x4BD6B0, GetPreorderInfo 0x4BD840, GetPreorderInfoHomeShare 0x4BD880,
// GetPreorderEntitlementList 0x4BD8C0). Each call is a request to SceShellCore's
// "SceNpPreorderMgr" IPC server (LibNpPreorderMgrIpcServer, SceShellCore 0x905800).
//
// SceShellCore answers from its per-user preorder lists (20 users, qword_1AC0880), which the
// preorder manager fills from PlayStation Network. The host has no NP, so the lists are empty.
#include <cstdint>

#include "host/boundary.h"

namespace ssn::eboot {
namespace {

constexpr int32_t kErrNotInitialized = static_cast<int32_t>(0x81A70001u);
constexpr int32_t kErrInvalidArgument = static_cast<int32_t>(0x81A70007u);

#pragma pack(push, 1)
// Sce.Vsh.Np.PreorderMgr.ScePreorderMgrEntitlementList (38 bytes).
struct EntitlementEntry {
  char entitlement_id[37];
  uint8_t is_preorder;
};
#pragma pack(pop)
static_assert(sizeof(EntitlementEntry) == 38);

bool g_initialized = false;  // byte_EE2DE0

}  // namespace

// scePreorderMgrIntInit (0x4BD6B0): once, an NP memory heap and the IPC client of
// "SceNpPreorderMgr" (byte_EE2DE0 set, client in qword_EE2DD8); 0.
SSN_PINVOKE("NpPreorderMgrApi", "scePreorderMgrIntInit", int32_t, ssn_NpPreorderMgr_Init, (void* /*param*/)) {
  g_initialized = true;
  return 0;
}

// scePreorderMgrIntGetPreorderEntitlementList(userId, type, list, count) — 0x4BD8C0 -> 0x4BDA40:
// not initialised -> 0x81A70001, no list -> 0x81A70007, else IPC method 2 with the list
// (38 * count bytes) as the output buffer. The server (SceShellCore 0x907090) clears every
// entry's isPreorder (+37), then for type 1 (0x908FD0) and each signed-in user marks the
// entries found in that user's preorder list whose release time has not passed (0x908DD0).
// With no preorder lists nothing is marked; the server returns 0 (0x81A70007 for a type other
// than 0 and 1, after the clear).
SSN_PINVOKE("NpPreorderMgrApi", "scePreorderMgrIntGetPreorderEntitlementList", int32_t,
            ssn_NpPreorderMgr_GetPreorderEntitlementList, (int32_t /*user*/, int32_t type, EntitlementEntry* list, int32_t count)) {
  if (!g_initialized) return kErrNotInitialized;
  if (!list) return kErrInvalidArgument;
  for (int32_t i = 0; i < count; ++i) list[i].is_preorder = 0;
  return type == 0 || type == 1 ? 0 : kErrInvalidArgument;
}

}  // namespace ssn::eboot
