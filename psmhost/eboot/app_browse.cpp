// ShellUI eboot AppBrowse: the DbPreparation unit "app.db" and the app.db client start
// (FW 4.03).
//
//   static init 0xC80   impl EBA448 (vtable 0xCC42F0) into the unit handle EBF1A8, listener
//                       EBA478 (vtable 0xCC4320: +16 0x640 DbInitialized, +24 0x860)
//   prepare 0x520       InitClient 0x30E230 (700 ms IPC timeout), 0x30E660, 0x30E5A0,
//                       DeleteInvalidUser callback 0x33B00(0xB20), listener on list 0 (0x35130)
//   terminate 0x5E0     listener removed (0x35150)
//   DbInitialized 0x640 InitializeAfterDbRecovery 0x299050 (0x30E6C0), Enable_AppDb 0x30E800
//                       (SyncManagement event-flag thread 0x2DC7B0: flags 18 at start ->
//                       0x30E700 concept and appinfo sync), catalog downloader job
//
// app.db recovery (DbRecovery app.db module 0x6514E0: /system_data/priv/mms/app.db):
//   reset   0x3463C0 RecoveryAppDbReset: unlink, DbAccessor::Init(path, 6, {lang, 114},
//           schema 0x347810), table caches
//   promote 0x346520: conceptinfo promote 0x331BD0(0, 0) then appinfo promote 0x312140(0, 0)
#include "eboot/app_browse.h"

#include <filesystem>
#include <mutex>

#include <sqlite3.h>

#include "core/log.h"
#include "eboot/db_preparation.h"
#include "host/boundary.h"
#include "shellcore/app_db.h"
#include "shellcore/user_service.h"

