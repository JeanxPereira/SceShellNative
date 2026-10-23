// Sce.Vsh.SysBridge.ErrorTable P/Invoke ([DllImport("libSysBridgeErrorTable")], FW 4.03).
//
// libSceSysBridge.sprx publishes "libSysBridgeErrorTable" as a {name, fn} table (0x78680 ->
// 0x786a0); its one entry, "sceErrorTableGetShortError", is the module's own code at 0x15e0:
//   null buffer -> 0x80EF0000 (-2131558400);
//   otherwise a linear search of the 20-byte records {u32 code; char shortError[16]} from
//   0x18e60 up to 0x5e150 (14166 records); a match writes the record's string with
//   snprintf(buf, 16, "%s"), no match writes snprintf(buf, 16, "E2-%08X", code); result 0.
// The table is the module's own data, so the host reads it from the dump's
// /system_ex/common_ex/lib/libSceSysBridge.sprx (a decrypted ELF; PT_LOAD mapping).
// Settings and the system dialogs use the result as the code shown in error dialogs
// (e.g. "CE-112759-5").
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <mutex>
#include <string>
#include <unordered_map>
#include <vector>

#include "core/log.h"
#include "host/boundary.h"
#include "host/context.h"

namespace ssn::sysbridge {
namespace {

constexpr uint64_t kTableBegin = 0x18e60;
constexpr uint64_t kTableEnd = 0x5e150;
constexpr size_t kRecord = 20;
constexpr int32_t kErrNullBuffer = static_cast<int32_t>(0x80EF0000u);

struct Table {
  bool loaded = false;
  std::unordered_map<uint32_t, std::string> map;
};

Table& table() {
  static Table t;
  static std::once_flag once;
  std::call_once(once, [] {
    Firmware* fw = host::ctx().firmware;
    if (!fw) return;
    const std::string path = fw->host_path("/system_ex/common_ex/lib/libSceSysBridge.sprx");
    std::ifstream f(std::filesystem::path(path), std::ios::binary);
    if (!f) {
      log::error("errortable", "cannot read {}", path);
      return;
    }
    const std::vector<uint8_t> d((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
    if (d.size() < 64 || std::memcmp(d.data(), "\x7f" "ELF", 4) != 0) {
      log::error("errortable", "{} is not an ELF image", path);
      return;
    }
    uint64_t phoff;
    uint16_t phentsize, phnum;
    std::memcpy(&phoff, &d[0x20], 8);
    std::memcpy(&phentsize, &d[0x36], 2);
    std::memcpy(&phnum, &d[0x38], 2);
    for (uint32_t i = 0; i < phnum; ++i) {
      const size_t o = static_cast<size_t>(phoff) + size_t(i) * phentsize;
      if (o + 56 > d.size()) break;
      uint32_t type;
      uint64_t off, va, filesz;
      std::memcpy(&type, &d[o], 4);
      std::memcpy(&off, &d[o + 8], 8);
      std::memcpy(&va, &d[o + 16], 8);
      std::memcpy(&filesz, &d[o + 32], 8);
      if (type != 1 || kTableBegin < va || kTableEnd > va + filesz) continue;  // PT_LOAD holding the table
      const uint64_t base = off + (kTableBegin - va);
      if (base + (kTableEnd - kTableBegin) > d.size()) break;
      for (uint64_t r = 0; r < kTableEnd - kTableBegin; r += kRecord) {
        uint32_t code;
        std::memcpy(&code, &d[base + r], 4);
        const char* s = reinterpret_cast<const char*>(&d[base + r + 4]);
        t.map.emplace(code, std::string(s, strnlen(s, 16)));  // first match wins, as the linear search
      }
      t.loaded = true;
      break;
    }
    if (!t.loaded) log::error("errortable", "error table 0x{:x}..0x{:x} not found in {}", kTableBegin, kTableEnd, path);
  });
  return t;
}

}  // namespace

// int sceErrorTableGetShortError(ref SceErrorTableShortString buf (char[16]), int errorCode) — 0x15e0.
SSN_PINVOKE("libSysBridgeErrorTable", "sceErrorTableGetShortError", int32_t, ssn_SysBridgeErrorTable_GetShortError,
            (char* buf, int32_t error_code)) {
  if (!buf) return kErrNullBuffer;
  const Table& t = table();
  const auto it = t.map.find(static_cast<uint32_t>(error_code));
  if (it != t.map.end()) std::snprintf(buf, 16, "%s", it->second.c_str());
  else std::snprintf(buf, 16, "E2-%08X", static_cast<uint32_t>(error_code));
  return 0;
}

}  // namespace ssn::sysbridge
