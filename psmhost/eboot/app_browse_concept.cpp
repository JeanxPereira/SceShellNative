// ShellUI eboot AppBrowse: tbl_conceptmetadata and the per-user tbl_concepticoninfo_<userId>
// rows (FW 4.03).
//
//   conceptinfo_sync::RecoveryAppDbPromoteTransactionCommand 0x331BD0 (execute 0x33A400):
//     the concepts SceShellCore changed since the stored conceptinfo_sync_index (search mode 1,
//     0x2DD700 -> 0x2DE0F0) -> 0x339310 -> 0x338FF0: ConceptInfo (0x332B40) -> tbl_conceptmetadata
//     record (0x332C50 + 0x337090) -> 0x337C80 update / insert (ConceptMetadata subject).
//   IconInfo observer 0x2EAEA0 (AppBrowse manager, vtable 0xCDC378 +0x20): the concept of the
//     changed tbl_iconinfo_ row -> 0x2EBF50 -> 0x2EC300 (aggregate) -> 0x2EA0D0 insert /
//     0x2EA1B0 update of tbl_concepticoninfo_<userId>.
#include <cstdio>

#include "core/log.h"
#include "eboot/app_browse.h"
#include "host/boundary.h"

namespace ssn::eboot::appbrowse {
namespace appdb = ssn::shellcore::appdb;

namespace {

bool empty(const Record* r) { return !r || r->empty(); }
const Field& get(const Record* r, const char* col) {
  static const Field none;
  if (!r) return none;
  auto it = r->find(col);
  return it == r->end() ? none : it->second;
}
std::string str(const Record* r, const char* col) { return get(r, col).str_or_empty(); }
int64_t num(const Record* r, const char* col) { return get(r, col).int_or(0); }

int64_t date_tick(const std::string& s) {
  int y, mo, d, h, mi, se, ms = 0;
  if (std::sscanf(s.c_str(), "%d-%d-%d %d:%d:%d.%d", &y, &mo, &d, &h, &mi, &se, &ms) < 6) return 0;
  return ((((((static_cast<int64_t>(y) * 13 + mo) * 32 + d) * 24 + h) * 60 + mi) * 60 + se) * 1000) + ms;
}
std::string later(const std::string& a, const std::string& b) { return date_tick(a) <= date_tick(b) ? b : a; }

// ---- tbl_conceptmetadata from the ConceptInfo (0x332C50 / 0x337090) ------------------------

const Value* cfind(const appdb::ConceptInfo& c, const char* key) {
  auto it = c.values.find(key);
  return it == c.values.end() ? nullptr : &it->second;
}
int64_t cint(const appdb::ConceptInfo& c, const char* key) {
  const Value* v = cfind(c, key);
  return v && v->type == 0 ? v->i : 0;
}
std::string cstr(const appdb::ConceptInfo& c, const char* key) {
  const Value* v = cfind(c, key);
  return v && v->type == 2 ? v->s : std::string();
}

// 0x23D980 on a ConceptInfo (0x237580 "concept_name", 0x237630 "publisher_name",
// 0x2376E0 "descriptions").
std::string clocalized(const appdb::ConceptInfo& c, const std::string& key) {
  const int32_t lang = system_language();
  if (lang >= 0 && lang <= 29) {
    char sfx[8];
    std::snprintf(sfx, sizeof sfx, "_%02d", lang);
    if (const Value* v = cfind(c, (key + sfx).c_str()); v && v->type == 2 && !v->s.empty()) return v->s;
  }
  return cstr(c, key.c_str());
}

// 0x235A90 / 0x2364B0 / 0x236ED0: <metadata_path><base>[_%02d]<ext>[?ts=] from the concept's
// path mask (0x239B10 / 0x23A070 / 0x23B460).
int32_t cpath(const appdb::ConceptInfo& c, const char* base, const char* ext_default, const char* mask_key, const char* ts_key,
              std::string* out) {
  out->clear();
  if (!cfind(c, "metadata_path")) return kErrNotFound;
  const Value* m = cfind(c, mask_key);
  if (!m) return kErrNotFound;
  const uint64_t mask = m->type == 0 ? static_cast<uint64_t>(m->i) : 0;
  std::string ext = ext_default;  // 0x237B90 a3
  if (mask & 0x4000000000000000ull) ext = ".jpg";
  else if (mask & 0x2000000000000000ull) ext = ".dds";
  else if (mask & 0x1000000000000000ull) ext = ".png";
  const std::string dir = cstr(c, "metadata_path");
  const int32_t lang = system_language();
  std::string path;
  if (lang < 0) {
    path = dir + base + ext;
  } else if (lang <= 29 && (mask >> lang) & 1) {
    char sfx[8];
    std::snprintf(sfx, sizeof sfx, "_%02d", lang);
    path = dir + base + sfx + ext;
  } else if (static_cast<int64_t>(mask) < 0 && std::string(mask_key) != "path_logo_info") {
    path = dir + base + ext;
  } else {
    return 0;
  }
  *out = path;
  const Value* ts = cfind(c, ts_key);
  if (!ts) return kErrNotFound;
  char buf[32];
  std::snprintf(buf, sizeof buf, "?ts=%lld", static_cast<long long>(ts->type == 0 ? ts->i : 0));
  *out += buf;
  return 0;
}

void build_metadata(const appdb::ConceptInfo& c, Record& m) {
  // 0x332C50
  m["conceptId"] = Field::integer(cint(c, "concept_id"));
  m["metaDataPath"] = Field::string(cstr(c, "metadata_path"));
  // 0x337090
  m["conceptName"] = Field::string(clocalized(c, "concept_name"));
  std::string p;
  cpath(c, "/icon0", ".png", "path_icon0_info", "path_icon0_info_time_stamp", &p);
  m["icon0Info"] = Field::string(p);
  cpath(c, "/pic0", ".png", "path_pic0_info", "path_pic0_info_time_stamp", &p);
  m["pic0Info"] = Field::string(p);
  cpath(c, "/logo", ".png", "path_logo_info", "path_logo_info_time_stamp", &p);
  m["logoImageInfo"] = Field::string(p);
  m["publisherName"] = Field::string(clocalized(c, "publisher_name"));
  m["description"] = Field::string(clocalized(c, "descriptions"));
  // 0x332C50 (continued)
  m["lastAccessTime"] = Field::string(cstr(c, "last_access_date"));
  m["lastAccessIndex"] = Field::integer(cint(c, "last_access_index"));
  m["sortPriority"] = Field::integer(cint(c, "sort_priority"));
  m["pathInfo"] = Field::integer(cint(c, "path_info"));
  m["pathInfo2"] = Field::integer(cint(c, "path_info_2"));
  m["dispLocation"] = Field::integer(cint(c, "display_location"));
  m["totalSize"] = Field::integer(cint(c, "total_size"));
  m["hddLocation"] = Field::integer(cint(c, "hdd_location"));
  m["externalHddAppStatus"] = Field::integer(cint(c, "external_hdd_app_status"));
  m["contentLocation"] = Field::integer(cint(c, "contents_location"));
  m["installStatus"] = Field::integer(cint(c, "install_status"));
  m["pprDeeplinkUri"] = Field::string(cstr(c, "deeplink_uri"));
  m["pprHubAppUri"] = Field::string(cstr(c, "hubapp_uri"));
  m["minimumAge"] = Field::integer(cint(c, "mimimum_age"));
  m["type"] = Field::string(cstr(c, "type"));
  m["genres"] = Field::string(cstr(c, "genres"));
  m["spaceType"] = Field::integer(cint(c, "space_type"));
  m["scpDataStored"] = Field::integer(cint(c, "scp_data_stored"));
  m["mergedOperationCapability"] = Field::integer(cint(c, "operation_capability"));
  m["mergedContentsStatus"] = Field::integer(cint(c, "contents_status"));
  m["mergedPromoteTime"] = Field::string(cstr(c, "promote_time"));
  m["mergedInstallTime"] = Field::string(cfind(c, "install_time") ? cstr(c, "install_time") : cstr(c, "promote_time"));
  m["mergedAppInfoContentType"] = Field::integer(cint(c, "content_type"));
}

// 0x2E4B80: the genre list (tab separated) -> genre bits.
int64_t genre_flags(const std::string& genres) {
  static const char* const kGenres[16] = {"ACTION",  "ADULT",   "ADVENTURE", "ARCADE",       "BRAIN_TRAINING", "CASUAL",
                                          "RACING",  "EDUCATIONAL", "FAMILY", "FIGHTING",   "FITNESS",        "HORROR",
                                          "MUSIC/RHYTHM", "PARTY", "PUZZLE",  "QUIZ"};
  int64_t flags = 0;
  size_t pos = 0;
  while (pos <= genres.size() && !genres.empty()) {
    const size_t tab = genres.find('\t', pos);
    const std::string g = genres.substr(pos, tab == std::string::npos ? std::string::npos : tab - pos);
    bool known = false;
    for (int i = 0; i < 16; ++i)
      if (g == kGenres[i]) {
        flags |= int64_t{1} << i;
        known = true;
      }
    if (!known && !g.empty())
      host::note_unimplemented("eboot AppBrowse genre bits after QUIZ (0x2E4B80) not traced");
    if (tab == std::string::npos) break;
    pos = tab + 1;
  }
  return flags;
}

// 0x2B8930(metadata, stored concept, record, force = 1): the concept's metadata fields.
void copy_metadata(const Record& m, Record& rec) {
  rec["conceptId"] = Field::integer(num(&m, "conceptId"));
  rec["publisherName"] = Field::string(str(&m, "publisherName"));
  rec["descriptions"] = Field::string(str(&m, "description"));
  rec["minimumAge"] = Field::integer(num(&m, "minimumAge"));
  rec["type"] = Field::string(str(&m, "type"));
  rec["genres"] = Field::string(str(&m, "genres"));
  rec["genreFlags"] = Field::integer(genre_flags(str(&m, "genres")));
  rec["spaceType"] = Field::integer(num(&m, "spaceType"));
  if (num(&m, "scpDataStored") != 0) {
    rec["conceptName"] = Field::string(str(&m, "conceptName"));
    rec["metaDataPath"] = Field::string(str(&m, "metaDataPath"));
    rec["icon0Info"] = Field::string(str(&m, "icon0Info"));
    rec["pic0Info"] = Field::string(str(&m, "pic0Info"));
  }
  rec["promoteTime"] = Field::string(str(&m, "mergedPromoteTime"));
  rec["installTime"] = Field::string(str(&m, "mergedInstallTime"));
}

// 0x2BD290: the representative (primary) title's fields.
void representative(const Record& icon, const Record* content, const Record* metadata, const Record* stored,
                    const std::string& interacted, Record& rec) {
  rec["primaryTitleId"] = Field::string(str(&icon, "titleId"));
  rec["primaryTitleName"] = Field::string(str(&icon, "titleName"));
  rec["primaryTitlePlatform"] = Field::integer(num(&icon, "platform"));
  rec["primaryTitleBgMatteType"] = Field::string(empty(content) ? "" : str(content, "bgMatteType"));
  rec["psNowFlag"] = Field::integer(num(&icon, "psNowFlag"));
  rec["deeplinkUri"] = Field::string(str(&icon, "deeplinkUri"));
  rec["hubAppUri"] = Field::string(str(&icon, "hubAppUri"));
  rec["snd0Info"] = Field::string(empty(content) ? "" : str(content, "snd0Info"));
  const bool scp = num(metadata, "scpDataStored") != 0;
  if (!scp && num(stored, "ucmScpDataStored") == 0) rec["conceptName"] = Field::string(str(&icon, "titleName"));
  if (!scp) {
    rec["metaDataPath"] = Field::string(empty(content) ? "" : str(content, "metaDataPath"));
    rec["icon0Info"] = Field::string(empty(content) ? "" : str(content, "icon0Info"));
    rec["pic0Info"] = Field::string(empty(content) ? "" : str(content, "pic0Info"));
  }
  rec["lastInteractedTime"] = Field::string(interacted);
}

// 0x2EC300: the concept's aggregate over the user's titles with this local concept id.
int32_t aggregate(int32_t user, const std::string& lcid, const Record* before, const Record* after, bool is_new, Record& rec) {
  Record stored, metadata;
  const int32_t gs = select_record(concepticoninfo_table(user), "localConceptId", lcid, &stored);  // 0x2EA350
  if (gs != 0 && gs != kErrNotFound) return gs;
  const int32_t gm = select_record("tbl_conceptmetadata", "localConceptId", lcid, &metadata);  // 0x2EA890 / 0x2EAB60
  if (gm != 0 && gm != kErrNotFound) return gm;
  const Record* st = gs == 0 ? &stored : nullptr;
  const Record* md = gm == 0 ? &metadata : nullptr;
  if (is_new) {
    int64_t valid = 0;
    if (empty(md)) {
      if (!empty(st) && !str(st, "ucmConceptName").empty()) {
        host::note_unimplemented("eboot AppBrowse concept fields from UCM data (0x2BB710) not reproduced");
        valid = 1;
      }
    } else {
      copy_metadata(*md, rec);  // 0x2B8930
      valid = 1;
    }
    rec["validFlag"] = Field::integer(valid);
  }
  // lastAccess* / lastPlayed* from the changed title.
  const int64_t ib = num(before, "lastAccessIndex"), ia = num(after, "lastAccessIndex");
  if (ia != 0 && ib != ia) {
    rec["lastAccessTime"] = Field::string(str(after, "lastAccessTime"));
    rec["lastAccessIndex"] = Field::integer(ia);
  }
  if (date_tick(str(after, "lastPlayedDate")) > date_tick(str(before, "lastPlayedDate")))
    rec["lastPlayedDate"] = Field::string(str(after, "lastPlayedDate"));
  if (date_tick(str(after, "lastPlayedDateOnConsole")) > date_tick(str(before, "lastPlayedDateOnConsole")))
    rec["lastPlayedDateOnConsole"] = Field::string(str(after, "lastPlayedDateOnConsole"));
  // 0x2F16B0 setPrimaryTitle.
  if (!empty(after)) {
    std::string interacted =
        later(later(str(after, "lastAccessTime"), str(after, "lastPlayedDateOnConsole")), str(after, "entPurchasedDate"));
    if (!empty(st)) interacted = later(interacted, str(st, "lastInteractedTime"));
    if (interacted.empty()) host::note_unimplemented("eboot AppBrowse lastInteractedTime without access dates (0x2F16B0 GLT / now) not reproduced");
    std::string notset;
    char t[32];
    std::snprintf(t, sizeof t, "tbl_info_%010d", user);
    Record ui;
    if (select_record(t, "key", "notset_primary_title", &ui) == 0)
      host::note_unimplemented("eboot AppBrowse notset_primary_title user info (0x2C1BB0) not reproduced");
    const std::string primary = !str(&rec, "primaryTitleId").empty() ? str(&rec, "primaryTitleId") : std::string();
    if (primary.empty()) {
      Record content;
      const bool has_content = select_record("tbl_contentinfo", "titleId", str(after, "titleId"), &content) == 0;  // 0x2B1A40
      representative(*after, has_content ? &content : nullptr, md, st, interacted, rec);
    } else {
      host::note_unimplemented("eboot AppBrowse primary title comparison (0x2F16B0 with a primary title) not reproduced");
    }
  }
  std::string primary = str(&rec, "primaryTitleId");
  if (primary.empty()) primary = str(st, "primaryTitleId");
  // The user's titles of the concept (0x284020: I.localConceptId = lcid).
  std::vector<std::string> titles;
  if (int32_t e = select_keys(iconinfo_table(user), "titleId", "localConceptId='" + lcid + "'", &titles); e != 0) return e;
  int64_t disp = 0, hidden = 0, installed = 0, per_platform = 0, package = 0, ent = 0, fil_ent = 0, est = 0, played = 0,
          platform = 0;
  std::string purchased, rapp, rapi;
  if (titles.empty()) disp = 1;
  for (const auto& tid : titles) {
    Record icon, content;
    if (int32_t e = select_record(iconinfo_table(user), "titleId", tid, &icon); e != 0) continue;
    const bool has_content = select_record("tbl_contentinfo", "titleId", tid, &content) == 0;
    int64_t d = 0;
    const Field& fd = icon["dispLocation"];
    if (fd.type == 0) {
      d = fd.i;
      if ((d & 1) && icon["visible"].type == 0 && icon["visible"].i == 1) d &= ~int64_t{1};
    }
    int64_t d2 = d | ((d & 0x10) ? 0x800 : 0x400);
    if (tid != primary) d2 &= ~int64_t{6};
    disp |= d2;
    const Field& h = icon["entHidden"];
    const bool shown = h.type != 0 || h.i == 0;
    if (has_content) {
      const Field& is = content["installStatus"];
      bool platform_bit = true;
      if (is.type != 0) installed |= 1;
      else {
        installed |= is.i == 0;
        platform_bit = is.i == 0;
      }
      if (platform_bit) {
        const Field& p = content["platform"];
        const int64_t pv = p.type == 0 ? p.i : 0;
        if (pv >= 0) per_platform |= int64_t{1} << pv;
      }
      package |= num(&content, "packageLocation");
    }
    ent |= num(&icon, "entEntitlement");
    est += num(&icon, "utmEstimatedSize");
    played += num(&icon, "gltTotalPlayedTime");
    purchased = later(purchased, str(&icon, "entPurchasedDate"));
    rapp = later(rapp, str(&icon, "recentActivityDatePlayedOrPurchased"));
    rapi = later(rapi, str(&icon, "recentActivityDatePlayedOrInstalled"));
    if (!shown) {
      hidden |= 2;
      continue;
    }
    const Field& p = icon["platform"];
    const int64_t pv = p.type == 0 ? p.i : 0;
    if (pv >= 0) platform |= int64_t{1} << pv;
    fil_ent |= num(&icon, "entFilTypeEnt");
    hidden |= 1;
  }
  // spaceType from the metadata (scpDataStored) or the stored UCM data.
  int64_t out_disp = disp;
  bool has_space = false;
  int64_t space = 0;
  if (num(md, "scpDataStored") == 1) {
    has_space = true;
    space = num(md, "spaceType");
  } else if (num(st, "ucmScpDataStored") == 1) {
    has_space = true;
    space = num(st, "ucmSpaceType");
  }
  if (has_space) out_disp = (disp & ~int64_t{6}) | (space <= 3 ? 2 * space : 0);
  rec["dispLocation"] = Field::integer(out_disp);
  rec["filterTypeHidden"] = Field::integer(hidden);
  rec["installedStatus"] = Field::integer(installed);
  rec["installedStatusPerPlatform"] = Field::integer(per_platform);
  rec["packageLocation"] = Field::integer(package);
  rec["entitlementStatus"] = Field::integer(ent);
  rec["filterTypeEntitlement"] = Field::integer(fil_ent);
  rec["totalEstimatedSize"] = Field::integer(est);
  rec["totalPlayedTime"] = Field::integer(played);
  rec["purchasedDate"] = Field::string(purchased);
  rec["platform"] = Field::integer(platform);
  rec["recentActivityDatePlayedOrPurchased"] = Field::string(rapp);
  rec["recentActivityDatePlayedOrInstalled"] = Field::string(rapi);
  return 0;
}

// 0x2EBF50: the concept of a title that became visible.
int32_t concept_insert(int32_t user, const Record* before, const Record* after) {
  const std::string lcid = str(after, "localConceptId");  // 0x2EB9B0
  if (lcid.empty()) return kErrInvalidArg;                   // 0x247D60
  Record stored;
  const int32_t g = select_record(concepticoninfo_table(user), "localConceptId", lcid, &stored);  // 0x2EA350
  if (g != 0 && g != kErrNotFound) return g;
  bool is_new = true;
  if (g == 0) {
    // 0x2DF5A0: the stored dispLocation is a non-zero integer with bit 0 clear.
    const Field& d = stored["dispLocation"];
    is_new = !(d.type == 0 && d.i != 0 && (d.i & 1) == 0);
  }
  Record rec;  // 0x2E9E10: a record instance for the concept
  rec["localConceptId"] = Field::string(lcid);
  if (int32_t r = aggregate(user, lcid, before, after, is_new, rec); r != 0) return r;
  // 0x2EA0D0 insert / 0x2EA1B0 update (0x2B4060 guard: the concept observers 0x2E79D0 skip).
  if (g == kErrNotFound) return insert_record(concepticoninfo_table(user), "localConceptId", lcid, rec);
  return update_record(concepticoninfo_table(user), "localConceptId", lcid, rec);
}

}  // namespace

int32_t on_icon_changed(int op, int32_t user, const Record* before, const Record* after) {
  // 0x2EBC50: the concept operation from the rows' visibility (0x2F76F0 visible != 0).
  const bool vb = (op == kUpdate || op == kDelete) && num(before, "visible") != 0;
  const bool va = (op == kUpdate || op == kInsert) && num(after, "visible") != 0;
  int cop = 0;
  if (!vb && va) cop = kInsert;
  else if (vb && !va) cop = kDelete;
  else if (vb && va) cop = kUpdate;
  if (cop == kInsert) return concept_insert(user, before, after);
  if (cop == kUpdate) {
    if (str(before, "localConceptId") == str(after, "localConceptId")) {
      const std::string lcid = str(after, "localConceptId");
      Record rec;
      rec["localConceptId"] = Field::string(lcid);
      if (int32_t r = aggregate(user, lcid, before, after, false, rec); r != 0) return r;  // 0x2EC300
      return update_record(concepticoninfo_table(user), "localConceptId", lcid, rec);    // 0x2EA1B0
    }
    host::note_unimplemented("eboot AppBrowse concept move of a title (0x2EAEA0 local concept change) not reproduced");
    return concept_insert(user, before, after);
  }
  if (cop == kDelete) host::note_unimplemented("eboot AppBrowse concept removal of a hidden title (0x2F0800) not reproduced");
  return 0;
}

int32_t sync_concepts(int64_t from_index, int64_t to_index) {
  // 0x2DD700: sync_index > from AND < to + 1, sorted by sync_index (search mode 1).
  appdb::ConditionChain cond;
  cond.join = 1;
  cond.conditions.push_back({"sync_index", {Value::integer(from_index)}, 5, 0});
  cond.conditions.push_back({"sync_index", {Value::integer(to_index + 1)}, 4, 0});
  std::vector<std::string> ids;
  int32_t r = appdb::search(cond, {{"sync_index", 1}}, 1, 0, &ids, nullptr);
  if (r != 0 && r != appdb::kErrNotFound) return r;
  host::note_unimplemented("eboot AppBrowse deleted-concept list (search mode 3, 0x2DE130 -> 0x338C60) not reproduced");
  int32_t result = 0;
  for (const auto& lcid : ids) {
    // 0x339310 -> 0x338FF0
    appdb::ConceptInfo c;
    const int32_t g = appdb::get_concept_info(lcid, &c);  // 0x332B40
    if (g == appdb::kErrNotFound) continue;
    if (g != 0) {
      result = g;
      continue;
    }
    Db& d = db();
    std::lock_guard lock(d.mutex);
    Record m;
    build_metadata(c, m);  // 0x332C50 / 0x337090
    Record existing;
    const int32_t e = select_record("tbl_conceptmetadata", "localConceptId", lcid, &existing);
    // 0x337C80: 0x2C8EE0 update, or 0x2C87A0 insert when absent.
    const int32_t w = e == 0 ? update_record("tbl_conceptmetadata", "localConceptId", lcid, m)
                             : insert_record("tbl_conceptmetadata", "localConceptId", lcid, m);
    if (w != 0) {
      result = w;
      continue;
    }
    // ConceptMetadata observer 0x2F7CE0 -> 0x2F7DB0 for every user's concept table: a stored
    // concept row takes the new metadata.
    for (int32_t user : d.concept_icon_tables) {
      Record stored;
      if (select_record(concepticoninfo_table(user), "localConceptId", lcid, &stored) != 0) continue;
      host::note_unimplemented("eboot AppBrowse concept row refresh on a metadata change (0x2F7DB0) not reproduced");
    }
  }
  Db& d = db();
  std::lock_guard lock(d.mutex);
  d.conceptinfo_sync_index = to_index;  // 0x2DDB10 + 0x338020
  set_version_value("conceptinfo_sync_index", to_index);
  return result;
}

}  // namespace ssn::eboot::appbrowse
