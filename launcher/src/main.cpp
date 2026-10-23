// SceShellNative — runs the PS5 ShellUI (NPXS40087) managed code natively on Windows.
//
//   SceShellNative.exe --firmware <dump root> [--overlay <dump root>]... [--cache <dir>] [--rnps <dir>] [--registry-db0 <file>]
//                      [--rnps-version-policy warning] [--hide-version-banner]
//                      [--headless] [--capture-dir <dir> (--capture-every N | --capture-frames a,b,c)]
//                      [--input-script "f300:cross;f420:down:30"] [--exit-after-frame N]
//                      [--pad-on-at-boot] [--cold-boot]
//
// On start the launcher makes sure the prepared assembly cache matches the current
// host boundary (running SceShellNative.Prep in a child process when it does not),
// then hosts Mono 6.8 and runs the firmware's app.exe on the main thread.
#include <windows.h>

#include <filesystem>
#include <fstream>
#include <sstream>
#include <string>
#include <vector>

#include "core/firmware.h"
#include "core/log.h"
#include "host/boundary.h"
#include "host/context.h"
#include "gfx/vk_device.h"
#include "libSceRegMgr/regmgr.h"
#include "mono/mono_host.h"
#include "platform/input.h"
#include "platform/input_script.h"
#include "platform/window.h"

#ifndef SSN_DEFAULT_MONO_ROOT
#define SSN_DEFAULT_MONO_ROOT ""
#endif
#ifndef SSN_DEFAULT_YOGA_DLL
#define SSN_DEFAULT_YOGA_DLL ""
#endif

namespace ssn::eboot {
void start_boot_manager_init();
}
namespace ssn::system {
void create_system_objects();
}

namespace fs = std::filesystem;
using namespace ssn;

