// Sce.PlayStation.Orbis.IO.NativeFile.Native (Sce.PlayStation.Orbis.dll), served by libScePsm.sprx
// (FW 4.03).
#include <windows.h>

#include <cstdint>
#include <string>

#include <mono/metadata/object.h>

#include "common/mono_util.h"
#include "host/boundary.h"

namespace ssn::psm {
namespace {

// SCE_KERNEL_ERROR_* = 0x80020000 | errno.
constexpr int32_t kEio = static_cast<int32_t>(0x80020005u);
constexpr int32_t kEnoent = static_cast<int32_t>(0x80020002u);
constexpr int32_t kEacces = static_cast<int32_t>(0x8002000Du);
constexpr int32_t kEexist = static_cast<int32_t>(0x80020011u);

// Mode per NativeSystemPermission (word_462100): 0x1FF, 0x1FD, 0x16D, 0x1C0, 0x168.
constexpr uint16_t kModes[5] = {0x1FF, 0x1FD, 0x16D, 0x1C0, 0x168};

// The kernel error the console's file system would give for what Windows reports.
int32_t kernel_error(DWORD e) {
  switch (e) {
    case ERROR_FILE_EXISTS:
    case ERROR_ALREADY_EXISTS: return kEexist;
    case ERROR_FILE_NOT_FOUND:
    case ERROR_PATH_NOT_FOUND:
    case ERROR_INVALID_NAME: return kEnoent;
    case ERROR_ACCESS_DENIED: return kEacces;
    default: return kEio;
  }
}

std::wstring wide(const std::string& s) {
  if (s.empty()) return {};
  const int n = MultiByteToWideChar(CP_UTF8, 0, s.data(), static_cast<int>(s.size()), nullptr, 0);
  std::wstring w(static_cast<size_t>(n), L'\0');
  MultiByteToWideChar(CP_UTF8, 0, s.data(), static_cast<int>(s.size()), w.data(), n);
  return w;
}

// CreateWithSystemPermission(string absFilePath, uint type, uint permission) — libScePsm 0x2f2f90:
//   permission > 4 -> -1; path = ScePsmMonoStringToUtf8(absFilePath) (null -> -1);
//   mode = word_462100[permission];
//   type != 0 (NativeDirectory): sceKernelMkdir(path, mode) (1-LFLmRFxxM);
//   type == 0 (NativeFile):      fd = sceKernelOpen(path, 0xA01 = O_WRONLY|O_CREAT|O_EXCL, mode)
//                                (1G3lF1Gg1k8), then sceKernelClose(fd) (UK2Tl2DWUns);
//   a negative kernel result is returned, otherwise 0.
// The shell calls it on a path that does not exist yet and then opens the same path with the
// managed File API (cookie / cache / notification files, /user/data/ui2), so the host creates the
// entry where that managed File API resolves the path (the same string, Windows-resolved; the
// shell's sandbox paths are drive-less rooted, see host::managed_path). The POSIX mode has no
// NTFS counterpart and is not applied.
int32_t CreateWithSystemPermission(MonoString* path_str, uint32_t type, uint32_t permission) {
  if (permission > 4) return -1;
  if (!path_str) return -1;
  (void)kModes[permission];
  const std::wstring path = wide(utf8(path_str));
  if (type != 0) {
    if (!CreateDirectoryW(path.c_str(), nullptr)) return kernel_error(GetLastError());
    return 0;
  }
  HANDLE h = CreateFileW(path.c_str(), GENERIC_WRITE, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr,
                         CREATE_NEW, FILE_ATTRIBUTE_NORMAL, nullptr);
  if (h == INVALID_HANDLE_VALUE) return kernel_error(GetLastError());
  CloseHandle(h);
  return 0;
}
SSN_ICALL("Sce.PlayStation.Orbis.IO.NativeFile/Native::CreateWithSystemPermission", CreateWithSystemPermission);

}  // namespace
}  // namespace ssn::psm
