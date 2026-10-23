// ShellUI eboot AppBrowse: the app.db connection, statements and schema (FW 4.03).
//
//   path        "/" + sceKernelGetFsSandboxRandomWord() + "/" + "priv/mms/app.db" (0x30DC90)
//   DbAccessor  0x33E080 ("SceShellLsdbDbAcc"); Init 0x33E210; openDb 0x33E4C0
//   version     tbl_version rows 'appbrowsever' (114) and 'locale' (system language):
//               read 0x33F540, check 0x33EAA0, write 0x33F0A0; 'access_index' 0x342FF0
//   schema      0x304CD0 (tbl_version) -> 0x304D40: per registered user (5EiQCnL2G1Y,
//               20 ids) 0x3064F0 tbl_addon_%010d, 0x306A00 tbl_info_%010d, 0x284790
//               tbl_iconinfo_%010d, 0x25A3E0 tbl_concepticoninfo_%010d; then 0x277970
//               tbl_contentinfo and 0x2C7CD0 tbl_conceptmetadata.
#include "eboot/app_browse.h"

#include <algorithm>
#include <cstdio>
#include <filesystem>

#include <sqlite3.h>

#include "core/log.h"
#include "host/boundary.h"
#include "host/context.h"
#include "libSceRegMgr/regmgr.h"
#include "shellcore/user_service.h"

