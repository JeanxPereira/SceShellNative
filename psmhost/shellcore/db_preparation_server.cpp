// SceShellCore DbPreparation manager + "SceDbPreparationServer" (FW 4.03). Addresses are
// SceShellCore.elf offsets. See db_preparation_server.h and docs/app-db.md.
#include "shellcore/db_preparation_server.h"

#include <cstring>
#include <filesystem>
#include <mutex>

#include "core/log.h"
#include "host/boundary.h"
#include "shellcore/app_db.h"
#include "shellcore/app_db_internal.h"

namespace ssn::shellcore::dbp_server {
namespace {

using dbp::Event;

// The ShellCore preparer (0x778A90: vtable 0x19AA380): +16 initialized, +20 prime user id,
// +24 recovery error, +28 preparation error, +32 "system db dir has files", +0x28 event.
struct Preparer {
  std::recursive_mutex mutex;  // +8 "SceDbPreparationPreparer"
  bool initialized = false;
  int32_t prime_user_id = -1;
  int32_t recovery_error = 0;
  int32_t preparation_error = 0;
  uint8_t system_db_dir = 0;
  int32_t field36 = 0;
  Event event{};
};

// The DbRecovery state singleton (0x1259530): bad power-down byte, state dword (initially 1).
struct RecoveryState {
  uint8_t bad_power_down = 0;
  int32_t state = 1;
};

struct Manager {
  std::recursive_mutex manager_mutex;            // 1AB4850 "SceDbPreparationManager"
  dbp::Thread preparation{manager_mutex};        // 1AB4858
  Preparer preparer;                             // 1AB4860
  std::recursive_mutex notifier_mutex;           // 1AB4868 +8 "SceDbPreparationNotifier"
  bool notified = false;                         // notifier +96
  RecoveryState recovery;
  std::once_flag started;
};

Manager& mgr() {
  static Manager m;
  return m;
}

// The ShellCore units (0x77A420). Units whose implementation is not reproduced prepare as
// a unit without an implementation (0x38D10 returns 0), logged once.
struct Unit {
  const char* name;
  int32_t error;  // the unit's error code (+48)
  int32_t (*prepare)();
  int32_t (*terminate)();
};
int32_t not_reproduced_unit() { return 0; }
const Unit kCexUnits[] = {
    {"app.db", static_cast<int32_t>(0x809C104Fu), [] { return shellcore::appdb::daemon_prepare(); },
     [] { return shellcore::appdb::daemon_terminate(); }},
    {"av_content.db", static_cast<int32_t>(0x809C1050u), nullptr, nullptr},
    {"savedata.db", static_cast<int32_t>(0x809C1051u), nullptr, nullptr},
    {"share", static_cast<int32_t>(0x809C1052u), nullptr, nullptr},
    {"add_content.db", static_cast<int32_t>(0x809C1053u), nullptr, nullptr},
    {"add_content_backup.db", static_cast<int32_t>(0x809C107Du), nullptr, nullptr},
};

// 0x7799F0 openErrorDialog: sceShellCoreUtilPostPsmEventToShellUI(104, {errorCode,
// errorDetail}) — the ShellUI's error dialog event is not reproduced.
void open_error_dialog(int32_t code, const char* detail) {
  log::error("dbp", "[DBP][Core] openErrorDialog errorCode=0x{:08x} errorDetail={}", static_cast<uint32_t>(code), detail);
  host::note_unimplemented("SceShellCore DbPreparation error dialog event to ShellUI (PostPsmEventToShellUI 104) not reproduced");
}

// 0x779860 prepare(which, event, record).
void prepare_units(int which, const Event& ev, bool record) {
  Preparer& p = mgr().preparer;
  if (ev.cex_or_kratos == -1 || ev.event_type == -1) {
    if (p.preparation_error == 0) p.preparation_error = dbp::kErrInvalidArgument;
    open_error_dialog(dbp::kErrInvalidArgument, "[bug 135995] Invalid Argument");
    return;
  }
  if (ev.cex_or_kratos != 0 || which != 0) {
    // Kratos lists (1AB4B20 / 1AB4B50) and the CEX user list (1AB4B18) are not reproduced.
    host::note_unimplemented("SceShellCore DbPreparation Kratos / per-user unit lists not reproduced");
    return;
  }
  const bool prepare = ev.event_type == 0 || ev.event_type == 2 || ev.event_type == 4;  // bit set 0x15
  for (const Unit& u : kCexUnits) {
    int32_t r = 0;
    auto fn = prepare ? u.prepare : u.terminate;
    if (fn) {
      r = fn();
    } else {
      host::note_unimplemented(std::string("SceShellCore DbPreparation unit '") + u.name + "' not reproduced");
      r = not_reproduced_unit();
    }
    if (r == 0) continue;
    log::warn("dbp", "[DBP Warning][Core] sendTelemetry [bug 135995][{}][err:0x{:08X}][detail:0x{:08X}]", u.name,
              static_cast<uint32_t>(u.error), static_cast<uint32_t>(r));
    if (record) {
      if (p.preparation_error == 0) p.preparation_error = u.error;
    } else {
      open_error_dialog(u.error, "[bug 135995] Preparation Error");
    }
  }
}

// 0x77BE90 cexInitialize.
int32_t cex_initialize(const Event& ev) {
  host::note_unimplemented("SceShellCore DbPreparation user-id cache (0x77E190, user_id_cache.json) not reproduced");
  // 0x7602B0: bad power-down detection. HOST VALUE: the PC keeps no power-loss record.
  host::note_unimplemented("HOST VALUE SceShellCore bad power-down detection (0x7602B0): none");
  // 0x77BDC0 existSystemDbDir: /system_data/priv/mms holds anything but bgft.db.
  {
    Preparer& p = mgr().preparer;
    p.system_db_dir = 0;
    std::error_code ec;
    for (const auto& e : std::filesystem::directory_iterator(shellcore::appdb::host_path("/system_data/priv/mms"), ec)) {
      const std::string n = e.path().filename().string();
      if (n != "." && n != ".." && n != "bgft.db") {
        p.system_db_dir = 1;
        break;
      }
    }
  }
  prepare_units(0, ev, true);
  host::note_unimplemented("SceShellCore DbPreparation recovery check items (0x779BA0) not reproduced");
  mgr().preparer.initialized = true;
  return 0;
}

// 0x779CD0 InitializeDb (same library code as eboot 0x36A00).
int32_t initialize_db(const Event& ev) {
  Preparer& p = mgr().preparer;
  std::lock_guard lock(p.mutex);
  p.event = ev;
  static const char* const kNames[7] = {"Invalid", "Initialize", "Terminate", "CreateUser", "DestroyUser", "Login", "Logout"};
  const uint32_t idx = static_cast<uint32_t>(ev.event_type + 1);
  log::info("dbp", "[DBP Message][Core] InitializeDb start. build = {}, event = {}", ev.cex_or_kratos, idx <= 6 ? kNames[idx] : "?");
  int32_t r = dbp::kErrInvalidArgument;
  if (ev.cex_or_kratos == 0) {
    if (ev.event_type == dbp::kCreateUser) {
      prepare_units(2, ev, false);  // 0x77BFC0
      r = 0;
    } else if (ev.event_type == dbp::kInitialize) {
      r = cex_initialize(ev);  // 0x77BE90 (after 0x12568xx cache enable)
    }
  } else if (ev.cex_or_kratos == 1 && ev.event_type == dbp::kLogin) {
    host::note_unimplemented("SceShellCore DbPreparation Kratos login (0x77C000 / 0x77C380) not reproduced");
    r = 0;
  }
  log::info("dbp", "[DBP Message][Core] InitializeDb end. 0x{:08x}", static_cast<uint32_t>(r));
  return r;
}

// 0x779F70: the error dialogs after the notification.
void report_errors() {
  Preparer& p = mgr().preparer;
  if (p.preparation_error != 0) open_error_dialog(p.preparation_error, "DbPreparation Error");
  else if (p.recovery_error != 0) open_error_dialog(p.recovery_error, "DbRecovery Error");
}

void start_shellcore_preparation() {
  // SceShellCore's own DbPreparation at its boot: the preparation thread with a CEX
  // INITIALIZE event (the ShellCore twin of eboot 0x34F20).
  Manager& m = mgr();
  m.preparation.event = Event{0, -1, 0, {}, dbp::kInitialize};
  m.preparation.start([](dbp::Thread* t) { t->status = initialize_db(t->event); }, "SceDbpPreparationThread");
}

}  // namespace

int32_t connect() {
  std::call_once(mgr().started, start_shellcore_preparation);
  host::note_unimplemented("HOST POLICY SceShellCore DbPreparation started on the first client connect");
  return 0;
}

int32_t initialize_db_preparation(const Event& ev) { return initialize_db(ev); }

int32_t terminate_db_preparation(const Event& ev) {
  // 0x779E80 TerminateDb: CEX TERMINATE -> 0x77BF40, DESTROY_USER -> 0x77BFE0.
  Preparer& p = mgr().preparer;
  std::lock_guard lock(p.mutex);
  p.event = ev;
  if (ev.cex_or_kratos == 0 && (ev.event_type == dbp::kTerminate || ev.event_type == dbp::kDestroyUser)) {
    prepare_units(ev.event_type == dbp::kTerminate ? 0 : 2, ev, false);
    return 0;
  }
  if (ev.cex_or_kratos == 1) {
    host::note_unimplemented("SceShellCore DbPreparation Kratos logout (0x77C2B0 / 0x77C460) not reproduced");
    return 0;
  }
  return dbp::kErrInvalidArgument;
}

int32_t notify_db_initialized(const Event& ev) {
  // 0x778710 -> notifier 0x778C80: the listeners' on-initialized once, then the error dialogs.
  bool first = false;
  {
    std::lock_guard lock(mgr().notifier_mutex);
    if (!mgr().notified) {
      host::note_unimplemented("SceShellCore app.db listener on-initialized (0x1B79D0: promote event 4, AppDBListener, catalog) not reproduced");
      mgr().notified = true;
      first = true;
    }
  }
  (void)ev;
  if (first) report_errors();
  return 0;
}

int32_t notify_db_terminating(const Event& ev) {
  // 0x7787C0 -> 0x778D30.
  std::lock_guard lock(mgr().notifier_mutex);
  (void)ev;
  mgr().notified = false;
  return 0;
}

int32_t get_invalid_user_num(int32_t* num) {
  // 0x77CA00: the invalid-user list (qword_1AB4848 +40..+48), filled by the user-id cache.
  if (!num) return dbp::kErrInvalidArgument;
  *num = 0;
  return 0;
}

int32_t get_invalid_user(int32_t index, int32_t* user) {
  // 0x77CA10: out of range leaves the value untouched.
  (void)index;
  if (!user) return dbp::kErrInvalidArgument;
  return 0;
}

int32_t delete_invalid_user() {
  // 0x777340: callbacks for each invalid user; the list is empty on the host.
  return 0;
}

int32_t is_bad_power_down_detected(uint8_t* detected) {
  if (!detected) return dbp::kErrInvalidArgument;
  *detected = mgr().recovery.bad_power_down;  // 0x1259590
  return 0;
}

int32_t get_status(uint8_t out[24]) {
  // 0x77CA60 -> 0x778820 -> 0x77C540: preparer bytes +16..+40, then [1] = notified,
  // dword [20] = the recovery state.
  if (!out) return dbp::kErrInvalidArgument;
  const Preparer& p = mgr().preparer;
  std::memset(out, 0, 24);
  out[0] = p.initialized ? 1 : 0;
  std::memcpy(out + 4, &p.prime_user_id, 4);
  std::memcpy(out + 8, &p.recovery_error, 4);
  std::memcpy(out + 12, &p.preparation_error, 4);
  out[16] = p.system_db_dir;
  out[1] = mgr().notified ? 1 : 0;
  std::memcpy(out + 20, &mgr().recovery.state, 4);
  return 0;
}

int32_t wait_for_initialization(int32_t* result) {
  // 0x77CAA0 / 0x77CCA0: WaitThread(1AB4858), then completes the async request (0x16000A).
  std::call_once(mgr().started, start_shellcore_preparation);
  const int32_t r = mgr().preparation.wait();
  if (result) *result = r;
  return 0;
}

}  // namespace ssn::shellcore::dbp_server
