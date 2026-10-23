// RNPS libcurl glue of libNativeExtensions.sprx (FW 4.03): the default User-Agent.
#include <cstdint>
#include <iomanip>
#include <sstream>
#include <string>

#include "core/log.h"
#include "host/boundary.h"
#include "libkernel/process_param.h"

namespace ssn::native_extensions {
namespace {

// Static initializer sub_26E0 (registered by the module initializer sub_4A20 on the
// global at 0x381C8; the std::string lives at 0x381D0): an ostringstream gets
//   "RNPS/" << major << "." << setfill('0') << setw(2) << minor << " (PlayStation 5)"
// with v = sceKernelGetProsperoCompiledSdkVersion() (import GGeRJk1XdWc; on failure
// v = 0x04000039, 0x272d..0x2736) and
//   major = ((v >> 24) & 0xF) + 2 * ((v >> 29) & 3)   (bextr 0x418 / 0x21D, 0x2857..0x2861)
//   minor = ((v >> 16) & 0xF) + 10 * ((v >> 20) & 0xF) (bextr 0x410 / 0x414, 0x28b2..0x28bf)
const std::string& default_ua() {
  static const std::string ua = [] {
    uint32_t v = 0;
    if (kernel::get_prospero_compiled_sdk_version(&v) < 0) v = 0x04000039;
    const uint32_t major = ((v >> 24) & 0xF) + 2 * ((v >> 29) & 3);
    const uint32_t minor = ((v >> 16) & 0xF) + 10 * ((v >> 20) & 0xF);
    std::ostringstream s;
    s << "RNPS/" << major << "." << std::setfill('0') << std::setw(2) << minor << " (PlayStation 5)";
    log::info("curl", "default UA \"{}\"", s.str());
    return s.str();
  }();
  return ua;
}

}  // namespace

// CurlGetDefaultUA() — libNativeExtensions 0x1580 (NID mtAGcMQCufk): returns the
// c_str() of the global std::string at 0x381D0 built by sub_26E0.
SSN_PINVOKE("libNativeExtensions.dll", "CurlGetDefaultUA", const char*, ssn_NativeExtensions_CurlGetDefaultUA, ()) {
  return default_ua().c_str();
}

}  // namespace ssn::native_extensions
