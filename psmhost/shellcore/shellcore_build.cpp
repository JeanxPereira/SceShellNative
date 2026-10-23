#include "shellcore/shellcore_build.h"

#include <cstdint>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <mutex>
#include <string>
#include <vector>

#include "core/log.h"
#include "host/context.h"

namespace ssn::shellcore {
namespace {

// GNU build-id (PT_NOTE, NT_GNU_BUILD_ID) of an ELF64 file; empty when absent.
std::vector<uint8_t> build_id(const std::string& path) {
  std::ifstream f(std::filesystem::path(path), std::ios::binary);
  if (!f) return {};
  std::vector<uint8_t> d((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
  if (d.size() < 64 || std::memcmp(d.data(), "\x7f" "ELF", 4) != 0) return {};
  uint64_t phoff;
  uint16_t phentsize, phnum;
  std::memcpy(&phoff, &d[0x20], 8);
  std::memcpy(&phentsize, &d[0x36], 2);
  std::memcpy(&phnum, &d[0x38], 2);
  for (uint32_t i = 0; i < phnum; ++i) {
    const size_t o = static_cast<size_t>(phoff) + size_t(i) * phentsize;
    if (o + 56 > d.size()) break;
    uint32_t type;
    uint64_t off, filesz;
    std::memcpy(&type, &d[o], 4);
    std::memcpy(&off, &d[o + 8], 8);
    std::memcpy(&filesz, &d[o + 32], 8);
    if (type != 4) continue;
    for (uint64_t n = off; n + 12 <= off + filesz && n + 12 <= d.size();) {
      uint32_t namesz, descsz, ntype;
      std::memcpy(&namesz, &d[n], 4);
      std::memcpy(&descsz, &d[n + 4], 4);
      std::memcpy(&ntype, &d[n + 8], 4);
      const uint64_t desc = n + 12 + ((namesz + 3) & ~3u);
      if (desc + descsz > d.size()) break;
      if (ntype == 3 && namesz == 4 && std::memcmp(&d[n + 12], "GNU", 4) == 0)
        return {d.begin() + static_cast<ptrdiff_t>(desc), d.begin() + static_cast<ptrdiff_t>(desc + descsz)};
      n = desc + ((descsz + 3) & ~3u);
    }
  }
  return {};
}

}  // namespace

bool devkit_build() {
  static bool devkit = false;
  static std::once_flag once;
  std::call_once(once, [] {
    // FW 4.03 retail J01736823: a8db017e e3b0f1b9 9113517a 42c95bac
    // FW 4.03 devkit J01736824: f4d8f5bf d1f5e5bb 468618e7 be32b320
    static const uint8_t kRetail[16] = {0xa8, 0xdb, 0x01, 0x7e, 0xe3, 0xb0, 0xf1, 0xb9,
                                        0x91, 0x13, 0x51, 0x7a, 0x42, 0xc9, 0x5b, 0xac};
    static const uint8_t kDevkit[16] = {0xf4, 0xd8, 0xf5, 0xbf, 0xd1, 0xf5, 0xe5, 0xbb,
                                        0x46, 0x86, 0x18, 0xe7, 0xbe, 0x32, 0xb3, 0x20};
    Firmware* fw = host::ctx().firmware;
    const auto id = fw ? build_id(fw->host_path("/system/vsh/SceShellCore.elf")) : std::vector<uint8_t>{};
    if (id.size() >= 16 && std::memcmp(id.data(), kDevkit, 16) == 0) {
      devkit = true;
      log::info("shellcore", "SceShellCore build: FW 4.03 devkit J01736824");
    } else if (id.size() >= 16 && std::memcmp(id.data(), kRetail, 16) == 0) {
      log::info("shellcore", "SceShellCore build: FW 4.03 retail J01736823");
    } else {
      log::warn("shellcore", "SceShellCore build not identified; the host follows the FW 4.03 retail build");
    }
  });
  return devkit;
}

}  // namespace ssn::shellcore
