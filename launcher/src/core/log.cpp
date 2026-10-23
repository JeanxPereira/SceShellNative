#include "core/log.h"

#include <chrono>
#include <cstdio>
#include <mutex>

#include <windows.h>

namespace ssn::log {
namespace {
std::mutex g_mutex;
FILE* g_file = nullptr;
const auto g_start = std::chrono::steady_clock::now();

const char* tag(Level l) {
  switch (l) {
    case Level::Debug: return "D";
    case Level::Info: return "I";
    case Level::Warn: return "W";
    case Level::Error: return "E";
  }
  return "?";
}
}  // namespace

void open_file(const std::string& path) {
  std::lock_guard lock(g_mutex);
  if (g_file) fclose(g_file);
  g_file = _fsopen(path.c_str(), "w", _SH_DENYWR);
}

void write(Level level, std::string_view channel, std::string_view message) {
  const double t = std::chrono::duration<double>(std::chrono::steady_clock::now() - g_start).count();
  const std::string line = std::format("{:10.4f} T{:<5} {} [{}] {}\n", t, GetCurrentThreadId(), tag(level), channel, message);
  std::lock_guard lock(g_mutex);
  fwrite(line.data(), 1, line.size(), stderr);
  if (g_file) {
    fwrite(line.data(), 1, line.size(), g_file);
    fflush(g_file);
  }
}

}  // namespace ssn::log
