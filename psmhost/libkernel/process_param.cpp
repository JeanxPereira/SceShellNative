#include "libkernel/process_param.h"

#include <cstring>
#include <fstream>
#include <iterator>
#include <string>
#include <vector>

#include "core/log.h"
#include "host/context.h"

namespace ssn::kernel {

uint64_t procparam_sdk_version() {
  static const uint64_t v = [] {
    const std::string path = host::ctx().firmware->host_path("/system_ex/app/NPXS40087/eboot.bin");
    std::ifstream f(path, std::ios::binary);
    std::vector<char> d((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
    uint64_t version = 0;
    if (d.size() > 0x40) {
      uint64_t phoff;
      uint16_t phnum;
      std::memcpy(&phoff, d.data() + 0x20, 8);
      std::memcpy(&phnum, d.data() + 0x38, 2);
      for (uint16_t i = 0; i < phnum && phoff + (i + 1) * 56 <= d.size(); ++i) {
        const char* ph = d.data() + phoff + i * 56;
        uint32_t type;
        uint64_t off;
        std::memcpy(&type, ph, 4);
        std::memcpy(&off, ph + 8, 8);
        if (type == 0x61000001 && off + 0x18 <= d.size()) std::memcpy(&version, d.data() + off + 0x10, 8);
      }
    }
    log::info("kernel", "process param sdk_version 0x{:016x} (eboot PT_SCE_PROCPARAM +0x10)", version);
    return version;
  }();
  return v;
}

int32_t get_prospero_compiled_sdk_version(uint32_t* out) {
  if (!out) return static_cast<int32_t>(0x80020016u);
  *out = static_cast<uint32_t>(procparam_sdk_version() >> 32);
  return 0;
}

}  // namespace ssn::kernel
