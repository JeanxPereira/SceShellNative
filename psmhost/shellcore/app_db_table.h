// SceShellCore's AppDb storage layer for appinfo.db (FW 4.03, vsh/shell/shell_core/app_db):
// the DbAccessor (dbutil::Connection over SQLite) and the appinfo_table / conceptinfo_table /
// version-table statements. Every statement text below is the one in the binary.
//
// Values are the AppDb variant the daemon keeps per key (AppInfo map entry +72): type 0 is a
// 64-bit integer, type 2 a string. Rows bind them through the connection's binder (0xBEEE50):
// binder type 1 = INTEGER, 3 = TEXT, 5 = NULL; an empty string binds NULL (0xBC7440, the
// constants at 0x17F0320 / 0x17EF7C0).
#pragma once
#include <cstdint>
#include <map>
#include <memory>
#include <string>
#include <vector>

struct sqlite3;

namespace ssn::shellcore::appdb {

// SCE_APP_DB errors (0x81F4xxxx) as the daemon returns them.
constexpr int32_t kOk = 0;
constexpr int32_t kErrInvalidParam = static_cast<int32_t>(0x81F40001u);
constexpr int32_t kErrNoMemory = static_cast<int32_t>(0x81F40002u);
constexpr int32_t kErrNoAccessor = static_cast<int32_t>(0x81F40003u);    // "m_db_accessor is Null"
constexpr int32_t kErrExists = static_cast<int32_t>(0x81F40004u);        // "already exists"
constexpr int32_t kErrNotFound = static_cast<int32_t>(0x81F40005u);
constexpr int32_t kErrBadCondition = static_cast<int32_t>(0x81F40007u);  // op/value type mismatch (0xBCBC30)
constexpr int32_t kErrInvalidArg = static_cast<int32_t>(0x81F40009u);
constexpr int32_t kErrUnsupported = static_cast<int32_t>(0x81F40013u);
constexpr int32_t kErrCorrupt = static_cast<int32_t>(0x81F40061u);       // open/version failure mapped by 0xBDE170/0xBDEBC0
constexpr int32_t kErrAlreadyInitialized = static_cast<int32_t>(0x81F40078u);
constexpr int32_t kErrNotInitialized = static_cast<int32_t>(0x81F40079u);

struct Value {
  int32_t type = 0;  // 0 integer, 2 string
  int64_t i = 0;
  std::string s;

  static Value integer(int64_t v) { return Value{0, v, {}}; }
  static Value string(std::string v) { return Value{2, 0, std::move(v)}; }
  bool is_int() const { return type == 0; }
  bool is_string() const { return type == 2; }
};

// std::map ordered by key, like the daemon's std::map<std::string, Value>.
using KeyValueMap = std::map<std::string, Value>;

// AppInfo (vtable 0x19C8A98, 128 bytes, ctor 0xB31850): the key/value map (+8), the title id
// (+40, empty) and the metadata id (+80, "prior"), and an int at +120.
struct AppInfo {
  KeyValueMap values;
  std::string title_id;
  std::string metadata_id = "prior";
  int32_t field120 = 0;

  bool has(const std::string& k) const { return values.count(k) != 0; }
  // std::map::operator[] (0xBCF60): inserts the default value {0, 0} when missing.
  Value& at(const std::string& k) { return values[k]; }
  int64_t int_or(const std::string& k, int64_t d) const;
  std::string str_or(const std::string& k, const std::string& d) const;
};

// ConceptInfo (vtable 0x19C8AC8, ctor 0xB2E1E0): the key/value map and the concept id (+40).
struct ConceptInfo {
  KeyValueMap values;
  uint64_t concept_id = 0;
};

struct PrimaryKey {
  std::string title_id;
  std::string metadata_id = "prior";
};

// Search condition (104-byte node built by JsonToCondition, 0xBAFEE0): key (+0), values
// (+40), op (+72: 0 "=?", 1 LIKE, 2 "!=?", 3 "&?=?", 4 "<?", 5 ">?"), bit mask (+80), next
// condition joined by AND (+88) or OR (+96).
struct Condition {
  std::string key;
  std::vector<Value> values;
  int32_t op = 0;
  int64_t bit_mask = 0;
};
struct ConditionChain {
  std::vector<Condition> conditions;
  int32_t join = 0;  // next_cond_type of the chain: 0 single, 1 AND, 2 OR
};
// Sort field (48-byte entry, 0xBB2B30): key and type (0 none, 1 ascending, otherwise descending).
struct SortField {
  std::string key;
  int32_t type = 0;
};

// DbAccessor (0xBDDFC0, "SceShellLsdbDbAcc") around one dbutil::Connection.
class DbAccessor {
 public:
  ~DbAccessor();
  // DbAccessor::Init(path, flags) — 0xBDE170: opens the connection (0xBE7B60) with the
  // SQLite open flags (2 = READWRITE, 6 = READWRITE|CREATE), then 0xBDF4D0: busy timeout
  // 30000 ms, PRAGMA foreign_keys=ON, recursive_triggers=ON, case_sensitive_like=ON. Open
  // failures are 0x809C0200|sqlite_rc; IOERR/CORRUPT/CANTOPEN/EMPTY/FORMAT/NOTADB map to
  // kErrCorrupt.
  int32_t init(const std::string& host_path, int flags);
  void close();  // 0xBDE110 (via 0xBBCF00)
  bool is_open() const { return db_ != nullptr; }
  sqlite3* db() const { return db_; }

