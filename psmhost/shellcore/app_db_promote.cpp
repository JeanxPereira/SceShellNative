// SceShellCore AppDb population from installed apps (FW 4.03, app_promoter.cpp and the
// AppDb daemon's InsertAppInfo/UpdateAppInfo), reproduced for the preinstalled React Native
// apps (/system_ex/rnps/apps/<titleId>/appdb/<dir>) and for the directory titles of /user/app/
// (<titleId>/sce_sys with no app.pkg). Addresses are SceShellCore.elf offsets.
//
// Not reproduced (each logged once): /system_ex/app/ titles, /user/app/ package titles (app.pkg,
// 0x220D10), appmeta-based titles, download data (0x2547B0), the post-promote hook 0x222DC0, the existing-title merge
// path of AppPromote, delete-mark purging (0xB713C0), content path checks (0xB71B60), the
// concept_title.db lookup (0xB82160/0xB9B260), notifications to AppDb clients (0xBA49E0).
#include <algorithm>
#include <chrono>
#include <cstdio>
#include <cstring>
#include <ctime>
#include <filesystem>
#include <fstream>
#include <iterator>

#include "core/log.h"
#include "shellcore/shellcore_build.h"
#include "host/boundary.h"
#include "libSceUrlConfigResolver/sce_json.h"
#include "libkernel/process_param.h"
#include "libSceRegMgr/regmgr.h"
#include "shellcore/app_db_internal.h"

