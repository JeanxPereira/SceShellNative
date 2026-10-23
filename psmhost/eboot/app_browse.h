// ShellUI eboot AppBrowse (FW 4.03, libSceVshAppDbWrapper inside eboot.bin): the per-console
// app.db the shell's managed accessors read (Sce.Vsh.Accessor.Db AppBrowseItemAccessor /
// ConceptBrowseItemAccessor, "app.db"), mirrored from SceShellCore's AppInfo store.
//
// Addresses are eboot.bin offsets. See docs/app-db.md.
#pragma once
#include <cstdint>
#include <map>
#include <mutex>
#include <string>
#include <vector>

#include "shellcore/app_db.h"

struct sqlite3;

namespace ssn::eboot::appbrowse {

using ssn::shellcore::appdb::AppInfo;
using ssn::shellcore::appdb::Value;

// AppBrowse error codes (SCE_APPDB 0x80F5xxxx as the eboot spells them).
constexpr int32_t kErrInvalidArg = static_cast<int32_t>(0x80F50001u);
constexpr int32_t kErrNoMemory = static_cast<int32_t>(0x80F50002u);
constexpr int32_t kErrNotInitialized = static_cast<int32_t>(0x80F50003u);
constexpr int32_t kErrNotFound = static_cast<int32_t>(0x80F50005u);
constexpr int32_t kErrInternal = static_cast<int32_t>(0x80F50000u);
constexpr int32_t kErrCantOpen = static_cast<int32_t>(0x80F50061u);  // -2131427231
constexpr int32_t kErrSyncIndex = static_cast<int32_t>(0x80F50081u);  // CheckDbSyncIndex 0x30E4C0

// app.db version (InitClient 0x30E230 / RecoveryAppDbReset 0x3463C0): {system language
// (regmgr 0x02020000, 0x345A50), appbrowsever 114}.
constexpr int32_t kAppBrowseVersion = 114;

// A record field: absent, integer (type 0) or string (type 2) — the AppDb value of
// 0x263440 / 0x27C760 / 0x253D80.
struct Field {
  int type = -1;
  int64_t i = 0;
  std::string s;
  static Field integer(int64_t v) {
    Field f;
    f.type = 0;
    f.i = v;
    return f;
  }
  static Field string(std::string v) {
    Field f;
    f.type = 2;
    f.s = std::move(v);
    return f;
  }
  bool present() const { return type >= 0; }
  int64_t int_or(int64_t d) const { return type == 0 ? i : d; }
  const std::string& str_or_empty() const {
    static const std::string e;
    return type == 2 ? s : e;
  }
};
using Record = std::map<std::string, Field>;

// The database (0x33E080 DbAccessor "SceShellLsdbDbAcc" + 0x3014D0 AppBrowse manager).
struct Db {
  std::recursive_mutex mutex;  // "SceAppDbDaemonShellUI" (0x30DA20 + 8)
  sqlite3* conn = nullptr;
  int64_t appinfo_sync_index = 0;      // tbl_version 'appinfo_sync_index' (0x301A00)
  int64_t conceptinfo_sync_index = 0;  // tbl_version 'conceptinfo_sync_index' (0x301B50)
  std::vector<int32_t> icon_tables;          // users with tbl_iconinfo_ (0x28DE50 list)
  std::vector<int32_t> concept_icon_tables;  // users with tbl_concepticoninfo_ (0x261B70 list)
};
Db& db();

// "/<word>/priv/mms/app.db" (0x30DC90).
std::string app_db_host_path();

// Table names (0x284700 / 0x25A350 / 0x30AE10 / 0x30B050).
std::string iconinfo_table(int32_t user_id);
std::string concepticoninfo_table(int32_t user_id);

// Statement helpers on Db::conn.
int32_t exec(const std::string& sql);
// SELECT * FROM table WHERE key_column = key -> the row's non-NULL columns.
int32_t select_record(const std::string& table, const char* key_column, const std::string& key, Record* out);
// SELECT key_column FROM table [WHERE where] -> the keys.
int32_t select_keys(const std::string& table, const char* key_column, const std::string& where, std::vector<std::string>* out);
// INSERT INTO table(key, columns...) (0x264300) / UPDATE table SET ... WHERE key=?1 (0x2649B0);
// empty strings bind as NULL (0x3AF3B0 type 3 with length 0).
int32_t insert_record(const std::string& table, const char* key_column, const std::string& key, const Record& r);
int32_t update_record(const std::string& table, const char* key_column, const std::string& key, const Record& r);
int32_t delete_record(const std::string& table, const char* key_column, const std::string& key);

// tbl_version (0x33F540 read, 0x301CA0 / 0x33F0A0 write).
int32_t get_version_value(const std::string& category, int64_t* v);
int32_t set_version_value(const std::string& category, int64_t v);

// Schema (0x304CD0 -> 0x304D40): tbl_version, per registered user tbl_addon_/tbl_info_/
// tbl_iconinfo_/tbl_concepticoninfo_, tbl_contentinfo, tbl_conceptmetadata.
int32_t create_schema();
int32_t create_user_tables(int32_t user_id);
// The per-user tables one at a time: 0x3064C0 (tbl_addon_ then tbl_info_), 0x28DC90
// (tbl_iconinfo_), 0x2619B0 (tbl_concepticoninfo_).
int32_t create_user_info_tables(int32_t user_id);
int32_t create_icon_table(int32_t user_id);
int32_t create_concept_icon_table(int32_t user_id);

// ---- the mirror (app_browse_content.cpp / app_browse_icon.cpp / app_browse_concept.cpp) ----

// appinfo_sync::InsertTransactionCommand (0x327800): tbl_contentinfo from the AppInfo, then the
// observers of the ContentInfo table (0x27BB20/0x27BD40).
int32_t sync_title(const std::string& title_id, int32_t package_location);
// appinfo_sync::DeleteTransactionCommand (0x312510 -> 0x315700).
int32_t delete_title(const std::string& title_id);
// AppInfo -> tbl_contentinfo record (0x322E60: 0x318150, 0x3224F0, 0x321D10).
int32_t build_content_record(const AppInfo& info, bool is_new, Record* rec);

// The ContentInfo observer of the AppBrowse manager (CDC108 +0x20 = 0x2E5600): every user's
// tbl_iconinfo_ row for the title (0x2E56C0).
int32_t on_content_changed(int op, const std::string& title_id, const Record* before, const Record* after);
// 0x2C24A0: one tbl_contentinfo row reflected into a user's tbl_iconinfo_ (modifyAppearanceControl
// 0x2E56C0 with operation 1 and the stored row as the new record).
int32_t reflect_content_to_user(int32_t user_id, const std::string& title_id);
// The IconInfo observer (CDC378 +0x20 = 0x2EAEA0): the user's tbl_concepticoninfo_ row.
int32_t on_icon_changed(int op, int32_t user_id, const Record* before, const Record* after);
// The ConceptMetadata observer (CDC3B8 +0x20 = 0x2F7CE0) and conceptinfo promote
// (conceptinfo_sync::RecoveryAppDbPromoteTransactionCommand, 0x331BD0).
int32_t sync_concepts(int64_t from_index, int64_t to_index);

// Record-level operation codes of the table subjects (0x27BB20 a3): 1 insert, 2 delete, 3 update.
enum Op : int { kInsert = 1, kDelete = 2, kUpdate = 3 };

// The system language (regmgr 0x02020000, 0x345A50).
int32_t system_language();

}  // namespace ssn::eboot::appbrowse
