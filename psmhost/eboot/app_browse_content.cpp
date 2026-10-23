// ShellUI eboot AppBrowse: tbl_contentinfo from SceShellCore's AppInfo (FW 4.03).
//
//   appinfo_sync::InsertTransactionCommand execute 0x327800:
//     0x315130/0x315510  the AppInfo of the title: GetAppInfo(titleId, "prior%") (0x274C80)
//     0x315620           the stored row (0x278AA0 GetRecord): absent -> a new record
//     0x322E60           0x318150 fields, 0x321D10 lastAccess* for a new record
//     0x3234E0           packageLocation (the command's flags from 0x3260B0)
//     0x315AC0           upgradePs5TitleIdList -> the command's disc-status map
//     0x315660           0x278B80 UPDATE, or 0x278440 INSERT when the row is absent; both
//                        notify the ContentInfo subject's observers (0x27BB20 / 0x27BD40)
//   appinfo_sync::DeleteTransactionCommand 0x312510 -> 0x315700 -> 0x279320.
#include <cstdio>
#include <filesystem>
#include <ctime>
#include <map>
#include <sstream>

#include "core/log.h"
#include "eboot/app_browse.h"
#include "host/boundary.h"
#include "libSceUrlConfigResolver/sce_json.h"
#include "shellcore/app_db_internal.h"