namespace ssn::shellcore::appdb {
namespace fs = std::filesystem;
namespace json = ssn::urlcfg::json;

namespace {

constexpr int32_t kParamError = static_cast<int32_t>(0x80BD000Au);  // app param parse error (0x1067790)

// ---------------------------------------------------------------------------------------
// Param list (the SFO-shaped table 0x1067790 fills: 64-byte entries {name[40], value*,
// u16 format, u32 length, u32 max}; format 0x0204 string, 0x0404 int32, 0x0604 int64).
// ---------------------------------------------------------------------------------------
struct ParamEntry {
  std::string name;
  uint16_t format = 0x0204;
  std::string s;
  int64_t i = 0;
};
struct ParamList {
  std::vector<ParamEntry> entries;
  std::string pubtool_version;      // buffer +21722
  std::string music_core_title_id;  // buffer +21730
  const ParamEntry* find(const std::string& n) const {
    for (const auto& e : entries)
      if (e.name == n) return &e;
    return nullptr;
  }
};

// TITLE_xx <-> language table (0x19DDD40).
constexpr const char* kTitleLang[30][2] = {
    {"TITLE_00", "ja-JP"}, {"TITLE_01", "en-US"}, {"TITLE_02", "fr-FR"},   {"TITLE_03", "es-ES"},  {"TITLE_04", "de-DE"},
    {"TITLE_05", "it-IT"}, {"TITLE_06", "nl-NL"}, {"TITLE_07", "pt-PT"},   {"TITLE_08", "ru-RU"},  {"TITLE_09", "ko-KR"},
    {"TITLE_10", "zh-Hant"}, {"TITLE_11", "zh-Hans"}, {"TITLE_12", "fi-FI"}, {"TITLE_13", "sv-SE"}, {"TITLE_14", "da-DK"},
    {"TITLE_15", "no-NO"}, {"TITLE_16", "pl-PL"}, {"TITLE_17", "pt-BR"},   {"TITLE_18", "en-GB"},  {"TITLE_19", "tr-TR"},
    {"TITLE_20", "es-419"}, {"TITLE_21", "ar-AE"}, {"TITLE_22", "fr-CA"},  {"TITLE_23", "cs-CZ"},  {"TITLE_24", "hu-HU"},
    {"TITLE_25", "el-GR"}, {"TITLE_26", "ro-RO"}, {"TITLE_27", "th-TH"},   {"TITLE_28", "vi-VN"},  {"TITLE_29", "id-ID"},
};

// SERVICE_ID_ADDCONT_ADD_n (0x19DDF20).
constexpr const char* kAddcontKeys[7] = {"SERVICE_ID_ADDCONT_ADD_1", "SERVICE_ID_ADDCONT_ADD_2", "SERVICE_ID_ADDCONT_ADD_3",
                                         "SERVICE_ID_ADDCONT_ADD_4", "SERVICE_ID_ADDCONT_ADD_5", "SERVICE_ID_ADDCONT_ADD_6",
                                         "SERVICE_ID_ADDCONT_ADD_7"};

bool parse_json_text(const std::string& text, json::Value* v) { return json::parse(*v, text.data(), text.size()) == 0; }

// Array of strings joined with '\n', the last '\n' removed (0x1067790's usbDir /
// titleIdForTransferring loops).
std::string join_lines(const json::Value& arr) {
  std::string s;
  for (const auto& e : arr.array()) {
    s += e.string();
    s += "\n";
  }
  if (!s.empty()) s.pop_back();
  return s;
}

void add_str(ParamList* p, const char* name, const std::string& v) { p->entries.push_back({name, 0x0204, v, 0}); }
void add_int(ParamList* p, const char* name, int64_t v, uint16_t fmt = 0x0404) { p->entries.push_back({name, fmt, {}, v}); }

bool is_upper_alpha_title_char(unsigned char c) { return (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z'); }

// 0x1067790: the RNPS param.json (camelCase keys) -> param list. The text is parsed four
// times, as the function does; a failure of the first two parses fails the whole call.
int32_t parse_rnps_param_json(const std::string& text, ParamList* out) {
  out->entries.clear();
  json::Value root;
  if (!parse_json_text(text, &root)) return kParamError;
  // 16 int32 entries, always present (getInteger of a missing member is 0).
  const auto geti = [&](const json::Value& v) { return static_cast<int64_t>(static_cast<int32_t>(v.integer())); };
  add_int(out, "CATEGORY_TYPE", geti(root.member("applicationCategoryType")));
  add_int(out, "ATTRIBUTE", geti(root.member("attribute")));
  add_int(out, "ATTRIBUTE2", geti(root.member("attribute2")));
  add_int(out, "ATTRIBUTE3", geti(root.member("attribute3")));
  add_int(out, "ATTRIBUTE_EXE", geti(root.member("attributeExe")));
  add_int(out, "DOWNLOAD_DATA_SIZE", geti(root.member("downloadDataSize")));
  add_int(out, "USER_DEFINED_PARAM_1", geti(root.member("userDefinedParam1")));
  add_int(out, "USER_DEFINED_PARAM_2", geti(root.member("userDefinedParam2")));
  add_int(out, "USER_DEFINED_PARAM_3", geti(root.member("userDefinedParam3")));
  add_int(out, "USER_DEFINED_PARAM_4", geti(root.member("userDefinedParam4")));
  add_int(out, "DISPLAYLOCATION", geti(root.member("displayLocation")));
  add_int(out, "PARENTAL_LEVEL", geti(root.member("parentalLevel")));
  add_int(out, "MASS_SIZE", geti(root.member("massSize")));
  add_int(out, "SERVICE_LAUNCH_BUTTON_KEY_CODE", geti(root.member("serviceLaunchButtonKeyCode")));
  add_int(out, "CONTENT_BADGE_TYPE", geti(root.member("contentBadgeType")));
  add_int(out, "NOTICE_SCREEN_VERSION", geti(root.member("systemService").member("noticeScreenVersion")));

  const auto str_in = [](const std::string& s, size_t max) { return !s.empty() && s.size() <= max; };
  // Second parse: identifiers, URIs, titles.
  if (const std::string& cid = root.member("contentId").string(); str_in(cid, 48)) add_str(out, "CONTENT_ID", cid);
  const std::string& tid = root.member("titleId").string();
  if (tid.size() != 9) {
    log::warn("appdb", "{} Invalid TitleId : [{}] strLength = {}", out->entries.size(), tid, tid.size());
    return kParamError;
  }
  for (size_t k = 0; k < 9; ++k) {
    const unsigned char c = static_cast<unsigned char>(tid[k]);
    const bool ok = k <= 3 ? is_upper_alpha_title_char(c) : (c >= '0' && c <= '9');  // ctype _XA|_LO|_UP / _DI
    if (!ok) {
      log::warn("appdb", "{} Invalid TitleId : [{}]", out->entries.size(), tid);
      return kParamError;
    }
  }
  add_str(out, "TITLE_ID", tid);
  if (const auto& v = root.member("webAppUri").string(); str_in(v, 2048)) add_str(out, "WEBAPP_URI", v);
  if (const auto& v = root.member("deeplinkUri").string(); str_in(v, 2048)) add_str(out, "DEEPLINK_URI", v);
  if (const auto& v = root.member("hubAppUri").string(); str_in(v, 2048)) add_str(out, "HUBAPP_URI", v);
  if (const auto& v = root.member("conceptId").string(); str_in(v, 128)) add_str(out, "CONCEPT_ID", v);
  if (const auto& v = root.member("musicCoreName").string(); str_in(v, 128)) add_str(out, "MUSIC_CORE_NAME", v);
  if (const auto& v = root.member("backgroundBasematType").string(); str_in(v, 64)) add_str(out, "BACKGROUND_BASEMAT_TYPE", v);
  if (const auto& v = root.member("sdkVersion").string(); str_in(v, 19)) add_str(out, "SDK_VERSION", v);
  if (const auto& v = root.member("contentVersion").string(); str_in(v, 11)) add_str(out, "CONTENT_VERSION", v);
  if (const auto& v = root.member("versionFileUri").string(); str_in(v, 256)) add_str(out, "VERSION_FILE_URI", v);
  const json::Value& lp = root.member("localizedParameters");
  for (const auto& tl : kTitleLang) {
    const std::string& t = lp.member(tl[1]).member("titleName").string();
    if (str_in(t, 128) && out->entries.size() <= 255) add_str(out, tl[0], t);
  }
  {
    const std::string& t = lp.member(lp.member("defaultLanguage").string()).member("titleName").string();
    if (str_in(t, 128) && out->entries.size() <= 255) add_str(out, "TITLE", t);
  }

  // Third parse: age level, savedata, DRM type, required system version, USB dirs, addcont,
  // hub background, game intent.
  if (const json::Value& age = root.member("ageLevel"); age.type() != json::Type::Null) {
    const std::string s = json::serialize(age);
    if (str_in(s, 1024)) add_str(out, "AGE_LEVEL", s);
  }
  const json::Value& sd = root.member("savedata");
  if (const auto& v = sd.member("titleIdForSharing").string(); str_in(v, 12)) add_str(out, "INSTALL_DIR_SAVEDATA", v);
  if (const std::string v = join_lines(sd.member("titleIdForTransferring")); str_in(v, 512))
    add_str(out, "SAVE_DATA_TRANSFER_TITLE_ID_LIST", v);
  if (const std::string v = join_lines(sd.member("titleIdForTransferringPs4")); str_in(v, 512))
    add_str(out, "SAVE_DATA_TRANSFER_TITLE_ID_LIST_PS4", v);
  {
    // applicationDrmType: standard 1, upgradable 2, demo 3, free 5, anything else 0.
    const std::string& drm = root.member("applicationDrmType").string();
    int64_t app_type = 0;
    if (drm == "standard") app_type = 1;
    else if (drm == "upgradable") app_type = 2;
    else if (drm == "demo") app_type = 3;
    else if (drm == "free") app_type = 5;
    add_int(out, "APP_TYPE", app_type);
  }
  if (const auto& v = root.member("requiredSystemSoftwareVersion").string(); str_in(v, 32)) {
    char* end = nullptr;
    const unsigned long long n = std::strtoull(v.c_str(), &end, 16);  // std::stoul(s, nullptr, 16)
    if (end != v.c_str()) add_int(out, "SYSTEM_VER_PPR", static_cast<int64_t>(n), 0x0604);
  }
  if (const std::string v = join_lines(root.member("usbDir")); str_in(v, 256)) add_str(out, "USB_DIR_LIST", v);
  {
    const auto& ids = root.member("addcont").member("serviceIdForSharing").array();
    for (size_t k = 0; k < ids.size() && k < 7; ++k) {
      const std::string& s = ids[k].string();
      if (s.size() == 19) {
        const std::string sub = s.substr(7);
        if (sub.rfind("CESA", 0) == 0 || sub.rfind("CESL", 0) == 0) continue;
      }
      if (s.empty() || s.size() > 20 || s[0] == ' ') continue;
      add_str(out, kAddcontKeys[k], s);
    }
  }
  if (const auto& v = root.member("hubData").member("backgroundImageId").string(); str_in(v, 64))
    add_str(out, "BACKGROUND_IMAGE_ID", v);
  if (const json::Value& gi = root.member("gameIntent"); gi.type() != json::Type::Null) {
    const std::string s = json::serialize(gi);
    if (str_in(s, 1024)) add_str(out, "GAME_INTENT", s);
  }
  // Fourth parse: values kept in the side buffer for AppPromote.
  if (const auto& v = root.member("pubtools").member("toolVersion").string(); str_in(v, 8)) out->pubtool_version = v;
  if (const auto& v = root.member("musicCoreTitleId").string(); str_in(v, 12)) out->music_core_title_id = v;
  if (out->entries.size() > 256) out->entries.resize(256);
  return 0;
}

// The 101 param names AppPromote copies into the AppInfo (0x243DD0 table at v1328).
bool is_promoted_param(const std::string& n) {
  static const char* const kNames[] = {
      "TITLE", "TITLE_00", "TITLE_01", "TITLE_02", "TITLE_03", "TITLE_04", "TITLE_05", "TITLE_06", "TITLE_07", "TITLE_08",
      "TITLE_09", "TITLE_10", "TITLE_11", "TITLE_12", "TITLE_13", "TITLE_14", "TITLE_15", "TITLE_16", "TITLE_17", "TITLE_18",
      "TITLE_19", "TITLE_20", "TITLE_21", "TITLE_22", "TITLE_23", "TITLE_24", "TITLE_25", "TITLE_26", "TITLE_27", "TITLE_28",
      "TITLE_29", "PARENTAL_LEVEL", "TITLE_ID", "VERSION", "SYSTEM_VER", "CATEGORY", "FORMAT", "INSTALL_DIR_SAVEDATA",
      "ATTRIBUTE", "ATTRIBUTE2", "ATTRIBUTE_INTERNAL", "ATTRIBUTE_EXE", "APP_VER", "TARGET_APP_VER", "SAVEDATA_MAX_SIZE",
      "CONTENT_ID", "APP_TYPE", "DOWNLOAD_DATA_SIZE", "DOWNLOAD_DATA_SIZE_1", "SERVICE_ID_ADDCONT_ADD_1",
      "SERVICE_ID_ADDCONT_ADD_2", "SERVICE_ID_ADDCONT_ADD_3", "SERVICE_ID_ADDCONT_ADD_4", "SERVICE_ID_ADDCONT_ADD_5",
      "SERVICE_ID_ADDCONT_ADD_6", "SERVICE_ID_ADDCONT_ADD_7", "DISP_LOCATION_1", "DISP_LOCATION_2",
      "PS3_TITLE_ID_LIST_FOR_BOOT", "USER_DEFINED_PARAM_1", "USER_DEFINED_PARAM_2", "USER_DEFINED_PARAM_3",
      "USER_DEFINED_PARAM_4", "REMOTE_PLAY_KEY_ASSIGN", "WEBLINK_URI", "STORE_SCHEMA", "IRO_TAG_BMP_0", "IRO_TAG_BMP_1",
      "IRO_TAG_BMP_2", "IRO_TAG_BMP_3", "IRO_TAG_BMP_4", "IRO_TAG_BMP_5", "IRO_TAG_BMP_6", "IRO_TAG_BMP_7", "USB_DIR_LIST",
      "REMASTER_TYPE", "SAVE_DATA_TRANSFER_TITLE_ID_LIST", "PT_PARAM", "DMEM_FLAG", "SELF_2MIB_PAGE_AMOUNT",
      "CATEGORY_TYPE", "ATTRIBUTE3", "DISPLAYLOCATION", "WEBAPP_URI", "DEEPLINK_URI", "HUBAPP_URI", "AGE_LEVEL",
      "CONCEPT_ID", "MASS_SIZE", "SYSTEM_VER_PPR", "SERVICE_LAUNCH_BUTTON_KEY_CODE", "MUSIC_CORE_NAME",
      "CONTENT_BADGE_TYPE", "BACKGROUND_BASEMAT_TYPE", "SDK_VERSION", "CONTENT_VERSION", "VERSION_FILE_URI",
      "BACKGROUND_IMAGE_ID", "GAME_INTENT", "SAVE_DATA_TRANSFER_TITLE_ID_LIST_PS4", "NOTICE_SCREEN_VERSION",
  };
  for (const char* k : kNames)
    if (n == k) return true;
  return false;
}

// ---------------------------------------------------------------------------------------
// AppInfo key helpers (the daemon's category accessors).
// ---------------------------------------------------------------------------------------

// 0xB280E0: CATEGORY_TYPE & 0xFFFF0000 (the value's bytes 2..3), -1 when the key is absent.
int32_t category_major(const AppInfo& a, bool* ok = nullptr) {
  auto it = a.values.find("CATEGORY_TYPE");
  if (it == a.values.end()) {
    if (ok) *ok = false;
    return -1;
  }
  if (ok) *ok = true;
  return it->second.type == 0 ? static_cast<int32_t>(static_cast<uint32_t>(it->second.i) & 0xFFFF0000u) : 0;
}
// 0xB28390: CATEGORY_TYPE & 0xFF00 (byte 1 << 8), -1 when absent.
int32_t category_minor(const AppInfo& a) {
  auto it = a.values.find("CATEGORY_TYPE");
  if (it == a.values.end()) return -1;
  return it->second.type == 0 ? static_cast<int32_t>(static_cast<uint32_t>(it->second.i) & 0xFF00u) : 0;
}
uint32_t ror8(int32_t v) { return (static_cast<uint32_t>(v) >> 8) | (static_cast<uint32_t>(v) << 24); }

// 0xB28780: DISPLAYLOCATION (integer, non-zero) & mask.
bool display_location_bit(const AppInfo& a, uint32_t mask, bool* out) {
  auto it = a.values.find("DISPLAYLOCATION");
  if (it == a.values.end() || it->second.type != 0 || it->second.i == 0) return false;
  *out = (mask & static_cast<uint32_t>(it->second.i)) != 0;
  return true;
}

// 0xB29A00: the metadata type when METADATA_ID does not name one.
int32_t metadata_type_from_paths(const AppInfo& a) {
  const std::string mp = a.str_or("_metadata_path", "");
  if (mp.rfind("/system_ex/", 0) == 0 || mp.rfind("/system_data/rnps/apps/", 0) == 0) return 4;
  bool ok = false;
  const int32_t major = category_major(a, &ok);
  if (!ok) return 0;
  if (major <= 0x1FFFF) {
    if (major != 0 && major != 0x10000) return 0;
  } else if (major != 0x20000) {
    if (major == 0x4000000) return 4;
    if (major == 0x6000000) {
      static constexpr int32_t kSub[5] = {3, 3, 6, 6, 5};  // dword_18DFC8C
      const uint32_t sub = ror8(category_minor(a));
      return sub <= 4 ? kSub[sub] : 0;
    }
    return 0;
  }
  auto it = a.values.find("_hdd_location");
  if (it == a.values.end()) return 1;
  return 2 - ((it->second.type != 0 || it->second.i == 0) ? 1 : 0);
}

// 0xB296F0: metadata type from METADATA_ID's tag, else 0xB29A00.
int32_t metadata_type(const AppInfo& a) {
  auto it = a.values.find("METADATA_ID");
  if (it == a.values.end() || it->second.type != 2 || it->second.s.empty()) return metadata_type_from_paths(a);
  const std::string& m = it->second.s;
  if (m.find(":internal:") != std::string::npos) return 1;
  if (m.find(":external:") != std::string::npos) return 2;
  if (m.find(":psnow") != std::string::npos) return 3;
  if (m.find(":shellapp") != std::string::npos) return 4;
  if (m.find(":push") != std::string::npos) return 5;
  if (m.find(":shareplay") != std::string::npos) return 6;
  return metadata_type_from_paths(a);
}

// 0xB29EA0: METADATA_ID (and the AppInfo's metadata id) for a metadata type.
void assign_metadata_id(AppInfo& a, int32_t type) {
  std::string m;
  switch (type) {
    case 1: {
      auto it = a.values.find("_m2_device_id");
      m = it == a.values.end() ? ":internal:0" : ":internal:" + std::to_string(it->second.type == 0 ? it->second.i : 0);
      break;
    }
    case 2: {
      const int64_t h = a.int_or("_hdd_location", 0);
      m = ":external:" + std::to_string(h);
      break;
    }
    case 3: m = ":psnow"; break;
    case 4: m = ":shellapp"; break;
    case 5: m = ":push"; break;
    case 6:
      m = ":shareplay";
      host::note_unimplemented("SceShellCore AppDb :shareplay metadata (shareplay_link_user, 0xB28970) not reproduced");
      break;
    default:
      return;
  }
  a.values["METADATA_ID"] = Value::string(m);
  a.metadata_id = m;
}

// 0xB6D690 ExtractPrimaryKey: title id from the AppInfo (or "_title_id"); the metadata id
// as stored unless it is "prior"/empty, in which case the type's tag (0xB29EA0).
int32_t extract_primary_key(AppInfo& a, PrimaryKey* pk) {
  pk->title_id = a.title_id;
  pk->metadata_id = a.metadata_id;
  if (pk->title_id.empty()) {
    auto it = a.values.find("_title_id");
    if (it == a.values.end()) return static_cast<int32_t>(0x81F40009u);
    pk->title_id = it->second.type == 2 ? it->second.s : std::string();
  }
  if (!pk->metadata_id.empty() && pk->metadata_id != "prior") return 0;
  const int32_t type = metadata_type(a);
  if (type == 6) return static_cast<int32_t>(0x81F40011u);
  if (type != 0) {
    assign_metadata_id(a, type);
    pk->metadata_id = a.metadata_id;
    return 0;
  }
  host::note_unimplemented("SceShellCore AppDb ExtractPrimaryKey for metadata type 0 (0xB6A780 lookup) not reproduced");
  return 0;
}

// ---------------------------------------------------------------------------------------
// Derived keys set while inserting (0xB60770 and helpers).
// ---------------------------------------------------------------------------------------

// 0xB5F210: default DISPLAYLOCATION by category.
int64_t default_display_location(const AppInfo& a, int64_t v) {
  if (v != 0) return v;
  const int32_t major = category_major(a);
  if (major <= 0x1FFFF) {
    if (major == 0) return 138;
    if (major == 0x10000) return 188;
    return 17;
  }
  if (major == 0x20000) return category_minor(a) == 512 ? 17 : 154;
  if (major == 0x4000000) return 154;
  if (major == 0x6000000) {
    static constexpr int64_t kLoc[5] = {130, 130, 131, 131, 130};  // dword_18DFD44
    const uint32_t sub = ror8(category_minor(a));
    return sub <= 4 ? kLoc[sub] : 138;
  }
  return 17;
}

// 0xB5ED10: DISPLAYLOCATION from DISP_LOCATION_1/2 when the app has no usable one.
int64_t display_location_from_disp(const AppInfo& a, int32_t concept_type) {
  int32_t l1 = 0, l2 = 0;
  if (auto it = a.values.find("DISP_LOCATION_1"); it != a.values.end() && it->second.type == 0) l1 = static_cast<int32_t>(it->second.i);
  if (auto it = a.values.find("DISP_LOCATION_2"); it != a.values.end() && it->second.type == 0) l2 = static_cast<int32_t>(it->second.i);
  const uint8_t b2 = static_cast<uint8_t>(l2);
  const int32_t odd = (b2 + 1) & 1;
  int32_t v20 = (l1 == -1) + 2 * odd + 132;
  if (((b2 + 1) & 0xA) == 0) v20 = (l1 == -1) + 2 * odd + 128;
  const int32_t v21 = (12 - 2 * b2) & 8;
  bool in_set = false;
  if (a.has("CATEGORY") && concept_type != 2) {
    host::note_unimplemented("SceShellCore AppDb DISPLAYLOCATION category set (unk_1AF9248) not reproduced");
  }
  int32_t v39 = v21 | v20;
  int32_t v43 = in_set ? ((v39 & 0xFFFFFF89) | 4) : v39;
  const bool view1 = a.int_or("_view_category", -1) == 1 && a.values.at("_view_category").type == 0;
  int32_t v54 = view1 ? (v43 | 0x10) : v43;
  if (concept_type == 1) return (v54 & 0xFFFFFFE9) | 0x14;
  return (concept_type == 3) | v54;
}

// 0xB5F320.
void fix_display_location(AppInfo& a, int32_t concept_type) {
  bool fmt_zero_or_bad = false;
  if (auto it = a.values.find("_app_format_type"); it != a.values.end())
    fmt_zero_or_bad = it->second.type != 0 || it->second.i == 0;
  auto dl = a.values.find("DISPLAYLOCATION");
  int64_t v;
  if (fmt_zero_or_bad) {
    if (dl == a.values.end() || dl->second.type != 0 || dl->second.i == 0) v = display_location_from_disp(a, concept_type);
    else v = dl->second.i;
  } else {
    if (dl == a.values.end()) return;
    v = default_display_location(a, dl->second.type == 0 ? dl->second.i : 0);
  }
  a.values["DISPLAYLOCATION"] = Value::integer(v & ~static_cast<int64_t>(a.field120));
}

// 0xB284F0: which hub (1 game hub, 2 media hub) the title belongs to.
int32_t hub_kind(const AppInfo& a, int32_t concept_type, int32_t* kind) {
  bool bit = false;
  if (a.has("DISPLAYLOCATION") && display_location_bit(a, 4, &bit)) {
    *kind = bit ? 2 : 1;
    return 0;
  }
  if (!a.has("CATEGORY_TYPE")) return static_cast<int32_t>(0x81F40011u);
  if (concept_type == 1) {
    *kind = 2;
    return 0;
  }
  const int32_t minor = category_minor(a);
  const int32_t major = category_major(a);
  if (major <= 0x1FFFF) {
    if (major == 0) *kind = 1;
    else if (major == 0x10000) *kind = minor != 62208 ? 2 : 1;
  } else if (major == 0x20000) {
    *kind = 2;
  } else if (major == 0x6000000) {
    const uint32_t sub = ror8(minor);
    if (sub < 4) *kind = 1;
    else if (sub == 4) {
      bool b = false;
      display_location_bit(a, 16, &b);
      *kind = b ? 2 : 1;
    }
  }
  return 0;
}

// 0xB60390.
void default_hub_uri(AppInfo& a, const std::string& title_id, const std::string& prefix) {
  if (a.str_or("HUBAPP_URI", "").size() != 0) return;
  const int32_t major = category_major(a);
  if (major == 0x20000 || major == 0x4000000) return;
  if (title_id == "NPXS29998") return;
  a.values["HUBAPP_URI"] = Value::string(prefix + title_id);
}

// 0xB5F8F0 (+ 0xB5F5F0).
void default_uris(AppInfo& a, const std::string& title_id, int32_t concept_type) {
  if (a.str_or("DEEPLINK_URI", "").empty()) {
    const int32_t major = category_major(a);
    if (major == 0 || major == 0x10000 || major == 0x20000) {
      a.values["DEEPLINK_URI"] = Value::string("psgm:play?id=" + title_id);
    } else if (major == 0x6000000) {
      const uint32_t sub = ror8(category_minor(a));
      if (sub >= 2 && sub <= 4) a.values["DEEPLINK_URI"] = Value::string("");
      else if (sub <= 1) a.values["DEEPLINK_URI"] = Value::string("pscloudplayer:play?titleId=" + title_id + "&context=es");
    }
  }
  int32_t kind = 0;
  hub_kind(a, concept_type, &kind);
  if (kind == 2) default_hub_uri(a, title_id, "psmediahub:main?titleId=");
  else if (kind == 1) default_hub_uri(a, title_id, "pshome:gamehub?titleId=");
  // 0xB5F5F0: broken contents.
  if (auto it = a.values.find("#_contents_status"); it != a.values.end() && it->second.type == 0 && it->second.i == 2) {
    a.values["HUBAPP_URI"] = Value::string("unsupported:main?type=broken");
    a.values["DEEPLINK_URI"] = Value::string("");
  }
}

// Static tables built by 0xB62F30.
int64_t sort_priority_for(const std::string& tid) {
  static const std::pair<const char*, int> kMap[] = {
      {"NPXS40041", 5},  {"NPXS40042", 5},  {"NPXS40047", 6},  {"NPXS20979", 6},  {"NPXS29998", 8},
      {"NPXS40097", 9},  {"NPXS20993", 10}, {"NPXS40063", 11}, {"NPXS40056", 11}, {"NPXS40053", 12},
      {"NPXS40054", 13}, {"NPXS40165", 14}, {"NPXS40071", 1000}, {"NPXS40139", 1000},
  };
  for (const auto& [k, v] : kMap)
    if (tid == k) return v;
  return 100;
}
bool ps_platform_for(const std::string& tid, int64_t* v) {
  static const std::pair<const char*, int> kMap[] = {{"NPXS40145", 1}, {"NPXS40144", -1}, {"NPXS40148", -1}};
  for (const auto& [k, p] : kMap)
    if (tid == k) {
      *v = p;
      return true;
    }
  return false;
}
const char* install_time_for(const std::string& tid) {
  static const std::pair<const char*, const char*> kMap[] = {
      {"NPXS40041", "1994-12-03 00:00:00.130"}, {"NPXS40042", "1994-12-03 00:00:00.120"},
      {"NPXS40047", "1994-12-03 00:00:00.110"}, {"NPXS20979", "1994-12-03 00:00:00.100"},
      {"NPXS29998", "1994-12-03 00:00:00.090"}, {"NPXS40097", "1994-12-03 00:08:00.080"},
      {"NPXS20993", "1994-12-03 00:00:00.070"}, {"NPXS40063", "1994-12-03 00:00:00.060"},
      {"NPXS40037", "1994-12-03 00:00:00.050"}, {"NPXS40032", "1994-12-03 00:00:00.040"},
      {"NPXS40075", "1994-12-03 00:00:00.030"}, {"NPXS40074", "1994-12-03 00:00:00.020"},
      {"NPXS40071", "1994-12-03 00:00:00.010"}, {"NPXS40056", "1994-12-03 00:00:00.550"},
      {"NPXS40053", "1994-12-03 00:00:00.540"}, {"NPXS40054", "1994-12-03 00:00:00.530"},
      {"NPXS40165", "1994-12-03 00:00:00.520"}, {"NPXS40139", "1994-12-03 00:00:00.510"},
  };
  for (const auto& [k, v] : kMap)
    if (tid == k) return v;
  return nullptr;
}

// 0xB60B40.
void set_install_time(AppInfo& a, const std::string& tid) {
  if (metadata_type(a) == 4 && a.has("DISC_INSERT_DATE")) {
    a.values["#_install_time"] = a.values["DISC_INSERT_DATE"];
    return;
  }
  if (const char* t = install_time_for(tid)) {
    a.values["#_install_time"] = Value::string(t);
    return;
  }
  if (a.has("#_ctime")) a.values["#_install_time"] = a.values["#_ctime"];
  else if (a.has("#_promote_time")) a.values["#_install_time"] = a.values["#_promote_time"];
}

// 0xB60770: derived keys of a title being stored.
void derive_keys(AppInfo& a, const std::string& tid, int32_t concept_type) {
  Daemon& d = daemon();
  a.values.emplace("_sort_priority", Value::integer(sort_priority_for(tid)));  // map insert: keeps an existing value
  fix_display_location(a, concept_type);                                      // 0xB5F320
  default_uris(a, tid, concept_type);                                          // 0xB5F8F0
  if (!a.has("_external_hdd_app_status")) a.values["_external_hdd_app_status"] = Value::integer(0);
  set_install_time(a, tid);  // 0xB60B40
  if (int64_t p = 0; ps_platform_for(tid, &p)) a.values["_ps_platform"] = Value::integer(p);
  a.values["sync_index"] = Value::integer(++d.appinfo_sync_index);  // 0xB61030
}

// 0xB2C9D0: _primary_title_sort.
int64_t primary_title_sort(const AppInfo& a) {
  int64_t ps = 0;
  if (auto it = a.values.find("_ps_platform"); it != a.values.end() && it->second.type == 0) ps = it->second.i;
  int64_t v = ps < 3 ? (196608 - (ps << 16)) : (ps << 16);
  int64_t add = 0;
  auto a3 = a.values.find("ATTRIBUTE3");
  if (a3 != a.values.end() && a3->second.type == 0 && (a3->second.i & 0x100)) {
    add = 512;
  } else {
    add = 1024;
    if (auto at = a.values.find("APP_TYPE"); at != a.values.end()) {
      add = 0;
      const uint64_t idx = at->second.type != 0 ? ~0ull : static_cast<uint64_t>(at->second.i - 1);
      static constexpr int64_t kAppType[5] = {0x400, 0x400, 0x100, 0x400, 0x400};  // qword_18DFCA0
      if (idx <= 4) add = kAppType[idx];
    }
  }
  v |= add;
  static constexpr int64_t kMeta[6] = {5, 5, 4, 3, 2, 1};  // qword_18DFCC8
  const int32_t t = metadata_type(a);
  if (t >= 1 && t <= 6) v |= kMeta[t - 1];
  return v;
}

// 0xB5D9C0: the copy InsertAppInfo/UpdateAppInfo store.
AppInfo stored_copy(const AppInfo& in) {
  AppInfo out;
  for (const auto& [k, v] : in.values)
    if (k != "#_access_index" && k != "#_last_access_time") out.values.emplace(k, v);
  // set unk_1B0ED68 (0xBD0200)
  for (const char* k : {"#_promote_time", "#_install_time", "#_last_access_time", "_sort_priority", "#_access_index", "_path_info",
                        "_path_info_2", "_local_concept_id"})
    out.values.erase(k);
  if (auto it = out.values.find("CONCEPT_ID"); it != out.values.end()) {
    if (it->second.type == 2) {
      char* end = nullptr;
      const unsigned long long n = std::strtoull(it->second.s.c_str(), &end, 10);
      if (!it->second.s.empty() && *end == 0) it->second = Value::integer(static_cast<int64_t>(n));
    }
    if (it->second.type != 0 || it->second.i == 0) out.values.erase(it);
  }
  out.field120 = in.field120;
  return out;
}

// 0xB6DE90 (for a title without a stored concept): CONCEPT_ID -> "cid:scp:%016lx",
// otherwise "cid:local:" + title id (0xB31280).
std::string local_concept_id(AppInfo& a, const std::string& tid, int32_t* concept_type) {
  *concept_type = 0;
  if (!a.has("CONCEPT_ID")) {
    host::note_unimplemented("SceShellCore AppDb concept_title.db lookup (/system/priv/mms_ro/concept_title.db, 0xB9B260) not reproduced");
  }
  if (auto it = a.values.find("CONCEPT_ID"); it != a.values.end()) {
    char buf[32];
    std::snprintf(buf, sizeof buf, "%016llx", static_cast<unsigned long long>(it->second.type == 0 ? it->second.i : 0));
    return std::string("cid:scp:") + buf;
  }
  return "cid:local:" + tid;
}

// AppInfo -> ConceptInfo key tables (0x1AFB660 / 0x1AFB750 / 0x1AFAA30, built by 0xB8DFF0).
void map_to_concept(const AppInfo& a, ConceptInfo* c, const std::pair<const char*, const char*>* t, size_t n) {
  for (size_t i = 0; i < n; ++i) {
    auto it = a.values.find(t[i].first);
    if (it != a.values.end()) c->values.emplace(t[i].second, it->second);  // 0xB8D850 + map insert
  }
}
ConceptInfo concept_for_insert(const AppInfo& a) {
  static const std::pair<const char*, const char*> kAccess[] = {
      {"CONCEPT_ID", "concept_id"}, {"#_last_access_time", "last_access_date"}, {"#_access_index", "last_access_index"}};
  static const std::pair<const char*, const char*> kLinks[] = {
      {"DEEPLINK_URI", "deeplink_uri"}, {"HUBAPP_URI", "hubapp_uri"}, {"_sort_priority", "sort_priority"}};
  static std::vector<std::pair<std::string, std::string>> names = [] {
    std::vector<std::pair<std::string, std::string>> v{{"TITLE", "concept_name"}};
    for (int i = 0; i < 30; ++i) {
      char k[16], c[24];
      std::snprintf(k, sizeof k, "TITLE_%02d", i);
      std::snprintf(c, sizeof c, "concept_name_%02d", i);
      v.emplace_back(k, c);
    }
    for (auto [k, c] : {std::pair<const char*, const char*>{"_metadata_path", "metadata_path"}, {"_path_info", "path_info"},
                        {"_path_info_2", "path_info_2"}, {"_title_id", "primary_title_id"},
                        {"_path_icon0_info", "path_icon0_info"}, {"_path_pic0_info", "path_pic0_info"},
                        {"_path_icon0_info_time_stamp", "path_icon0_info_time_stamp"},
                        {"_path_pic0_info_time_stamp", "path_pic0_info_time_stamp"}})
      v.emplace_back(k, c);
    return v;
  }();
  ConceptInfo c;  // 0xB2E1E0(obj, 0)
  map_to_concept(a, &c, kAccess, 3);  // 0xB740E0
  if (auto it = a.values.find("#_size"); it != a.values.end()) c.values.emplace("total_size", it->second);
  map_to_concept(a, &c, kLinks, 3);  // 0xB80870
  if (c.concept_id == 0)
    for (const auto& [k, n] : names)
      if (auto it = a.values.find(k); it != a.values.end()) c.values.emplace(n, it->second);
  return c;
}

// 0xB09850: stat succeeds and the mode is >= 0x4000 (a directory or a regular file). The time
// is the stat's st_mtim seconds (0xB09F70 copies it to +32 of its record).
bool content_file(const std::string& console, int64_t* mtime) {
  std::error_code ec;
  const fs::path p = host_path(console);
  const auto st = fs::status(p, ec);
  if (ec || (st.type() != fs::file_type::regular && st.type() != fs::file_type::directory)) return false;
  // HOST VALUE: the host file's modification time, seconds since the epoch.
  const auto t = fs::last_write_time(p, ec);
  if (ec) return false;
  *mtime = std::chrono::duration_cast<std::chrono::seconds>(
               std::chrono::clock_cast<std::chrono::system_clock>(t).time_since_epoch())
               .count();
  return true;
}

// 0xB364C0: for each language 0..29 the file <dir><base>_%02d<ext> (suffix 0xB318B0) sets bit
// i of the mask, then <dir><base><ext> sets bit 63; the time stamp is the latest mtime.
void scan_content_files(const std::string& base, const std::string& ext, const std::string& dir, uint64_t* mask, int64_t* ts) {
  *mask = 0;
  for (int i = 0; i < 30; ++i) {
    char suffix[8];
    std::snprintf(suffix, sizeof suffix, "_%02d", i);
    int64_t t = 0;
    if (content_file(dir + base + suffix + ext, &t)) {
      *mask |= 1ull << i;
      if (t >= *ts) *ts = t;
    }
  }
  int64_t t = 0;
  if (content_file(dir + base + ext, &t)) {
    *mask |= 1ull << 63;
    if (t >= *ts) *ts = t;
  }
}

// 0xB71B60 checkContentPath(titleId, info, fromInfo = 1): the content files under
// _metadata_path become the _path_* keys.
void check_content_path(AppInfo& a) {
  const Value md = a.at("_metadata_path");
  if (md.type != 2) return;  // no _metadata_path in the record: 0x80F80005, nothing stored
  const std::string& dir = md.s;
  uint64_t icon0 = 0, pic1 = 0, changeinfo = 0, promotion = 0, pic0 = 0, snd0 = 0;
  int64_t icon0_ts = 0, pic1_ts = 0, changeinfo_ts = 0, promotion_ts = 0, pic0_ts = 0, snd0_ts = 0;
  scan_content_files("/icon0", ".png", dir, &icon0, &icon0_ts);
  if (icon0) icon0 |= 0x1000000000000000ull;
  scan_content_files("/pic1", ".dds", dir, &pic1, &pic1_ts);
  scan_content_files("/changeinfo/changeinfo", ".xml", dir, &changeinfo, &changeinfo_ts);
  scan_content_files("/promotion_banner0", ".png", dir, &promotion, &promotion_ts);
  scan_content_files("/pic0", ".dds", dir, &pic0, &pic0_ts);
  if (pic0) pic0 |= 0x2000000000000000ull;
  auto put = [&](const char* k, uint64_t v) { a.values[k] = Value::integer(static_cast<int64_t>(v)); };
  // 0xB726F5: _path_info = changeinfo << 32 | bzhi(icon0, 60); _path_info_2 likewise.
  put("_path_info", (changeinfo << 32) | (icon0 & 0x0FFFFFFFFFFFFFFFull));
  put("_path_info_2", (promotion << 32) | (pic1 & 0x0FFFFFFFFFFFFFFFull));
  put("_path_icon0_info", icon0);
  put("_path_icon0_info_time_stamp", static_cast<uint64_t>(icon0_ts));
  put("_path_pic1_info", pic1);
  put("_path_pic1_info_time_stamp", static_cast<uint64_t>(pic1_ts));
  put("_path_changeinfo_info", changeinfo);
  put("_path_promotion0_info", promotion);
  put("_path_pic0_info", pic0);
  put("_path_pic0_info_time_stamp", static_cast<uint64_t>(pic0_ts));
  scan_content_files("/snd0", ".at9", dir, &snd0, &snd0_ts);
  if (snd0) put("_path_snd0_info_time_stamp", static_cast<uint64_t>(snd0_ts));
}

// 0xB6C700 InsertAppInfo -> 0xB6F980 execInsertAppinfo, for a title the database does not
// hold yet. Returns the stored metadata id through *stored_md.
int32_t insert_app_info(AppInfo& promoted, std::string* stored_md) {
  Daemon& d = daemon();
  PrimaryKey pk;
  if (int32_t r = extract_primary_key(promoted, &pk); r != 0 && r != kErrNotFound) return r;
  AppInfo info = stored_copy(promoted);  // 0xB5D9C0
  info.title_id = pk.title_id;
  info.values["_title_id"] = Value::string(pk.title_id);
  info.metadata_id = pk.metadata_id;  // 0xB6DD50
  info.values["METADATA_ID"] = Value::string(pk.metadata_id);
  info.values["_primary_title_sort"] = Value::integer(primary_title_sort(info));  // 0xB2C9D0
  int32_t concept_type = 0;
  const std::string lcid = local_concept_id(info, pk.title_id, &concept_type);  // 0xB6DE90
  info.values["_local_concept_id"] = Value::string(lcid);
  host::note_unimplemented("SceShellCore AppDb InsertAppInfo primary-title concept list (0xB6EB30) not reproduced");

  std::lock_guard lock(d.mutex);
  if (int32_t r = d.db.begin(); r != 0) return r;
  host::note_unimplemented("SceShellCore AppDb execInsertAppinfo delete-mark purge (0xB713C0, 0xB715D0) and 0xB5E860 not reproduced");
  check_content_path(info);  // 0xB71B60(titleId, info, 1)
  // 0xB6A780: no stored row for the title -> it becomes the prioritised content.
  AppInfo existing;
  int64_t n = 0;
  if (d.db.count_app_info({pk.title_id, "prior%"}, &n) == 0) {
    d.db.end(kErrExists);
    host::note_unimplemented("SceShellCore AppDb execInsertAppinfo for an already stored title (checkMetadataIdStatus 0xB2BEE0) not reproduced");
    return kErrExists;
  }
  const std::string md = "prior" + pk.metadata_id;
  info.metadata_id = md;
  info.values["METADATA_ID"] = Value::string(md);
  derive_keys(info, pk.title_id, concept_type);  // 0xB60770
  int32_t r = d.db.insert_app_info({pk.title_id, md}, info, false);  // 0xBCA1C0 -> 0xBC6BD0
  if (r == 0) {
    // insertConceptInfo (0xB74630) for a new local concept id.
    bool exists = false;
    r = d.db.exists_concept_info(lcid, &exists);
    if (r == 0 && !exists) {
      ConceptInfo c = concept_for_insert(info);
      // 0xB97550: a concept without concept_id / space_type only gets 0xB97C80:
      // sync_index = ++conceptinfo counter (qword_1AFEF58).
      if (c.values.count("concept_id") && c.values.count("space_type"))
        host::note_unimplemented("SceShellCore AppDb 0xB97550 display_location merge for a concept_id concept not reproduced");
      c.values["sync_index"] = Value::integer(++d.conceptinfo_sync_index);
      r = d.db.insert_concept_info(lcid, std::string(), c, false);
    }
    else if (r == 0) host::note_unimplemented("SceShellCore AppDb concept merge for an existing local concept (0xB80C80) not reproduced");
  }
  d.db.end(r);
  if (r == 0) {
    // 0xB61FE0 / 0xB989A0: the sync indices into tbl_version.
    d.db.set_category("appinfo_sync_index", d.appinfo_sync_index);
    d.db.set_category("conceptinfo_sync_index", d.conceptinfo_sync_index);
    host::note_unimplemented("SceShellCore AppDb notifications to AppDb clients (0xBA49E0/0xB9BBF0) not reproduced");
  }
  *stored_md = md;
  return r;
}

// 0x237740: install status of a promoted title.
void install_status(AppInfo& a) {
  const Value cs = a.at("#_contents_status");
  const Value ext = a.at("_contents_ext_type");
  const Value ai = a.at("ATTRIBUTE_INTERNAL");
  a.at("CONTENT_ID");
  const Value ehs = a.at("_external_hdd_app_status");
  const Value ct = a.at("CATEGORY_TYPE");
  const int64_t c = cs.type == 0 ? cs.i : 0;
  const int64_t cat = ct.type == 0 ? ct.i : 0;
  int32_t st = 0, sub = 0, notsub = 0;
  const bool ai_bad = ai.type == 0 && (ai.i & 2) != 0;  // ((v30 & 2) == 0 || v70 != 0)
  if ((c & ~1ll) != 2 && !(ext.type == 0 && ext.i == 3) && !ai_bad && cat != 0x6000200 && cat != 0x6000400 && cat != 0x6000300) {
    if (ehs.type != 0 || ehs.i != 1) {
      if ((cat & ~0x100ll) == 0x6000000) {
        st = 1;
        notsub = 5;
      } else if (c == 1) {
        host::note_unimplemented("SceShellCore AppDb install status of a pre-promoted title (bgft query 0x239AE0) not reproduced");
        sub = 1;
      } else {
        sub = 1;
      }
    } else {
      sub = 4;
    }
  } else {
    st = 1;
    notsub = c == 3 ? 3 : (c == 2 ? 2 : 1);
  }
  a.values["_install_status"] = Value::integer(st);
  a.values["_install_sub_status"] = Value::integer(sub);
  a.values["_not_install_sub_status"] = Value::integer(notsub);
}

std::string utc_timestamp() {
  // 0xB0A080 sceRtcGetCurrentTick + 0xB0A4D0: "YYYY-MM-DD HH:MM:SS.mmm" (UTC).
  // HOST VALUE: the host clock.
  using namespace std::chrono;
  const auto now = system_clock::now();
  const std::time_t t = system_clock::to_time_t(now);
  std::tm tm{};
  gmtime_s(&tm, &t);
  const int ms = static_cast<int>(duration_cast<milliseconds>(now.time_since_epoch()).count() % 1000);
  char buf[32];
  std::snprintf(buf, sizeof buf, "%04d-%02d-%02d %02d:%02d:%02d.%03d", tm.tm_year + 1900, tm.tm_mon + 1, tm.tm_mday, tm.tm_hour,
                tm.tm_min, tm.tm_sec, ms);
  return buf;
}

bool is_regular_file(const std::string& console) {
  std::error_code ec;
  return fs::is_regular_file(host_path(console), ec);
}

// 0x24C300 _uninstallable.
int64_t uninstallable(const AppInfo& a) {
  const bool ai4 = (a.int_or("ATTRIBUTE_INTERNAL", 0) & 4) != 0;
  const bool has_cat = a.has("CATEGORY");
  const bool has_org = a.has("_org_path");
  bool fmt = false;
  if (auto it = a.values.find("_app_format_type"); it != a.values.end()) fmt = (it->second.type == 0 ? it->second.i : 0) != 0;
  if (ai4 || !has_org || !(fmt || has_cat)) return 1;
  const std::string cat = a.str_or("CATEGORY", "");
  for (const char* c : {"gda", "gdf", "gdd", "gdg", "gdh", "gdi", "gdj"})
    if (cat == c) return 0;
  const std::string org = a.str_or("_org_path", "");
  if (org.rfind("/preinst2", 0) == 0 || org.rfind("/system_ex/app", 0) == 0) return 0;
  const int64_t ct = a.int_or("CATEGORY_TYPE", 0);
  for (int64_t v : {0x4000000ll, 0x5000000ll, 0x200F200ll, 0x3000000ll, 0x200F000ll, 0x200F100ll, 0x1000000ll, 0x2000000ll})
    if (ct == v) return 0;
  return 1;
}

// 0x24CA80 _view_category (only CATEGORY-based apps get one).
int64_t view_category(const AppInfo& a, const std::string& tid) {
  if (!a.has("CATEGORY")) return 0;
  host::note_unimplemented("SceShellCore AppPromote _view_category for titles with a CATEGORY (0x24CA80) not reproduced");
  (void)tid;
  return 0;
}

}  // namespace

// 0x243DD0 AppPromote (main) + 0x232950, for a param.json.
int32_t app_promote_json(const std::string& param_path, const std::string& metadata_dir, const std::string& app_path) {
  // 0x232950: a param.json under /system_ex/rnps/apps/, and any param path without ".sfo"
  // elsewhere, is read by 0x10675E0 -> 0x1067790 (file size 1..0x20000).
  std::ifstream f(host_path(param_path), std::ios::binary);
  if (!f) return static_cast<int32_t>(0x80BD0009u);
  const std::string text((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
  if (text.empty() || text.size() > 0x20000) return static_cast<int32_t>(0x80BD0009u);
  ParamList params;
  if (int32_t r = parse_rnps_param_json(text, &params); r != 0) return r;

  AppInfo a;
  std::string title_id;
  for (const auto& e : params.entries) {
    if (!is_promoted_param(e.name)) continue;
    if (e.name == "TITLE_ID") {
      a.values["_title_id"] = Value::string(e.s);
      title_id = e.s.substr(0, 12);
    }
    if (e.name == "USB_DIR_LIST" || e.name == "PS3_TITLE_ID_LIST_FOR_BOOT")
      host::note_unimplemented("SceShellCore AppPromote list splitting (_usb_dir_list_xx / _ps3_title_id_for_boot_xx) not reproduced");
    if (e.format == 0x0604) a.values[e.name] = Value::integer(e.i);
    else if (e.format == 0x0404) a.values[e.name] = Value::integer(static_cast<int32_t>(e.i));
    else a.values[e.name] = Value::string(e.s);
  }
  a.values["_current_slot"] = Value::integer(0);  // 0x261A10: no "<tid>/NN" in the app path
  if (a.has("CATEGORY") && a.str_or("CATEGORY", "") == "gdn")
    host::note_unimplemented("SceShellCore AppPromote gdn sub-app title (0x2393A0) not reproduced");
  if (a.str_or("MUSIC_CORE_NAME", "") == "CustomMusicCore")
    host::note_unimplemented("SceShellCore AppPromote CustomMusicCore sub-app title (0x239750) not reproduced");
  // 0x24C1C0: integer 0 when absent.
  for (const char* k : {"DISP_LOCATION_1", "DISP_LOCATION_2", "USER_DEFINED_PARAM_1", "USER_DEFINED_PARAM_2",
                        "USER_DEFINED_PARAM_3", "USER_DEFINED_PARAM_4", "#exit_type"})
    a.values.emplace(k, Value::integer(0));
  a.values.emplace("TITLE", Value::string(""));
  a.values.emplace("ATTRIBUTE_INTERNAL", Value::integer(0));
  a.values["PUBTOOL_VERSION"] = Value::string(params.pubtool_version);
  {
    // IrjEm08_qoY (system software version, value at +40). HOST VALUE: the firmware version
    // the launcher reports (process parameter sdk version, see libSysBridge/kernel_version).
    const uint64_t v = kernel::procparam_sdk_version() & 0xFFFF00FFFFFFFFFFull;
    a.values["_install_version"] = Value::integer(static_cast<int64_t>(v));
  }
  a.values["_app_format_type"] = Value::integer(1);  // param.json (a9 == 0)
  {
    const int64_t ct = a.int_or("CATEGORY_TYPE", -1);
    int64_t ps = 0;
    if (a.has("CATEGORY_TYPE") && a.values["CATEGORY_TYPE"].type == 0) {
      if ((ct & ~0x200ll) == 0x6000100) ps = 1;
      else if (ct == 0x6000000) ps = 2;
    }
    a.values["_ps_platform"] = Value::integer(ps);
  }
  a.values["_metadata_path"] = Value::string(metadata_dir);
  a.values["_org_path"] = Value::string(app_path.substr(0, 127));
  a.values["_contents_location"] = Value::integer(2);
  a.values["_uninstallable"] = Value::integer(uninstallable(a));
  a.values["_view_category"] = Value::integer(view_category(a, title_id));
  a.values["#_mtime"] = Value::string(utc_timestamp());
  if (is_regular_file(app_path + "/app.pkg"))
    host::note_unimplemented("SceShellCore AppPromote #_ctime from app.pkg (0x243DD0) not reproduced");
  // 0x2606E0(storage 0) + "%s%s/%s": /system_data/priv/appmeta/<tid>/pre_promote.dat.
  a.values["#_contents_status"] = Value::integer(is_regular_file("/system_data/priv/appmeta/" + title_id + "/pre_promote.dat") ? 1 : 0);
  const int64_t ai = a.int_or("ATTRIBUTE_INTERNAL", 0);
  a.values["_disable_live_detail"] = Value::integer((ai & 0x20) != 0);
  host::note_unimplemented("SceShellCore AppPromote disc info (0x261E10) not reproduced; no disc");
  if (ai & 0x10) a.values["_contents_ext_type"] = Value::integer(3);
  a.values["_hdd_location"] = Value::integer(0);
  a.values["_m2_device_id"] = Value::integer(0);

  // ExtractPrimaryKey (0xB3EC40), then GetAppInfo of the stored title (0xB3D4E0 with the
  // type's metadata pattern 0xB2D360, "%:shellapp" for type 4).
  PrimaryKey pk;
  if (int32_t r = extract_primary_key(a, &pk); r != 0) return r;
  Daemon& d = daemon();
  {
    std::lock_guard lock(d.mutex);
    int64_t n = 0;
    std::string pattern = "prior%";
    switch (metadata_type(a)) {
      case 1: pattern = "%:internal:%"; break;
      case 2: pattern = "%:external:%"; break;
      case 3: pattern = "%:psnow"; break;
      case 4: pattern = "%:shellapp"; break;
      case 5: pattern = "%:push"; break;
      case 6: pattern = "%:shareplay"; break;
      default: break;
    }
    if (d.db.count_app_info({pk.title_id, pattern}, &n) == 0) {
      host::note_unimplemented("SceShellCore AppPromote of an already stored title (merge path of 0x243DD0) not reproduced");
      return 0;
    }
  }
  std::string stored_md;
  int32_t r = insert_app_info(a, &stored_md);  // 0xB3D960 -> 0xB6C700
  if (r < 0) {
    log::warn("appdb", "hdb.InsertAppInfo() error {:x}", static_cast<uint32_t>(r));
    return r;
  }
  // LABEL_885: install status, then UpdateAppInfo (0xB3DA20 -> 0xB79B40 -> 0xB7B320): the
  // update stores the promote AppInfo (0xB5D9C0 copy, 0xB60770 derived keys) over the
  // title's prioritised rows with REPLACE (0xBCA240 -> 0xBC6BD0).
  install_status(a);
  host::note_unimplemented("SceShellCore AppDb UpdateAppInfo steps 0xB61FF0/0xB71B60/0xB75BF0/0xB6B740/0xB7C170/0xB5E340/0xB796C0 not reproduced");
  AppInfo upd = stored_copy(a);
  upd.title_id = pk.title_id;
  upd.values["_title_id"] = Value::string(pk.title_id);
  upd.metadata_id = stored_md;
  upd.values["METADATA_ID"] = Value::string(stored_md);
  int32_t ct = 0;
  derive_keys(upd, pk.title_id, ct);
  std::lock_guard lock(d.mutex);
  if ((r = d.db.begin()) != 0) return r;
  r = d.db.insert_app_info({pk.title_id, stored_md}, upd, true);
  d.db.end(r);
  if (r == 0) d.db.set_category("appinfo_sync_index", d.appinfo_sync_index);
  if (r != 0) log::warn("appdb", "hdb.UpdateAppInfo() ret = {:x}", static_cast<uint32_t>(r));
  return r;
}

namespace {

// 0x2294F0 appInstallRNPSTitleDirMain(titleId, root). The devkit build (0x2334F0) has no
// NPXS29998 case.
int32_t install_rnps_title_dir(const std::string& tid, const std::string& root) {
  // sywg_RnhZMA() == 13 also skips NPXS20105; the query is not traced (HOST VALUE: not 13).
  if (tid == "NPXS29998" && !devkit_build()) return 0;
  const std::string appdb = root + tid + "/appdb/";
  std::vector<std::string> subdirs;  // 0x260B80 GetAllSubDir
  std::error_code ec;
  for (const auto& e : fs::directory_iterator(host_path(appdb), ec))
    if (e.is_directory(ec)) subdirs.push_back(e.path().filename().string());
  if (ec) return 0;
  std::sort(subdirs.begin(), subdirs.end());  // HOST POLICY: directory order (getdents) not reproduced; sorted by name
  int32_t result = 0;
  for (const auto& sub : subdirs) {
    const std::string dir = root + tid + "/appdb/" + sub;
    // 0x260CF0: param.json preferred under /system_ex.
    std::string param = dir + "/param.json";
    if (!is_regular_file(param)) param = dir + "/param.sfo";
    if (!is_regular_file(param)) continue;
    if (is_regular_file(dir + "/ignore_cex.dat")) continue;  // CEX unit
    const std::string app_tid = (sub == "default" ? tid : sub).substr(0, 12);
    bool promoted_elsewhere = false;
    for (const char* base : {"/system_data/priv/appmeta/", "/system_data/priv/appmeta/external/"})
      for (const char* fn : {"param.sfo", "param.json"})
        promoted_elsewhere = promoted_elsewhere || is_regular_file(std::string(base) + app_tid + "/" + fn);
    if (promoted_elsewhere) continue;
    if (param.size() < 5 || param.compare(param.size() - 5, 5, ".json") != 0) {
      host::note_unimplemented("SceShellCore AppPromote of an RNPS param.sfo not reproduced");
      continue;
    }
    host::note_unimplemented("SceShellCore appInstallRNPSTitleDirMain download data (0x2547B0) and post hook (0x222DC0) not reproduced");
    const int32_t r = app_promote_json(param, dir, root + tid);
    if (r < 0) {
      log::warn("appdb", "appInstallRNPSTitleDirMain AppPromote Error [{}] 0x{:08x}", tid, static_cast<uint32_t>(r));
      result = r;
      break;
    }
  }
  return result;
}

constexpr int32_t kErrInvalidArg = static_cast<int32_t>(0x80A30003u);
constexpr int32_t kErrNoEntry = static_cast<int32_t>(0x80A30008u);

bool is_directory(const std::string& console) {
  std::error_code ec;
  return fs::is_directory(host_path(console), ec);
}

// 0x254430 appInstallCheckDir(root, titleId): the title directory and its sce_sys exist, with a
// param.sfo or a param.json and an icon0.png.
int32_t install_check_dir(const std::string& root, const std::string& tid) {
  if (root.size() + tid.size() + 8 + 11 > 1023) return kErrInvalidArg;
  if (!is_directory(root) || !is_directory(root + tid)) return kErrNoEntry;
  const std::string sce_sys = root + tid + "/sce_sys";
  if (!is_directory(sce_sys)) return kErrNoEntry;
  if (!is_regular_file(sce_sys + "/param.sfo") && !is_regular_file(sce_sys + "/param.json")) return kErrNoEntry;
  return is_regular_file(sce_sys + "/icon0.png") ? 0 : kErrNoEntry;
}

// 0x260CF0 for a directory outside /system_ex: param.sfo when it exists, else param.json.
std::string param_file(const std::string& sce_sys) {
  const std::string sfo = sce_sys + "/param.sfo";
  return is_regular_file(sfo) ? sfo : sce_sys + "/param.json";
}

// 0x21E940 AppInstallTitleDirMain(titleId, "/user/app/", 1, 0), storage 0 (0x260040).
int32_t install_user_title_dir(const std::string& tid, const std::string& root) {
  // sywg_RnhZMA() == 13 also skips NPXS20105; the query is not traced (HOST VALUE: not 13).
  if (tid == "NPXS29998") return 0;
  // A package title: <tid>/app.pkg, or the first slot 01..15 with <tid>/NN/app.pkg (0x261D00,
  // slot 0 is the unslotted path), goes to 0x220D10, which mounts the package.
  for (int slot = 0; slot < 16; ++slot) {
    char rel[32];
    if (slot == 0) std::snprintf(rel, sizeof rel, "/app.pkg");
    else std::snprintf(rel, sizeof rel, "/%02d/app.pkg", slot);
    if (is_regular_file(root + tid + rel)) {
      host::note_unimplemented("SceShellCore AppInstallTitleDirMain package title (app.pkg, 0x220D10) not reproduced");
      return 0;
    }
  }
  if (const int32_t r = install_check_dir(root, tid); r < 0) {
    log::warn("appdb", "appInstallCheckDir Error [{}] ret = [0x{:08x}]", tid, static_cast<uint32_t>(r));
    return r;
  }
  const std::string sce_sys = root + tid + "/sce_sys";
  const std::string param = param_file(sce_sys);
  if (is_regular_file(sce_sys + "/nptitle.dat"))
    host::note_unimplemented("SceShellCore AppInstallTitleDirMain nptitle.dat copy (0x2606E0, 0x265790) not reproduced");
  host::note_unimplemented("SceShellCore AppInstallTitleDirMain download data (0x2547B0) and post hook (0x222DC0) not reproduced");
  // 0x232950(param, sce_sys, sce_sys, root + tid, 2, 0, 1, 1, -1): a path with ".sfo" goes to the
  // SFO parser (0x106B9F0).
  if (param.find(".sfo") != std::string::npos) {
    host::note_unimplemented("SceShellCore AppPromote of a param.sfo (0x106B9F0) not reproduced");
    return 0;
  }
  const int32_t r = app_promote_json(param, sce_sys, root + tid);
  if (r < 0) log::warn("appdb", "AppPromote Error [{}] ret = [0x{:08x}]", tid, static_cast<uint32_t>(r));
  return r;
}

}  // namespace

// 0x225E40 AppInstallAll.
int32_t app_install_all() {
  host::note_unimplemented("SceShellCore AppInstallAll for /system_ex/app/ and appmeta titles, and its progress counters (0x234D00), not reproduced");
  std::error_code ec;
  {
    const std::string root = "/system_ex/rnps/apps/";
    std::vector<std::string> titles;  // 0x228BA0: the app directories of the root
    for (const auto& e : fs::directory_iterator(host_path(root), ec))
      if (e.is_directory(ec)) titles.push_back(e.path().filename().string());
    std::sort(titles.begin(), titles.end());
    for (const auto& t : titles) {
      const int32_t r = install_rnps_title_dir(t, root);
      if (r < 0) log::warn("appdb", "appInstallRNPSTitleDirMain {} 0x{:08x}", t, static_cast<uint32_t>(r));
    }
  }
  {
    // 0x226C20("/user/app/"): the subdirectories of the root (0x260B80), each through 0x21E940.
    const std::string root = "/user/app/";
    std::vector<std::string> titles;
    for (const auto& e : fs::directory_iterator(host_path(root), ec))
      if (e.is_directory(ec)) titles.push_back(e.path().filename().string());
    std::sort(titles.begin(), titles.end());  // HOST POLICY: directory order (getdents) not reproduced; sorted by name
    for (const auto& t : titles) {
      const int32_t r = install_user_title_dir(t, root);
      if (r < 0) log::warn("appdb", "AppInstallTitleDirMain {} 0x{:08x}", t, static_cast<uint32_t>(r));
    }
  }
  return 0;
}

}  // namespace ssn::shellcore::appdb
