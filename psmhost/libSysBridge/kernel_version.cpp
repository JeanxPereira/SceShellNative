// System software version queries (libSysBridgeKernel -> libkernel_sys, FW 4.03).
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <vector>

#include "core/log.h"
#include "host/boundary.h"
#include "host/context.h"
#include "libkernel/process_param.h"

namespace ssn::sysbridge {
namespace {

// The kernel reports the running firmware's version through sysctl {1, 0x34} (an
// 8-byte value). The host takes the version the firmware itself declares: the
// sdk_version field of the ShellUI eboot's process parameter (PT_SCE_PROCPARAM, +0x10),
// which system modules carry as the version of the firmware they belong to
// (0x0403000009040001 for the 4.03 dump).
uint64_t firmware_version() { return kernel::procparam_sdk_version(); }

}  // namespace

// sceKernelGetGenuineProsperoSystemSwVersion(ref {size, char info[28], value}) —
// libkernel_sys 0x1c610: null -> 0x80020016; info =
// "%2lx.%02lx.%02lx.%02lx-%02lx.%02lx.%02lx.%1lx.%1lx" of the version bytes (byte 5
// replaced by the value of import CL9DIlKFCnA, not traced: 0 on the host), value = the
// version with that byte inserted.
SSN_PINVOKE("libSysBridgeKernel", "sceKernelGetGenuineProsperoSystemSwVersion", int32_t,
            ssn_SysBridgeKernel_sceKernelGetGenuineProsperoSystemSwVersion, (uint8_t* ver)) {
  if (!ver) return static_cast<int32_t>(0x80020016u);
  const uint64_t v = firmware_version();
  const uint8_t b5 = 0;
  char info[28];
  std::snprintf(info, sizeof info, "%2llx.%02llx.%02llx.%02llx-%02llx.%02llx.%02llx.%1llx.%1llx", (v >> 56) & 0xff, (v >> 48) & 0xff,
                (unsigned long long)b5, (v >> 32) & 0xff, (v >> 24) & 0xff, (v >> 16) & 0xff, (v >> 8) & 0xff, (v & 0xff) >> 4, v & 0xf);
  std::memcpy(ver + 8, info, sizeof info);
  const uint64_t value = (v & 0xFFFF00FFFFFFFFFFull) | (uint64_t(b5) << 40);
  std::memcpy(ver + 40, &value, 8);
  return 0;
}

// sceKernelGetPrefixVersion(char* str, size_t len) — libkernel_sys 0x1cd10: from the
// genuine version's low byte >> 4: 2 -> "Preview %x.%x", 1 -> "Beta %x.%x", otherwise
// the release prefix "21.02" (needs len >= 6, else 0x80020016).
SSN_PINVOKE("libSysBridgeKernel", "sceKernelGetPrefixVersion", int32_t, ssn_SysBridgeKernel_sceKernelGetPrefixVersion,
            (char* str, uint64_t len)) {
  const uint64_t v = firmware_version();
  const unsigned kind = unsigned(v & 0xff) >> 4;
  if (kind == 2) {
    if (len < 0xe) return static_cast<int32_t>(0x80020016u);
    std::snprintf(str, 14, "Preview %x.%x", unsigned((v >> 8) & 0xff), unsigned((v >> 40) & 0xff));
  } else if (kind == 1) {
    if (len < 0xb) return static_cast<int32_t>(0x80020016u);
    std::snprintf(str, 11, "Beta %x.%x", unsigned((v >> 8) & 0xff), unsigned((v >> 40) & 0xff));
  } else {
    if (len < 6) return static_cast<int32_t>(0x80020016u);
    std::strcpy(str, "21.02");
  }
  return 0;
}

}  // namespace ssn::sysbridge

namespace ssn::sysbridge {

// sceKernelGetOpenPsIdForSystem(SceKernelOpenPsIdForSystem*) — libkernel_sys 0x1ea30
// (ul57hvm6mBc): null -> 22 (EINVAL); otherwise sysctlbyname("machdep.openpsid_for_sys")
// into the 16 bytes. HOST VALUE: the PC has no console identity; the id reads as 16 zero
// bytes.
SSN_PINVOKE("libSysBridgeKernel", "sceKernelGetOpenPsIdForSystem", int32_t, ssn_SysBridgeKernel_sceKernelGetOpenPsIdForSystem,
            (uint8_t* id)) {
  if (!id) return 22;
  host::note_unimplemented("HOST VALUE OpenPSID (machdep.openpsid_for_sys): no console identity; 16 zero bytes");
  std::memset(id, 0, 16);
  return 0;
}

}  // namespace ssn::sysbridge