namespace {

struct Options {
  std::string firmware;
  std::vector<std::string> overlays;  // extra dump roots (searched after --firmware)
  std::string cache;
  std::string rnps;
  std::string mono_root = SSN_DEFAULT_MONO_ROOT;
  std::string yoga = SSN_DEFAULT_YOGA_DLL;
  std::string registry_db0;           // the kernel's registry default table (registry.db0), optional
  bool rnps_version_warning = false;  // --rnps-version-policy warning
  bool hide_version_banner = false;   // --hide-version-banner
  // --headless and its diagnostics (no window; offscreen rendering, PNG captures, scripted input)
  bool headless = false;
  gfx::CaptureSchedule capture;
  std::string input_script;
  uint64_t exit_after_frame = 0;
  bool pad_on_at_boot = false;  // --pad-on-at-boot: the DualSense is already on (platform/input.h)
  bool cold_boot = false;       // --cold-boot: ShellUI launched as at power-on (psmhost/eboot/boot_args.cpp)
  std::string exec;                // run this managed program from the cache instead of app.exe
  std::vector<std::string> exec_args;
  bool prep_only = false;  // internal: child process that rebuilds the cache
};

fs::path exe_dir() {
  wchar_t buf[MAX_PATH];
  GetModuleFileNameW(nullptr, buf, MAX_PATH);
  return fs::path(buf).parent_path();
}

Options parse(int argc, char** argv) {
  Options o;
  for (int i = 1; i < argc; ++i) {
    std::string a = argv[i];
    auto next = [&]() -> std::string { return i + 1 < argc ? argv[++i] : ""; };
    if (a == "--firmware") o.firmware = next();
    else if (a == "--cache") o.cache = next();
    else if (a == "--overlay") o.overlays.push_back(next());
    else if (a == "--rnps") o.rnps = next();
    else if (a == "--mono") o.mono_root = next();
    else if (a == "--yoga") o.yoga = next();
    else if (a == "--registry-db0") o.registry_db0 = next();
    else if (a == "--rnps-version-policy") o.rnps_version_warning = next() == "warning";
    else if (a == "--hide-version-banner") o.hide_version_banner = true;
    else if (a == "--headless") o.headless = true;
    else if (a == "--capture-dir") o.capture.dir = next();
    else if (a == "--capture-every") o.capture.every = std::strtoull(next().c_str(), nullptr, 10);
    else if (a == "--capture-frames") {
      std::stringstream ss(next());
      for (std::string f; std::getline(ss, f, ',');)
        if (!f.empty()) o.capture.frames.push_back(std::strtoull(f.c_str(), nullptr, 10));
    }
    else if (a == "--input-script") o.input_script = next();
    else if (a == "--exit-after-frame") o.exit_after_frame = std::strtoull(next().c_str(), nullptr, 10);
    else if (a == "--pad-on-at-boot") o.pad_on_at_boot = true;
    else if (a == "--cold-boot") o.cold_boot = true;
    else if (a == "--prep") o.prep_only = true;
    else if (a == "--exec") {
      o.exec = next();
      while (i + 1 < argc) o.exec_args.push_back(argv[++i]);
    }
  }
  if (o.cache.empty()) o.cache = (exe_dir() / "cache").string();
  return o;
}

std::string read_file(const fs::path& p) {
  std::ifstream f(p, std::ios::binary);
  std::stringstream ss;
  ss << f.rdbuf();
  return ss.str();
}

uint64_t fnv1a(const std::string& s, uint64_t h = 1469598103934665603ull) {
  for (unsigned char c : s) h = (h ^ c) * 1099511628211ull;
  return h;
}

// The cache is valid for one (firmware, host boundary, prep tool) combination.
std::string cache_stamp(const Options& o, const std::string& table) {
  const fs::path managed = exe_dir() / "managed";
  uint64_t h = fnv1a(o.firmware);
  h = fnv1a(table, h);
  h = fnv1a(read_file(managed / "SceShellNative.Prep.exe"), h);
  h = fnv1a(read_file(managed / "SceShellNative.Runtime.dll"), h);
  return std::format("{:016x}", h);
}

int run_prep_child(const Options& o) {
  std::string cmd = std::format("\"{}\" --prep --firmware \"{}\" --cache \"{}\" --mono \"{}\" --yoga \"{}\"",
                                (exe_dir() / "SceShellNative.exe").string(), o.firmware, o.cache, o.mono_root, o.yoga);
  if (o.rnps_version_warning) cmd += " --rnps-version-policy warning";
  if (o.hide_version_banner) cmd += " --hide-version-banner";
  if (!o.rnps.empty()) cmd += std::format(" --rnps \"{}\"", o.rnps);
  STARTUPINFOA si{sizeof(si)};
  PROCESS_INFORMATION pi{};
  // Headless runs never open a console window for the prep child either; its output goes
  // wherever the launcher's own goes.
  DWORD flags = 0;
  BOOL inherit = FALSE;
  if (o.headless) {
    flags = CREATE_NO_WINDOW;
    inherit = TRUE;
    si.dwFlags = STARTF_USESTDHANDLES;
    si.hStdInput = GetStdHandle(STD_INPUT_HANDLE);
    si.hStdOutput = GetStdHandle(STD_OUTPUT_HANDLE);
    si.hStdError = GetStdHandle(STD_ERROR_HANDLE);
  }
  if (!CreateProcessA(nullptr, cmd.data(), nullptr, nullptr, inherit, flags, nullptr, nullptr, &si, &pi)) {
    log::error("prep", "CreateProcess failed ({})", GetLastError());
    return -1;
  }
  WaitForSingleObject(pi.hProcess, INFINITE);
  DWORD code = 1;
  GetExitCodeProcess(pi.hProcess, &code);
  CloseHandle(pi.hThread);
  CloseHandle(pi.hProcess);
  return static_cast<int>(code);
}

// HOST POLICY (--rnps): the console's /system_ex/rnps/apps/<titleId> holds the app's
// manifest, appdb and assets (string catalogs, images) next to an RNPSHEDR-encrypted bundle
// that only /dev/rnps can decrypt. --rnps supplies plaintext bundles (and their manifests)
// for some titles. The host composes the app tree the managed code opens with System.IO
// (one directory per app) in <cache>/rnps: every file of the dump's app directory, then the
// --rnps files of that title over them. Files are hard-linked when possible, else copied;
// nothing is generated. Titles only in the dump keep their (encrypted) bundle.
std::string compose_rnps_root(const Firmware& fw, const std::string& user_rnps, const std::string& cache) {
  const fs::path out = fs::path(cache) / "rnps";
  std::error_code ec;
  // Win32 paths over MAX_PATH need the \\?\ form (app asset trees nest deeply).
  auto lp = [](const fs::path& p) { return fs::path(L"\\\\?\\" + fs::absolute(p).make_preferred().wstring()); };
  auto place = [&](const fs::path& from, const fs::path& to) {
    fs::create_directories(to.parent_path(), ec);
    if (fs::exists(to, ec)) {
      if (fs::equivalent(from, to, ec)) return;
      if (fs::file_size(from, ec) == fs::file_size(to, ec) && fs::last_write_time(from, ec) <= fs::last_write_time(to, ec)) return;
      fs::remove(to, ec);
    }
    fs::create_hard_link(from, to, ec);
    if (ec) fs::copy_file(from, to, fs::copy_options::overwrite_existing, ec);
    if (ec) log::warn("rnps", "cannot place {} -> {} ({})", from.string(), to.string(), ec.message());
  };
  auto mirror = [&](const fs::path& src_, const fs::path& dst_) {
    const fs::path src = lp(src_), dst = lp(dst_);
    if (!fs::is_directory(src, ec)) return;
    for (auto it = fs::recursive_directory_iterator(src, ec); it != fs::recursive_directory_iterator(); it.increment(ec)) {
      if (ec) break;
      if (it->is_regular_file(ec)) place(it->path(), dst / fs::relative(it->path(), src, ec));
    }
  };
  for (const char* kind : {"apps", "bgs"}) {
    const fs::path dump = fw.host_path(std::string("/system_ex/rnps/") + kind);
    const fs::path user = fs::path(user_rnps) / kind;
    if (!fs::is_directory(user, ec)) continue;
    for (const auto& title : fs::directory_iterator(user, ec)) {
      if (!title.is_directory(ec)) continue;
      const fs::path name = title.path().filename();
      mirror(dump / name, out / kind / name);
      mirror(title.path(), out / kind / name);
    }
  }
  host::note_unimplemented("HOST POLICY rnps root: dump app trees with the --rnps files over them, composed in " + out.string());
  return out.string();
}

void register_libraries(const Options& o) {
  if (o.rnps_version_warning) host::Boundary::get().add_policy("rnps-version-check=warning");
  // HOST POLICY (cosmetic, temporary; --hide-version-banner): in Warning mode ReactNative.PUI
  // RNPSVersionCheck.ShowWarning appends a red "FW Version Mismatch" label to the app's root
  // scene. With this policy Prep makes ShowWarning return at once; the mismatch is still
  // traced by HandleRNPSIncompatibleVersion (Tracer.Warn). Part of the host table, so the
  // cache stamp tells the two builds apart.
  if (o.hide_version_banner) host::Boundary::get().add_policy("rnps-version-banner=hidden");
  // HOST POLICY: --rnps names a directory of plaintext RNPS bundles supplied by the user (the
  // console's are RNPSHEDR containers only /dev/rnps can decrypt). ReactNative.Vsh.Common's
  // ReactApplicationFactoryManager.Setup hard-codes FactoryConfig.RequireEncryptedBundles = true,
  // which makes JavaScriptBundleDecryptor.ProcessBundleData reject plaintext ("Bundle file must be
  // encrypted."). With --rnps, Prep makes JavaScriptBundleDecryptor.Init store false into
  // _isEncryptionRequired; ProcessBundleData keeps both of its original branches.
  if (!o.rnps.empty()) host::Boundary::get().add_policy("rnps-bundles=plaintext");
  // The ShellUIBackgroundLayer renderer (psmhost/eboot/bglayer*) repaints the display buffer from
  // UpdateNative as on the console, so BGLayerPlugin keeps the firmware's own
  // ClearFrameBufferAutomatically = false (the former host policy "bglayer=not-rendered",
  // Prep ApplyBgLayerNotRendered, is no longer declared).
  // libSceFacebook.Yoga.sprx is Facebook's open-source Yoga; the DllImports name it
  // "SceFacebook.Yoga.sprx".
  if (!o.yoga.empty() && fs::exists(o.yoga)) host::Boundary::get().add_library("SceFacebook.Yoga.sprx", o.yoga);
  // The eboot links SQLite 3.35.5 statically and exports sqlite3_* to Mono, so the
  // DllImport("sqlite3") calls of Sce.Vsh.SQLite reach it; the same release built with the
  // eboot's compile options (third_party/sqlite.cmake) is next to the launcher.
  if (const fs::path sq = exe_dir() / "sqlite3.dll"; fs::exists(sq)) host::Boundary::get().add_library("sqlite3", sq.string());
}

}  // namespace

