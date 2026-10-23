#include "core/firmware.h"

#include <filesystem>

namespace ssn {

namespace fs = std::filesystem;

void Firmware::add_root(std::string root) {
  Root r{std::move(root), {}};
  std::error_code ec;
  // /system and /system_ex are mounted from the active update slot; a dump of the slot
  // partitions names them <dir>_a / <dir>_b. Map the console directory to the slot directory
  // when the root has no plain one and exactly one slot.
  for (const char* top : {"system", "system_ex"}) {
    if (fs::is_directory(fs::path(r.path) / top, ec)) continue;
    std::string found;
    int n = 0;
    for (const char* slot : {"_a", "_b"}) {
      const std::string name = std::string(top) + slot;
      if (fs::is_directory(fs::path(r.path) / name, ec)) {
        found = name;
        ++n;
      }
    }
    if (n == 1) r.top.emplace_back(top, found);
  }
  roots_.push_back(std::move(r));
}

std::vector<std::string> Firmware::roots() const {
  std::vector<std::string> v;
  for (const auto& r : roots_) v.push_back(r.path);
  return v;
}

std::string Firmware::map(const Root& r, const std::string& rel) const {
  const size_t slash = rel.find_first_of("/\\");
  const std::string first = rel.substr(0, slash);
  for (const auto& [console, dir] : r.top)
    if (first == console) return (fs::path(r.path) / (dir + (slash == std::string::npos ? "" : rel.substr(slash)))).lexically_normal().string();
  return (fs::path(r.path) / rel).lexically_normal().string();
}

bool Firmware::looks_valid() const {
  std::error_code ec;
  return fs::exists(host_path("/system_ex/common_ex/lib/Sce.PlayStation.Core.dll.sprx"), ec) &&
         fs::exists(host_path("/system_ex/app/NPXS40087/psm/Application/app.exe.sprx"), ec);
}

std::string Firmware::host_path(const std::string& console_path) const {
  std::string p = console_path;
  while (!p.empty() && (p.front() == '/' || p.front() == '\\')) p.erase(p.begin());
  std::error_code ec;
  for (const auto& r : roots_) {
    const std::string candidate = map(r, p);
    if (fs::exists(fs::path(candidate), ec)) return candidate;
  }
  return map(roots_.front(), p);
}

std::vector<std::string> Firmware::rnps_decryption_dirs() const {
  std::vector<std::string> v;
  std::error_code ec;
  for (const auto& r : roots_) {
    const fs::path d = fs::path(r.path) / "rnps";
    if (fs::is_directory(d, ec)) v.push_back(d.lexically_normal().string());
  }
  return v;
}

std::string Firmware::describe() const {
  std::string s;
  for (const auto& r : roots_) {
    s += r.path;
    for (const auto& [console, dir] : r.top) s += " /" + console + "=" + dir;
    s += "; ";
  }
  return s;
}

}  // namespace ssn
