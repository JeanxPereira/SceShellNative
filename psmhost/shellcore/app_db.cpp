// SceShellCore AppDb daemon ("SceAppDbDaemonShellCore") and AppInfo server, FW 4.03.
// Addresses are SceShellCore.elf offsets. See app_db.h and docs/app-db.md.
#include "shellcore/app_db.h"

#include <cstdio>
#include <filesystem>
#include <mutex>

#include <sqlite3.h>

#include "core/log.h"
#include "shellcore/shellcore_build.h"
#include "host/boundary.h"
#include "host/context.h"
#include "libSceRegMgr/regmgr.h"
#include "shellcore/app_db_internal.h"

namespace ssn::shellcore::appdb {
namespace fs = std::filesystem;

namespace {

// 0xBAB580: the schema/application versions this build expects (0x1D9CCD01B6 =
// {appver 0x9CCD01B6, appinfover 0x1D}).
constexpr int32_t kAppVer = static_cast<int32_t>(0x9CCD01B6u);
constexpr int32_t kAppInfoVer = 29;
constexpr int32_t kStatusRebuild = 256;

// 0xB48DA0: "/system_data" + "/" + "priv/mms/appinfo.db".
constexpr const char* kAppInfoDbPath = "/system_data/priv/mms/appinfo.db";

bool g_client_initialized = false;
std::mutex g_init_mutex;

// 0xBBD4D0 runCheckVersionTransaction: the stored versions against 0xBAB580. The stored
// build_target must be the one this build writes: 0 (retail), 1 (devkit build, 0xBC9B30).
int32_t check_version(Daemon& d) {
  int32_t appver = -1, appinfover = -1, build_target = 0;
  const int32_t r = d.db.get_version(&appver, &appinfover, &build_target);  // 0xBDEBC0
  if (r != 0) {
    d.recovery_status = kStatusRebuild;
    return r;
  }
  if (appinfover <= 0 || build_target != (devkit_build() ? 1 : 0)) {
    d.recovery_status = kStatusRebuild;
    return 0;
  }
  if (appinfover > 25) {
    if (appinfover > kAppInfoVer) {
      d.recovery_status = kStatusRebuild;
      return 0;
    }
  } else if (appinfover != kAppInfoVer) {
    d.recovery_status = kStatusRebuild;
    return 0;
  }
  if (appinfover < kAppInfoVer || appver != kAppVer) {
    const int32_t app_changed = (appver != kAppVer) << 6;
    int32_t flags = (appinfover >= kAppInfoVer || appinfover <= 25) ? app_changed + 32 : (app_changed | 0xA0);
    d.recovery_status = flags;
    d.stored_appinfover = appinfover;
  }
  return 0;
}

// 0xB61130 / 0xB61180 and 0xB97D80 / 0xB97DD0: the sync counters.
void init_sync_counters(Daemon& d) {
  if (d.recovery_status == kStatusRebuild) {
    d.appinfo_sync_index = 0;
    d.conceptinfo_sync_index = 0;
    return;
  }
  int64_t stored = 0, rows = 0;
  bool found = false, rows_found = false;
  d.db.get_category("appinfo_sync_index", &stored, &found);  // 0xB61620
  d.db.max_sync_index(&rows, &rows_found);                    // 0xBCB7A0
  d.appinfo_sync_index = found && stored > 0 ? stored : 0;
  if (rows_found && rows > d.appinfo_sync_index) {
    d.db.set_category("appinfo_sync_index", rows);  // 0xB61960
    d.appinfo_sync_index = rows;
  }
  int64_t cstored = 0;
  bool cfound = false;
  d.db.get_category("conceptinfo_sync_index", &cstored, &cfound);  // 0xB983C0
  d.conceptinfo_sync_index = cfound && cstored > 0 ? cstored : 0;
  host::note_unimplemented("SceShellCore AppDb conceptinfo row sync index scan (0xBDAF00) not reproduced");
}

// The recovery of appinfo.db: 0x12268B0 Reset then 0x1226A50 Promote (DaemonServer).
int32_t recover_appinfo_db(Daemon& d) {
  const std::string path = appinfo_db_host_path();
  d.db.close();  // 0xBBCF00
  std::error_code ec;
  fs::remove(path, ec);  // sceKernelUnlink
  fs::remove(path + "-journal", ec);
  host::note_unimplemented("SceShellCore AppDb reset promote event 6 (0x23FFA0) not reproduced");
  // 0xBBCF20: create the database (open flags READWRITE|CREATE), tables, access index.
  int32_t r = d.db.init(path, SQLITE_OPEN_READWRITE | SQLITE_OPEN_CREATE);
  if (r != 0) return r;
  if ((r = d.db.begin()) != 0) return r;
  r = d.db.create_tables();
  if (r == 0) r = d.db.init_access_index();
  d.db.end(r);
  if (r != 0) return r;
  d.appinfo_sync_index = 0;
  d.conceptinfo_sync_index = 0;
  // 0xBBD1A0 RecoveryPromoteAppInfo(normal mode): DeleteCatalogAppMetaDir, promote event 1
  // (0x1B8220 -> 0x225E40 AppInstallAll), then SetValueTblVersion(0x1D9CCD01B6).
  host::note_unimplemented("SceShellCore AppDb DeleteCatalogAppMetaDir (catalog downloader job, 0xB9D950) not reproduced");
  r = app_install_all();
  if (r != 0) log::warn("appdb", "[AppDb][AppInfo] recoveryFunctionShellCore: sceAppPromoter::AppInstallAll() : ret=0x{:08x}", static_cast<uint32_t>(r));
  std::lock_guard lock(d.mutex);
  if ((r = d.db.begin()) != 0) return r;
  r = d.db.set_version(kAppVer, kAppInfoVer);  // 0xBC0420 -> 0xBDEBF0
  d.db.end(r);
  return r;
}

}  // namespace

Daemon& daemon() {
  static Daemon d;
  return d;
}

std::string appinfo_db_host_path() { return host_path(kAppInfoDbPath); }

std::string host_path(const std::string& console_path) {
  const auto& c = host::ctx();
  auto under = [&](const std::string& prefix, const fs::path& base) -> std::string {
    std::string rest = console_path.substr(prefix.size());
    while (!rest.empty() && rest.front() == '/') rest.erase(rest.begin());
    return (base / fs::path(rest)).string();
  };
  if (console_path.rfind("/system_data", 0) == 0) return under("/system_data", fs::path(c.cache_dir) / "system_data");
  if (console_path.rfind("/system_ex/rnps", 0) == 0 && !c.rnps_root.empty()) return under("/system_ex/rnps", fs::path(c.rnps_root));
  if (console_path.rfind("/user", 0) == 0) return under("/user", fs::path(c.cache_dir) / "user");
  if (c.firmware) return c.firmware->host_path(console_path);
  return console_path;
}

std::string localized_title_key() {
  // 0xB359A0: mPYKD12UDQI(0x02020000 = SCE_SYSTEM_SERVICE_PARAM_ID_LANG) -> 0xB35B20.
  int32_t lang = -1;
  regmgr::get_int(0x02020000, &lang);
  if (lang < 0 || lang > 29) {
    host::note_unimplemented("SceShellCore 0xB35B20 with a language outside the TITLE_xx table not traced; \"TITLE\"");
    return "TITLE";
  }
  char buf[16];
  std::snprintf(buf, sizeof buf, "TITLE_%02d", lang);
  return buf;
}

// 0x1B78E0 -> 0xBAAEF0 AppInfoDaemon::Init.
int32_t daemon_prepare() {
  std::lock_guard g(g_init_mutex);
  Daemon& d = daemon();
  if (d.initialized) return 0;
  std::error_code ec;
  // 0xB48920 + 0xB08060/0xB07F90: /system_data/priv/mms, mode 0700.
  fs::create_directories(host_path("/system_data/priv/mms"), ec);
  host::note_unimplemented("SceShellCore AppDb servers (SceAppDbServer 0xBA4190, SceAppInfoServer 0xBAE740, 0xBE25B0) served as a C++ API");
  host::note_unimplemented("SceShellCore AppDb SyncManagement / DbRecovery corruption check (0xB9BAF0, 0xBBD450) not reproduced");
  int32_t r = d.db.init(appinfo_db_host_path(), SQLITE_OPEN_READWRITE);  // 0xBDE170(path, 2)
  log::info("appdb", "[AppDb][AppInfo] Init()[159]: [Svr] DbAccessor::Init() : {:x}", static_cast<uint32_t>(r));
  if (r != 0) {
    d.recovery_status = kStatusRebuild;
  } else {
    std::lock_guard lock(d.mutex);
    d.db.begin();
    const int32_t v = check_version(d);
    d.db.end(0);
    if (v != 0)
      log::warn("appdb", "[AppDb][AppInfo] runCheckVersionTransaction() : {:x}, m_appinfo_recovery_status = {:x}",
                static_cast<uint32_t>(v), d.recovery_status);
  }
  init_sync_counters(d);
  // 0xBAB2B0 NotifyDbRecovery: status 256 reports the database as corrupted to DbRecovery
  // (0x125B2A0 -> dbr.db-err, 0xCAFE02..), whose recovery runs Reset (0x12268B0) and Promote
  // (0x1226A50). HOST POLICY: the DbRecovery service and the ShellUI's recovery screens are not
  // reproduced; the daemon runs that Reset + Promote itself here, so a missing or outdated
  // appinfo.db is rebuilt from the installed apps at start.
  if (d.recovery_status == kStatusRebuild) {
    host::note_unimplemented("HOST POLICY appinfo.db missing/invalid: DbRecovery Reset+Promote run directly by the AppDb daemon");
    r = recover_appinfo_db(d);
    if (r != 0) log::error("appdb", "appinfo.db rebuild failed: 0x{:08x}", static_cast<uint32_t>(r));
  } else if (d.recovery_status & 0x20) {
    host::note_unimplemented("SceShellCore AppDb schema/app version update (DbRecoveryReportSchemaUpdate, promote event 9) not reproduced");
  }
  // 0x1B78E0 tail: promote callback 0x1B6FF0, app-info hook 0x1B7120, then registers the
  // unit as a DbPreparation listener (0x7787E0) — the ShellCore DbPreparation server does it.
  d.initialized = r == 0;
  return r;
}

int32_t daemon_terminate() {
  // 0x1B7940.
  std::lock_guard g(g_init_mutex);
  Daemon& d = daemon();
  std::lock_guard lock(d.mutex);
  d.db.close();
  d.initialized = false;
  return 0;
}

bool daemon_initialized() { return daemon().initialized; }
int32_t daemon_recovery_status() { return daemon().recovery_status; }

int32_t ipc_client_initialize() {
  // HOST POLICY: ShellCore's services start with the launcher; the client's
  // connectWithWaitServer returns once the daemon serves.
  return daemon_prepare() == 0 ? 0 : kErrNotInitialized;
}

int32_t app_info_client_initialize() {
  // 0x2740C0: a second Initialize is 0x81F40078.
  std::lock_guard g(g_init_mutex);
  if (g_client_initialized) return kErrAlreadyInitialized;
  g_client_initialized = true;
  return 0;
}

int32_t get_app_info_value_list(const std::string& title_id, const std::string& metadata_id,
                                const std::vector<std::string>& keys, int32_t app_type, AppInfo* out) {
  if (app_type == 1) {
    host::note_unimplemented("SceShellCore AppInfoServer GetConceptInfoValueList (0xB49750) not reproduced");
    return kErrUnsupported;
  }
  if (app_type != 0) {
    log::warn("appdb", "[AppDb][AppInfo] AppDbIpcGetAppInfoValueList({}) nonsupportAppTpye {:#x}.", title_id, app_type);
    return kErrUnsupported;
  }
  Daemon& d = daemon();
  if (!d.initialized) return kErrNoAccessor;
  std::lock_guard lock(d.mutex);
  const int32_t r = d.db.get_app_info({title_id, metadata_id}, keys, out);  // 0xB6BD20 -> 0xBC4030
  if (r != 0 && r != kErrNotFound)
    log::warn("appdb", "[AppDb][AppInfo] AppInfoServer GetAppInfoValueList({}) returned {:#x}.", title_id, static_cast<uint32_t>(r));
  return r;
}

int32_t search(const ConditionChain& conditions, const std::vector<SortField>& sort, int32_t search_mode,
               int32_t result_type, std::vector<std::string>* title_ids, std::vector<PrimaryKey>* pairs) {
  Daemon& d = daemon();
  if (search_mode == 1) {
    // 0xB49870: the conceptinfo search (0xB81EC0 -> 0xBD5D40); ids starting with 'x' (the
    // delete marks) are left out.
    if (!d.initialized) return kErrNoAccessor;
    std::vector<std::string> ids;
    {
      std::lock_guard lock(d.mutex);
      const int32_t r = d.db.search_concepts(conditions, sort, &ids);
      if (r != 0) return r;
    }
    if (title_ids) {
      title_ids->clear();
      for (auto& id : ids)
        if (id.empty() || id[0] != 'x') title_ids->push_back(std::move(id));
    }
    return 0;
  }
  if (search_mode == 2 || search_mode == 3) {
    // 0xB3E420 / 0xB49B40: the deleted titles / concepts (delete marks).
    host::note_unimplemented("SceShellCore AppInfoServer deleted-list searches (modes 2/3: 0xB3E420, 0xB49B40) not reproduced; empty");
    if (title_ids) title_ids->clear();
    if (pairs) pairs->clear();
    return 0;
  }
  if (search_mode != 0) return kErrUnsupported;
  if (result_type != 0 && result_type != 1) return kErrUnsupported;
  if (!d.initialized) return kErrNoAccessor;
  std::vector<PrimaryKey> rows;
  {
    std::lock_guard lock(d.mutex);
    const int32_t r = d.db.search_primary_keys(conditions, sort, &rows);  // 0xB7E600 -> 0xBCBC30
    if (r != 0) return r;
  }
  if (result_type == 1) {
    if (pairs) *pairs = rows;  // 0xB3E2D0
    return 0;
  }
  if (title_ids) {
    title_ids->clear();
    for (const auto& pk : rows)  // 0xB3E1D0: the prioritised rows' title ids
      if (pk.metadata_id.find("prior") != std::string::npos) title_ids->push_back(pk.title_id);
  }
  return 0;
}

void sync_indices(int64_t* appinfo, int64_t* conceptinfo) {
  Daemon& d = daemon();
  std::lock_guard lock(d.mutex);
  *appinfo = d.appinfo_sync_index;
  *conceptinfo = d.conceptinfo_sync_index;
}

int32_t get_concept_info(const std::string& local_concept_id, ConceptInfo* out) {
  // ConceptInfo value list (the conceptinfo twin of GetAppInfoValueList; 0xBD50D0).
  Daemon& d = daemon();
  if (!d.initialized) return kErrNoAccessor;
  std::lock_guard lock(d.mutex);
  return d.db.get_concept_info(local_concept_id, out);
}

int32_t update_app_info(const AppInfo& info) {
  Daemon& d = daemon();
  if (!d.initialized) return kErrNoAccessor;
  host::note_unimplemented("SceShellCore AppDb UpdateAppInfo derived-key/concept steps of 0xB7B320 not reproduced");
  std::lock_guard lock(d.mutex);
  int32_t r = d.db.begin();
  if (r != 0) return r;
  r = d.db.insert_app_info({info.title_id, info.metadata_id}, info, true);  // 0xBCA240 -> 0xBC6BD0(replace)
  d.db.end(r);
  return r;
}

int32_t purge_deleted_list(const std::string& title_id, uint32_t flags) {
  (void)title_id;
  (void)flags;
  host::note_unimplemented("SceShellCore AppInfoServer PurgeDeletedList not reproduced");
  return 0;
}

}  // namespace ssn::shellcore::appdb
