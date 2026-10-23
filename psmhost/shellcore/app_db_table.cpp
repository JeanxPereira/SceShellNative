// SceShellCore AppDb storage layer for appinfo.db (FW 4.03). See app_db_table.h.
#include "shellcore/app_db_table.h"

#include <cstdio>
#include <cstring>

#include <sqlite3.h>

#include "core/log.h"
#include "shellcore/shellcore_build.h"
#include "host/boundary.h"

namespace ssn::shellcore::appdb {
namespace {

// The three CREATE statements 0xBC2D50 executes, in the order of its pointer table at
// 0x19CA210 (strings 0x1769BA0, 0x178D132, 0x16F72FC).
constexpr const char* kCreateSql[3] = {
    "CREATE TABLE IF NOT EXISTS tbl_version(category TEXT NOT NULL,status INT,PRIMARY KEY(category));",
    "CREATE TABLE IF NOT EXISTS tbl_appinfo(titleId TEXT NOT NULL,metaDataId TEXT,key INT NOT NULL,val,PRIMARY KEY(titleId, metaDataId, key));",
    "CREATE TABLE IF NOT EXISTS tbl_conceptinfo(local_concept_id TEXT NOT NULL,metadata_id TEXT,key INT NOT NULL,val,PRIMARY KEY(local_concept_id, key));",
};

// Search operators, pointer table 0x19CA248.
constexpr const char* kOps[6] = {"=?", " LIKE ? ESCAPE '$'", "!=?", "&?=?", "<?", ">?"};

// dbutil::Connection error for a failed open (0xBE7B60): rc | 0x809C0200.
int32_t open_error(int rc) { return static_cast<int32_t>(0x809C0200u | static_cast<uint32_t>(rc)); }

// 0xBDE170 / 0xBDEBC0: connection errors IOERR(10), CORRUPT(11), CANTOPEN(14), EMPTY(16),
// FORMAT(24), NOTADB(26) (bit set 0x14053 over 0x809C020A..) become kErrCorrupt.
int32_t map_corrupt(int32_t r) {
  const uint32_t d = static_cast<uint32_t>(r) - 0x809C020Au;
  if (d <= 0x10 && ((0x14053u >> d) & 1u)) return kErrCorrupt;
  return r;
}

// dbutil statement error for a failed step/prepare: the SQLite code in the same 0x809C02xx
// space (only its non-zero-ness is used by the callers reproduced here).
int32_t sql_error(int rc) { return open_error(rc); }

struct Stmt {
  sqlite3_stmt* s = nullptr;
  ~Stmt() {
    if (s) sqlite3_finalize(s);
  }
};

// The binder (0xBEEE50) with the AppDb conventions: empty string -> NULL.
int bind_text_or_null(sqlite3_stmt* s, int idx, const std::string& v) {
  if (v.empty()) return sqlite3_bind_null(s, idx);
  return sqlite3_bind_text(s, idx, v.c_str(), static_cast<int>(v.size()), SQLITE_TRANSIENT);
}
int bind_value(sqlite3_stmt* s, int idx, const Value& v) {
  if (v.type == 2) return bind_text_or_null(s, idx, v.s);
  return sqlite3_bind_int64(s, idx, v.i);
}

// Column -> variant (0xBC3B60 / 0xBCBB60): INTEGER -> type 0, TEXT -> type 2.
Value column_value(sqlite3_stmt* s, int col) {
  switch (sqlite3_column_type(s, col)) {
    case SQLITE_INTEGER:
      return Value::integer(sqlite3_column_int64(s, col));
    case SQLITE_NULL:
      return Value::string({});
    default: {
      const unsigned char* t = sqlite3_column_text(s, col);
      return Value::string(t ? reinterpret_cast<const char*>(t) : "");
    }
  }
}

std::string column_text(sqlite3_stmt* s, int col) {
  const unsigned char* t = sqlite3_column_text(s, col);
  return t ? reinterpret_cast<const char*>(t) : "";
}

}  // namespace

int64_t AppInfo::int_or(const std::string& k, int64_t d) const {
  auto it = values.find(k);
  return (it != values.end() && it->second.type == 0) ? it->second.i : d;
}

std::string AppInfo::str_or(const std::string& k, const std::string& d) const {
  auto it = values.find(k);
  return (it != values.end() && it->second.type == 2) ? it->second.s : d;
}

bool is_prior_shared_key(const std::string& key) {
  // 0xBD0200 (lines building unk_1B0ED98 with 0xB62B90).
  static const char* const kKeys[] = {
      "#_last_access_time", "#_access_index", "_sort_priority", "#catalog_path_icon0_info",
      "#catalog_path_pic0_info", "#catalog_path_icon0_info_time_stamp", "#catalog_path_pic0_info_time_stamp",
      "DISC_STATUS", "DISC_INSERT_DATE", "DISC_COMPILATION",
  };
  for (const char* k : kKeys)
    if (key == k) return true;
  return false;
}

DbAccessor::~DbAccessor() { close(); }

int32_t DbAccessor::init(const std::string& host_path, int flags) {
  close();
  // 0xBE7B60 -> sqlite3_open_v2(path, &db, flags, nullptr) (0xC0F930). The busy/commit hooks
  // the connection installs (0xC0E430, 0xC0E3C0, 0xC0E4A0) are dbutil diagnostics.
  sqlite3* db = nullptr;
  const int rc = sqlite3_open_v2(host_path.c_str(), &db, flags, nullptr);
  if (rc != SQLITE_OK) {
    if (db) sqlite3_close(db);
    log::info("appdb", "[File:Open] openDb() flags[0x{:X}] result[0x{:X}, {}]", flags,
              static_cast<uint32_t>(open_error(rc)), open_error(rc));
    return map_corrupt(open_error(rc));
  }
  db_ = db;
  // 0xBDF4D0.
  sqlite3_busy_timeout(db_, 30000);
  exec("PRAGMA foreign_keys=ON");
  exec("PRAGMA recursive_triggers=ON");
  exec("PRAGMA case_sensitive_like=ON");
  log::info("appdb", "[File:Open] openDb() flags[0x{:X}] result[0x0, 0]", flags);
  return 0;
}

void DbAccessor::close() {
  if (db_) sqlite3_close(db_);
  db_ = nullptr;
}

int32_t DbAccessor::exec(const char* sql) {
  if (!db_) return kErrNoAccessor;
  char* err = nullptr;
  const int rc = sqlite3_exec(db_, sql, nullptr, nullptr, &err);
  if (err) sqlite3_free(err);
  return rc == SQLITE_OK ? 0 : sql_error(rc);
}

int32_t DbAccessor::begin() {
  host::note_unimplemented("SceShellCore dbutil::Connection transaction statement text (0xBDE360) not traced; BEGIN/COMMIT");
  return exec("BEGIN");
}

int32_t DbAccessor::end(int32_t result) { return exec(result == 0 ? "COMMIT" : "ROLLBACK"); }

int32_t DbAccessor::get_version(int32_t* appver, int32_t* appinfover, int32_t* build_target) {
  if (!db_) return kErrNoAccessor;
  Stmt st;
  int rc = sqlite3_prepare_v2(db_, "SELECT count(*) FROM sqlite_master WHERE type='table' AND name='tbl_version'", -1, &st.s, nullptr);
  if (rc != SQLITE_OK) return map_corrupt(sql_error(rc));
  int64_t n = 0;
  if (sqlite3_step(st.s) == SQLITE_ROW) n = sqlite3_column_int64(st.s, 0);
  if (n == 0) return 0;  // no table: values stay as the caller initialised them
  Stmt q;
  rc = sqlite3_prepare_v2(db_, "SELECT category, status FROM tbl_version", -1, &q.s, nullptr);
  if (rc != SQLITE_OK) return map_corrupt(sql_error(rc));
  while ((rc = sqlite3_step(q.s)) == SQLITE_ROW) {
    const std::string cat = column_text(q.s, 0);
    // "status": the integer column value, 0 for any other type.
    const int32_t status = sqlite3_column_type(q.s, 1) == SQLITE_INTEGER ? static_cast<int32_t>(sqlite3_column_int64(q.s, 1)) : 0;
    if (cat == "appver" && appver) *appver = status;
    if (cat == "appinfover" && appinfover) *appinfover = status;
    if (cat == "build_target" && build_target) *build_target = status;
  }
  return rc == SQLITE_DONE ? 0 : map_corrupt(sql_error(rc));
}

int32_t DbAccessor::set_version(int32_t appver, int32_t appinfover) {
  // 0xBDEBF0: keeps the stored appinfover when the requested one is negative; writes
  // appver, appinfover and build_target with "REPLACE INTO tbl_version VALUES('%s', %d)".
  // build_target is 0 in the retail build and 1 in the devkit build (0xBEB250).
  int32_t cur_ver = -1, cur_info = -1;
  if (int32_t r = get_version(&cur_ver, &cur_info, nullptr); r != 0) return r;
  if (appinfover < 0) appinfover = cur_info;
  const std::pair<const char*, int32_t> rows[3] = {{"appver", appver}, {"appinfover", appinfover}, {"build_target", devkit_build() ? 1 : 0}};
  for (const auto& [cat, v] : rows) {
    char sql[128];
    std::snprintf(sql, sizeof sql, "REPLACE INTO tbl_version VALUES('%s', %d)", cat, v);
    if (int32_t r = exec(sql); r != 0) return r;
  }
  return 0;
}

int32_t DbAccessor::get_category(const std::string& category, int64_t* status, bool* found) {
  if (!db_) return kErrNoAccessor;
  *found = false;
  Stmt st;
  int rc = sqlite3_prepare_v2(db_, "SELECT category, status FROM tbl_version WHERE category = ?1", -1, &st.s, nullptr);
  if (rc != SQLITE_OK) return sql_error(rc);
  bind_text_or_null(st.s, 1, category);
  rc = sqlite3_step(st.s);
  if (rc == SQLITE_ROW) {
    *found = true;
    *status = sqlite3_column_int64(st.s, 1);
    return 0;
  }
  return rc == SQLITE_DONE ? 0 : sql_error(rc);
}

int32_t DbAccessor::set_category(const std::string& category, int64_t status) {
  if (!db_) return kErrNoAccessor;
  Stmt st;
  int rc = sqlite3_prepare_v2(db_, "REPLACE INTO tbl_version VALUES(?1, ?2)", -1, &st.s, nullptr);
  if (rc != SQLITE_OK) return sql_error(rc);
  bind_text_or_null(st.s, 1, category);
  sqlite3_bind_int64(st.s, 2, status);
  rc = sqlite3_step(st.s);
  return rc == SQLITE_DONE ? 0 : sql_error(rc);
}

int32_t DbAccessor::create_tables() {
  for (const char* sql : kCreateSql)
    if (int32_t r = exec(sql); r != 0) return r;
  return 0;
}

int32_t DbAccessor::init_access_index() {
  // 0xBC3480: reads 'access_index' (0xBC2E60; a non-integer or missing value counts as 0),
  // increments it and writes "REPLACE INTO tbl_version VALUES('access_index', %ld)".
  int64_t v = 0;
  bool found = false;
  int32_t r = get_category("access_index", &v, &found);
  if (!found) v = 0;
  if (r < 0) return r;
  ++v;
  char sql[64];
  std::snprintf(sql, sizeof sql, "REPLACE INTO tbl_version VALUES('%s', %lld)", "access_index", static_cast<long long>(v));
  return exec(sql);
}

int32_t DbAccessor::count_app_info(const PrimaryKey& pk, int64_t* count) {
  // 0xBC4EF0: count 0 -> kErrNotFound.
  if (!db_) return kErrNoAccessor;
  Stmt st;
  int rc = sqlite3_prepare_v2(db_, "SELECT count(*) FROM tbl_appinfo WHERE titleId=? AND metaDataId LIKE ?", -1, &st.s, nullptr);
  if (rc != SQLITE_OK) return sql_error(rc);
  bind_text_or_null(st.s, 1, pk.title_id);
  bind_text_or_null(st.s, 2, pk.metadata_id);
  rc = sqlite3_step(st.s);
  if (rc != SQLITE_ROW) return sql_error(rc);
  if (sqlite3_column_type(st.s, 0) != SQLITE_INTEGER) return static_cast<int32_t>(0x81F40000u);
  *count = sqlite3_column_int64(st.s, 0);
  return *count != 0 ? 0 : kErrNotFound;
}

int32_t DbAccessor::get_app_info(const PrimaryKey& pk, const std::vector<std::string>& keys, AppInfo* out) {
  // 0xBC4030.
  if (pk.title_id.empty() || pk.metadata_id.empty()) return kErrInvalidArg;
  int64_t n = 0;
  if (int32_t r = count_app_info(pk, &n); r != 0) return r;
  out->values.clear();
  std::string filter;
  if (!keys.empty()) {
    filter = " AND (key=?";
    for (size_t i = 1; i < keys.size(); ++i) filter += " OR key=?";
    filter += ")";
  }
  const bool prior = pk.metadata_id.find("prior") != std::string::npos;
  std::string sql = prior ? "SELECT key, val FROM tbl_appinfo WHERE titleId=? AND metaDataId LIKE ?"
                          : "SELECT key, val FROM tbl_appinfo WHERE titleId=? AND (metaDataId LIKE ? OR metaDataId LIKE ?)";
  sql += filter;
  Stmt st;
  int rc = sqlite3_prepare_v2(db_, sql.c_str(), -1, &st.s, nullptr);
  if (rc != SQLITE_OK) return sql_error(rc);
  int idx = 1;
  bind_text_or_null(st.s, idx++, pk.title_id);
  bind_text_or_null(st.s, idx++, prior ? std::string("prior%") : pk.metadata_id);
  if (!prior) bind_text_or_null(st.s, idx++, "prior");
  for (const auto& k : keys) bind_text_or_null(st.s, idx++, k);
  while ((rc = sqlite3_step(st.s)) == SQLITE_ROW) {
    const std::string key = column_text(st.s, 0);
    Value v = column_value(st.s, 1);
    if (key == "sync_index") {
      // The larger sync index of the rows wins (0xBC4030, "sync_index" branch).
      auto it = out->values.find(key);
      if (it != out->values.end() && it->second.type == 0 && v.type == 0 && v.i <= it->second.i) continue;
      out->values[key] = v;
      continue;
    }
    out->values.emplace(key, v);  // std::map insert (0x2046F0) keeps the first row of a key
  }
  if (rc != SQLITE_DONE) return sql_error(rc);
  out->title_id = pk.title_id;
  out->metadata_id = pk.metadata_id;
  return 0;
}

int32_t DbAccessor::insert_app_info(const PrimaryKey& pk, const AppInfo& info, bool replace) {
  // 0xBC6BD0: titleId and metaDataId must be non-empty (kErrInvalidArg); every key must be
  // non-empty (0xBC7310); an INSERT of an existing title is kErrExists.
  if (pk.title_id.empty() || pk.metadata_id.empty()) return kErrInvalidArg;
  for (const auto& [k, v] : info.values)
    if (k.empty()) return kErrInvalidArg;
  int64_t n = 0;
  const int32_t c = count_app_info(pk, &n);
  if (!(c == kErrNotFound || (c == 0 && replace))) return c == 0 ? kErrExists : c;
  const char* sql = replace ? "REPLACE INTO tbl_appinfo(titleId, metaDataId, key, val) VALUES(?,?,?,?)"
                            : "INSERT INTO tbl_appinfo(titleId, metaDataId, key, val) VALUES(?,?,?,?)";
  Stmt st;
  int rc = sqlite3_prepare_v2(db_, sql, -1, &st.s, nullptr);
  if (rc != SQLITE_OK) return sql_error(rc);
  for (const auto& [key, val] : info.values) {
    if (key == "_title_id") continue;
    std::string md = pk.metadata_id;
    if (is_prior_shared_key(key)) {
      if (pk.metadata_id.find("prior") == std::string::npos) continue;
      md = "prior";
    }
    sqlite3_reset(st.s);
    sqlite3_clear_bindings(st.s);
    bind_text_or_null(st.s, 1, pk.title_id);
    bind_text_or_null(st.s, 2, md);
    bind_text_or_null(st.s, 3, key);  // the key column holds the key name (TEXT)
    bind_value(st.s, 4, val);
    rc = sqlite3_step(st.s);
    if (rc == SQLITE_CONSTRAINT || (rc & 0xFF) == SQLITE_CONSTRAINT) return kErrExists;
    if (rc != SQLITE_DONE) return sql_error(rc);
  }
  return 0;
}

int32_t DbAccessor::delete_app_info(const PrimaryKey& pk) {
  if (!db_) return kErrNoAccessor;
  Stmt st;
  int rc = sqlite3_prepare_v2(db_, "DELETE FROM tbl_appinfo WHERE titleId=? AND metaDataId LIKE ?", -1, &st.s, nullptr);
  if (rc != SQLITE_OK) return sql_error(rc);
  bind_text_or_null(st.s, 1, pk.title_id);
  bind_text_or_null(st.s, 2, pk.metadata_id);
  rc = sqlite3_step(st.s);
  return rc == SQLITE_DONE ? 0 : sql_error(rc);
}

int32_t DbAccessor::max_sync_index(int64_t* out, bool* found) {
  // The daemon reads the largest stored "sync_index" value when it starts (0xB61180 ->
  // 0xBCB7A0); the exact statement of 0xBCB7A0 is not traced, the value is the same.
  host::note_unimplemented("SceShellCore appinfo_table max sync_index statement (0xBCB7A0) not traced; MAX(val) over key='sync_index'");
  *found = false;
  Stmt st;
  int rc = sqlite3_prepare_v2(db_, "SELECT MAX(val) FROM tbl_appinfo WHERE key='sync_index'", -1, &st.s, nullptr);
  if (rc != SQLITE_OK) return sql_error(rc);
  if (sqlite3_step(st.s) == SQLITE_ROW && sqlite3_column_type(st.s, 0) == SQLITE_INTEGER) {
    *found = true;
    *out = sqlite3_column_int64(st.s, 0);
  }
  return 0;
}

int32_t DbAccessor::exists_concept_info(const std::string& local_concept_id, bool* exists) {
  // 0xBD5540 -> "SELECT count(*) FROM tbl_conceptinfo WHERE local_concept_id=?".
  if (!db_) return kErrNoAccessor;
  Stmt st;
  int rc = sqlite3_prepare_v2(db_, "SELECT count(*) FROM tbl_conceptinfo WHERE local_concept_id=?", -1, &st.s, nullptr);
  if (rc != SQLITE_OK) return sql_error(rc);
  bind_text_or_null(st.s, 1, local_concept_id);
  rc = sqlite3_step(st.s);
  if (rc != SQLITE_ROW) return sql_error(rc);
  *exists = sqlite3_column_int64(st.s, 0) != 0;
  return 0;
}

int32_t DbAccessor::get_concept_info(const std::string& local_concept_id, ConceptInfo* out) {
  if (!db_) return kErrNoAccessor;
  out->values.clear();
  Stmt st;
  int rc = sqlite3_prepare_v2(db_, "SELECT key, val FROM tbl_conceptinfo WHERE local_concept_id=?", -1, &st.s, nullptr);
  if (rc != SQLITE_OK) return sql_error(rc);
  bind_text_or_null(st.s, 1, local_concept_id);
  while ((rc = sqlite3_step(st.s)) == SQLITE_ROW) out->values[column_text(st.s, 0)] = column_value(st.s, 1);
  if (rc != SQLITE_DONE) return sql_error(rc);
  return out->values.empty() ? kErrNotFound : 0;
}

int32_t DbAccessor::search_concepts(const ConditionChain& chain, const std::vector<SortField>& sort_in,
                                    std::vector<std::string>* out) {
  // 0xBD5D40 (the tbl_conceptinfo twin of 0xBCBC30): type-0 sort entries dropped.
  if (!db_) return kErrNoAccessor;
  std::vector<SortField> sort;
  for (const auto& f : sort_in)
    if (f.type != 0) sort.push_back(f);
  const bool or_chain = chain.join == 2;
  const char* first_select = or_chain ? "SELECT DISTINCT a0.local_concept_id FROM tbl_conceptinfo a0 "
                                      : "SELECT a0.local_concept_id FROM tbl_conceptinfo a0 ";
  std::string sql = "SELECT DISTINCT local_concept_id FROM tbl_conceptinfo a ";
  std::string join, where;
  std::vector<Value> binds, sort_binds;
  bool any = false;
  for (size_t i = 0; i < chain.conditions.size(); ++i) {
    const Condition& c = chain.conditions[i];
    if (sort.empty() && c.key.empty()) continue;
    if (c.key == "local_concept_id") return kErrInvalidParam;
    const std::string alias = "a" + std::to_string(i);
    std::string sep;
    if (i == 0) {
      if (sort.empty()) {
        sql = first_select;
      } else {
        sql = "SELECT DISTINCT t.local_concept_id,";
        for (size_t k = 0; k < sort.size(); ++k) {
          if (k) sql += ",";
          sql += "t" + std::to_string(k) + ".val";
        }
        sql += " FROM (";
        sql += first_select;
      }
      sep = "WHERE ";
    } else if (!or_chain) {
      if (i == 1) {
        sql += " INNER JOIN tbl_conceptinfo " + alias;
        join += " ON ";
      } else {
        sql += ",tbl_conceptinfo " + alias;
        join += " AND ";
      }
      join += "a0.local_concept_id = " + alias + ".local_concept_id ";
      sep = " AND ";
    } else {
      sep = " OR ";
    }
    const std::string a = or_chain ? std::string("a0") : alias;
    where += sep + "(" + a + ".key=?";
    binds.push_back(Value::string(c.key));
    if (!c.values.empty()) {
      where += " AND (";
      for (size_t v = 0; v < c.values.size(); ++v) {
        const Value& val = c.values[v];
        if (val.type == 2 && c.op == 3) return kErrBadCondition;
        if (val.type == 0 && c.op == 1) return kErrBadCondition;
        if (c.op < 0 || c.op > 5) return kErrBadCondition;
        if (v) where += " OR ";
        where += a + ".val" + kOps[c.op];
        if (c.op == 3) binds.push_back(Value::integer(c.bit_mask));
        binds.push_back(val);
      }
      where += ")";
    }
    where += ")";
    any = true;
  }
  if (!any && !sort.empty()) {
    sql = "SELECT DISTINCT t.local_concept_id,";
    for (size_t k = 0; k < sort.size(); ++k) {
      if (k) sql += ",";
      sql += "t" + std::to_string(k) + ".val";
    }
    sql += " FROM (tbl_conceptinfo";
  }
  sql += join + where;
  if (!sort.empty()) {
    sql += ")t ";
    std::string order;
    for (size_t k = 0; k < sort.size(); ++k) {
      const std::string t = "t" + std::to_string(k), b = "b" + std::to_string(k);
      sql += " INNER JOIN (SELECT " + b + ".local_concept_id," + b + ".val FROM tbl_conceptinfo " + b + " WHERE (" + b +
             ".key=?) )" + t + " ON t.local_concept_id=" + t + ".local_concept_id";
      sort_binds.push_back(Value::string(sort[k].key));
      if (k) order += ",";
      order += t + ".val " + (sort[k].type == 1 ? "ASC" : "DESC");
    }
    sql += " ORDER BY " + order;
    binds.insert(binds.end(), sort_binds.begin(), sort_binds.end());
  }
  Stmt st;
  int rc = sqlite3_prepare_v2(db_, sql.c_str(), -1, &st.s, nullptr);
  if (rc != SQLITE_OK) {
    log::warn("appdb", "search_concepts: prepare failed ({}): {}", rc, sql);
    return sql_error(rc);
  }
  for (size_t i = 0; i < binds.size(); ++i) bind_value(st.s, static_cast<int>(i + 1), binds[i]);
  out->clear();
  while ((rc = sqlite3_step(st.s)) == SQLITE_ROW) out->push_back(column_text(st.s, 0));
  return rc == SQLITE_DONE ? 0 : sql_error(rc);
}

int32_t DbAccessor::insert_concept_info(const std::string& local_concept_id, const std::string& metadata_id,
                                        const ConceptInfo& info, bool replace) {
  // 0xBD8F00: rows (local_concept_id, metadata_id, key, val) for every key except
  // "local_concept_id"; an existing concept is kErrExists for INSERT.
  if (local_concept_id.empty()) return kErrInvalidArg;
  bool exists = false;
  if (int32_t r = exists_concept_info(local_concept_id, &exists); r != 0) return r;
  if (exists && !replace) return kErrExists;
  const char* sql = replace ? "REPLACE INTO tbl_conceptinfo(local_concept_id, metadata_id, key, val) VALUES(?,?,?,?)"
                            : "INSERT INTO tbl_conceptinfo(local_concept_id, metadata_id, key, val) VALUES(?,?,?,?)";
  Stmt st;
  int rc = sqlite3_prepare_v2(db_, sql, -1, &st.s, nullptr);
  if (rc != SQLITE_OK) return sql_error(rc);
  for (const auto& [key, val] : info.values) {
    if (key == "local_concept_id") continue;
    sqlite3_reset(st.s);
    sqlite3_clear_bindings(st.s);
    bind_text_or_null(st.s, 1, local_concept_id);
    bind_text_or_null(st.s, 2, metadata_id);
    bind_text_or_null(st.s, 3, key);
    bind_value(st.s, 4, val);
    rc = sqlite3_step(st.s);
    if ((rc & 0xFF) == SQLITE_CONSTRAINT) return kErrExists;
    if (rc != SQLITE_DONE) return sql_error(rc);
  }
  return 0;
}

int32_t DbAccessor::build_search_sql(const ConditionChain& chain, const std::vector<SortField>& sort_in,
                                     const std::string& localized_title_key, std::string* out,
                                     std::vector<Value>* binds) {
  // 0xBCBC30 (SearchPrimaryKeyList).
  // Sort list: entries with type 0 are dropped; the pseudo key "TITLE_SYSTEM_LANG"
  // (0xB28DD0 of "TITLE") becomes the localized title key (0xB359A0) followed by "TITLE",
  // ordered as COALESCE of the two.
  std::vector<SortField> sort;
  int title_index = -1;
  for (const auto& f : sort_in) {
    if (f.key != "TITLE_SYSTEM_LANG") {
      if (f.type != 0) sort.push_back(f);
      continue;
    }
    title_index = static_cast<int>(sort.size());
    sort.push_back({localized_title_key, f.type});
    sort.push_back({"TITLE", f.type});
  }
  const bool or_chain = chain.join == 2;
  const char* first_select = or_chain ? "SELECT DISTINCT a0.titleId, a0.metaDataId FROM tbl_appinfo a0 "
                                      : "SELECT a0.titleId, a0.metaDataId FROM tbl_appinfo a0 ";
  std::string sql = "SELECT DISTINCT titleId, metaDataId FROM tbl_appinfo a ";
  std::string join;   // v648
  std::string where;  // v645
  binds->clear();
  std::vector<Value> sort_binds;
  bool any = false;
  for (size_t i = 0; i < chain.conditions.size(); ++i) {
    const Condition& c = chain.conditions[i];
    if (sort.empty() && c.key.empty()) continue;  // LABEL_252
    if (c.key == "_title_id") return kErrInvalidParam;
    const std::string alias = "a" + std::to_string(i);
    std::string sep;
    if (i == 0) {
      if (sort.empty()) {
        sql = first_select;
      } else {
        sql = "SELECT DISTINCT t.titleId, t.metaDataId,";
        for (size_t k = 0; k < sort.size(); ++k) {
          if (k) sql += ",";
          sql += "t" + std::to_string(k) + ".val";
        }
        sql += " FROM (";
        sql += first_select;
      }
      sep = "WHERE ";
    } else if (!or_chain) {
      if (i == 1) {
        sql += " INNER JOIN tbl_appinfo " + alias;
        join += " ON ";
      } else {
        sql += ",tbl_appinfo " + alias;
        join += " AND ";
      }
      if (!is_prior_shared_key(c.key))
        join += "a0.titleId = " + alias + ".titleId " + "AND a0.metaDataId = " + alias + ".metaDataId ";
      else
        join += "a0.titleId = " + alias + ".titleId ";
      sep = " AND ";
    } else {
      sep = " OR ";
    }
    const std::string a = or_chain ? std::string("a0") : alias;
    where += sep + "(" + a + ".key=?";
    binds->push_back(Value::string(c.key));
    if (!c.values.empty()) {
      where += " AND (";
      for (size_t v = 0; v < c.values.size(); ++v) {
        const Value& val = c.values[v];
        if (val.type == 2 && c.op == 3) return kErrBadCondition;  // string with bit mask
        if (val.type == 0 && c.op == 1) return kErrBadCondition;  // integer with LIKE
        if (v) where += " OR ";
        if (c.op < 0 || c.op > 5) return kErrBadCondition;
        where += a + ".val" + kOps[c.op];
        if (c.op == 3) binds->push_back(Value::integer(c.bit_mask));
        binds->push_back(val);
      }
      where += ")";
    }
    where += ")";
    any = true;
  }
  if (!any && !sort.empty()) {
    // No condition key: the inner select is the whole table.
    sql = "SELECT DISTINCT t.titleId, t.metaDataId,";
    for (size_t k = 0; k < sort.size(); ++k) {
      if (k) sql += ",";
      sql += "t" + std::to_string(k) + ".val";
    }
    sql += " FROM (tbl_appinfo";
  }
  sql += join + where;
  if (!sort.empty()) {
    sql += ")t ";
    std::string order;
    for (size_t k = 0; k < sort.size(); ++k) {
      const std::string t = "t" + std::to_string(k), b = "b" + std::to_string(k);
      sql += " LEFT JOIN (SELECT " + b + ".titleId, " + b + ".metaDataId, " + b + ".val FROM tbl_appinfo " + b + " WHERE (" + b +
             ".key=?) )" + t;
      sort_binds.push_back(Value::string(sort[k].key));
      if (!is_prior_shared_key(sort[k].key))
        sql += " ON t.titleId=" + t + ".titleId AND t.metaDataId=" + t + ".metaDataId";
      else
        sql += " ON t.titleId=" + t + ".titleId";
      if (title_index >= 0 && static_cast<int>(k) == title_index + 1) continue;  // folded into the CASE
      if (k) order += ",";
      const char* dir = sort[k].type == 1 ? "ASC" : "DESC";
      if (static_cast<int>(k) == title_index) {
        const std::string t2 = "t" + std::to_string(k + 1);
        order += "(CASE WHEN " + t + ".val IS NULL THEN " + t2 + ".val ELSE " + t + ".val END) " + dir;
      } else {
        order += t + ".val " + dir;
      }
    }
    sql += " ORDER BY " + order;
    // The condition placeholders (inside "FROM (...)t") come before the sort joins' ones in
    // the statement text.
    binds->insert(binds->end(), sort_binds.begin(), sort_binds.end());
  }
  *out = sql;
  return 0;
}

int32_t DbAccessor::search_primary_keys(const ConditionChain& chain, const std::vector<SortField>& sort,
                                        std::vector<PrimaryKey>* out) {
  if (!db_) return kErrNoAccessor;
  std::string sql;
  std::vector<Value> binds;
  if (int32_t r = build_search_sql(chain, sort, localized_title_key(), &sql, &binds); r != 0) return r;
  Stmt st;
  int rc = sqlite3_prepare_v2(db_, sql.c_str(), -1, &st.s, nullptr);
  if (rc != SQLITE_OK) {
    log::warn("appdb", "search: prepare failed ({}): {}", rc, sql);
    return sql_error(rc);
  }
  for (size_t i = 0; i < binds.size(); ++i) bind_value(st.s, static_cast<int>(i + 1), binds[i]);
  out->clear();
  while ((rc = sqlite3_step(st.s)) == SQLITE_ROW) out->push_back({column_text(st.s, 0), column_text(st.s, 1)});
  return rc == SQLITE_DONE ? 0 : sql_error(rc);
}

}  // namespace ssn::shellcore::appdb
