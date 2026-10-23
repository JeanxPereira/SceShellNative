// SceShellCore AppDb daemon internals shared by app_db.cpp and app_db_promote.cpp (FW 4.03).
#pragma once
#include <cstdint>
#include <mutex>
#include <string>
#include <vector>

#include "shellcore/app_db_table.h"

namespace ssn::shellcore::appdb {

// The daemon's state: the DbAccessor (AppInfoDaemon +8), the recovery status
// (m_appinfo_recovery_status, +0: 256 = rebuild, 0x20/0x40/0x80 = schema/app version
// update flags), the stored appinfover (+4), and the promote singleton's sync counters
// (qword_1AF92F0 appinfo, qword_1AFEF58 conceptinfo).
struct Daemon {
  std::recursive_mutex mutex;  // the accessor's rmutex (0xB0B680)
  DbAccessor db;
  bool initialized = false;
  int32_t recovery_status = 0;
  int32_t stored_appinfover = 0;
  int64_t appinfo_sync_index = 0;
  int64_t conceptinfo_sync_index = 0;
};
Daemon& daemon();

// Console path -> host path for the trees the daemon reads and writes (host::console_host_path).
std::string host_path(const std::string& console_path);

// 0x225E40 AppInstallAll (promote event 1).
int32_t app_install_all();

// 0x232950 with a param.json (parsed by 0x10675E0), then the 0x243DD0 AppPromote and the
// 0xB6C700 InsertAppInfo it ends: one RNPS appdb directory or one /user/app directory title.
int32_t app_promote_json(const std::string& param_path, const std::string& metadata_dir, const std::string& app_path);

}  // namespace ssn::shellcore::appdb
