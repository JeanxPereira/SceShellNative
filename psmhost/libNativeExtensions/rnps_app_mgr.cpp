// RNPS app manager wrapper exported by libNativeExtensions.sprx (FW 4.03).
#include <cstring>
#include <filesystem>

#include "common/managed_path.h"
#include "host/boundary.h"
#include "host/context.h"

namespace ssn::native_ext {

// int RnpsAppMgrWrapperGetPath(const char* titleId, char* path, int pathsize)
// — libNativeExtensions 0x163e0 (NID D-2SZbPFqzQ):
//   pathsize < 128 or a null argument -> -1; asks the RNPS app manager for the title's
//   info record and copies its 128-byte install path; 1 when installed, 0 when the
//   manager reports "not installed" (0x81860008), the manager's error otherwise.
// The host serves installed RNPS apps from <rnps root>/apps/<titleId>; the path is a
// host path (in the drive-less form of host::managed_path) because the managed side opens it
// with System.IO directly.
SSN_PINVOKE("NativeExtensions.sprx", "RnpsAppMgrWrapperGetPath", int, ssn_NativeExtensions_RnpsAppMgrWrapperGetPath,
            (const char* title_id, char* path, int pathsize)) {
  if (pathsize < 128 || !title_id || !path) return -1;
  const auto dir = std::filesystem::path(host::ctx().rnps_root) / "apps" / title_id;
  if (!std::filesystem::is_directory(dir)) return 0;
  const std::string s = host::managed_path(dir);
  if (s.size() >= 128) return -1;
  std::memset(path, 0, 128);
  std::memcpy(path, s.data(), s.size());
  return 1;
}

}  // namespace ssn::native_ext
