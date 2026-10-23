// SceShellCore launcher service, ShellUI event queue (FW 4.03).
//
// ShellUI's LncUtil client (libSceSystemService, lnc_util_ipc_client.cpp):
//   initialize (0x6b40): IPMI client "SceLncIpcClient" connecting to the server
//     "SceLncService" with one 5736-byte message queue; then getAppStatus for its own appId.
//   getEventForShellUI (0x9000): client not initialised -> 0x80940001; null buffer ->
//     0x80940005; otherwise IpmiClient::tryGetMessage(queue 0, buf, &size, 5736)
//     (libSceIpmi vtable +56, 0xdf0 -> 0x4a20: ipmimgr call 0x252 through libkernel
//     Hk7iHmGxB18, errno -> SCE error by D4yla3vx4tY (libkernel_sys 0x16130: 0x80020000 + errno)).
//     0xdf0 passes the status through unless it is 0x80020002/0x8002000C/0x8002000E/0x80020059.
//   Kernel side (the FW 7.00 kernel image; no FW 4.03 kernel is in hand): op 0x252 ->
//     0xFFFFFFFF80CFB4B0 -> getMessage 0xFFFFFFFF80CFAB30 with a zero wait flag -> the
//     non-blocking receive 0xFFFFFFFF805E1A30, which answers EAGAIN (35) when the channel's
//     head (+136) is empty; getMessage returns it unchanged. ShellUI therefore reads 0x80020023.
// The managed EventMonitor.Update only reads an event when UpdateEvent returns 0, so any
// non-zero status means "no event".
//
// The launcher's application list (LncManager, SceShellCore lnc_manager.cpp; the instance is
// qword_1A55FD8): +4 the registered ShellUI's appId, +8 the focused appId, +12 the
// controller-focused appId, +32 the list. An application record (0x4614E0, 1448 bytes) holds
// +0 appId, +4 the launching app (parent), +8 "launched by the registered ShellUI", +12 userId,
// +16 the app info (+64 title id, +256 type, +260 attr, +272 category, +304 category type),
// +1284 local pid (-1).
#include "shellcore/lnc_service.h"

#include <algorithm>
#include <cstring>
#include <deque>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <list>
#include <mutex>
#include <string>
#include <vector>

#include "host/boundary.h"
#include "host/context.h"
#include "libSceUrlConfigResolver/sce_json.h"
#include "shellcore/lnc_host_app.h"

