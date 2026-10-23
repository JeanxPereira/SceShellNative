// HOST POLICY: the process of an application SceShellCore's launcher starts. See lnc_host_app.h.
#include "shellcore/lnc_host_app.h"

#include <windows.h>
#include <shellapi.h>
#include <tlhelp32.h>

#include <chrono>
#include <cwctype>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <map>
#include <thread>

#include "core/log.h"
#include "host/boundary.h"
#include "host/context.h"
#include "libSceUrlConfigResolver/sce_json.h"
#include "platform/input.h"
#include "platform/window.h"

namespace ssn::shellcore::lnc_host {
namespace {

namespace fs = std::filesystem;
namespace json = urlcfg::json;

constexpr int32_t kErrNoEntry = static_cast<int32_t>(0x80020002u);  // ENOENT
constexpr auto kAppearTimeout = std::chrono::seconds(120);
constexpr auto kPoll = std::chrono::seconds(2);

struct Program {
  std::string exe, cwd, uri, watch;
  std::vector<std::string> args;
  std::string command_line;  // "args" given as text: appended as it is
};

std::wstring wide(const std::string& s) {
  if (s.empty()) return {};
  const int n = MultiByteToWideChar(CP_UTF8, 0, s.data(), static_cast<int>(s.size()), nullptr, 0);
  std::wstring w(n, L'\0');
  MultiByteToWideChar(CP_UTF8, 0, s.data(), static_cast<int>(s.size()), w.data(), n);
  return w;
}

const std::map<std::string, Program>& programs() {
  static const std::map<std::string, Program> table = [] {
    std::map<std::string, Program> t;
    std::ifstream f(fs::path(host::ctx().cache_dir) / "host" / "apps.json", std::ios::binary);
    if (!f) return t;
    const std::string text((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
    json::Value v;
    if (json::parse(v, text.data(), text.size()) != 0 || v.type() != json::Type::Object) return t;
    for (const auto& [tid, e] : v.object()) {
      Program p;
      p.exe = e.member("exe").string();
      p.cwd = e.member("cwd").string();
      p.uri = e.member("uri").string();
      p.watch = e.member("watch").string();
      if (e.member("args").type() == json::Type::String) p.command_line = e.member("args").string();
      for (const json::Value& a : e.member("args").array()) p.args.push_back(a.string());
      if (!p.exe.empty() || !p.uri.empty()) t[tid] = std::move(p);
    }
    host::note_unimplemented("HOST POLICY application processes: host/apps.json names a PC program for " +
                             std::to_string(t.size()) + " titles");
    return t;
  }();
  return table;
}

// Windows command-line quoting of one argument.
std::wstring quote(const std::wstring& a) {
  if (!a.empty() && a.find_first_of(L" \t\"") == std::wstring::npos) return a;
  std::wstring o = L"\"";
  size_t slashes = 0;
  for (wchar_t c : a) {
    if (c == L'\\') {
      ++slashes;
      continue;
    }
    o.append(c == L'"' ? slashes * 2 + 1 : slashes, L'\\');
    slashes = 0;
    o.push_back(c);
  }
  o.append(slashes * 2, L'\\');
  return o + L"\"";
}

std::wstring lower(std::wstring s) {
  for (auto& c : s) c = static_cast<wchar_t>(std::towlower(c));
  return s;
}

// A process whose executable lies under `dir` is running.
bool watched_running(const std::wstring& dir) {
  std::wstring prefix = lower(fs::path(dir).lexically_normal().make_preferred().wstring());
  if (!prefix.empty() && prefix.back() != L'\\') prefix.push_back(L'\\');
  HANDLE snap = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
  if (snap == INVALID_HANDLE_VALUE) return false;
  PROCESSENTRY32W pe{sizeof pe};
  bool found = false;
  for (BOOL ok = Process32FirstW(snap, &pe); ok && !found; ok = Process32NextW(snap, &pe)) {
    HANDLE h = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, pe.th32ProcessID);
    if (!h) continue;
    wchar_t buf[MAX_PATH * 2];
    DWORD n = static_cast<DWORD>(std::size(buf));
    if (QueryFullProcessImageNameW(h, 0, buf, &n)) found = lower(std::wstring(buf, n)).rfind(prefix, 0) == 0;
    CloseHandle(h);
  }
  CloseHandle(snap);
  return found;
}

}  // namespace

int32_t spawn(const std::string& title_id, const std::vector<std::string>& args, std::function<void()> on_exit) {
  const auto it = programs().find(title_id);
  if (it == programs().end()) {
    host::note_unimplemented("SceShellCore spawnApp of a title the PC cannot run (no host/apps.json entry): 0x80020002");
    return kErrNoEntry;
  }
  const Program& p = it->second;
  HANDLE process = nullptr;
  if (!p.uri.empty()) {
    SHELLEXECUTEINFOW sei{sizeof sei};
    sei.fMask = SEE_MASK_NOCLOSEPROCESS;
    const std::wstring uri = wide(p.uri);
    sei.lpFile = uri.c_str();
    sei.nShow = SW_SHOWNORMAL;
    if (!ShellExecuteExW(&sei)) {
      log::warn("lnc", "{}: opening {} failed ({})", title_id, p.uri, GetLastError());
      return kErrNoEntry;
    }
    process = sei.hProcess;
  } else {
    std::wstring cmd = quote(wide(p.exe));
    for (const std::string& a : p.args) cmd += L" " + quote(wide(a));
    if (!p.command_line.empty()) cmd += L" " + wide(p.command_line);
    for (const std::string& a : args) cmd += L" " + quote(wide(a));
    const std::wstring cwd = wide(p.cwd);
    STARTUPINFOW si{sizeof si};
    PROCESS_INFORMATION pi{};
    if (!CreateProcessW(nullptr, cmd.data(), nullptr, nullptr, FALSE, 0, nullptr, cwd.empty() ? nullptr : cwd.c_str(), &si, &pi)) {
      log::warn("lnc", "{}: starting {} failed ({})", title_id, p.exe, GetLastError());
      return kErrNoEntry;
    }
    CloseHandle(pi.hThread);
    process = pi.hProcess;
  }
  log::info("lnc", "{}: started {}", title_id, p.uri.empty() ? p.exe : p.uri);
  // The console hands the screen and the pad to the application; the PC's window steps aside
  // for the application's own window, and the pad follows the window in front.
  platform::main_window().step_aside();
  platform::input::set_host_app_running(true);
  std::thread([process, watch = wide(p.watch), title_id, on_exit = std::move(on_exit)] {
    if (process) {
      WaitForSingleObject(process, INFINITE);
      CloseHandle(process);
    }
    if (!watch.empty()) {
      const auto deadline = std::chrono::steady_clock::now() + kAppearTimeout;
      bool seen = false;
      for (;;) {
        const bool running = watched_running(watch);
        seen |= running;
        if (seen ? !running : std::chrono::steady_clock::now() >= deadline) break;
        std::this_thread::sleep_for(kPoll);
      }
    }
    log::info("lnc", "{}: application ended", title_id);
    platform::input::set_host_app_running(false);
    platform::main_window().come_to_front();
    on_exit();
  }).detach();
  return 0;
}

}  // namespace ssn::shellcore::lnc_host
