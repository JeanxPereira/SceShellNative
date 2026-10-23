// sceShellCoreUtilGetVMStatsForTelemetry (libSceSystemService 0x14b90, NID ESxtLvQZO5Y),
// reached through "libSceShellCoreUtilWrapper" (eboot table 0xcf5030 -> the import itself).
//
// The library formats the shared VM statistics block (qword_45218, filled by ShellCore)
// as JSON: per process class {count, vmSize, rss}, each pair read only when its count is
// non-zero. No block -> 0x80E30002; text >= 2048 -> 0x80E30004; buffer too small ->
// 0x80E30005.
//
// HOST VALUE: the block exists but counts no console processes in any class (the PC runs
// none of SceShellUI / SceNK* / SceJSCd / SceCdlgApp as console processes), so every
// triple is 0.
#include <cstdint>
#include <cstdio>
#include <cstring>

#include "host/boundary.h"

namespace ssn::systemservice {

SSN_PINVOKE("libSceShellCoreUtilWrapper", "sceShellCoreUtilGetVMStatsForTelemetry", int32_t,
            ssn_ShellCoreUtil_GetVMStatsForTelemetry, (char* out, uint64_t size)) {
  host::note_unimplemented("HOST VALUE ShellCore VM statistics block: no console processes (all zero)");
  const uint32_t v[7][3] = {};  // SceShellUI, SceNKNetworkProcess, SceNKUIProcess, SceNKWebProcess, SceJSCd, SceCdlgApp, Other
  char text[2048] = {};
  const int n = std::snprintf(text, 2047,
                              "{ \"SceShellUI\" : [%u, %u, %u], \"SceNKNetworkProcess\" : [%u, %u, %u], \"SceNKUIProcess\" : [%u, %u, %u], "
                              "\"SceNKWebProcess\" : [%u, %u, %u], \"SceJSCd\" : [%u, %u, %u], \"SceCdlgApp\" : [%u, %u, %u], \"Other\" : [%u, %u, %u] }",
                              v[0][0], v[0][1], v[0][2], v[1][0], v[1][1], v[1][2], v[2][0], v[2][1], v[2][2], v[3][0], v[3][1],
                              v[3][2], v[4][0], v[4][1], v[4][2], v[5][0], v[5][1], v[5][2], v[6][0], v[6][1], v[6][2]);
  if (n < 0 || uint32_t(n) > 0x7FF) return static_cast<int32_t>(0x80E30004u);
  if (uint64_t(n) + 1 > size) return static_cast<int32_t>(0x80E30005u);
  std::strncpy(out, text, size);  // _6sJWiWSRuqk (strncpy) with the caller's size
  return 0;
}

}  // namespace ssn::systemservice
