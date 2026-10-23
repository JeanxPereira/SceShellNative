#include "host/context.h"

#include <filesystem>

namespace ssn::host {

Context& ctx() {
  static Context c;
  return c;
}

std::string console_host_path(const std::string& console_path) {
  namespace fs = std::filesystem;
  const auto& c = ctx();
  auto under = [&](const std::string& prefix, const fs::path& base) -> std::string {
    std::string rest = console_path.substr(prefix.size());
    while (!rest.empty() && rest.front() == '/') rest.erase(rest.begin());
    return (base / fs::path(rest)).string();
  };
  if (console_path.rfind("/system_data", 0) == 0) return under("/system_data", fs::path(c.cache_dir) / "system_data");
  if (console_path.rfind("/system_ex/rnps", 0) == 0 && !c.rnps_root.empty()) return under("/system_ex/rnps", fs::path(c.rnps_root));
  if (console_path.rfind("/user", 0) == 0) return under("/user", fs::path(c.cache_dir) / "user");
  if (c.firmware) return c.firmware->host_path(console_path);
  return console_path;
}

}  // namespace ssn::host