int main(int argc, char** argv) {
  Options o = parse(argc, argv);
  fs::create_directories(o.cache);
  const fs::path assemblies = fs::path(o.cache) / "assemblies";
  register_libraries(o);
  const std::string table = host::Boundary::get().table();

  if (o.prep_only) {
    log::open_file((fs::path(o.cache) / "prep.log").string());
    fs::create_directories(assemblies);
    const fs::path table_path = fs::path(o.cache) / "host-table.txt";
    std::ofstream(table_path, std::ios::binary) << table;
    const fs::path managed = exe_dir() / "managed";
    mono::RuntimePaths rp{o.mono_root, managed.string()};
    // The managed sets are found through the firmware roots (update-slot directories mapped).
    const Firmware pfw(o.firmware);
    return mono::run_program(rp, (managed / "SceShellNative.Prep.exe").string(),
                             {"--firmware", o.firmware, "--bcl", o.mono_root + "/lib/mono/4.5", "--out", assemblies.string(),
                              "--host-table", table_path.string(), "--runtime",
                              (managed / "SceShellNative.Runtime.dll").string(), "--lib-dir",
                              pfw.host_path("/system_ex/common_ex/lib"), "--app-dir",
                              pfw.host_path("/system_ex/app/NPXS40087/psm/Application")});
  }

  log::open_file((fs::path(o.cache) / "launcher.log").string());
  Firmware fw(o.firmware);
  for (const auto& r : o.overlays) fw.add_root(r);
  if (o.firmware.empty() || !fw.looks_valid()) {
    log::error("main", "--firmware must point at a decrypted dump root (system/, system_ex/ or their update-slot directories system_b/, system_ex_b/); got '{}'", o.firmware);
    return 2;
  }
  log::info("main", "firmware roots: {}", fw.describe());

  const std::string stamp = cache_stamp(o, table);
  const fs::path stamp_path = fs::path(o.cache) / "stamp.txt";
  if (read_file(stamp_path) != stamp) {
    log::info("main", "preparing firmware assemblies into {}", assemblies.string());
    if (int rc = run_prep_child(o); rc != 0) {
      log::error("main", "prep failed ({}), see {}", rc, (fs::path(o.cache) / "prep.log").string());
      return 3;
    }
    std::ofstream(stamp_path, std::ios::binary) << stamp;
  }

  auto& c = host::ctx();
  // The console's registry (system.reg equivalent) lives in the cache directory.
  regmgr::set_store_path((fs::path(o.cache) / "registry.txt").string());
  if (!o.registry_db0.empty()) regmgr::set_db0_path(o.registry_db0);
  c.firmware = &fw;
  c.cache_dir = o.cache;
  c.main_thread_id = GetCurrentThreadId();
  c.cold_boot = o.cold_boot;
  c.rnps_root = o.rnps.empty() ? fw.host_path("/system_ex/rnps") : compose_rnps_root(fw, o.rnps, o.cache);
  if (!o.rnps.empty())
    host::note_unimplemented("HOST POLICY rnps-bundles=plaintext: JavaScriptBundleDecryptor encryption requirement off for --rnps bundles");
  if (o.hide_version_banner)
    host::note_unimplemented("HOST POLICY rnps-version-banner=hidden (cosmetic, temporary): RNPSVersionCheck.ShowWarning returns at once");

  // Video out: one window showing the 1920x1080 display buffer. Headless: no window at
  // all; the display buffer is only rendered offscreen (and captured on request).
  auto& win = platform::main_window();
  auto& dev = gfx::device();
  if (o.headless) {
    if (!dev.init(GetModuleHandleW(nullptr), nullptr)) {
      log::error("main", "cannot create the Vulkan device");
      return 4;
    }
  } else if (!win.create(L"SceShellNative", 1920, 1080) || !dev.init(win.instance(), win.hwnd())) {
    log::error("main", "cannot create the window / Vulkan device");
    return 4;
  }
  // Keyboard -> the emulated DualSense (platform/input.h, docs/input.md).
  platform::input::set_powered_at_boot(o.pad_on_at_boot);
  win.on_key = platform::input::on_key;
  win.on_key_scan = platform::input::on_key_scan;
  if (!o.capture.dir.empty()) {
    std::error_code ec;
    fs::create_directories(o.capture.dir, ec);
    dev.set_capture_schedule(o.capture);
  }
  if (!o.input_script.empty()) {
    std::string err;
    if (!platform::input_script::load(o.input_script, err)) {
      log::error("main", "--input-script: {}", err);
      return 2;
    }
  }
  const bool scripted = !o.input_script.empty();
  const uint64_t exit_after = o.exit_after_frame;
  if (scripted || exit_after) {
    dev.on_frame_presented = [scripted, exit_after](uint64_t frame) {
      if (scripted) platform::input_script::tick(frame);
      if (exit_after && frame >= exit_after) {
        gfx::device().flush_captures();
        log::info("main", "--exit-after-frame {}: exiting", exit_after);
        host::dump_unimplemented_counts();  // the stub hit counts, as at a normal exit
        // Managed threads are still running: end the process without unwinding them.
        TerminateProcess(GetCurrentProcess(), 0);
      }
    };
  }

  system::create_system_objects();
  // The eboot starts its boot-manager init thread next to the PSM runtime.
  eboot::start_boot_manager_init();

  // The firmware's own working directory is the application directory.
  fs::current_path(assemblies);
  mono::RuntimePaths rp{o.mono_root, assemblies.string()};
  if (!o.exec.empty()) return mono::run_program(rp, (assemblies / o.exec).string(), o.exec_args);
  return mono::run_program(rp, (assemblies / "app.exe").string(), {});
}