  // Transactions (0xBDE360 / 0xBDE440): dbutil's BEGIN/COMMIT text is not traced.
  int32_t begin();
  int32_t end(int32_t result);

  // --- version table (tbl_version) ---
  // 0xBDE6A0: "SELECT count(*) FROM sqlite_master WHERE type='table' AND name='tbl_version'",
  // then "SELECT category, status FROM tbl_version": appver, appinfover, build_target.
  int32_t get_version(int32_t* appver, int32_t* appinfover, int32_t* build_target);
  // 0xBDEBF0: "REPLACE INTO tbl_version VALUES('%s', %d)" for appver, appinfover, build_target(0).
  int32_t set_version(int32_t appver, int32_t appinfover);
  // 0xBC2E60 / 0xBC3180: "SELECT category, status FROM tbl_version WHERE category = ?1",
  // "REPLACE INTO tbl_version VALUES(?1, ?2)".
  int32_t get_category(const std::string& category, int64_t* status, bool* found);
  int32_t set_category(const std::string& category, int64_t status);

  // --- create (0xBBCF20 / 0xBC2D50) ---
  int32_t create_tables();
  int32_t init_access_index();  // 0xBC3480

  // --- appinfo_table ---
  int32_t count_app_info(const PrimaryKey& pk, int64_t* count);                       // 0xBC4EF0
  int32_t get_app_info(const PrimaryKey& pk, const std::vector<std::string>& keys, AppInfo* out);  // 0xBC4030
  int32_t insert_app_info(const PrimaryKey& pk, const AppInfo& info, bool replace);   // 0xBC6BD0
  int32_t delete_app_info(const PrimaryKey& pk);                                     // 0xBC8820
  int32_t search_primary_keys(const ConditionChain& cond, const std::vector<SortField>& sort,
                              std::vector<PrimaryKey>* out);                          // 0xBCBC30
  int32_t max_sync_index(int64_t* out, bool* found);

  // --- conceptinfo_table ---
  int32_t exists_concept_info(const std::string& local_concept_id, bool* exists);     // 0xBD5540
  int32_t insert_concept_info(const std::string& local_concept_id, const std::string& metadata_id,
                              const ConceptInfo& info, bool replace);                 // 0xBD8F00
  // 0xBD50D0: "SELECT key, val FROM tbl_conceptinfo WHERE local_concept_id=?".
  int32_t get_concept_info(const std::string& local_concept_id, ConceptInfo* out);
  // 0xBD5D40: the local concept ids matching a condition chain, sorted.
  int32_t search_concepts(const ConditionChain& cond, const std::vector<SortField>& sort,
                          std::vector<std::string>* out);

  // The SQL 0xBCBC30 builds for a condition chain and sort list (exposed for tests).
  static int32_t build_search_sql(const ConditionChain& cond, const std::vector<SortField>& sort,
                                  const std::string& localized_title_key, std::string* sql,
                                  std::vector<Value>* binds);

 private:
  int32_t exec(const char* sql);
  sqlite3* db_ = nullptr;
};

// The keys appinfo_table stores under metaDataId "prior" only (set 0x1B0ED98, built by 0xBD0200).
bool is_prior_shared_key(const std::string& key);

// 0xB359A0 -> 0xB35B20: "TITLE" with the suffix of the system language's entry in the table
// at 0x19C8B20 (lang 0..29 -> "TITLE_00".."TITLE_29"); defined in app_db.cpp.
std::string localized_title_key();

}  // namespace ssn::shellcore::appdb
