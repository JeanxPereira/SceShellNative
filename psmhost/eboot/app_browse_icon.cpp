// ShellUI eboot AppBrowse: the per-user tbl_iconinfo_<userId> rows (FW 4.03).
//
// The ContentInfo table's observer in the AppBrowse manager (0x2AE770 object, sub-object
// vtable 0xCDC108, after-change slot +0x20 = 0x2E5600) runs, for every user that has an
// iconinfo table (0x28DE50), modifyAppearanceControl 0x2E56C0:
//   0x2E04F0 checkContentVisibility -> title status (0x2E0A10, record type 0x2D39A0) and
//            the visibility class of table 0xBF4380
//   0x2E1250 updateLastAccess, 0x2E1850 updateRepresentativeInfo, 0x2E4400
//            updateConceptId4AppInfo, then 0x2AF300 insert / 0x2AEC60 update of the row.
// Row writes go through the IconInfo table subject, whose AppBrowse observer 0x2EAEA0 keeps
// tbl_concepticoninfo_ (app_browse_concept.cpp). The AppBrowse manager's own IconInfo
// observer 0x2E6520 is suppressed during these writes (0x2E6B70 guard).
#include <cstdio>
#include <ctime>

#include <chrono>

#include "core/log.h"
#include "eboot/app_browse.h"
#include "host/boundary.h"
#include "libSceUrlConfigResolver/sce_json.h"
#include "shellcore/user_service.h"