namespace ssn::eboot::appbrowse {
namespace fs = std::filesystem;

namespace {

// The CREATE statements (exact text of the eboot; column lists at 0xB39BC9, 0xB35460,
// 0xB05ECC, 0xB1FC52, 0xB4EC72, 0xB0E632).
constexpr const char* kVersionTable =
    "CREATE TABLE IF NOT EXISTS tbl_version(category TEXT NOT NULL,status INT,PRIMARY KEY(category));";
constexpr const char* kContentInfoColumns =
    "(titleId TEXT NOT NULL,contentId TEXT,titleName TEXT COLLATE NOCASE,metaDataPath TEXT,lastAccessTime DATETIME,"
    "contentStatus INT,contentLocation INT,ps4parentalLevel INT,sortPriority INT,pathInfo INT,lastAccessIndex INT,"
    "dispLocation INT,uninstallable INT,ps4contentType INT,pathInfo2 INT,size INT,promoteTime DATETIME,installTime "
    "DATETIME,viewCategory TEXT,ps4disableLiveDetail INT,ps4serviceIdAddCont1 TEXT,ps4serviceIdAddCont2 TEXT,"
    "ps4serviceIdAddCont3 TEXT,ps4serviceIdAddCont4 TEXT,ps4serviceIdAddCont5 TEXT,ps4serviceIdAddCont6 TEXT,"
    "ps4serviceIdAddCont7 TEXT,hddLocation INT,externalHddAppStatus INT,mTime DATETIME,sizeOtherHdd INT,"
    "appFormatType INT,categoryType INT,ps4Attribute INT,pprAttribute3 INT,attributeInternal INT,conceptId INT,"
    "localConceptId TEXT,pprDeeplinkUri TEXT,pprHubAppUri TEXT,platform INT,installStatus INT,snd0Info TEXT,"
    "icon0Info TEXT,pic0Info TEXT,pic1Info TEXT,discStatus INT,pprAgeLevel TEXT,appType INT,primaryTitleSort INT,"
    "bgMatteType TEXT,bgImageId TEXT,appinfoContentType INT,contentBadgeType INT,is3rdPartyVideoApp INT,"
    "upgradePs5TitleIdList TEXT,packageLocation INT,AppInfoJson TEXT,PRIMARY KEY(titleId));";
constexpr const char* kConceptMetadataColumns =
    "(localConceptId TEXT NOT NULL,conceptId INT,conceptName TEXT,metaDataPath TEXT,lastAccessTime DATETIME,"
    "lastAccessIndex INT,sortPriority INT,pathInfo INT,pathInfo2 INT,dispLocation INT,totalSize INT,hddLocation INT,"
    "externalHddAppStatus INT,contentLocation INT,installStatus INT,pprDeeplinkUri TEXT,pprHubAppUri TEXT,icon0Info "
    "TEXT,pic0Info TEXT,logoImageInfo TEXT,publisherName TEXT,description TEXT,minimumAge INT,type TEXT,genres TEXT,"
    "spaceType INT,scpDataStored INT,mergedOperationCapability INT,mergedContentsStatus INT,mergedPromoteTime "
    "DATETIME,mergedInstallTime DATETIME,mergedAppInfoContentType INT,PRIMARY KEY(localConceptId));";
constexpr const char* kIconInfoColumns =
    "(titleId TEXT NOT NULL,titleName TEXT COLLATE NOCASE,localConceptId TEXT,conceptId INT,lastAccessTime DATETIME,"
    "lastAccessIndex INT,lastPlayedDate DATETIME,lastPlayedIndex INT,lastPlayedDateOnConsole DATETIME,"
    "recentActivityDatePlayedOrPurchased DATETIME,recentActivityDatePlayedOrInstalled DATETIME,installedDate "
    "DATETIME,installStatus INT,dispLocation INT,visible INT,externalLaunchFlag INT,psNowFlag INT,deeplinkUri TEXT,"
    "hubAppUri TEXT,platform INT,appDrmType INT,betaFlag INT,contentBadgeType INT,discStatus INT,primaryTitleSort "
    "INT,contentId TEXT,utmLocalConceptId TEXT,utmConceptId INT,utmCatalogConceptId INT,utmTitleName TEXT COLLATE "
    "NOCASE,utmPlatform INT,utmEstimatedSize INT,utmIconUrl TEXT,utmBgImageUrl TEXT,utmAgeLevel TEXT,utmType TEXT,"
    "utmSpaceType INT,utmDrmType INT,utmContentBadgeType INT,utmBetaFlag INT,utmRevisionId TEXT,utmScpDataStored "
    "INT,entEntitlement INT,entConceptId INT,entLastUpdateTime TEXT,entPlayabilityDate DATETIME,entContentType TEXT,"
    "entSkuId TEXT,entPrimaryParentStatus INT,entRelatedTitles1 TEXT,entRelatedTitles2 TEXT,entRelatedTitles3 TEXT,"
    "entRelatedTitles4 TEXT,entRelatedTitles5 TEXT,entRelatedTitles6 TEXT,entRelatedTitles7 TEXT,entActiveDate "
    "DATETIME,entId TEXT,entFreePsPlusContent INT,entActiveFlag INT,entHidden INT,entPurchasedDate DATETIME,"
    "entStatusType INT,entFilTypeEnt INT,entUniId TEXT, entUniActiveFlag INT, entStrId TEXT, entStrActiveFlag INT, "
    "entPreorderPlaceholderFlag INT, entGatingEntitlementJson TEXT, gltlastPlayedDate DATETIME,gltTotalPlayedTime "
    "INT,gltConceptId INT,gltPlayCount INT,gltCategory TEXT,emsPushedFlag INT,emsDeletedFlag INT,PRIMARY "
    "KEY(titleId));";
constexpr const char* kConceptIconInfoColumns =
    "(localConceptId TEXT NOT NULL,validFlag INT,conceptId INT,conceptName TEXT COLLATE NOCASE,lastAccessTime "
    "DATETIME,lastAccessIndex INT,lastPlayedDate DATETIME,lastPlayedDateOnConsole DATETIME,totalPlayedTime INT,"
    "recentActivityDatePlayedOrPurchased DATETIME,recentActivityDatePlayedOrInstalled DATETIME,primaryTitleId TEXT,"
    "primaryTitleName TEXT,primaryTitlePlatform INT,primaryTitleBgMatteType TEXT,lastInteractedTime DATETIME,"
    "dispLocation INT,installedStatus INT,installedStatusPerPlatform INT,entitlementStatus INT,filterTypeEntitlement "
    "INT,filterTypeHidden INT,totalEstimatedSize INT,purchasedDate DATETIME,promoteTime DATETIME,installTime "
    "DATETIME,psNowFlag INT,deeplinkUri TEXT,hubAppUri TEXT,metaDataPath TEXT,icon0Info TEXT,pic0Info TEXT,snd0Info "
    "TEXT,platform INT,minimumAge INT,publisherName TEXT,descriptions TEXT,type TEXT,genres TEXT,genreFlags INT,"
    "spaceType INT,packageLocation TEXT,ucmConceptId INT,ucmIcon0Info TEXT,ucmPic0Info TEXT,ucmLogoUrl TEXT,"
    "ucmMinimumAge INT,ucmPublisherName TEXT,ucmDescriptions TEXT,ucmType TEXT,ucmGenres TEXT,ucmSpaceType INT,"
    "ucmScpDataStored INT,ucmConceptName TEXT,PRIMARY KEY(localConceptId));";
constexpr const char* kAddonColumns =
    "(entitlementId TEXT NOT NULL,titleId TEXT,conceptId INT,packageType TEXT,subType TEXT,activeDate "
    "DATETIME,PRIMARY KEY(entitlementId));";
constexpr const char* kInfoColumns = "(key TEXT NOT NULL,value TEXT,PRIMARY KEY(key));";

std::string user_table(const char* prefix, int32_t user_id) {
  char buf[48];
  std::snprintf(buf, sizeof buf, "%s%010d", prefix, user_id);
  return buf;
}

int32_t sqlite_error(int rc) {
  // dbutil maps SQLite results to 0x809C0200 | rc (0x3AF3B0 / 0x3A9500).
  return rc == SQLITE_OK || rc == SQLITE_DONE || rc == SQLITE_ROW ? 0 : static_cast<int32_t>(0x809C0200u | static_cast<uint32_t>(rc));
}

int32_t bind(sqlite3_stmt* st, int idx, const Field& f) {
  if (f.type == 0) return sqlite3_bind_int64(st, idx, f.i);
  if (f.type == 2 && !f.s.empty()) return sqlite3_bind_text(st, idx, f.s.data(), static_cast<int>(f.s.size()), SQLITE_TRANSIENT);
  return sqlite3_bind_null(st, idx);  // 0x3AF3B0 type 3 with length 0 / type 5
}

int32_t run(const std::string& sql, const std::vector<const Field*>& binds) {
  Db& d = db();
  if (!d.conn) return kErrNotInitialized;
  sqlite3_stmt* st = nullptr;
  int rc = sqlite3_prepare_v2(d.conn, sql.c_str(), -1, &st, nullptr);
  if (rc != SQLITE_OK) {
    log::error("appbrowse", "prepare failed ({}): {} [{}]", rc, sqlite3_errmsg(d.conn), sql);
    return sqlite_error(rc);
  }
  for (size_t i = 0; i < binds.size(); ++i) bind(st, static_cast<int>(i + 1), *binds[i]);
  rc = sqlite3_step(st);
  sqlite3_finalize(st);
  if (rc != SQLITE_DONE && rc != SQLITE_ROW) {
    log::error("appbrowse", "step failed ({}): {} [{}]", rc, sqlite3_errmsg(d.conn), sql);
    return sqlite_error(rc);
  }
  return 0;
}

}  // namespace

Db& db() {
  static Db d;
  return d;
}

std::string app_db_host_path() {
  // HOST VALUE: the sandbox word's tree is <cache>/system_data (see
  // libScePsm/environment/system_parameters.cpp).
  return (fs::path(host::ctx().cache_dir) / "system_data" / "priv" / "mms" / "app.db").string();
}

std::string iconinfo_table(int32_t user_id) { return user_table("tbl_iconinfo_", user_id); }
std::string concepticoninfo_table(int32_t user_id) { return user_table("tbl_concepticoninfo_", user_id); }

int32_t system_language() {
  int32_t v = 0;
  regmgr::get_int(0x02020000u, &v);  // 0x345A50
  return v;
}

int32_t exec(const std::string& sql) { return run(sql, {}); }

int32_t select_record(const std::string& table, const char* key_column, const std::string& key, Record* out) {
  Db& d = db();
  out->clear();
  if (!d.conn) return kErrNotInitialized;
  const std::string sql = "SELECT * FROM " + table + " WHERE " + key_column + "=?1";
  sqlite3_stmt* st = nullptr;
  int rc = sqlite3_prepare_v2(d.conn, sql.c_str(), -1, &st, nullptr);
  if (rc != SQLITE_OK) return sqlite_error(rc);
  sqlite3_bind_text(st, 1, key.data(), static_cast<int>(key.size()), SQLITE_TRANSIENT);
  rc = sqlite3_step(st);
  int32_t r = kErrNotFound;
  if (rc == SQLITE_ROW) {
    for (int c = 0; c < sqlite3_column_count(st); ++c) {
      const char* name = sqlite3_column_name(st, c);
      switch (sqlite3_column_type(st, c)) {
        case SQLITE_INTEGER: (*out)[name] = Field::integer(sqlite3_column_int64(st, c)); break;
        case SQLITE_TEXT: (*out)[name] = Field::string(reinterpret_cast<const char*>(sqlite3_column_text(st, c))); break;
        default: break;
      }
    }
    r = 0;
  } else if (rc != SQLITE_DONE) {
    r = sqlite_error(rc);
  }
  sqlite3_finalize(st);
  return r;
}

int32_t select_keys(const std::string& table, const char* key_column, const std::string& where, std::vector<std::string>* out) {
  Db& d = db();
  out->clear();
  if (!d.conn) return kErrNotInitialized;
  std::string sql = std::string("SELECT ") + key_column + " FROM " + table;
  if (!where.empty()) sql += " WHERE " + where;
  sqlite3_stmt* st = nullptr;
  int rc = sqlite3_prepare_v2(d.conn, sql.c_str(), -1, &st, nullptr);
  if (rc != SQLITE_OK) return sqlite_error(rc);
  while ((rc = sqlite3_step(st)) == SQLITE_ROW)
    if (const unsigned char* t = sqlite3_column_text(st, 0)) out->emplace_back(reinterpret_cast<const char*>(t));
  sqlite3_finalize(st);
  return rc == SQLITE_DONE ? 0 : sqlite_error(rc);
}

int32_t insert_record(const std::string& table, const char* key_column, const std::string& key, const Record& r) {
  // 0x264300: "INSERT INTO " + table + "(titleId" + ",col".. + ") VALUES(?1" + ",?n".. + ")".
  std::string cols = std::string("(") + key_column, vals = ") VALUES(?1";
  const Field k = Field::string(key);
  std::vector<const Field*> binds{&k};
  int n = 1;
  for (const auto& [c, f] : r) {
    if (c == key_column || !f.present()) continue;
    cols += "," + c;
    vals += ",?" + std::to_string(++n);
    binds.push_back(&f);
  }
  return run("INSERT INTO " + table + cols + vals + ")", binds);
}

int32_t update_record(const std::string& table, const char* key_column, const std::string& key, const Record& r) {
  // 0x2649B0: "UPDATE " + table + " SET " + "col=?2" + ",col=?n".. + " WHERE titleId=?1".
  const Field k = Field::string(key);
  std::vector<const Field*> binds{&k};
  std::string set;
  int n = 1;
  for (const auto& [c, f] : r) {
    if (c == key_column || !f.present()) continue;
    if (!set.empty()) set += ",";
    set += c + "=?" + std::to_string(++n);
    binds.push_back(&f);
  }
  if (set.empty()) return 0;
  return run("UPDATE " + table + " SET " + set + " WHERE " + key_column + "=?1", binds);
}

int32_t delete_record(const std::string& table, const char* key_column, const std::string& key) {
  const Field k = Field::string(key);
  return run("DELETE FROM " + table + " WHERE " + key_column + "=?1", {&k});
}

int32_t get_version_value(const std::string& category, int64_t* v) {
  // 0x342E00: "SELECT category, status FROM tbl_version WHERE category = ?1".
  Record r;
  const int32_t e = select_record("tbl_version", "category", category, &r);
  if (e != 0) return e;
  *v = r["status"].int_or(0);
  return 0;
}

int32_t set_version_value(const std::string& category, int64_t v) {
  // 0x301CA0 / 0x342FF0: "REPLACE INTO tbl_version VALUES('%s', %ld)".
  char buf[160];
  std::snprintf(buf, sizeof buf, "REPLACE INTO tbl_version VALUES('%s', %lld)", category.c_str(), static_cast<long long>(v));
  return exec(buf);
}

int32_t create_user_info_tables(int32_t user_id) {
  // 0x3064C0: tbl_addon_ (0x3064F0), then tbl_info_ (0x306A00) when it succeeded.
  int32_t r = exec("CREATE TABLE IF NOT EXISTS " + user_table("tbl_addon_", user_id) + " " + kAddonColumns);
  if (r >= 0) r = exec("CREATE TABLE IF NOT EXISTS " + user_table("tbl_info_", user_id) + " " + kInfoColumns);
  return r;
}

int32_t create_icon_table(int32_t user_id) {
  // 0x28DC90 -> 0x284790: "CREATE TABLE " + tbl_iconinfo_%010d + " " + columns, when the user's
  // table is not in the list (0x2842A0 loads it from sqlite_master); the list records it on
  // success.
  Db& d = db();
  int32_t r = 0;
  if (std::find(d.icon_tables.begin(), d.icon_tables.end(), user_id) == d.icon_tables.end()) {
    r = exec("CREATE TABLE " + iconinfo_table(user_id) + " " + kIconInfoColumns);
    if (r == 0) d.icon_tables.push_back(user_id);
  }
  return r;
}

int32_t create_concept_icon_table(int32_t user_id) {
  // 0x2619B0 -> 0x25A3E0: likewise tbl_concepticoninfo_%010d.
  Db& d = db();
  int32_t r = 0;
  if (std::find(d.concept_icon_tables.begin(), d.concept_icon_tables.end(), user_id) == d.concept_icon_tables.end()) {
    r = exec("CREATE TABLE " + concepticoninfo_table(user_id) + " " + kConceptIconInfoColumns);
    if (r == 0) d.concept_icon_tables.push_back(user_id);
  }
  return r;
}

int32_t create_user_tables(int32_t user_id) {
  // 0x304D40, per registered user: 0x3064F0 / 0x306A00, 0x28DC90, 0x2619B0 (its result).
  create_user_info_tables(user_id);
  create_icon_table(user_id);
  return create_concept_icon_table(user_id);
}

int32_t create_schema() {
  // 0x304CD0
  int32_t r = exec(kVersionTable);
  if (r != 0) return r;
  // 0x304D40: sceUserServiceGetRegisteredUserIdList (5EiQCnL2G1Y), 20 entries.
  int32_t ids[shellcore::users::kListSize];
  shellcore::users::registered_user_ids(ids);
  for (int32_t id : ids)
    if (id != -1) r = create_user_tables(id);
  r = exec(std::string("CREATE TABLE IF NOT EXISTS tbl_contentinfo ") + kContentInfoColumns);      // 0x277970
  r = exec(std::string("CREATE TABLE IF NOT EXISTS tbl_conceptmetadata ") + kConceptMetadataColumns);  // 0x2C7CD0
  return r;
}

}  // namespace ssn::eboot::appbrowse