namespace ssn::eboot::appbrowse {
namespace appdb = ssn::shellcore::appdb;
namespace json = ssn::urlcfg::json;

namespace {

const Value* find(const AppInfo& a, const char* key) {
  auto it = a.values.find(key);
  return it == a.values.end() ? nullptr : &it->second;
}

// The pattern of 0x318150: count(key) == 0 -> default; else the value of the wanted type,
// otherwise the default.
int64_t int_or(const AppInfo& a, const char* key, int64_t dflt, int64_t wrong_type = 0) {
  const Value* v = find(a, key);
  if (!v) return dflt;
  return v->type == 0 ? v->i : wrong_type;
}
std::string str_or(const AppInfo& a, const char* key) {
  const Value* v = find(a, key);
  return v && v->type == 2 ? v->s : std::string();
}
void put_int(Record& r, const char* col, int64_t v) { r[col] = Field::integer(v); }
void put_str(Record& r, const char* col, std::string v) { r[col] = Field::string(std::move(v)); }

// 0x23D270: the language's key — "TITLE" + "_%02d" for the languages of table 0xCDB3A0 (ids
// 0..29), else the key itself.
std::string localized_key(const std::string& key, int32_t lang) {
  if (lang < 0 || lang > 29) return key;
  char buf[8];
  std::snprintf(buf, sizeof buf, "_%02d", lang);
  return key + buf;
}

// 0x23D980 (AppInfo localized string, 0x26F150 "TITLE"): the system language's value when
// it is a non-empty string, else the key's own value, else "".
std::string localized_string(const AppInfo& a, const std::string& key) {
  const std::string lk = localized_key(key, system_language());
  if (const Value* v = find(a, lk.c_str()); v && v->type == 2 && !v->s.empty()) return v->s;
  if (const Value* v = find(a, key.c_str()); v && v->type == 2) return v->s;
  return {};
}

// 0x239840 / 0x23A070 / 0x23B460: <_metadata_path><base>[_%02d]<ext>[?ts=<time stamp>] from
// the _path_*_info bit mask the SceShellCore content-path check stored (bit = language,
// bit 63 = the unlocalized file, bits 60/61/62 = .png/.dds/.jpg).
int32_t content_path(const AppInfo& a, const char* base, const char* mask_key, const char* ts_key, std::string* out) {
  out->clear();
  const Value* md = find(a, "_metadata_path");  // 0x239ED0
  if (!md) return kErrNotFound;
  const Value* m = find(a, mask_key);  // 0x239B10: 0x23D610 lookup
  if (!m) return kErrNotFound;
  const uint64_t mask = m->type == 0 ? static_cast<uint64_t>(m->i) : 0;
  const char* ext = nullptr;
  if (mask & 0x4000000000000000ull) ext = ".jpg";
  else if (mask & 0x2000000000000000ull) ext = ".dds";
  else if (mask & 0x1000000000000000ull) ext = ".png";
  const std::string dir = md->type == 2 ? md->s : std::string();
  const int32_t lang = system_language();
  std::string path;
  if (lang < 0) {
    path = dir + base + (ext ? ext : "");
  } else if (lang <= 29 && (mask >> lang) & 1) {
    char sfx[8];
    std::snprintf(sfx, sizeof sfx, "_%02d", lang);
    path = dir + base + sfx + (ext ? ext : "");
  } else if (static_cast<int64_t>(mask) < 0) {
    path = dir + base + (ext ? ext : "");
  } else {
    return 0;  // ""
  }
  *out = path;
  // 0x23B460: "?ts=%ld" from the time stamp key (0x23B800 lookup; absent -> 0x80F50005).
  const Value* ts = find(a, ts_key);
  if (!ts) return kErrNotFound;
  char buf[32];
  std::snprintf(buf, sizeof buf, "?ts=%lld", static_cast<long long>(ts->type == 0 ? ts->i : 0));
  *out += buf;
  return 0;
}

// 0x23A8B0 snd0Info: <dir>/snd0.at9 when the file exists (0x23B280 -> 0x3B3910 stat).
int32_t snd0_path(const AppInfo& a, std::string* out) {
  out->clear();
  const Value* md = find(a, "_metadata_path");
  if (!md) return kErrNotFound;
  const std::string p = (md->type == 2 ? md->s : std::string()) + "/snd0" + ".at9";
  std::error_code ec;  // 0x3B3910: stat succeeds and the mode is >= 0x4000
  const auto st = std::filesystem::status(appdb::host_path(p), ec);
  if (ec || (st.type() != std::filesystem::file_type::regular && st.type() != std::filesystem::file_type::directory)) return 0;
  *out = p;
  const Value* ts = find(a, "_path_snd0_info_time_stamp");
  if (!ts) return kErrNotFound;
  char buf[32];
  std::snprintf(buf, sizeof buf, "?ts=%lld", static_cast<long long>(ts->type == 0 ? ts->i : 0));
  *out += buf;
  return 0;
}

// 0x26F880: AppInfoContentType from METADATA_ID.
int32_t appinfo_content_type(const AppInfo& a, int32_t* t) {
  *t = 0;
  const std::string md = str_or(a, "METADATA_ID");
  if (md.empty()) {
    host::note_unimplemented("eboot AppBrowse AppInfoContentType without METADATA_ID (0x26FB90) not reproduced");
    return 0;
  }
  if (md.find(":internal:") != std::string::npos) *t = 1;
  else if (md.find(":external:") != std::string::npos) *t = 2;
  else if (md.find(":psnow") != std::string::npos) *t = 3;
  else if (md.find(":shellapp") != std::string::npos) *t = 4;
  else if (md.find(":push") != std::string::npos) *t = 5;
  else if (md.find(":shareplay") != std::string::npos) *t = 6;
  else host::note_unimplemented("eboot AppBrowse AppInfoContentType fallback (0x26FB90) not reproduced");
  return 0;
}

}  // namespace

// AppInfo vtable +0 (0x268F70 ToJson): {"parserId":0,"mimeType":0,"mimeTypeFormatVersion":0,
// "promoterFormatVersion":0,"field_list":[{"key","type"(0 int / 2 string),"size"(8 / length),
// "data"}...]} in the AppInfo map's key order, serialized compact (sce::Json, sorted keys).
std::string appinfo_json(const AppInfo& a) {
  json::Array fields;
  for (const auto& [k, v] : a.values) {
    json::Object f;
    f["key"] = json::Value::make_string(k);
    if (v.type == 2) {
      f["type"] = json::Value::make_int(2);
      f["size"] = json::Value::make_int(static_cast<int64_t>(v.s.size()));
      f["data"] = json::Value::make_string(v.s);
    } else {
      f["type"] = json::Value::make_int(0);
      f["size"] = json::Value::make_int(8);
      f["data"] = json::Value::make_int(v.i);
    }
    fields.push_back(json::Value::make_object(std::move(f)));
  }
  json::Object o;
  o["parserId"] = json::Value::make_int(0);
  o["mimeType"] = json::Value::make_int(0);
  o["mimeTypeFormatVersion"] = json::Value::make_int(0);
  o["promoterFormatVersion"] = json::Value::make_int(0);
  o["field_list"] = json::Value::make_array(std::move(fields));
  return json::serialize(json::Value::make_object(std::move(o)));
}

namespace {

// 0x3224F0: titleName, snd0Info, icon0Info, pic0Info, pic1Info.
int32_t set_titles_and_paths(const AppInfo& a, Record& r) {
  put_str(r, "titleName", localized_string(a, "TITLE"));  // 0x26F150
  std::string s;
  int32_t e = snd0_path(a, &s);  // 0x26DA90
  if (e != 0 && e != kErrNotFound) return e;
  put_str(r, "snd0Info", s);
  if (find(a, "#catalog_path_icon0_info"))  // 0x26B090 -> 0x269E10
    host::note_unimplemented("eboot AppBrowse catalog icon0 path (0x269E10) not reproduced");
  e = content_path(a, "/icon0", "_path_icon0_info", "_path_icon0_info_time_stamp", &s);
  if (e != 0 && e != kErrNotFound) return e;
  put_str(r, "icon0Info", s);
  if (find(a, "#catalog_path_pic0_info"))  // 0x26BF10
    host::note_unimplemented("eboot AppBrowse catalog pic0 path (0x269E10) not reproduced");
  e = content_path(a, "/pic0", "_path_pic0_info", "_path_pic0_info_time_stamp", &s);
  if (e != 0 && e != kErrNotFound) return e;
  put_str(r, "pic0Info", s);
  e = content_path(a, "/pic1", "_path_pic1_info", "_path_pic1_info_time_stamp", &s);  // 0x26CB60
  if (e != 0 && e != kErrNotFound) return e;
  put_str(r, "pic1Info", s);
  return 0;
}

// 0x317FA0 on SERVICE_ID_ADDCONT_ADD_n: the value becomes "" (the function returns an empty
// string whatever it is given).
std::string service_id(const AppInfo& a, const char* key) {
  (void)a;
  (void)key;
  return {};
}

}  // namespace

int32_t build_content_record(const AppInfo& a, bool is_new, Record* rec) {
  Record& r = *rec;
  // 0x318150
  if (!find(a, "_app_format_type")) return 0;
  const int64_t fmt = int_or(a, "_app_format_type", 0);
  put_int(r, "appFormatType", fmt);
  put_str(r, "contentId", str_or(a, "CONTENT_ID"));
  if (int32_t e = set_titles_and_paths(a, r); e != 0) log::warn("appbrowse", "0x3224F0: 0x{:08x}", static_cast<uint32_t>(e));
  put_str(r, "metaDataPath", str_or(a, "_metadata_path"));
  put_int(r, "contentStatus", static_cast<int32_t>(int_or(a, "#_contents_status", 0)));
  put_int(r, "contentLocation", static_cast<int32_t>(int_or(a, "_contents_location", 0)));
  if (fmt == 0) put_int(r, "ps4parentalLevel", static_cast<int32_t>(int_or(a, "PARENTAL_LEVEL", 0)));
  put_int(r, "sortPriority", static_cast<int32_t>(int_or(a, "_sort_priority", 100)));
  put_int(r, "pathInfo", int_or(a, "_path_info", 0));
  const int32_t disp = static_cast<int32_t>(int_or(a, "DISPLAYLOCATION", 0));
  put_int(r, "dispLocation", disp);
  {
    const Value* v = find(a, "_uninstallable");
    put_int(r, "uninstallable", v ? (v->type == 0 && v->i != 0) : 1);
  }
  if (fmt == 0) {
    int64_t t = 0;
    if (const Value* v = find(a, "_with_ps3_disc")) t = v->type == 0 && v->i == 1;
    if (const Value* v = find(a, "ATTRIBUTE_INTERNAL"); v && v->type == 0 && (v->i & 2)) t += 2;
    if (const Value* v = find(a, "_contents_ext_type")) {
      const int64_t x = v->type == 0 ? v->i : 0;
      if (x == 3) t |= 0x10;
      else if (x == 2) t |= 8;
      else if (x == 1) t |= 4;
    }
    put_int(r, "ps4contentType", t);
  }
  put_int(r, "pathInfo2", int_or(a, "_path_info_2", 0));
  put_int(r, "size", int_or(a, "#_size", 0));
  put_str(r, "promoteTime", str_or(a, "#_promote_time"));
  // installTime: #_install_time, else #_promote_time, else "".
  put_str(r, "installTime", find(a, "#_install_time") ? str_or(a, "#_install_time") : str_or(a, "#_promote_time"));
  {
    std::string vc;
    if (const Value* v = find(a, "_view_category")) {
      const int64_t x = v->type == 0 ? v->i : 0;
      vc = x == 1 ? "app" : x == 0 ? "game" : "";
    }
    put_str(r, "viewCategory", vc);
  }
  if (fmt == 0) {
    put_int(r, "ps4disableLiveDetail", static_cast<int32_t>(int_or(a, "_disable_live_detail", 0)));
    static const char* const kAddCont[7][2] = {
        {"SERVICE_ID_ADDCONT_ADD_1", "ps4serviceIdAddCont1"}, {"SERVICE_ID_ADDCONT_ADD_2", "ps4serviceIdAddCont2"},
        {"SERVICE_ID_ADDCONT_ADD_3", "ps4serviceIdAddCont3"}, {"SERVICE_ID_ADDCONT_ADD_4", "ps4serviceIdAddCont4"},
        {"SERVICE_ID_ADDCONT_ADD_5", "ps4serviceIdAddCont5"}, {"SERVICE_ID_ADDCONT_ADD_6", "ps4serviceIdAddCont6"},
        {"SERVICE_ID_ADDCONT_ADD_7", "ps4serviceIdAddCont7"}};
    for (const auto& kc : kAddCont) put_str(r, kc[1], service_id(a, kc[0]));
  }
  put_int(r, "hddLocation", int_or(a, "_hdd_location", 0));
  put_int(r, "externalHddAppStatus", int_or(a, "_external_hdd_app_status", 0));
  put_str(r, "mTime", str_or(a, "#_mtime"));
  put_int(r, "sizeOtherHdd", int_or(a, "_size_other_hdd", 0));
  put_int(r, "categoryType", static_cast<int32_t>(int_or(a, "CATEGORY_TYPE", 0)));
  if (fmt == 0) put_int(r, "ps4Attribute", static_cast<int32_t>(int_or(a, "ATTRIBUTE", 0)));
  if (fmt == 1) put_int(r, "pprAttribute3", static_cast<int32_t>(int_or(a, "ATTRIBUTE3", 0)));
  put_int(r, "attributeInternal", static_cast<int32_t>(int_or(a, "ATTRIBUTE_INTERNAL", 0)));
  put_int(r, "conceptId", int_or(a, "CONCEPT_ID", 0));
  put_str(r, "localConceptId", str_or(a, "_local_concept_id"));
  put_str(r, "pprDeeplinkUri", str_or(a, "DEEPLINK_URI"));
  put_str(r, "pprHubAppUri", str_or(a, "HUBAPP_URI"));
  put_int(r, "platform", int_or(a, "_ps_platform", 0));
  put_int(r, "installStatus", int_or(a, "_install_status", 0));
  put_int(r, "discStatus", int_or(a, "DISC_STATUS", 0));
  if (fmt == 1) put_str(r, "pprAgeLevel", str_or(a, "AGE_LEVEL"));
  put_int(r, "appType", static_cast<int32_t>(int_or(a, "APP_TYPE", 0)));
  put_int(r, "primaryTitleSort", static_cast<int32_t>(int_or(a, "_primary_title_sort", 0)));
  put_str(r, "bgMatteType", str_or(a, "BACKGROUND_BASEMAT_TYPE"));
  put_str(r, "bgImageId", str_or(a, "BACKGROUND_IMAGE_ID"));
  int32_t ct = 0;
  if (int32_t e = appinfo_content_type(a, &ct); e != 0) return e;  // 0x26F870
  put_int(r, "appinfoContentType", ct);
  put_int(r, "contentBadgeType", static_cast<int32_t>(int_or(a, "CONTENT_BADGE_TYPE", 0)));
  {
    bool third = fmt == 1 && (disp & 0x20) != 0;
    if (fmt == 0)
      if (const Value* v = find(a, "DISP_LOCATION_2"); v && v->type == 0) third = (v->i & 2) != 0 || third;
    put_int(r, "is3rdPartyVideoApp", third);
  }
  put_str(r, "upgradePs5TitleIdList", str_or(a, "_upgrade_ps5_title_id_list"));
  put_str(r, "AppInfoJson", appinfo_json(a));  // AppInfo vtable +0 (0x268F70)

  // 0x321D10, for a record the database does not hold yet.
  if (is_new) {
    put_str(r, "lastAccessTime", find(a, "#_install_time") ? str_or(a, "#_install_time") : str_or(a, "#_promote_time"));
    put_int(r, "lastAccessIndex", int_or(a, "#_access_index", 0));
  }
  return 0;
}

namespace {

// 0x3260B0: the metadata ids of the title's AppInfo rows -> 1 "internal", 2 "external".
int32_t package_location_flags(const std::string& title_id, int32_t* flags) {
  *flags = 0;
  appdb::ConditionChain cond;
  cond.conditions.push_back({"TITLE_ID", {Value::string(title_id)}, 0, 0});
  std::vector<appdb::PrimaryKey> pairs;
  const int32_t r = appdb::search(cond, {}, 0, 1, nullptr, &pairs);  // 0x276480
  if (r != 0) return r;
  if (pairs.empty()) return kErrNotFound;
  for (const auto& p : pairs) {
    if (p.metadata_id.find("internal") != std::string::npos) *flags |= 1;
    if (p.metadata_id.find("external") != std::string::npos) *flags |= 2;
  }
  return 0;
}

}  // namespace

int32_t sync_title(const std::string& title_id, int32_t package_location) {
  Db& d = db();
  std::lock_guard lock(d.mutex);
  // 0x315510: GetAppInfo(titleId, "prior%").
  AppInfo info;
  int32_t r = appdb::get_app_info_value_list(title_id, "prior", {}, 0, &info);
  if (r != 0) return r == appdb::kErrNotFound ? kErrNotFound : r;
  Record existing;
  const int32_t g = select_record("tbl_contentinfo", "titleId", title_id, &existing);  // 0x278AA0
  if (g != 0 && g != kErrNotFound) return g;
  const bool is_new = g == kErrNotFound;
  Record rec;
  rec["titleId"] = Field::string(title_id);
  if ((r = build_content_record(info, is_new, &rec)) != 0) return r;  // 0x322E60
  rec["packageLocation"] = Field::integer(package_location);           // 0x3234E0
  const std::string upgrade = rec["upgradePs5TitleIdList"].str_or_empty();
  if (!upgrade.empty())  // 0x315AC0 -> 0x326540 UpdateIconInfoDiscStatus
    host::note_unimplemented("eboot AppBrowse upgradePs5TitleIdList disc status (0x315AC0 / 0x326540) not reproduced");
  if (!is_new) {
    // 0x278B80: UPDATE, then the subject's observers with the stored and the new row.
    if ((r = update_record("tbl_contentinfo", "titleId", title_id, rec)) != 0) return r;
    Record after;
    select_record("tbl_contentinfo", "titleId", title_id, &after);
    return on_content_changed(kUpdate, title_id, &existing, &after);
  }
  // 0x278440: INSERT, then the observers.
  if ((r = insert_record("tbl_contentinfo", "titleId", title_id, rec)) != 0) return r;
  Record after;
  select_record("tbl_contentinfo", "titleId", title_id, &after);
  return on_content_changed(kInsert, title_id, nullptr, &after);
}

int32_t delete_title(const std::string& title_id) {
  // 0x315700 -> 0x279320: "DELETE FROM tbl_contentinfo WHERE " + titleId, observers notified.
  Db& d = db();
  std::lock_guard lock(d.mutex);
  Record before;
  int32_t r = select_record("tbl_contentinfo", "titleId", title_id, &before);
  if (r == kErrNotFound) return 0;
  if (r != 0) return r;
  if ((r = delete_record("tbl_contentinfo", "titleId", title_id)) != 0) return r;
  return on_content_changed(kDelete, title_id, &before, nullptr);
}

// appinfo_sync::RecoveryAppDbPromoteTransactionCommand (0x312140, execute 0x324BA0): the
// titles SceShellCore changed since `from_index` (sync_index > from AND < to + 1, search mode
// 0, pairs; deleted titles: mode 2) are inserted/updated or deleted, then the index is stored.
int32_t promote_appinfo(int64_t from_index, int64_t to_index) {
  appdb::ConditionChain cond;  // 0x2DD410
  cond.join = 1;
  cond.conditions.push_back({"sync_index", {Value::integer(from_index)}, 5, 0});
  cond.conditions.push_back({"sync_index", {Value::integer(to_index + 1)}, 4, 0});
  std::vector<appdb::PrimaryKey> updated;
  // 0x2AFF20: sorted by sync_index ascending.
  int32_t r = appdb::search(cond, {{"sync_index", 1}}, 0, 1, nullptr, &updated);  // 0x276480
  if (r != 0 && r != appdb::kErrNotFound) return r;
  host::note_unimplemented("eboot AppBrowse deleted-title list (search mode 2, 0x276570) not reproduced");
  std::map<std::string, int32_t> titles;  // 0x324BA0: titleId -> package location flags
  std::vector<std::string> deleted;
  for (const auto& p : updated) {
    if (titles.count(p.title_id)) continue;
    int32_t flags = 0;
    const int32_t e = package_location_flags(p.title_id, &flags);  // 0x3260B0
    if (e == kErrNotFound) deleted.push_back(p.title_id);
    else if (e == 0) titles[p.title_id] = flags;
  }
  int32_t result = 0;
  for (const auto& [tid, flags] : titles) {
    host::note_unimplemented("eboot DbRecovery progress (0x64AD10) during the AppBrowse promote not reproduced");
    if (int32_t e = sync_title(tid, flags); e != 0) {  // 0x326DB0 -> 0x327B60 -> 0x327800
      log::warn("appbrowse", "InsertTransactionCommand {}: 0x{:08x}", tid, static_cast<uint32_t>(e));
      result = e;
    }
  }
  for (const auto& tid : deleted)
    if (int32_t e = delete_title(tid); e != 0) result = e;  // 0x312510 -> 0x3238B0
  // 0x2DDAC0 + 0x315A00: appinfo_sync_index.
  Db& d = db();
  {
    std::lock_guard lock(d.mutex);
    d.appinfo_sync_index = to_index;
    set_version_value("appinfo_sync_index", to_index);
  }
  return result;
}

}  // namespace ssn::eboot::appbrowse