namespace ssn::eboot::appbrowse {
namespace json = ssn::urlcfg::json;

namespace {

// 0x263C10 / 0x27CF30: an absent or empty record.
bool empty(const Record* r) { return !r || r->empty(); }
const Field& get(const Record* r, const char* col) {
  static const Field none;
  if (!r) return none;
  auto it = r->find(col);
  return it == r->end() ? none : it->second;
}
std::string str(const Record* r, const char* col) { return get(r, col).str_or_empty(); }

// 0x3B48F0: "YYYY-MM-DD HH:MM:SS.mmm" -> comparable tick; unparsable -> 0.
int64_t date_tick(const std::string& s) {
  int y, mo, d, h, mi, se, ms = 0;
  if (std::sscanf(s.c_str(), "%d-%d-%d %d:%d:%d.%d", &y, &mo, &d, &h, &mi, &se, &ms) < 6) return 0;
  return ((((((static_cast<int64_t>(y) * 13 + mo) * 32 + d) * 24 + h) * 60 + mi) * 60 + se) * 1000) + ms;
}
// 0x2B8170: the later of two dates (the second when not later than it).
std::string later(const std::string& a, const std::string& b) { return date_tick(a) <= date_tick(b) ? b : a; }

// 0x306FD0: now, "YYYY-MM-DD HH:MM:SS.mmm" (sceRtcGetCurrentNetworkTick, else the current
// tick; 0x3B4590 formats with ' ' between date and time).
std::string now_string() {
  // HOST VALUE: the PC clock in UTC.
  const auto now = std::chrono::system_clock::now();
  const auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(now.time_since_epoch()).count();
  const std::time_t t = static_cast<std::time_t>(ms / 1000);
  std::tm tm{};
  gmtime_s(&tm, &t);
  char buf[32];
  std::snprintf(buf, sizeof buf, "%04d-%02d-%02d %02d:%02d:%02d.%03d", tm.tm_year + 1900, tm.tm_mon + 1, tm.tm_mday, tm.tm_hour,
                tm.tm_min, tm.tm_sec, static_cast<int>(ms % 1000));
  return buf;
}

// 0x2479F0: 9 alphanumeric characters.
bool valid_title_id(const std::string& t) {
  if (t.size() != 9) return false;
  for (unsigned char c : t)
    if (!std::isalnum(c)) return false;
  return true;
}

// tbl_info_<userId> (user info, 0x21B900): "SELECT value" / "REPLACE" by key.
int32_t get_user_info(int32_t user_id, const std::string& key, std::string* value) {
  char t[32];
  std::snprintf(t, sizeof t, "tbl_info_%010d", user_id);
  Record r;
  const int32_t e = select_record(t, "key", key, &r);
  if (e != 0) return e;
  *value = r["value"].str_or_empty();
  return 0;
}
int32_t set_user_info(int32_t user_id, const std::string& key, const std::string& value) {
  char t[32];
  std::snprintf(t, sizeof t, "tbl_info_%010d", user_id);
  // 0x21BC50
  if (exec(std::string("DELETE FROM ") + t + " WHERE key='" + key + "'") != 0) return kErrInternal;
  Record r;
  r["value"] = Field::string(value);
  return insert_record(t, "key", key, r);
}

// 0x2656D0 -> 0x26F880: AppInfoContentType of the content row (from its AppInfoJson's
// METADATA_ID; stored in the appinfoContentType column by 0x318150).
int32_t content_type(const Record* c) { return static_cast<int32_t>(get(c, "appinfoContentType").int_or(0)); }

// The content's AppInfoJson parsed back into an AppInfo (0x2C33E0 / 0x2C2C80: AppInfo
// vtable +8 FromJson): the integer value of `key` when the field_list has it as type 0.
bool appinfo_json_int(const Record* c, const char* key, int64_t* value) {
  json::Value v;
  const std::string& text = str(c, "AppInfoJson");
  if (text.empty() || json::parse(v, text.data(), text.size()) != 0) return false;
  for (const auto& f : v.member("field_list").array())
    if (f.member("key").string() == key) {
      if (f.member("type").integer() != 0) return false;
      *value = f.member("data").integer();
      return true;
    }
  return false;
}
// 0x2C33E0: _m2_device_id is an integer other than 0.
bool on_m2(const Record* c) {
  int64_t v = 0;
  return !empty(c) && appinfo_json_int(c, "_m2_device_id", &v) && v != 0;
}
// 0x2C2C80: DISC_COMPILATION is the integer 1.
bool disc_compilation(const Record* c) {
  int64_t v = 0;
  return !empty(c) && appinfo_json_int(c, "DISC_COMPILATION", &v) && v == 1;
}

// 0x2D39A0 AppBrowseRecord::GetType(icon, content): 1 unknown, 2 internal/external app,
// 3 entitlement/disc, 4 PS Now, 5 push, 6 share play, 7 shell app, 8 GLT-only.
int32_t record_type(const Record* icon, const Record* content, int32_t* type) {
  *type = 1;
  if (empty(icon) && empty(content)) return 0;
  if (!empty(content)) {
    const int32_t ct = content_type(content);
    if (ct == 3) *type = 4;            // 0x2654F0
    else if (ct == 4) *type = 7;       // 0x265550
    else if (ct == 1 || ct == 2) *type = 2;  // 0x2655B0
    else if (ct == 6) *type = 6;       // 0x265610
    else if (ct == 5) *type = 5;       // 0x265670
  }
  // 0x2D4800: content with an empty metaDataPath (0x2D5050) that is not internal/external
  // and not PS Now, with an entitlement (0x2D4B30) -> 3.
  const bool has_md = !str(content, "metaDataPath").empty();                       // 0x2D5050
  const bool has_ent = !empty(icon) && get(icon, "entEntitlement").int_or(0) != 0;  // 0x2D4B30
  const int32_t ct0 = empty(content) ? 0 : content_type(content);
  const bool v100 = has_ent && !has_md && !(ct0 == 1 || ct0 == 2) && ct0 != 3;
  if (v100) {
    *type = 3;
  } else if (has_ent && (*type == 5 || *type == 6)) {
    *type = 3;
  }
  // 0x2D4C80: GLT data (gltlastPlayedDate, 0x2D5240) without content -> 8.
  const bool has_glt = !empty(icon) && !str(icon, "gltlastPlayedDate").empty();
  bool glt_only = false;
  if (has_glt && !has_md && !has_ent) {
    const int32_t ct = empty(content) ? 0 : content_type(content);
    glt_only = !(ct == 1 || ct == 2) && ct != 3;
  }
  if (glt_only) {
    *type = 8;
    return 0;
  }
  if (*type == 1) {
    log::warn("appbrowse", "[AppDb][AppBrowse] GetType(): AppBrowseRecord::GetType() : unknown type!");
    return static_cast<int32_t>(0x80F5000Au);  // -2131427318
  }
  return 0;
}

// 0x2C17E0: the content's dispLocation is a string, or bit 0 is clear.
bool disp_visible(const Record* c) {
  if (empty(c)) return false;
  const Field& f = get(c, "dispLocation");
  if (!f.present()) return false;
  return f.type != 0 || (f.i & 1) == 0;
}

// 0x2E0A10 getTitleStatus.
int32_t title_status(int32_t user, const Record* content, const Record* icon, bool pushed, bool deleted, int32_t* status) {
  int32_t type = 1;
  if (int32_t e = record_type(icon, content, &type); e != 0) return e;
  // disc in a login user's drive (0x2C2B30 discStatus == 1, 0x2C2C80 no DISC_COMPILATION).
  bool disc_user = false;
  if (!empty(content) && get(content, "discStatus").type == 0 && get(content, "discStatus").i == 1 &&
      !disc_compilation(content)) {
    int32_t ids[shellcore::users::kMaxLogin];
    shellcore::users::login_user_ids(ids);  // fPhymKNvK-A
    for (int32_t id : ids) disc_user = disc_user || id == user;
  }
  // share play link (AppInfo "shareplay_link_user" of this user, 0x26E830).
  bool shareplay = false;
  if (!empty(content) && type == 6)
    host::note_unimplemented("eboot AppBrowse share-play link visibility (0x26E830) not reproduced");
  const bool pushable = pushed && !deleted;
  switch (type) {
    case 1: *status = 0; break;
    case 2: *status = !(disp_visible(content) || pushable) ? 6 : disc_user ? 13 : 12; break;
    case 3: *status = 9; break;
    case 4: *status = disp_visible(content) ? 11 : 5; break;
    case 5: *status = pushable ? 8 : 3; break;
    case 6: *status = shareplay ? 7 : 2; break;
    case 7: *status = (disp_visible(content) || pushable) ? 10 : 4; break;
    case 8: *status = 1; break;
    default: return static_cast<int32_t>(0x80F5000Au);
  }
  return 0;
}

// 0xBF4380: status -> visibility class (1 visible, 2 hidden).
constexpr int32_t kVisibility[14] = {2, 2, 2, 2, 2, 2, 2, 1, 1, 2, 1, 1, 1, 1};

struct Visibility {
  bool access_up = false;   // a8: the status rose to a shown one (or a pushed content)
  int32_t klass = 2;        // a9
  bool pushed_now = false;  // a10
  bool pushable = false;    // a11: (0x3558 >> status) & 1
};

// 0x2E04F0 checkContentVisibility.
int32_t check_visibility(int32_t user, const std::string& tid, const Record* before, const Record* after, const Record* icon,
                         Visibility* v) {
  // 0x2C1930: user info "nsx_content_flag_<titleId>" = {"pushed":..,"deleted":..}.
  bool pushed = false, deleted = false;
  std::string flags;
  const int32_t e = get_user_info(user, "nsx_content_flag_" + tid, &flags);
  if (e == 0) {
    json::Value j;
    if (json::parse(j, flags.data(), flags.size()) == 0) {
      pushed = j.member("pushed").truthy();
      deleted = j.member("deleted").truthy();
    }
  } else if (e != kErrNotFound) {
    return e;
  }
  int32_t old_status = 0, new_status = 0;
  if (int32_t r = title_status(user, before, icon, pushed, deleted, &old_status); r != 0) return r;
  if (int32_t r = title_status(user, after, icon, pushed, deleted, &new_status); r != 0) return r;
  if (new_status >= 14) return static_cast<int32_t>(0x80F5000Au);
  v->klass = kVisibility[new_status];
  v->pushable = (0x3558u >> new_status) & 1;
  if (old_status < new_status && v->klass != 2) v->access_up = true;
  // 0x27E9A0: emsPushedFlag == 1 on the stored row.
  const bool ems_pushed = get(icon, "emsPushedFlag").type == 0 && get(icon, "emsPushedFlag").i == 1;
  if (v->pushable && pushed && !deleted && !ems_pushed) {
    v->access_up = true;
    v->pushed_now = true;
  }
  return 0;
}

// 0x2C3230: the content is on an extended storage (hddLocation) or an M.2 drive
// (_m2_device_id in its AppInfoJson, 0x2C33E0).
bool on_other_storage(const Record* c) {
  if (empty(c)) return false;
  const Field& h = get(c, "hddLocation");
  return (h.type == 0 && h.i != 0) || on_m2(c);
}

// 0x2997F0: tbl_version 'access_index' + 1 and the current time.
int32_t next_access(std::string* time, int64_t* index) {
  int64_t v = 0;
  const int32_t e = get_version_value("access_index", &v);
  if (e != 0 && e != kErrNotFound) return e;
  if (e == kErrNotFound) v = 0;
  ++v;
  if (int32_t r = set_version_value("access_index", v); r != 0) return r;
  *index = v;
  *time = now_string();
  return 0;
}

// 0x2E1250 updateLastAccess.
int32_t update_last_access(const std::string& tid, int32_t user, const Record* before, const Record* after, const Visibility& v,
                           Record& rec) {
  (void)tid;
  (void)user;
  std::string time;
  int64_t index = 0;
  if (v.access_up) {
    if (int32_t e = next_access(&time, &index); e != 0) return e;  // 0x2AEC00
    if ((empty(before)) && !empty(after) && on_other_storage(after)) time = str(after, "installTime");
    rec["lastAccessTime"] = Field::string(time);  // 0x2A7430
    rec["lastAccessIndex"] = Field::integer(index);
  }
  if (v.klass == 2) {
    rec["lastAccessTime"] = Field::string("");
    rec["lastAccessIndex"] = Field::integer(0);
  }
  if (v.access_up)  // 0x2AE9E0 / 0x2DE450 / 0x2AFA80
    host::note_unimplemented("eboot AppBrowse last-access write-back to SceShellCore AppInfo (#_last_access_time/#_access_index per user, 0x2DE450 -> SyncManagement 0x2DCC70) not reproduced");
  return 0;
}

// 0x2B7120: (installStatus is not an integer or is 0) << 32 | primaryTitleSort.
int64_t primary_title_sort(const Record* c, const Record* icon) {
  if (!empty(c)) {
    const Field& is = get(c, "installStatus");
    const Field& ps = get(c, "primaryTitleSort");
    if (ps.present()) {
      const int64_t hi = (is.present() && (is.type != 0 || is.i == 0)) ? (int64_t{1} << 32) : 0;
      return hi | (ps.type == 0 ? ps.i : 0);
    }
  }
  if (!empty(icon)) host::note_unimplemented("eboot AppBrowse primaryTitleSort from UTM data (0x2B7420) not reproduced");
  return 0;
}

// 0x2E1850 updateRepresentativeInfo (the fields of the row a7 = stored icon, a5 = new content).
int32_t update_representative(int32_t user, const std::string& tid, const Record* before, const Record* after,
                              const Record* icon, const Visibility& v, Record& rec) {
  (void)before;
  // 0x2DFFD0: the UTM (catalog) data wins only for an SCP concept with UTM data.
  const std::string lcid = str(after, "localConceptId");
  bool use_utm = false;
  if (lcid.find("cid:scp:") != std::string::npos) {
    const std::string utm = str(icon, "utmLocalConceptId");
    use_utm = utm.find("cid:scp:") != std::string::npos && lcid.empty();
  }
  if (use_utm) {
    host::note_unimplemented("eboot AppBrowse representative info from UTM data (0x2B4260/0x2B45A0/0x2B48E0) not reproduced");
  } else {
    rec["localConceptId"] = Field::string(empty(after) ? "" : str(after, "localConceptId"));  // 0x2B4070
    rec["conceptId"] = Field::integer(empty(after) ? 0 : get(after, "conceptId").int_or(0));     // 0x2B4450
    rec["titleName"] = Field::string(empty(after) ? "" : str(after, "titleName"));            // 0x2B46F0
  }
  // externalLaunchFlag (0x2B7BC0 of the stored row, then 0x2A9040).
  int64_t ext = 0;
  if (!empty(icon)) {
    const Field& f = get(icon, "externalLaunchFlag");
    ext = f.type == 0 && f.i == 1;
  }
  rec["externalLaunchFlag"] = Field::integer(ext);
  if (!empty(before) && !empty(after) && on_other_storage(after))
    host::note_unimplemented("eboot AppBrowse external launch flag from the last-access comparison (0x2B3F00) not reproduced");
  // 0x2C3910 / 0x2C33E0: packageLocation bit 2, or an M.2 device -> keep; else 0.
  const Field& pl = get(after, "packageLocation");
  const bool external_pkg = !empty(after) && pl.type == 0 && (pl.i & 2) != 0;
  if (!external_pkg && !on_m2(after)) rec["externalLaunchFlag"] = Field::integer(0);
  // emsPushedFlag (0x2C1930 pushed flag) for a pushable status.
  if (v.pushable) {
    std::string flags;
    const int32_t e = get_user_info(user, "nsx_content_flag_" + tid, &flags);
    if (e == 0) {
      json::Value j;
      if (json::parse(j, flags.data(), flags.size()) == 0) rec["emsPushedFlag"] = Field::integer(j.member("pushed").truthy());
    } else if (e != kErrNotFound) {
      return e;
    }
  }
  // dispLocation: the content's without bit 0 for a visible title.
  int64_t disp = 0;
  if (v.klass == 1) {
    const Field& d = get(after, "dispLocation");
    if (d.type == 0) disp = d.i & ~int64_t{1};
  }
  // visible (0x2C44A0 entitlement, 0x2C44E0 GLT, or the visibility class).
  const bool ent = !empty(icon) && get(icon, "entEntitlement").type == 0 && get(icon, "entEntitlement").i != 0;
  const bool glt = !empty(icon) && !str(icon, "gltlastPlayedDate").empty();
  rec["visible"] = Field::integer(ent || glt || v.klass == 1);  // 0x2A7320
  if (!empty(icon) && (ent || glt))
    host::note_unimplemented("eboot AppBrowse dispLocation from entitlement data (0x2B77E0) not reproduced");
  int64_t d2 = disp;
  if (rec["externalLaunchFlag"].int_or(0) == 1) d2 |= 0x80;  // 0x2A71D0
  if (d2 == 0 && !empty(after)) {
    const Field& d = get(after, "dispLocation");
    if (d.type == 0) d2 = d.i;
  }
  rec["dispLocation"] = Field::integer(d2);  // 0x2A7F50
  // 0x2B4AD0 contentId (entId when the content has none).
  std::string cid = str(after, "contentId");
  if (cid.empty() && !empty(icon)) cid = str(icon, "entId");
  rec["contentId"] = Field::string(cid);
  // 0x2B7D10 recentActivityDatePlayedOrPurchased: later of the stored lastPlayedDate and
  // entPurchasedDate.
  rec["recentActivityDatePlayedOrPurchased"] =
      Field::string(empty(icon) ? "" : later(str(icon, "lastPlayedDate"), str(icon, "entPurchasedDate")));
  // 0x2B8260 recentActivityDatePlayedOrInstalled: later of lastPlayedDate and installTime.
  rec["recentActivityDatePlayedOrInstalled"] = Field::string(later(str(icon, "lastPlayedDate"), str(after, "installTime")));
  rec["installedDate"] = Field::string(empty(after) ? "" : str(after, "installTime"));  // 0x2B8610
  {
    // 0x2B87E0: 0 -> 2, 1 -> 1, others 0; absent content 0.
    int64_t st = 0;
    if (!empty(after)) {
      const Field& f = get(after, "installStatus");
      st = 2;
      if (f.type == 0 && f.i != 0) st = f.i == 1;
    }
    rec["installStatus"] = Field::integer(st);
  }
  // 0x2B5660 psNowFlag: the content's AppInfoContentType is PS Now (3).
  rec["psNowFlag"] = Field::integer(!empty(after) && content_type(after) == 3);
  rec["deeplinkUri"] = Field::string(str(after, "pprDeeplinkUri"));  // 0x2B5890
  {
    // 0x2B5A80 hubAppUri: the content's; else for UTM space types 1/2 the hub links.
    std::string hub = str(after, "pprHubAppUri");
    if ((empty(after) || !get(after, "pprHubAppUri").present()) && !empty(icon)) {
      const Field& sp = get(icon, "utmSpaceType");
      if (sp.type == 0 && sp.i == 1) hub = "pshome:gamehub?titleId=" + tid;
      else if (sp.type == 0 && sp.i == 2) hub = "psmediahub:main?titleId=" + tid;
    }
    rec["hubAppUri"] = Field::string(hub);
  }
  rec["platform"] = Field::integer(!empty(after) ? get(after, "platform").int_or(0) : get(icon, "utmPlatform").int_or(0));  // 0x2B4E00
  rec["appDrmType"] = Field::integer(!empty(after) ? get(after, "appType").int_or(0) : get(icon, "utmDrmType").int_or(0));  // 0x2B5010
  rec["betaFlag"] = Field::integer(!empty(after) ? (get(after, "pprAttribute3").int_or(0) & 0x100) != 0
                                                  : get(icon, "utmBetaFlag").int_or(0));  // 0x2B5220
  rec["contentBadgeType"] =
      Field::integer(!empty(after) ? get(after, "contentBadgeType").int_or(0) : get(icon, "utmContentBadgeType").int_or(0));  // 0x2B5450
  rec["primaryTitleSort"] = Field::integer(primary_title_sort(after, icon));  // 0x2B7120
  // 0x2B6000 discStatus: the content's for a non-disc record.
  {
    int32_t type = 1;
    record_type(icon, after, &type);
    if (type == 3) host::note_unimplemented("eboot AppBrowse disc status of an entitlement record (0x2B6000 type 3) not reproduced");
    rec["discStatus"] = Field::integer(type == 3 ? 0 : get(after, "discStatus").int_or(0));
  }
  return 0;
}

// 0x2AF300 (insert) / 0x2AEC60 (update): the row, then the IconInfo subject's observers.
int32_t write_icon(int32_t user, const std::string& tid, const Record* stored, Record& rec, bool insert) {
  const std::string table = iconinfo_table(user);
  // go_west_by_user: a row carrying a lastAccessIndex marks the user (0x21BC50).
  const Field& li = rec["lastAccessIndex"];
  if (li.type == 0 && li.i != 0) set_user_info(user, "go_west_by_user", "1");
  int32_t r = insert ? insert_record(table, "titleId", tid, rec) : update_record(table, "titleId", tid, rec);  // 0x283EC0 / 0x283EF0
  if (r != 0) return r;
  Record after;
  select_record(table, "titleId", tid, &after);
  return on_icon_changed(insert ? kInsert : kUpdate, user, stored, &after);
}

// 0x2E56C0 modifyAppearanceControl.
int32_t modify_appearance(int op, int32_t user, const std::string& tid, const Record* before, const Record* after) {
  if (!valid_title_id(tid)) return kErrInvalidArg;
  Record stored;
  const int32_t g = select_record(iconinfo_table(user), "titleId", tid, &stored);  // 0x2B1B20
  if (g != 0 && g != kErrNotFound) return g;
  const Record* icon = g == 0 ? &stored : nullptr;
  if (!((op | 2) == 3 || empty(icon))) {
    // deletion of the content: keep the row while it has entitlement / GLT data (0x27E6C0,
    // 0x27EAD0), else delete it (0x2AF860).
    const bool keep = get(icon, "entEntitlement").int_or(0) != 0 || !str(icon, "gltlastPlayedDate").empty();
    if (!keep) {
      const int32_t r = delete_record(iconinfo_table(user), "titleId", tid);
      if (r != 0) return r;
      return on_icon_changed(kDelete, user, icon, nullptr);
    }
  }
  Visibility v;
  if (int32_t r = check_visibility(user, tid, before, after, icon, &v); r != 0) {
    log::warn("appbrowse", "[AppBrowse] modifyAppearanceControl() err: checkContentVisibility() error ret=[0x{:x}]", static_cast<uint32_t>(r));
    return r;
  }
  Record rec;  // 0x2AE860: a record instance for the title
  rec["titleId"] = Field::string(tid);
  if (int32_t r = update_last_access(tid, user, before, after, v, rec); r != 0) return r;
  if (int32_t r = update_representative(user, tid, before, after, icon, v, rec); r != 0) return r;
  if (!empty(icon) && !str(icon, "utmConceptId").empty())  // 0x2E4400
    host::note_unimplemented("eboot AppBrowse updateConceptId4AppInfo (0x2E4400, CONCEPT_ID back to SceShellCore) not reproduced");
  return write_icon(user, tid, icon, rec, empty(icon));
}

}  // namespace

int32_t on_content_changed(int op, const std::string& title_id, const Record* before, const Record* after) {
  // 0x2E5600: every user with an iconinfo table (0x28DE50).
  int32_t r = 0;
  const std::vector<int32_t> users = db().icon_tables;
  for (int32_t user : users) r = modify_appearance(op, user, title_id, before, after);
  // The 0x30F540 notifier's ContentInfo observer (0x3113F0 -> 0x310060): UI change events.
  host::note_unimplemented("eboot AppBrowse change notifications to the shell (0x30F850 notifier: 0x310060 / 0x3102B0) not reproduced");
  return r;
}

int32_t reflect_content_to_user(int32_t user_id, const std::string& title_id) {
  // 0x2C24A0: the stored tbl_contentinfo row (0x278290 / 0x278AA0), then 0x2E56C0 with the
  // operation record {1, (none), row}.
  Record content;
  const int32_t r = select_record("tbl_contentinfo", "titleId", title_id, &content);
  if (r != 0) return r;
  return modify_appearance(kInsert, user_id, title_id, nullptr, &content);
}

}  // namespace ssn::eboot::appbrowse
