// Sce.PlayStation.Core.Net.HttpCacheManager.Native (libScePsm.sprx, FW 4.03).
#include <atomic>
#include <cstdint>

#include "host/boundary.h"

namespace ssn::psm {
namespace {

// byte_541410: set once the system module has been loaded.
std::atomic<bool> g_loaded{false};

// LoadModule() — 0x2e97f0: when byte_541410 is clear, sceSysmoduleLoadModuleInternal(0x80000078)
// (39iV5E1HoCk); a zero result sets the byte. Returns that result (0 when already loaded).
// Module id 0x80000078 is a record of libSceSysmodule's module table (libSceSysmodule.sprx file
// offset 0x20ba0), i.e. a system module the firmware carries, whose load answers 0 on the console.
// The host has no PRX loader: the module's functions are served (or stubbed) by the host, so the
// load is complete as soon as it is asked for.
int32_t LoadModule() {
  g_loaded.store(true);
  return 0;
}
SSN_ICALL("Sce.PlayStation.Core.Net.HttpCacheManager/Native::LoadModule", LoadModule);

}  // namespace
}  // namespace ssn::psm
