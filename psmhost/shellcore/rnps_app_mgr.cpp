// SceShellCore's RNPS app manager as ShellUI sees it (FW 4.03).
//
// The managed Sce.Vsh.RnpsAppMgrWrapper P/Invokes "libSceRnpsAppMgrWrapper"; the ShellUI
// eboot's {name, fn} table (0xcfec00..0xcfece0) points each entry at the import of the same
// name in libSceRnpsAppMgr.sprx, a thin IPMI client of the SceRnpsAppMgr service that runs
// inside SceShellCore (vsh/shell/shell_core/rnps_app_mgr/*.cpp).
//
// The service's app table (m_apps, rnps_app_mgr_app_info.cpp) holds one record per
// installed RNPS app. The install roots it knows (SceShellCore 0x5c7b00):
//   /system_ex/rnps/apps   (+48, preinstalled apps)
//   /system_ex/rnps/bgs    (+88, preinstalled backgrounds)
//   /mnt/rnps/apps, /mnt/rnps/bgft, /mnt/rnps (downloaded apps, ufs image
//   /system_data/nobackup/rnps_apps.img)
// The host's --rnps root stands in for /system_ex/rnps: <rnps>/apps/<titleId> is a
// preinstalled app. Paths handed to ShellUI are host paths (host::managed_path form), because the managed side opens
// <path>/manifest.json and <path>/main.jsbundle with System.IO directly.
#include <cstdint>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <mutex>
#include <set>
#include <string>

#include "common/managed_path.h"
#include "host/boundary.h"
#include "host/context.h"
#include "libSceUrlConfigResolver/sce_json.h"

