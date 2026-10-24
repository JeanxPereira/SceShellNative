// HOST POLICY: the categories of the Settings root a host chooses to show. See
// settings_categories.h.
#include "libNativeExtensions/jsc/settings_categories.h"

#include <cstring>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <optional>
#include <vector>

#include "core/log.h"
#include "host/boundary.h"
#include "host/context.h"
#include "libSceUrlConfigResolver/sce_json.h"

namespace ssn::native_ext::jsc {
namespace {

namespace json = urlcfg::json;

// Module 932 of the 4.03 Settings bundle: the list starts with the User's Guide category and
// the module ends by exporting it.
constexpr const char* kListStart = "r=[].concat([],[{itemId:\"id_users_guide_legal\"";
constexpr const char* kExport = "s.default=r}));";

// Module 865 (categoriesList): the reducer cases that add Debug Settings and Voice Command
// after the list.
constexpr const char* kAddDebug = "case\"CategoriesList/ENABLE_DEBUG_SETTINGS\":return _.default.createStateForAddItems(e,t.payload);";
constexpr const char* kAddVoice =
    "case\"CategoriesList/ENABLE_VOICE_COMMAND\":return _.default.createStateForAddItems(e,t.payload,\"id_general_games_apps\");";

// The itemIds to keep, from <cache>/host/settings.json; none without the file.
const std::optional<std::vector<std::string>>& kept() {
  static const std::optional<std::vector<std::string>> ids = []() -> std::optional<std::vector<std::string>> {
    std::ifstream f(std::filesystem::path(host::ctx().cache_dir) / "host" / "settings.json", std::ios::binary);
    if (!f) return std::nullopt;
    const std::string text((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
    json::Value v;
    if (json::parse(v, text.data(), text.size()) != 0 || v.member("categories").type() != json::Type::Array) {
      log::warn("rnps", "host/settings.json: no \"categories\" list; Settings is left as it is");
      return std::nullopt;
    }
    std::vector<std::string> out;
    for (const json::Value& c : v.member("categories").array())
      if (c.type() == json::Type::String) out.push_back(c.string());
    host::note_unimplemented("HOST POLICY settings categories: host/settings.json keeps " + std::to_string(out.size()) +
                             " of the Settings root categories");
    return out;
  }();
  return ids;
}

// JS predicate keeping the items whose itemId is listed.
std::string keep_predicate(const std::vector<std::string>& ids) {
  std::string list = "[";
  for (size_t i = 0; i < ids.size(); ++i) {
    std::string q;
    for (char c : ids[i])
      if (c != '"' && c != '\\') q.push_back(c);
    list += (i ? ",\"" : "\"") + q + "\"";
  }
  return "function(c){return " + list + "].indexOf(c.itemId)>=0}";
}

bool replace(std::string& s, const char* from, const std::string& to) {
  const size_t at = s.find(from);
  if (at == std::string::npos) return false;
  s.replace(at, std::strlen(from), to);
  return true;
}

// The list module: its export filtered.
std::string patch_list(const std::string& s, const std::string& keep) {
  const size_t at = s.rfind(kExport);
  if (at == std::string::npos || s.find_first_not_of(" \t\r\n", at + std::strlen(kExport)) != std::string::npos) {
    log::warn("rnps", "Settings categories module not in the 4.03 form; Settings is left as it is");
    return {};
  }
  return s.substr(0, at) + "s.default=r.filter(" + keep + ")}));" + s.substr(at + std::strlen(kExport));
}

// categoriesList: the items added after the list filtered too; nothing left, the state stays.
std::string patch_adds(std::string s, const std::string& keep) {
  const auto add = [&](const char* tail) {
    return "return function(k){return k.length?_.default.createStateForAddItems(e,k" + std::string(tail) +
           "):e}(t.payload.filter(" + keep + "));";
  };
  const bool debug = replace(s, kAddDebug, "case\"CategoriesList/ENABLE_DEBUG_SETTINGS\":" + add(""));
  const bool voice = replace(s, kAddVoice, "case\"CategoriesList/ENABLE_VOICE_COMMAND\":" + add(",\"id_general_games_apps\""));
  if (!debug || !voice) {
    log::warn("rnps", "Settings categoriesList module not in the 4.03 form; its added items are left as they are");
    return {};
  }
  return s;
}

}  // namespace

std::string settings_categories_patch(const char* script) {
  if (!script) return {};
  const bool list = std::strstr(script, kListStart) != nullptr;
  const bool adds = !list && std::strstr(script, "\"CategoriesList/ENABLE_DEBUG_SETTINGS\"") &&
                    std::strstr(script, "createStateForAddItems");
  if (!list && !adds) return {};
  const auto& ids = kept();
  if (!ids) return {};
  const std::string keep = keep_predicate(*ids);
  return list ? patch_list(script, keep) : patch_adds(script, keep);
}

}  // namespace ssn::native_ext::jsc
