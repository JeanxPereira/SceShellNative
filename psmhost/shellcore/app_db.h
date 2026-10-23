// SceShellCore AppDb daemon and AppInfo server (FW 4.03, "SceAppDbDaemonShellCore"):
// appinfo.db (/system_data/priv/mms/appinfo.db) and the answers its IPC servers give the
// ShellUI eboot's AppDb client (application_info::IpcClientStub: GetAppInfoJson,
// GetTitleListJson, UpdateAppInfo, PurgeDeletedList; appDbIpc). Served here as a C++ API for
// the eboot's AppBrowse; the JSON/IPMI transport (3584-byte chunks) is not reproduced — the
// API passes the structures the server parses out of / serialises into that JSON.
//
// See docs/app-db.md.
#pragma once
#include <cstdint>
#include <string>
#include <vector>

#include "shellcore/app_db_table.h"

namespace ssn::shellcore::appdb {

// The ShellCore DbPreparation "app.db" unit (impl 0x1A14FE8, prepare 0x1B78E0):
// AppInfoDaemon::Init (0xBAAEF0) and the promote callback registration. Idempotent.
int32_t daemon_prepare();
// 0x1B7940: the unit's terminate.
int32_t daemon_terminate();
bool daemon_initialized();
// m_appinfo_recovery_status after Init (256 = database rebuilt by the host policy).
int32_t daemon_recovery_status();

// appDbIpcClientInitialize (eboot 0x30EBB0) / AppInfoClient::Initialize (eboot 0x2740C0):
// the client side connects to "SceAppDbIpc..." / "SceAppInfoServer" (connectWithWaitServer,
// 10 s). Returns 0 once the daemon serves, kErrAlreadyInitialized for a second
// AppInfoClient::Initialize, as 0x2740C0 does.
int32_t ipc_client_initialize();
int32_t app_info_client_initialize();

// AppDbIpcGetAppInfoValueList (server 0xBAF2A0 -> 0xBB28C0 -> 0xB3D7D0): the values of
// `keys` (all keys when empty) stored for (title id, metadata id). app_type 0 = application
// (1 = concept info, 0xB49750, not reproduced; anything else kErrUnsupported).
int32_t get_app_info_value_list(const std::string& title_id, const std::string& metadata_id,
                                const std::vector<std::string>& keys, int32_t app_type, AppInfo* out);

// AppDbIpcGetTitleListJson / AppDbIpcSearch (server 0xBAFEE0): search_mode 0 = applications
// (0xB3E1D0 / 0xB3E2D0 over 0xBCBC30); result_type 0 = title ids of the prioritised rows,
// 1 = (title id, metadata id) pairs. Modes 1..3 (0xB49870, 0xB3E420, 0xB49B40) are not
// reproduced (kErrUnsupported).
int32_t search(const ConditionChain& conditions, const std::vector<SortField>& sort, int32_t search_mode,
               int32_t result_type, std::vector<std::string>* title_ids, std::vector<PrimaryKey>* pairs);

// AppDbIpcUpdateAppInfo (server 0xBAE8B0 -> 0xB3DA70 mode 0 -> 0xB79B40 -> 0xB7B320): stores
// the AppInfo's keys over the rows of (info.title_id, info.metadata_id) with REPLACE
// (0xBCA240 -> 0xBC6BD0). The derived-key and concept steps of 0xB7B320 are not reproduced.
int32_t update_app_info(const AppInfo& info);

// The current appinfo / conceptinfo sync indices (the values SyncManagement clients read,
// eboot 0x276CF0(0 / 1)).
void sync_indices(int64_t* appinfo, int64_t* conceptinfo);

// The concept's key/value rows of tbl_conceptinfo (0xBD50D0); kErrNotFound when none.
int32_t get_concept_info(const std::string& local_concept_id, ConceptInfo* out);

// PurgeDeletedList (server side of eboot 0x2775B0): not reproduced (logged; returns 0).
int32_t purge_deleted_list(const std::string& title_id, uint32_t flags);

// The host path of appinfo.db (<cache>/system_data/priv/mms/appinfo.db).
std::string appinfo_db_host_path();

}  // namespace ssn::shellcore::appdb