namespace ssn::shellcore {
namespace {

constexpr int32_t kErrNotInitialized = static_cast<int32_t>(0x81860001u);  // SCE_RNPS_APP_MGR_ERROR_NOT_INITIALIZED
constexpr int32_t kErrInvalidArg = static_cast<int32_t>(0x81860003u);      // RNPSAM_CHECK failures
constexpr int32_t kErrNotFound = static_cast<int32_t>(0x81860008u);        // "itr != m_apps.end()"

// SceRnpsAppMgrAppInfo — 1227 bytes, the IPMI output size of method 0x30009
// (libSceRnpsAppMgr 0x950) and the managed RnpsAppMgrWrapper.AppInfo layout.
#pragma pack(push, 1)
struct AppInfo {
  char title_id[10];        // +0
  char version[64];         // +10
  uint8_t preinstalled;     // +74
  char path[128];           // +75
  char download_url[1024];  // +203
};
#pragma pack(pop)
static_assert(sizeof(AppInfo) == 1227);

// isValidTitleId (SceShellCore 0x5c7ce0, rnps_app_mgr_common.cpp:24..27): non-null,
// strlen == 9, four isupper() letters then five isdigit() digits.
bool is_valid_title_id(const char* t) {
  if (!t || std::strlen(t) != 9) return false;
  for (int i = 0; i < 4; ++i)
    if (t[i] < 'A' || t[i] > 'Z') return false;
  for (int i = 4; i < 9; ++i)
    if (t[i] < '0' || t[i] > '9') return false;
  return true;
}

// The record's version (SceShellCore 0x5bddd0): sce::Json parse of <path>/manifest.json,
// value of "applicationVersion" as a string; "" when the file does not parse.
std::string manifest_version(const std::filesystem::path& dir) {
  std::ifstream f(dir / "manifest.json", std::ios::binary);
  if (!f) return {};
  const std::string text((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
  urlcfg::json::Value v;
  if (urlcfg::json::parse(v, text.data(), text.size()) != 0) return {};
  return v.member("applicationVersion").string();
}

void copy_field(char* dst, const std::string& s, size_t n) {
  // SfQIZcqvvms = strncpy(dst, src, n)
  std::strncpy(dst, s.c_str(), n);
}

}  // namespace

// int sceRnpsAppMgrGetAppInfo(const char* titleId, SceRnpsAppMgrAppInfo* info)
//   client (libSceRnpsAppMgr 0x16a0, NID JgBgvkAFMwc): not initialised -> 0x81860001;
//   null titleId / info -> 0x81860003; else IPMI 0x30009 (titleId, strlen+1) -> 1227 bytes.
//   server (SceShellCore 0x5d63b0 sceRnpsAppMgrServiceGetAppInfo -> 0x5be840 GetAppInfo):
//   invalid titleId -> 0x81860003; not in m_apps -> 0x81860008; otherwise 0x5be4a0 copies
//   titleId (strncpy 10), version (64), preinstalled = path contains "/system_ex/rnps/apps"
//   or "/system_ex/rnps/bgs", path (128), downloadUrl (1024).
SSN_PINVOKE("libSceRnpsAppMgrWrapper", "sceRnpsAppMgrGetAppInfo", int32_t, ssn_RnpsAppMgr_GetAppInfo,
            (const char* title_id, void* out)) {
  auto* info = static_cast<AppInfo*>(out);
  if (host::ctx().rnps_root.empty()) return kErrNotInitialized;
  if (!title_id || !info) return kErrInvalidArg;
  if (!is_valid_title_id(title_id)) return kErrInvalidArg;
  const auto dir = std::filesystem::path(host::ctx().rnps_root) / "apps" / title_id;
  if (!std::filesystem::is_directory(dir)) return kErrNotFound;
  const std::string path = host::managed_path(dir);
  if (path.size() >= sizeof info->path) return kErrInvalidArg;
  copy_field(info->title_id, title_id, sizeof info->title_id);
  copy_field(info->version, manifest_version(dir), sizeof info->version);
  info->preinstalled = 1;  // the host root stands in for /system_ex/rnps/apps
  copy_field(info->path, path, sizeof info->path);
  // The record's downloadUrl (+168) comes from the service's app URL table
  // (rnps_app_mgr_app_urls.cpp), filled from the server-provided update URLs; not traced.
  host::note_unimplemented("SceShellCore RnpsAppMgr app record downloadUrl (app URL table) not traced; empty");
  copy_field(info->download_url, "", sizeof info->download_url);
  return 0;
}

namespace {

// The per-app blocker list (m_apps record +320, rnps_app_mgr_app_info.cpp). ShellUI is the only
// client on the host, so the list holds the blocked title ids (see BlockAppInstall below).
std::mutex g_block_lock;
std::set<std::string> g_blocked;

bool installed(const char* title_id) {
  return std::filesystem::is_directory(std::filesystem::path(host::ctx().rnps_root) / "apps" / title_id);
}

}  // namespace

// bool sceRnpsAppMgrIsCrlUpdated()
//   client (libSceRnpsAppMgr 0x20f0, NID GIdowi29+oY): not initialised (0x23E0) -> false;
//   otherwise IpmiClient::pollEventFlag(0, bit 1, OR | CLEAR_PAT) and true only when the poll
//   succeeds with bit 0 set. The server sets that client event-flag bit in
//   SetEventFlagForClients(1) (SceShellCore 0x5CC800), called only from DownloadCrl
//   (0x5E7AA0) after a CRL was downloaded over the network and installed ("CRL download :
//   installed"). The host has no network, so the bit is never set: false.
SSN_PINVOKE("libSceRnpsAppMgrWrapper", "sceRnpsAppMgrIsCrlUpdated", uint8_t, ssn_RnpsAppMgr_IsCrlUpdated, ()) {
  return 0;
}

// int sceRnpsAppMgrBlockAppInstall(const char* titleId) / sceRnpsAppMgrUnblockAppInstall
//   client (0x1a20 / 0x1ae0): not initialised -> 0x81860001; null -> 0x81860003; else IPMI
//   0x3000C / 0x3000D with the title id (0xB20 / 0xBA0), the server's result.
//   server (0x5D4820 / 0x5D4900): the caller must be a system process (else 0x81860004; ShellUI
//   is one); the caller's title id comes from SceLncService (0x47B320); then
//   sceRnpsAppMgrServiceBlockAppInstall (0x5D65E0 -> 0x5BAC00 -> 0x5C0FC0) /
//   ...Unblock... (0x5D6670 -> 0x5BAC10 -> 0x5C1420):
//     invalid title id -> 0x81860003; not in m_apps -> 0x81860008;
//     Block: blocker map[callerAppId] = callerTitleId, 0;
//     Unblock: entry of the caller erased when present (an app with no blocker left runs
//     0x5BD510), 0 either way.
//   The caller's app id (ShellCore's id of ShellUI, the map key) is not traced; with ShellUI
//   as the only caller the host keeps one entry per blocked title.
SSN_PINVOKE("libSceRnpsAppMgrWrapper", "sceRnpsAppMgrBlockAppInstall", int32_t, ssn_RnpsAppMgr_BlockAppInstall,
            (const char* title_id)) {
  if (host::ctx().rnps_root.empty()) return kErrNotInitialized;
  if (!title_id || !is_valid_title_id(title_id)) return kErrInvalidArg;
  if (!installed(title_id)) return kErrNotFound;
  std::lock_guard l(g_block_lock);
  g_blocked.insert(title_id);
  return 0;
}

SSN_PINVOKE("libSceRnpsAppMgrWrapper", "sceRnpsAppMgrUnblockAppInstall", int32_t, ssn_RnpsAppMgr_UnblockAppInstall,
            (const char* title_id)) {
  if (host::ctx().rnps_root.empty()) return kErrNotInitialized;
  if (!title_id || !is_valid_title_id(title_id)) return kErrInvalidArg;
  if (!installed(title_id)) return kErrNotFound;
  std::lock_guard l(g_block_lock);
  g_blocked.erase(title_id);
  return 0;
}

}  // namespace ssn::shellcore
