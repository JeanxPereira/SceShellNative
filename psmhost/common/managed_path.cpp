#include "common/managed_path.h"

#include <windows.h>

#include <cctype>

#include "host/boundary.h"

namespace ssn::host {

std::string managed_path(const std::filesystem::path& dir) {
  const std::string s = std::filesystem::absolute(dir).generic_string();
  wchar_t cwd[MAX_PATH] = {};
  GetCurrentDirectoryW(MAX_PATH, cwd);
  const bool same_drive = s.size() >= 3 && s[1] == ':' && s[2] == '/' &&
                          std::toupper(static_cast<unsigned char>(s[0])) == std::toupper(static_cast<int>(cwd[0])) && cwd[1] == L':';
  if (!same_drive) {
    note_unimplemented("HOST VALUE managed_path: " + s + " is not on the current drive; passed with its drive letter");
    return s;
  }
  note_unimplemented("HOST VALUE managed_path: host directories reach managed code '/'-rooted without a drive letter (current drive)");
  return s.substr(2);
}

}  // namespace ssn::host