namespace ssn::eboot::appbrowse {
namespace fs = std::filesystem;
namespace appdb = ssn::shellcore::appdb;

int32_t promote_appinfo(int64_t from_index, int64_t to_index);  // app_browse_content.cpp

namespace {

// Version-check result bits of DbAccessor::Init (0x33EAA0): 0x40 no version, 2 newer
// appbrowsever, 0x10 older, 1 other locale, 0x100 open failed (corrupt/missing).
// 0x33B060: a result that asks for DbRecovery.
bool needs_recovery(uint32_t flags) { return (flags & 0xFFFFF9CEu) != 0; }

// 0x33E4C0 openDb: dbutil::Connection::Open(path, flags), PRAGMAs (0x33FB50), cache size.
int32_t open_db(int flags) {
  Db& d = db();
  const std::string path = app_db_host_path();
  sqlite3* c = nullptr;
  const int sflags = (flags & 4) ? SQLITE_OPEN_READWRITE | SQLITE_OPEN_CREATE : SQLITE_OPEN_READWRITE;
  int rc = sqlite3_open_v2(path.c_str(), &c, sflags | SQLITE_OPEN_FULLMUTEX, nullptr);
  log::info("appbrowse", "[AppDb][AppBrowse] openDb(): [File:Open] openDb() call dbutil::Connection::Open() flags[0x{:X}] result[0x{:X}]",
            flags, rc);
  if (rc != SQLITE_OK) {
    if (c) sqlite3_close(c);
    // SQLITE_CANTOPEN / CORRUPT / NOTADB / EMPTY / FORMAT / IOERR (bit set loc_14053 on
    // rc + 0x7F63F6F6) -> 0x80F50061.
    return kErrCantOpen;
  }
  d.conn = c;
  sqlite3_busy_timeout(c, 30000);  // 0x3ABBA0
  exec("PRAGMA foreign_keys=ON");
  exec("PRAGMA recursive_triggers=ON");
  // "PRAGMA page_size;" -> "PRAGMA cache_size = <0x40000 / page_size>;"
  sqlite3_stmt* st = nullptr;
  int page = 0;
  if (sqlite3_prepare_v2(c, "PRAGMA page_size;", -1, &st, nullptr) == SQLITE_OK && sqlite3_step(st) == SQLITE_ROW)
    page = sqlite3_column_int(st, 0);
  if (st) sqlite3_finalize(st);
  if (page != 0) exec("PRAGMA cache_size = " + std::to_string(0x40000 / page) + ";");
  // A database that is not one (empty file aside) fails here like the console's open.
  if (sqlite3_exec(c, "SELECT count(*) FROM sqlite_master", nullptr, nullptr, nullptr) != SQLITE_OK) {
    sqlite3_close(c);
    d.conn = nullptr;
    return kErrCantOpen;
  }
  return 0;
}

void close_db() {
  Db& d = db();
  if (d.conn) sqlite3_close(d.conn);
  d.conn = nullptr;
}

// 0x33F540: tbl_version 'locale' and 'appbrowsever' (-1 when absent).
void read_version(int64_t* locale, int64_t* appbrowsever) {
  *locale = -1;
  *appbrowsever = -1;
  Record r;
  if (select_record("sqlite_master", "name", "tbl_version", &r) != 0) return;
  get_version_value("locale", locale);
  get_version_value("appbrowsever", appbrowsever);
}

// 0x33EAA0 runCheckVersionTransaction.
uint32_t check_version() {
  int64_t locale = -1, ver = -1;
  read_version(&locale, &ver);
  uint32_t flags = 0;
  if (ver <= 0) flags = 64;
  else if (kAppBrowseVersion < ver) flags = 2;
  else if (kAppBrowseVersion > ver) flags |= 0x10;
  else if (system_language() != locale) flags |= 1;
  return flags;
}

// The table caches the AppBrowse builders load (0x277940, 0x28DC70 / 0x2842A0, 0x261990 /
// 0x259EF0): the users that have an iconinfo / concepticoninfo table.
void load_table_lists() {
  Db& d = db();
  d.icon_tables.clear();
  d.concept_icon_tables.clear();
  std::vector<std::string> names;
  select_keys("sqlite_master", "tbl_name", "type='table' and name like '%tbl_iconinfo%'", &names);
  for (const auto& n : names)
    if (n.size() > 13) d.icon_tables.push_back(std::stoi(n.substr(13)));  // "tbl_iconinfo_" + %010d
  select_keys("sqlite_master", "tbl_name", "type='table' and name like '%tbl_concepticoninfo%'", &names);
  for (const auto& n : names)
    if (n.size() > 20) d.concept_icon_tables.push_back(std::stoi(n.substr(20)));
}

// 0x3463C0 RecoveryAppDbReset (+ 0x33EB90 runCreateSchemaTransaction).
int32_t recovery_reset() {
  close_db();
  std::error_code ec;
  fs::remove(app_db_host_path(), ec);  // 0x33DE00
  fs::create_directories(fs::path(app_db_host_path()).parent_path(), ec);
  int32_t r = open_db(6);
  if (r != 0) return r;
  // 0x347810: the table lists (none yet), then 0x304CD0 (create_schema); 0x33F0A0 writes
  // appbrowsever then locale; 0x342FF0 bumps access_index.
  load_table_lists();
  if ((r = create_schema()) != 0) return r;
  set_version_value("appbrowsever", kAppBrowseVersion);
  set_version_value("locale", system_language());
  int64_t ai = 0;
  get_version_value("access_index", &ai);
  set_version_value("access_index", ai + 1);
  load_table_lists();
  log::info("appbrowse", "[AppDb][AppBrowse] RecoveryAppDbReset(): [Cnt] Reset DbAccessor::Init() : 0");
  return 0;
}

// 0x346520 RecoveryAppDbPromote.
int32_t recovery_promote() {
  int64_t sc_app = 0, sc_concept = 0;
  appdb::sync_indices(&sc_app, &sc_concept);  // 0x2DCCD0 / 0x2DCCF0
  int32_t r = sync_concepts(0, sc_concept);  // 0x331BD0(0, 0)
  if (r != 0) log::warn("appbrowse", "conceptinfo promote: 0x{:08x}", static_cast<uint32_t>(r));
  r = promote_appinfo(0, sc_app);  // 0x312140(0, 0)
  if (r != 0) log::warn("appbrowse", "appinfo promote: 0x{:08x}", static_cast<uint32_t>(r));
  return r;
}

// 0x34A780(manager, userId, 0, 0) -> 0x34A7D0: a user SceShellCore created gets its tables.
// Under the manager lock, with a connection (else 0x80F50003): 0x3064C0 tbl_addon_ / tbl_info_
// (inside BeginTransaction 0x33ED80 / EndTransaction 0x33EEF0, committed or rolled back by
// its result), then — whatever that result — 0x2AA740: the
// ReflectContentInfoToIconInfoTransactionCommand for the user (vtable 0xCDC748, execute
// 0x3005E0): tbl_iconinfo_ (0x28DC90) and tbl_concepticoninfo_ (0x2619B0), then every
// tbl_contentinfo title (0x2C2290) through 0x2C24A0, stopping at the first error. The fourth
// argument is 0, so 0x34A880 (the entitlement tables) does not run.
int32_t add_user(int32_t user_id) {
  Db& d = db();
  std::lock_guard lock(d.mutex);
  if (!d.conn) return kErrNotInitialized;
  int32_t r = create_user_info_tables(user_id);
  if (r != 0)
    log::warn("appbrowse", "[AppDb][AppBrowse] user 0x{:x}: tbl_addon_/tbl_info_ 0x{:08x}", static_cast<uint32_t>(user_id),
              static_cast<uint32_t>(r));
  if ((r = create_icon_table(user_id)) < 0) return r;
  if ((r = create_concept_icon_table(user_id)) < 0) return r;
  std::vector<std::string> titles;
  r = select_keys("tbl_contentinfo", "titleId", "", &titles);
  if (r != 0 && r != kErrNotFound) return r;
  for (const auto& tid : titles)
    if ((r = reflect_content_to_user(user_id, tid)) != 0) return r;
  host::note_unimplemented(
      "eboot AppBrowse SetInitOrderTransactionCommand for a created user (0x3005E0 tail: 0x300AB0, 0x2AC900, 0x2FB1E0) not reproduced");
  return 0;
}

// 0x30E550: the sceUserServiceRegisterEventCallback callback InitClient registers. Event type
// 256 (user created; SceShellCore 0x6FB800 -> 0x6FC0F0 at 0x6FBDBB) -> 0x34A780; type 257 (user
// destroyed; 0x6FC360 -> 0x6FC0F0 at 0x6FCA97) -> 0x34B300; other types are ignored.
void on_user_event(const int32_t* ev, void*) {
  if (ev[0] == 256) {
    const int32_t r = add_user(ev[1]);
    log::info("appbrowse", "[AppDb][AppBrowse] user 0x{:x} created: tables and icon rows 0x{:08x}", static_cast<uint32_t>(ev[1]),
              static_cast<uint32_t>(r));
  } else if (ev[0] == 257) {
    host::note_unimplemented("eboot AppBrowse removal of a deleted user's tables (0x34B300 / 0x3070B0) not reproduced");
  }
}

// nQ5AHlyJghQ (the kiosk/"Kratos" build test): HOST VALUE retail unit (as in
// db_preparation.cpp), so InitClient registers the user event callback.
constexpr bool kKratos = false;

// 0x30E230 InitClient.
int32_t init_client() {
  Db& d = db();
  std::lock_guard lock(d.mutex);
  int32_t r = appdb::ipc_client_initialize();  // 0x30EBB0
  if (r != 0) {
    log::error("appbrowse", "[AppDb][AppBrowse] InitClient()[132]: [Cnt][Er] appDbIpcClientInitialize() : {:x}", static_cast<uint32_t>(r));
    return r;
  }
  r = appdb::app_info_client_initialize();  // 0x2740C0
  if (r != 0 && r != static_cast<int32_t>(0x81F40078u))
    log::error("appbrowse", "[AppDb][AppBrowse] InitClient()[135]: [Cnt][Er] AppInfoClient Initialize() : {:x}", static_cast<uint32_t>(r));
  // 0x33E210 DbAccessor::Init(path, 2, {lang, 114}, no schema): open, version check.
  host::note_unimplemented("eboot DbRecoveryCheckCorruptionReport for app.db (0x57F3D0) not reproduced");
  uint32_t flags = 0;
  r = open_db(2);
  if (r == kErrCantOpen) {
    flags |= 0x100;
    r = static_cast<int32_t>(0x80F50000u);  // no schema callback: Init fails
  } else if (r == 0) {
    flags = check_version();
    if ((flags & 0x40) != 0 || (flags & 0x12) == 2) {
      close_db();
      r = static_cast<int32_t>(0x80F50000u);
    }
  }
  log::info("appbrowse", "[AppDb][AppBrowse] InitClient()[155]: [Cnt] InitClient DbAccessor::Init() : ret={:x}, ver_check_result=0x{:x}",
            static_cast<uint32_t>(r), flags);
  if (r == 0) {
    load_table_lists();
    get_version_value("appinfo_sync_index", &d.appinfo_sync_index);
    get_version_value("conceptinfo_sync_index", &d.conceptinfo_sync_index);
    // 0x30E4C0 CheckDbSyncIndex: app.db ahead of SceShellCore -> 0x100.
    int64_t sc_app = 0, sc_concept = 0;
    appdb::sync_indices(&sc_app, &sc_concept);
    if (!needs_recovery(flags) && (sc_app < d.appinfo_sync_index || sc_concept < d.conceptinfo_sync_index)) {
      log::warn("appbrowse", "[AppDb][AppBrowse] CheckDbSyncIndex()[258]: [currentSyncIndexFromAppInfo:currentAppInfoSyncIndex]=[{}:{}]  "
                "[currentSyncIndexFromConceptInfo:currentConceptInfoSyncIndex]=[{}:{}]",
                sc_app, d.appinfo_sync_index, sc_concept, d.conceptinfo_sync_index);
      flags |= 0x100;
    }
    if ((flags & 1) != 0)
      host::note_unimplemented("eboot AppBrowse locale change refresh (0x30E5C0 SceAppDbLocaleChanged) not reproduced");
  }
  if (r != 0 || needs_recovery(flags)) {
    // HOST POLICY: the console reports app.db to DbRecovery (0x57FEE0 "Daemon::InitClient()
    // error" / its check items) and its recovery service runs RecoveryAppDbReset +
    // RecoveryAppDbPromote; the host runs those two directly.
    host::note_unimplemented("HOST POLICY app.db missing/invalid: DbRecovery RecoveryAppDbReset + RecoveryAppDbPromote run directly");
    if ((r = recovery_reset()) != 0) {
      log::error("appbrowse", "[AppDb][AppBrowse] RecoveryAppDbReset: 0x{:08x}", static_cast<uint32_t>(r));
      return r;
    }
    d.appinfo_sync_index = 0;
    d.conceptinfo_sync_index = 0;
    recovery_promote();
  }
  host::note_unimplemented("eboot AppBrowse InitClient callback registration (0x51FE30(0x2A5F60)) not reproduced");
  // 0x30E49B: on a retail unit (nQ5AHlyJghQ <= 0), sceUserServiceRegisterEventCallback
  // (wuI7c7UNk0A) with 0x30E550 and the client. The console does this at the end of a
  // successful Init; after the host policy above the database is in the state the console's
  // recovery leaves it in, so the host registers in both cases.
  if (!kKratos) {
    const int32_t e = shellcore::users::register_event_callback(&on_user_event, nullptr);
    if (e != 0) log::error("appbrowse", "[AppDb][AppBrowse] InitClient: sceUserServiceRegisterEventCallback 0x{:08x}", static_cast<uint32_t>(e));
  }
  return 0;
}

// 0x30E700 sync callback (event-flag bits 18 at Enable_AppDb): conceptinfo then appinfo.
void sync_now() {
  Db& d = db();
  int64_t sc_app = 0, sc_concept = 0;
  appdb::sync_indices(&sc_app, &sc_concept);
  int64_t from_concept, from_app;
  {
    std::lock_guard lock(d.mutex);
    from_concept = d.conceptinfo_sync_index;
    from_app = d.appinfo_sync_index;
  }
  if (sc_concept != from_concept) sync_concepts(from_concept, sc_concept);  // 0x331BD0(1, index)
  if (sc_app != from_app) {
    promote_appinfo(from_app, sc_app);  // 0x312140(1, index)
    host::note_unimplemented("eboot AppBrowse GoWestByInitOrderTransactionCommand (0x327E80) after a sync not reproduced");
  }
}

class AppDbListener final : public dbp::Listener {
 public:
  // 0x640 DbInitialized.
  void on_initialized(const dbp::Event& ev) override {
    (void)ev;
    if (!db().conn) {
      log::error("appbrowse", "[AppDb][AppBrowse] DbInitialized()[114]: AppDb has not Initialized.");
      return;
    }
    // 0x299050 InitializeAfterDbRecovery -> 0x30E6C0: the table caches again.
    {
      std::lock_guard lock(db().mutex);
      load_table_lists();
    }
    // 0x30E800 Enable_AppDb: SyncManagement thread; its first wait returns flags 18.
    host::note_unimplemented("eboot AppBrowse SyncManagement event-flag thread (0x2DC7B0, SceShellCore sync events) not reproduced; one sync at DbInitialized");
    sync_now();
    host::note_unimplemented("eboot AppBrowse catalog downloader job and periodic job (0x73BE80, 0x7F2EF0) not reproduced");
  }
  void on_terminating(const dbp::Event& ev) override { (void)ev; }  // 0x860
};
AppDbListener g_listener;

// 0xB20: an invalid (deleted) user -> 0x30E550(257, uid) -> 0x34B300: its tables dropped.
void on_invalid_user(int32_t user_id) {
  host::note_unimplemented("eboot AppBrowse removal of a deleted user's tables (0x34B300 / 0x3070B0) not reproduced");
  (void)user_id;
}

class AppDbUnit final : public dbp::UnitImpl {
 public:
  // 0x520
  int32_t prepare(const dbp::Event& ev) override {
    (void)ev;
    // sceIpmiClient timeout 700 ms around InitClient (aI_OeCz8xrQ / W0Hpm2X0uPE): IPC only.
    init_client();
    // 0x30E660: manager mode 3 (0x301D60(1)), notifier object (0x301670).
    // 0x30E5A0: system event 37 (language change) -> 0x30E5C0.
    host::note_unimplemented("eboot AppBrowse language-change system event (0x30E5A0, event 37) not reproduced");
    const int32_t r = dbp::register_invalid_user_callback(on_invalid_user);  // 0x33B00(0xB20)
    if (r == 0) dbp::register_listener(&g_listener, 0);                     // 0x35130(a1 + 48, 0)
    return r;
  }
  // 0x5E0
  int32_t terminate(const dbp::Event& ev) override {
    (void)ev;
    dbp::unregister_listener(&g_listener);  // 0x35150
    return 0;
  }
};
AppDbUnit g_unit;

// 0xC80 static initialiser.
const bool g_registered = [] {
  dbp::register_unit_impl(dbp::UnitSlot::AppDbCex, &g_unit);
  return true;
}();

}  // namespace

// Sce.Vsh.Accessor.Db.AppBrowseAccessorWrapper::GetAppBrowseDbRevisionNative(out long) —
// eboot 0x299070: tbl_version 'table_access_index' (0x2991C0); absent -> 0, result 0.
SSN_PINVOKE("libSceVshAppDbWrapper.a", "GetAppBrowseDbRevisionNative", int32_t, ssn_AppDbWrapper_GetAppBrowseDbRevisionNative,
            (int64_t * revision)) {
  if (!revision) return kErrInvalidArg;
  Db& d = db();
  std::lock_guard lock(d.mutex);
  *revision = 0;
  int64_t v = 0;
  const int32_t r = get_version_value("table_access_index", &v);
  if (r == kErrNotFound) return 0;
  if (r == 0) *revision = v;
  return r;
}
}  // namespace ssn::eboot::appbrowse
