// Sce.PlayStation.Core.Environment.SystemParameters (libScePsm.sprx, FW 4.03).
#include <windows.h>
#include <winioctl.h>

#include <algorithm>
#include <cstdint>
#include <cstring>

#include <filesystem>
#include <vector>

#include "common/managed_path.h"
#include "common/mono_util.h"
#include "core/log.h"
#include "host/boundary.h"
#include "host/context.h"
#include "libScePsm/error.h"
#include "libScePsm/input/pss_input.h"
#include "libSceRegMgr/regmgr.h"

namespace ssn::psm {
namespace {

// SystemParameters.ParameterKey (Sce.PlayStation.Core.dll).
enum ParameterKey : uint32_t { Language = 0, FileSystemSandboxRandomWord = 6, ProcessName = 11 };

// Language tags indexed by SCE_SYSTEM_SERVICE_PARAM_ID_LANG; table at 0x4a22b0
// (30 entries) in libScePsm.sprx.
constexpr const char* kLanguageTags[30] = {
    "ja-JP", "en-US", "fr-FR", "es-ES", "de-DE", "it-IT", "nl-NL", "pt-PT", "ru-RU", "ko-KR",
    "zh-Hant", "zh-Hans", "fi-FI", "sv-SE", "da-DK", "nb-NO", "pl-PL", "pt-BR", "en-GB", "tr-TR",
    "es-419", "ar-AE", "fr-CA", "cs-CZ", "hu-HU", "el-GR", "ro-RO", "th-TH", "vi-VN", "id-ID",
};

// The host's nullfs: `target` appears at `link` as a directory junction (no copy; the
// firmware's files are read in place). A junction already pointing at `target` is kept.
bool mount_dir(const std::filesystem::path& link, const std::filesystem::path& target) {
  namespace fs = std::filesystem;
  std::error_code ec;
  const fs::path abs = fs::absolute(target, ec).lexically_normal();
  if (fs::is_symlink(link, ec) || fs::exists(fs::symlink_status(link, ec))) {
    if (fs::read_symlink(link, ec).lexically_normal() == abs) return true;
    fs::remove(link, ec);  // a junction (or empty directory) only; never its target
  }
  if (!fs::create_directory(link, ec)) return false;
  HANDLE h = CreateFileW(link.c_str(), GENERIC_WRITE, 0, nullptr, OPEN_EXISTING,
                         FILE_FLAG_BACKUP_SEMANTICS | FILE_FLAG_OPEN_REPARSE_POINT, nullptr);
  if (h == INVALID_HANDLE_VALUE) return false;
  const std::wstring sub = L"\\??\\" + abs.wstring(), print = abs.wstring();
  // REPARSE_DATA_BUFFER, MountPointReparseBuffer (ntifs.h; not in the user-mode headers).
  std::vector<uint8_t> buf(16 + (sub.size() + print.size() + 2) * sizeof(wchar_t));
  auto put16 = [&](size_t at, size_t v) { const uint16_t w = static_cast<uint16_t>(v); std::memcpy(&buf[at], &w, 2); };
  const uint32_t tag = IO_REPARSE_TAG_MOUNT_POINT;
  std::memcpy(&buf[0], &tag, 4);
  put16(4, buf.size() - 8);                              // ReparseDataLength
  put16(8, 0);                                           // SubstituteNameOffset
  put16(10, sub.size() * sizeof(wchar_t));               // SubstituteNameLength
  put16(12, (sub.size() + 1) * sizeof(wchar_t));         // PrintNameOffset
  put16(14, print.size() * sizeof(wchar_t));             // PrintNameLength
  std::memcpy(&buf[16], sub.c_str(), (sub.size() + 1) * sizeof(wchar_t));
  std::memcpy(&buf[16 + (sub.size() + 1) * sizeof(wchar_t)], print.c_str(), (print.size() + 1) * sizeof(wchar_t));
  DWORD n = 0;
  const bool ok = DeviceIoControl(h, FSCTL_SET_REPARSE_POINT, buf.data(), static_cast<DWORD>(buf.size()), nullptr, 0, &n, nullptr);
  CloseHandle(h);
  if (!ok) fs::remove(link, ec);
  return ok;
}

// SceShellCore's sandbox (lnc_application.cpp, 0x4668F0) mounts with nullfs, for each
// {privileged, path} of the table at 0x199C030 (/preinst/common, /preinst/priv, ...,
// /system_ex/common_ex, /system_ex/priv_ex, ..., /user/priv), the path at
// "/" + random word + "/" + basename(path) (0x416530); the privileged entries only for a
// privileged process, which ShellUI is. The host's word is <cache>/system_data, which holds the
// /system_data tree itself; of the firmware's directories it mounts /system_ex/priv_ex (the
// profile cache's default icons, /<word>/priv_ex/etc, read by Sce.Vsh.ProfileCache).
void mount_sandbox(const std::filesystem::path& word_dir) {
  Firmware* fw = host::ctx().firmware;
  if (!fw) return;
  const std::string target = fw->host_path("/system_ex/priv_ex");
  if (!mount_dir(word_dir / "priv_ex", target))
    log::warn("psm", "sandbox: mounting {} at {} failed", target, (word_dir / "priv_ex").string());
  host::note_unimplemented(
      "SceShellCore sandbox mounts (0x4668F0): of the firmware's directories only /system_ex/priv_ex is mounted "
      "(host junction)");
}

// GetString(ParameterKey, out string) — libScePsm 0x2d46f0.
//   key 0  : sceSystemServiceParamGetInt(SCE_SYSTEM_SERVICE_PARAM_ID_LANG=0x02020000);
//            on success with id <= 29 the tag from the table, otherwise "en-US".
//   key 6  : the sandbox random word (see below).
//   key 11 : the process name (not yet traced).
//   others : PSM_ERROR_INVALID_PARAMETER (0x80b30001); null out-pointer: 0x80b30002.
int32_t GetString(uint32_t key, MonoString** value) {
  if (!value) return kErrorArgumentNull;
  if (key == Language) {
    int32_t id = -1;
    const bool ok = regmgr::get_int(0x02020000, &id) == 0;
    const char* tag = (ok && id >= 0 && id <= 29) ? kLanguageTags[id] : "en-US";
    store_ref(value, new_string(tag));
    return 0;
  }
  if (key == FileSystemSandboxRandomWord) {
    // 0x2d46f0 key 6 returns the string at qword_53FC90, which the PSM initialiser
    // (0x2c9460, 0x2cdd2c) sets to strdup(sceKernelGetFsSandboxRandomWord()) when
    // stat("/adm") fails, i.e. the name of the per-process directory through which the
    // process sees its /system_data tree (the shell builds "/" + word + "/priv/..." paths).
    // HOST VALUE: the host process sees that tree in <cache>/system_data; the word is that
    // directory in the drive-less rooted form the shell's paths resolve with
    // (host::managed_path), without its leading '/'.
    static const std::string word = [] {
      const std::filesystem::path dir = std::filesystem::path(host::ctx().cache_dir) / "system_data";
      std::error_code ec;
      std::filesystem::create_directories(dir / "priv", ec);
      mount_sandbox(dir);
      std::string m = host::managed_path(dir);
      while (!m.empty() && m.front() == '/') m.erase(m.begin());
      return m;
    }();
    host::note_unimplemented("HOST VALUE file-system sandbox word: <cache>/system_data as '" + word + "'");
    store_ref(value, new_string(word.c_str()));
    return 0;
  }
  if (key == ProcessName) {
    host::note_unimplemented("libScePsm SystemParameters::GetString key " + std::to_string(key));
    store_ref(value, nullptr);
    return 0;
  }
  return kErrorInvalidParameter;
}
SSN_ICALL("Sce.PlayStation.Core.Environment.SystemParameters::GetString", GetString);

// GetLanguageByIndexNative(int index, out string) — 0x2d4810: null out-pointer 0x80b30002; the
// tag of the 0x4a22b0 table for index <= 29 (compared unsigned, so negative indices too fall
// through), otherwise "" (the empty string at 0x366e35); returns 0.
int32_t GetLanguageByIndexNative(uint32_t index, MonoString** value) {
  if (!value) return kErrorArgumentNull;
  store_ref(value, new_string(index <= 29 ? kLanguageTags[index] : ""));
  return 0;
}
SSN_ICALL("Sce.PlayStation.Core.Environment.SystemParameters::GetLanguageByIndexNative", GetLanguageByIndexNative);

// GetThreadIdNative() — 0x2957d0: a thunk to scePthreadGetthreadid (EI-5-jlq2dE), the calling
// thread's kernel thread id. The host's kernel thread id is the OS thread id, the id
// System.Threading.ExtendedMethods.GetNativeIdInternal also answers (runtime/thread_ext.cpp).
int32_t GetThreadIdNative() { return static_cast<int32_t>(GetCurrentThreadId()); }
SSN_ICALL("Sce.PlayStation.Core.Environment.SystemParameters::GetThreadIdNative", GetThreadIdNative);

// GetFloat(ParameterKey, out float) — libScePsm 0x2d4610. Null out-pointer: 0x80b30002.
//   key 21 (DisplaySafeArea): sceRegMgrGetInt(0x0A0D0000); when that succeeds with v > 0 the
//          value is clamp(v / 1920.0, 0.9, 1.0) (constants 0x3502c8, 0x350218, 0x35015c),
//          otherwise 1.0.
//   key 4 / 5 (DisplayDpiX / DisplayDpiY): "display.dpi.x" / "display.dpi.y" from the
//          module's configuration tree (0x17af90 over unk_4FBF78), else 100.0 (0x350428).
//   others : 0x80b30001.
int32_t GetFloat(uint32_t key, float* value) {
  if (!value) return kErrorArgumentNull;
  if (key == 21) {
    int32_t v = 0;
    float f = 1.0f;
    if (regmgr::get_int(0x0A0D0000, &v) == 0 && v > 0) f = std::min(1.0f, std::max(0.9f, static_cast<float>(v) / 1920.0f));
    *value = f;
    return 0;
  }
  if (key == 4 || key == 5) {
    // The configuration tree behind 0x17af90 is not traced; its lookup failing gives the
    // firmware's own fallback.
    host::note_unimplemented("libScePsm SystemParameters::GetFloat display.dpi configuration tree (0x17af90) not traced; 100.0");
    *value = 100.0f;
    return 0;
  }
  return kErrorInvalidParameter;
}
SSN_ICALL("Sce.PlayStation.Core.Environment.SystemParameters::GetFloat", GetFloat);

// Asset-manager sizes and shared-video-memory flags (SetInt 0x2d45a0 / GetInt keys 13..17):
// dword_53FC98 / 53FC9C / 53FCA0 (bss, 0 until set), byte_53FCA4 / 53FCA5 (PsmFramework
// initialisation 0x2cb46e: PsmInitParam +0x160 != 0 and +0x103; the host has no PsmInitParam
// from the eboot, so they start at 0).
int32_t g_system_asset_size = 0, g_high_reso_asset_size = 0, g_total_asset_size = 0;
bool g_shared_vm_system_asset = false, g_shared_vm_high_reso = false;

// GetInt(ParameterKey, out int) — 0x2d43e0. Null out-pointer 0x80b30002; unknown key
// 0x80b30001 (Language/DisplayDpi/… are not integer keys).
int32_t GetInt(uint32_t key, int32_t* value) {
  if (!value) return kErrorArgumentNull;
  switch (key) {
    case 1:  // LanguageNum
      *value = 30;
      return 0;
    case 2:  // GamePadButtonMeaning: dword_53FC88 == 2
      *value = input::pad_button_meaning() == 2;
      return 0;
    case 3:  // YesNoLayout
    case 8:  // IsSimulator
      *value = 0;
      return 0;
    case 7:   // IsInternalDevelopmentMode
    case 12:  // IsAllowLimitedUiInspector
      // Both start with a kernel development-mode query (import UtO0OHMCgmI) and are 0 unless
      // it answers 1. HOST VALUE: the host is a retail console.
      static const bool noted_ = (host::note_unimplemented("HOST VALUE libScePsm SystemParameters development-mode keys 7/12: retail console (0)"), true);
      (void)noted_;
      *value = 0;
      return 0;
    case 9: {  // IsSystemSoundKeytoneMute: sceRegMgrGetInt(0x0B040000) succeeds and != 0
      int32_t v = 0;
      *value = regmgr::get_int(0x0B040000, &v) == 0 && v != 0;
      return 0;
    }
    case 13: *value = g_system_asset_size; return 0;
    case 14: *value = g_high_reso_asset_size; return 0;
    case 15: *value = g_total_asset_size; return 0;
    case 16: *value = g_shared_vm_system_asset; return 0;
    case 17: *value = g_shared_vm_high_reso; return 0;
    case 18:  // SystemSoftwareVersion: sceKernelGetSystemSwVersion's version field, -1 when it fails
      host::note_unimplemented("libScePsm SystemParameters key 18: sceKernelGetSystemSwVersion not reproduced (the firmware's failure value -1)");
      *value = -1;
      return 0;
    case 19:  // IsSecureClockSynced: /dev/srtc; when it cannot be read the answer is 0
      host::note_unimplemented("libScePsm SystemParameters key 19: no /dev/srtc on the host (the firmware's failure value 0)");
      *value = 0;
      return 0;
    case 20:  // VolatileVideoMemorySize: the graphics context's +16 when below 0x7FFFFFFF, else 0
      host::note_unimplemented("libScePsm SystemParameters key 20: graphics context volatile video memory size (+16) not traced; 0");
      *value = 0;
      return 0;
    case 100:
      *value = 3;
      return 0;
    default:
      return kErrorInvalidParameter;
  }
}
SSN_ICALL("Sce.PlayStation.Core.Environment.SystemParameters::GetInt", GetInt);

// SetInt(ParameterKey, int) — 0x2d45a0: keys 13..15 (negative -> 0x80b30003), 16/17 (as bool);
// others 0x80b30001.
int32_t SetInt(uint32_t key, int32_t v) {
  switch (key) {
    case 13: if (v < 0) return kErrorOutOfRange; g_system_asset_size = v; return 0;
    case 14: if (v < 0) return kErrorOutOfRange; g_high_reso_asset_size = v; return 0;
    case 15: if (v < 0) return kErrorOutOfRange; g_total_asset_size = v; return 0;
    case 16: g_shared_vm_system_asset = v != 0; return 0;
    case 17: g_shared_vm_high_reso = v != 0; return 0;
    default: return kErrorInvalidParameter;
  }
}
SSN_ICALL("Sce.PlayStation.Core.Environment.SystemParameters::SetInt", SetInt);

// GetIntPair(ParameterKey, out int, out int) — 0x2d43b0: key 10 (RenderTargetResolution) ->
// dword_4FA728/2C (1920 x 1080 in the module's data, replaced at initialisation by the
// PsmInitParam's +0x50/+0x54; the host has no PsmInitParam and renders 1920 x 1080).
int32_t GetIntPair(uint32_t key, int32_t* a, int32_t* b) {
  if (!a || !b) return kErrorArgumentNull;
  if (key != 10) return kErrorInvalidParameter;
  *a = 1920;
  *b = 1080;
  return 0;
}
SSN_ICALL("Sce.PlayStation.Core.Environment.SystemParameters::GetIntPair", GetIntPair);

// AccessibilityParameters.GetIntNative(ParameterKey, out int) — 0x2d4ab0: registry keys;
// keys 1, 2, 5, 9 return the value (0 when the read fails), the others whether it is non-zero.
int32_t AccessibilityGetInt(uint32_t key, int32_t* value) {
  if (!value) return kErrorArgumentNull;
  static const uint32_t kKeys[14] = {0x09400100, 0x09400200, 0x09400300, 0x09400400, 0x09010000, 0x09080000, 0x09030000,
                                     0x09040000, 0x09050000, 0x09060000, 0x09C00100, 0x09C00200, 0x09C00300, 0x09C04000};
  if (key > 13) return kErrorInvalidParameter;
  int32_t v = 0;
  const bool ok = regmgr::get_int(kKeys[key], &v) == 0;
  if (key == 1 || key == 2 || key == 5 || key == 9) *value = ok ? v : 0;
  else *value = ok && v != 0;
  return 0;
}
SSN_ICALL("Sce.PlayStation.Core.Environment.AccessibilityParameters::GetIntNative", AccessibilityGetInt);

// SystemStatus.GetIntNative(StatusKey, out int) — 0x2d4bf0: sceSystemServiceGetStatus; key 0 ->
// status +5 (isInBackgroundExecution), key 1 -> +4 (isSystemUiOverlaid), each 0 when the call
// fails; other keys 0x80b30001, null 0x80b30002. HOST VALUE: the host shell process is the
// foreground system UI with no overlay, so both flags are 0.
int32_t SystemStatusGetInt(uint32_t key, int32_t* value) {
  if (!value) return kErrorArgumentNull;
  if (key > 1) return kErrorInvalidParameter;
  static const bool noted_ = (host::note_unimplemented("HOST VALUE libScePsm SystemStatus: not in background execution, no system UI overlay"), true);
  (void)noted_;
  *value = 0;
  return 0;
}
SSN_ICALL("Sce.PlayStation.Core.Environment.SystemStatus::GetIntNative", SystemStatusGetInt);

}  // namespace
}  // namespace ssn::psm