namespace ssn::shellcore {
namespace {

namespace fs = std::filesystem;

// IPMI tryGetMessage on an empty queue: EAGAIN (kernel 0xFFFFFFFF805E1A30) as SCE_KERNEL_ERROR_EAGAIN.
constexpr int32_t kQueueEmpty = static_cast<int32_t>(0x80020023u);

constexpr int32_t kErrNotFound = static_cast<int32_t>(0x80940004u);
constexpr int32_t kErrInvalidArgument = static_cast<int32_t>(0x80940005u);
constexpr int32_t kErrInvalidAppType = static_cast<int32_t>(0x80940027u);

// SCE_LNC_EVENT_*.
enum : int32_t {
  kEventLaunchStart = 1,
  kEventLaunchEnd = 2,
  kEventKillEnd = 4,
  kEventFocusStart = 4097,
  kEventFocusEnd = 4098,
  kEventControllerFocusStart = 4099,
  kEventControllerFocusEnd = 4100,
};

// SCE_LNC_APP_TYPE_*.
enum : int32_t { kTypeShellUI = 1, kTypeDaemon = 2, kTypeMiniApp = 4, kTypeBigApp = 5, kTypeShellCore = 6 };

struct App {
  int32_t app_id = -1;
  int32_t parent_id = -1;
  bool by_shellui = false;
  int32_t user_id = -1;
  std::string title_id;
  int32_t type = -1;
  int32_t attr = 0;
  std::string category;
  uint32_t category_type = 0;
  std::string app0_dir;
  std::vector<uint8_t> args;
};

struct Manager {
  std::recursive_mutex mu;
  bool initialised = false;
  int32_t shellui_id = -1;           // +4
  int32_t focused_id = -1;           // +8
  int32_t controller_focus_id = -1;  // +12
  std::list<App> apps;               // +32
  int32_t next_app_id = 1;
};

std::mutex g_lock;
std::deque<LncEvent> g_queue;

void copy_str(char* dst, const std::string& s, size_t n) {
  std::memset(dst, 0, n);
  std::memcpy(dst, s.data(), std::min(s.size(), n));
}

// notifyEvent (lnc_event_notifier.cpp, 0x428A40): the record ShellUI reads, from the app.
void notify(int32_t type, const App& a, int32_t error, uint64_t context_id = 0) {
  LncEvent e{};
  e.event_type = type;
  copy_str(e.title_id, a.title_id, sizeof e.title_id);
  e.app_id = a.app_id;
  e.app_type = a.type;
  e.app_attr = a.attr;
  e.launch_request_app_id = a.parent_id;
  e.user_id = a.user_id;
  const size_t n = std::min(a.args.size(), sizeof e.args);
  if (n) std::memcpy(e.args, a.args.data(), n);
  e.args_size = static_cast<int32_t>(a.args.size());
  e.error_code = error;
  e.error_data = 0;      // app +312
  e.app_local_pid = -1;  // app +1284
  copy_str(e.path, a.app0_dir, sizeof e.path);  // 0x40DDD0
  copy_str(e.sce_sys_dir, a.app0_dir.empty() ? std::string() : a.app0_dir + "/sce_sys", sizeof e.sce_sys_dir);  // 0x40DE40
  e.cause_code = 0;
  e.existing_app_id = -1;  // app +808: not traced
  e.check_flag = 0;
  e.context_id = context_id;
  std::memcpy(e.category, a.category.data(), std::min<size_t>(a.category.size(), sizeof e.category));
  e.app_category_type = a.category_type;
  push_event_for_shellui(e);
}

// The category the launcher gives a title from its CATEGORY_TYPE when the app info has no
// CATEGORY (GetOrElse 0x4147F0; a PS5 param.json has none).
std::string category_for(uint32_t ct) {
  switch (ct) {
    case 0: case 0x10000: case 0x10100: case 0x10200: case 0x20000: case 0x20100: return "gd";
    case 0x20200: return "gdp";
    case 0x1000000: return "gdf";
    case 0x2000000: case 0x2000100: return "gdd";
    case 0x3000000: return "gdg";
    case 0x4000000: return "gdi";
    default: return "unknown";
  }
}

// The app type of a category (0x414210, without the VR-mode flags of the check flag).
int32_t type_for(const std::string& c) {
  if (c == "gd" || c == "gda" || c == "gde" || c == "gdc") return kTypeBigApp;
  if (c == "gdf") return kTypeShellUI;
  if (c == "gdd") return kTypeDaemon;
  if (c == "gdg") return 3;
  if (c == "gdj" || c == "gdk") return kTypeMiniApp;
  if (c == "gdl") return 7;
  if (c == "gdi" || c == "gdp" || c == "pn3" || c == "pn4" || c == "gdm" || c == "gdo")
    host::note_unimplemented("SceShellCore lnc 0x414210: app type of category " + c + " not reproduced");
  return -1;
}

Manager& mgr() {
  static Manager m;
  std::lock_guard l(m.mu);
  if (!m.initialised) {
    m.initialised = true;
    // SceShellCore starts ShellUI through the same launchApp (0x768650 -> 0x479760); its
    // param.json has applicationCategoryType 0x1000000 ("gdf", type ShellUI). ShellUI then
    // registers itself (registerShellUI 0x485370 -> 0x47A300), which sets +4.
    App ui;
    ui.app_id = m.next_app_id++;
    ui.title_id = "NPXS40087";
    ui.category_type = 0x1000000;
    ui.category = category_for(ui.category_type);
    ui.type = type_for(ui.category);
    ui.app0_dir = "/system_ex/app/NPXS40087";
    m.shellui_id = ui.app_id;
    m.focused_id = ui.app_id;
    m.apps.push_back(ui);
    host::note_unimplemented(
        "HOST VALUE appIds: the kernel's numbering (sceKernelBeginAppMount2, syscall 706) is not traced; the host "
        "numbers applications 1, 2, ... with ShellUI first");
  }
  return m;
}

App* find_app(Manager& m, int32_t app_id) {
  for (App& a : m.apps)
    if (a.app_id == app_id) return &a;
  return nullptr;
}

// LncUtilWrapper.AppStatus (36 bytes).
#pragma pack(push, 4)
struct AppStatus {
  int32_t app_id, launch_request_app_id, app_type, app_attr;
  char title_id[16];
  uint32_t app_category_type;
};
#pragma pack(pop)
static_assert(sizeof(AppStatus) == 36);

// LncUtilWrapper.LaunchAppParam (40 bytes, LaunchAppParamInit 0x8E00).
#pragma pack(push, 4)
struct LaunchAppParam {
  uint32_t size;
  int32_t user_id, app_attr, enable_crash_report;
  uint64_t check_flag, context_id;
  uint8_t is_speculative_launch;
};
#pragma pack(pop)

void fill_status(const Manager& m, const App& a, AppStatus* s) {
  s->app_id = a.app_id;
  s->launch_request_app_id = a.by_shellui ? m.shellui_id : a.parent_id;
  s->app_type = a.type;
  s->app_attr = a.attr;
  copy_str(s->title_id, a.title_id, sizeof s->title_id);
  s->app_category_type = a.category_type;
}

// onAppExit (0x436460): the application's process has ended.
void on_app_exit(int32_t app_id) {
  Manager& m = mgr();
  std::lock_guard l(m.mu);
  for (auto it = m.apps.begin(); it != m.apps.end(); ++it) {
    if (it->app_id != app_id) continue;
    if (m.shellui_id == app_id) m.shellui_id = -1;
    const App a = *it;
    m.apps.erase(it);  // m_app_list.remove(app)
    host::note_unimplemented(
        "SceShellCore lnc onAppExit: unmounts, sandbox, map file, CDLG shared memory and internal event 32776 not "
        "reproduced");
    notify(kEventKillEnd, a, 0);
    return;
  }
}

// launchApp (lnc_manager.cpp, 0x432630) for ShellUI's request.
int32_t launch_app(const std::string& title_id, const std::vector<uint8_t>& args, const LaunchAppParam& param, int32_t parent) {
  if (title_id.size() != 9) return kErrInvalidArgument;  // isValid(titleId), 0x4183E0
  if (parent < 0) return kErrInvalidArgument;
  Manager& m = mgr();
  App app;
  app.parent_id = parent;
  app.by_shellui = parent == m.shellui_id;
  app.user_id = param.user_id;
  app.attr = param.app_attr;
  app.title_id = title_id;
  app.args = args;
  // app->m_info.query / app->initialize: the installed title's param.json (0x40FE30 ->
  // 0x10675E0), its category and type.
  app.app0_dir = "/user/app/" + title_id;
  std::ifstream f(fs::path(host::console_host_path(app.app0_dir)) / "sce_sys" / "param.json", std::ios::binary);
  const std::string text((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
  urlcfg::json::Value pj;
  if (!f.is_open() || urlcfg::json::parse(pj, text.data(), text.size()) != 0) {
    host::note_unimplemented("SceShellCore lnc launchApp of a title without /user/app/<titleId> (app info query) not reproduced");
    notify(kEventLaunchEnd, app, kErrNotFound, param.context_id);
    return kErrNotFound;
  }
  app.category_type = static_cast<uint32_t>(pj.member("applicationCategoryType").integer());
  app.category = category_for(app.category_type);
  app.type = type_for(app.category);
  if (app.type == -1) {
    notify(kEventLaunchEnd, app, kErrInvalidAppType, param.context_id);
    return kErrInvalidAppType;
  }
  host::note_unimplemented(
      "SceShellCore lnc launchApp: workspace mount, preLaunchCheck, checkExistingApp, CPU budget, application save and "
      "mount roots not reproduced");
  notify(kEventLaunchStart, app, 0, param.context_id);
  {
    std::lock_guard l(m.mu);
    app.app_id = m.next_app_id++;  // beginAppMount: sceKernelBeginAppMount2's result
  }
  const int32_t app_id = app.app_id;
  std::vector<std::string> argv;
  for (size_t i = 0; i < args.size();) {
    const auto* s = reinterpret_cast<const char*>(args.data() + i);
    const size_t len = strnlen(s, args.size() - i);
    argv.emplace_back(s, len);
    i += len + 1;
  }
  // Held until LAUNCH_END is queued, so an application that ends at once is removed after.
  std::lock_guard l(m.mu);
  // spawnApp: HOST POLICY, the PC program of the title.
  if (const int32_t r = lnc_host::spawn(title_id, argv, [app_id] { on_app_exit(app_id); }); r < 0) {
    notify(kEventLaunchEnd, app, r, param.context_id);
    return r;
  }
  m.apps.push_back(app);
  // The kernel gives a spawned application every focus (sceApplicationSpawn2's isSetAllFocus
  // for any type but daemons and ShellCore); for a big or mini app the launcher records it as
  // focused.
  if ((app.type & ~1) == kTypeMiniApp) m.focused_id = app_id;
  notify(kEventLaunchEnd, app, 0, param.context_id);
  return app_id;
}

}  // namespace

void push_event_for_shellui(const LncEvent& e) {
  std::lock_guard<std::mutex> lk(g_lock);
  g_queue.push_back(e);
}

int32_t get_event_for_shellui(LncEvent* out) {
  if (!out) return static_cast<int32_t>(0x80940005u);  // LNC_CHECK "event" (0x9000, line 496)
  std::lock_guard<std::mutex> lk(g_lock);
  if (g_queue.empty()) return kQueueEmpty;
  *out = g_queue.front();
  g_queue.pop_front();
  return 0;
}

// sceLncUtilGetGpuCrashFullDumpAppStatus(status) — libSceSystemService 0x6650 -> 0xA560:
// null -> 0x80940005; client not initialised -> 0x80940001; otherwise IPMI 0x3002F with a
// 1320-byte output -> SceShellCore 0x4866F0 (system processes only) -> lnc_manager 0x43EBC0:
// the first running application whose GPU-crash full-dump flag (+1280) is set fills the
// status; with none, 0x80940004. The host runs no applications, so none has crashed.
SSN_PINVOKE("libSceLncUtilWrapper", "sceLncUtilGetGpuCrashFullDumpAppStatus", int32_t,
            ssn_LncUtil_GetGpuCrashFullDumpAppStatus, (void* status)) {
  if (!status) return static_cast<int32_t>(0x80940005u);
  return static_cast<int32_t>(0x80940004u);
}

// Every call below comes from ShellUI; the server takes the caller's appId from its pid
// (_sceApplicationGetAppId), which is ShellUI's.

// sceLncUtilGetAppStatus(status) — 0x63B0 -> 0x7020: null -> 0x80940005; IPC getAppStatus
// (0x484530) -> 0x43C4F0: the caller's record.
SSN_PINVOKE("libSceLncUtilWrapper", "sceLncUtilGetAppStatus", int32_t, ssn_LncUtil_GetAppStatus, (AppStatus* status)) {
  if (!status) return kErrInvalidArgument;
  Manager& m = mgr();
  std::lock_guard l(m.mu);
  const App* a = find_app(m, m.shellui_id);
  if (!a) return kErrNotFound;
  fill_status(m, *a, status);
  return 0;
}

// sceLncUtilGetAppStatusList(list, numEntries, &outEntries) — getAppStatusList (0x4845F0, system
// processes) -> 0x43C830: null list or count -> 0x80940005; numEntries above 16 -> 0x80940005;
// the records in list order, daemons (2) and ShellCore (6) left out.
SSN_PINVOKE("libSceLncUtilWrapper", "sceLncUtilGetAppStatusList", int32_t, ssn_LncUtil_GetAppStatusList,
            (AppStatus* list, uint32_t num, uint32_t* out)) {
  if (!list || !out || num > 16) return kErrInvalidArgument;
  Manager& m = mgr();
  std::lock_guard l(m.mu);
  uint32_t n = 0;
  for (const App& a : m.apps) {
    if (n >= num) break;
    if ((a.type | 4) == kTypeShellCore) continue;  // 2 and 6
    fill_status(m, a, &list[n++]);
  }
  *out = n;
  return 0;
}

// sceLncUtilGetAppTitleId(appId, titleId) — getAppTitleId (0x484CB0): null -> 0x80940005, else
// "" first; 0x43CC00: the record's title id (16 bytes), not found -> 0x80940004.
SSN_PINVOKE("libSceLncUtilWrapper", "sceLncUtilGetAppTitleId", int32_t, ssn_LncUtil_GetAppTitleId, (int32_t app_id, char* title_id)) {
  if (!title_id) return kErrInvalidArgument;
  title_id[0] = 0;
  Manager& m = mgr();
  std::lock_guard l(m.mu);
  const App* a = find_app(m, app_id);
  if (!a) return kErrNotFound;
  copy_str(title_id, a->title_id, 16);
  return 0;
}

// sceLncUtilGetAppId(titleId) — getAppId (0x484DF0) -> 0x43CDC0: an invalid title id ->
// 0x80940005; the appId of the running title, -1 when none.
SSN_PINVOKE("libSceLncUtilWrapper", "sceLncUtilGetAppId", int32_t, ssn_LncUtil_GetAppId, (const char* title_id)) {
  if (!title_id || std::strlen(title_id) != 9) return kErrInvalidArgument;
  Manager& m = mgr();
  std::lock_guard l(m.mu);
  for (const App& a : m.apps)
    if (a.title_id == title_id) return a.app_id;
  return -1;
}

// sceLncUtilLaunchApp(titleId, argv, param) — 0x6290 -> 0x72D0: null or not 9-character title,
// or null param -> 0x80940005; argv (NUL-separated, at most 4096 bytes) -> 0x80940005 above
// that. Async IPC 0x30000 -> begin_launchApp (0x482580: the same checks, the caller's appId as
// parent, a LaunchAppJobItem) -> launchApp (0x432630); the reply is its result, the appId.
SSN_PINVOKE("libSceLncUtilWrapper", "sceLncUtilLaunchApp", int32_t, ssn_LncUtil_LaunchApp,
            (const char* title_id, const char* const* argv, const LaunchAppParam* param)) {
  if (!title_id || std::strlen(title_id) != 9 || !param) return kErrInvalidArgument;
  std::vector<uint8_t> args;
  for (const char* const* a = argv; a && *a; ++a) args.insert(args.end(), *a, *a + std::strlen(*a) + 1);
  if (args.size() > 4096) return kErrInvalidArgument;
  return launch_app(title_id, args, *param, mgr().shellui_id);
}

// sceLncUtilSetAppFocus(appId, flag, contextId) — 0x6590 -> 0x9A70 (IPC) -> begin_setAppFocus
// (0x4857F0) -> setAppFocus (0x43F110).
SSN_PINVOKE("libSceLncUtilWrapper", "sceLncUtilSetAppFocus", int32_t, ssn_LncUtil_SetAppFocus,
            (int32_t app_id, uint64_t check_flag, uint64_t context_id)) {
  (void)check_flag;
  Manager& m = mgr();
  std::lock_guard l(m.mu);
  App* a = find_app(m, app_id);
  if (!a) {
    App none;
    none.app_id = app_id;
    notify(kEventFocusEnd, none, kErrNotFound, context_id);
    return kErrNotFound;
  }
  // checkParentalLevel (0x439630) and the resume of a suspended app (0x46FC10 / 0x439EF0): the
  // host suspends no application.
  notify(kEventFocusStart, *a, 0, context_id);
  // sceApplicationSetApplicationFocus(appId): the kernel's focus; the host's windows keep theirs.
  m.focused_id = app_id;
  m.controller_focus_id = -1;  // sceApplicationSetControllerFocus(SCE_APPLICATION_INVALID_ID)
  notify(kEventFocusEnd, *a, 0, context_id);
  host::note_unimplemented(
      "SceShellCore lnc setAppFocus: kernel focus, suspension of the previously focused app and CDLG suspend/resume "
      "not reproduced");
  return 0;
}

// sceLncUtilSetControllerFocus(appId) — begin_setControllerFocus (0x485A60) -> 0x43FA20.
SSN_PINVOKE("libSceLncUtilWrapper", "sceLncUtilSetControllerFocus", int32_t, ssn_LncUtil_SetControllerFocus, (int32_t app_id)) {
  Manager& m = mgr();
  std::lock_guard l(m.mu);
  App* a = find_app(m, app_id);
  if (!a) {
    App none;
    none.app_id = app_id;
    notify(kEventControllerFocusEnd, none, kErrNotFound);
    return kErrNotFound;
  }
  // +1272 (an active common dialog of the app) -> 0x8094000F; the host has none.
  notify(kEventControllerFocusStart, *a, 0);
  m.controller_focus_id = app_id;  // sceApplicationSetControllerFocus(appId)
  notify(kEventControllerFocusEnd, *a, 0);
  return 0;
}

}  // namespace ssn::shellcore
