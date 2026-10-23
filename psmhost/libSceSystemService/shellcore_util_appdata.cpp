// sceShellCoreUtilGetAppData / sceShellCoreUtilSetAppData (libSceSystemService, FW 4.03), reached
// through "libSceShellCoreUtilWrapper" (eboot table 0xcf4ad0 -> the imports).
//
// Client (the library's SceShellCoreUtil IPMI client, qword_45228; not connected -> 0x80E30002):
//   GetAppData WISL-JH-6Ic (0x10770): key null or strnlen(key, 4096) > 0xFFF -> 0x80E30005;
//     a null value buffer counts as size 0; a null outSize goes to a dummy; IPMI method 34
//     (0x169A0) and the server's result.
//   SetAppData xKSgaSVX1io (0x106F0): key / value null or longer than 0xFFF -> 0x80E30005;
//     IPMI method 33 (0x16900) and the server's result.
// Server (SceShellCore): a process-lifetime store (singleton 0x616F00, mutex
// "SceShellCoreUtilAppData", no file behind it) of key -> value maps, one per caller title id
// (the caller's title from SceLncService, 0x47B320):
//   set 0x616EC0 -> 0x616FB0: map[title][key] = value, 0;
//   get 0x6173A0: key absent -> value[0] = 0, *outSize = 0; present -> strncpy(value, v, size),
//     *outSize = strlen(v) + 1; 0.
// ShellUI is the only caller on the host, so one map stands for its title's.
#include <cstdint>
#include <cstring>
#include <map>
#include <mutex>
#include <string>

#include "host/boundary.h"

namespace ssn::systemservice {
namespace {

constexpr int32_t kErrInvalidArg = static_cast<int32_t>(0x80E30005u);

std::mutex g_lock;  // "SceShellCoreUtilAppData"
std::map<std::string, std::string> g_app_data;

bool too_long(const char* s) { return strnlen(s, 4096) > 0xFFF; }

}  // namespace

SSN_PINVOKE("libSceShellCoreUtilWrapper", "sceShellCoreUtilGetAppData", int32_t, ssn_ShellCoreUtil_GetAppData,
            (const char* key, char* value, uint32_t size, uint32_t* out_size)) {
  if (!key || too_long(key)) return kErrInvalidArg;
  if (!value) size = 0;
  uint32_t dummy = 0;
  if (!out_size) out_size = &dummy;
  std::lock_guard l(g_lock);
  const auto it = g_app_data.find(key);
  if (it == g_app_data.end()) {
    if (value) value[0] = 0;
    *out_size = 0;
    return 0;
  }
  if (value && size) std::strncpy(value, it->second.c_str(), size);
  *out_size = static_cast<uint32_t>(it->second.size()) + 1;
  return 0;
}

SSN_PINVOKE("libSceShellCoreUtilWrapper", "sceShellCoreUtilSetAppData", int32_t, ssn_ShellCoreUtil_SetAppData,
            (const char* key, const char* value)) {
  if (!key || !value || too_long(key) || too_long(value)) return kErrInvalidArg;
  std::lock_guard l(g_lock);
  g_app_data[key] = value;
  return 0;
}

}  // namespace ssn::systemservice
