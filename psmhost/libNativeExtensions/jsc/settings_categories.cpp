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

}  // namespace

std::string settings_categories_patch(const char* script) {
  if (!script || !std::strstr(script, kListStart)) return {};
  const auto& ids = kept();
  if (!ids) return {};
  const std::string s(script);
  const size_t at = s.rfind(kExport);
  if (at == std::string::npos || s.find_first_not_of(" \t\r\n", at + std::strlen(kExport)) != std::string::npos) {
    log::warn("rnps", "Settings categories module not in the 4.03 form; Settings is left as it is");
    return {};
  }
  std::string list = "[";
  for (size_t i = 0; i < ids->size(); ++i) {
    std::string q;
    for (char c : (*ids)[i])
      if (c != '"' && c != '\\') q.push_back(c);
    list += (i ? ",\"" : "\"") + q + "\"";
  }
  list += "]";
  return s.substr(0, at) + "s.default=r.filter(function(c){return " + list + ".indexOf(c.itemId)>=0})}));" +
         s.substr(at + std::strlen(kExport));
}

}  // namespace ssn::native_ext::jsc
