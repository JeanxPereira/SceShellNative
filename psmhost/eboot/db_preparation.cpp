// ShellUI eboot DbPreparation (FW 4.03). Addresses are eboot.bin offsets; the managed side is
// Sce.Vsh.DbPreparationWrapper (icall table 0xCC5500..0xCC5630, thunks 0x3B550..0x3B5F0).
// See db_preparation.h and docs/app-db.md.
//
// How the managed side uses it (Legacy.cs BootManager):
//   BackgroundInitDbRecovery: CexCheckRequest() — waits for SceDbpPreparationThread, reads
//     the DbRecovery request; when none, starts SceDbpNotificationThread.
//   EvalNotifyDBInitializeStarted: WaitNotificationThread() — joins the notification thread,
//     then GetDbInitializedEvent() and posts PsmEvent OnNotifyDbInitialized with it.
#include "eboot/db_preparation.h"

#include <algorithm>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <list>
#include <mutex>
#include <vector>

#include "core/log.h"
#include "host/boundary.h"
#include "host/context.h"
#include "shellcore/db_preparation_server.h"

namespace ssn::eboot::dbp {
namespace dbps = ssn::shellcore::dbp_server;
using ssn::dbp::kErrInvalidArgument;
using ssn::dbp::kErrNotImplemented;
using ssn::dbp::kErrState;

int32_t async_storage_initialize(const Event& ev);  // db_preparation_async_storage.cpp

namespace {

// nQ5AHlyJghQ (the kiosk/"Kratos" build test): HOST VALUE retail unit, the CEX paths.
// (The same answer psmhost/shellcore/user_service.cpp uses.)
constexpr bool kKratos = false;

// The DbRecovery state singleton (0x57E170): bad power-down byte, state dword (1).
struct RecoveryState {
  uint8_t bad_power_down = 0;
  int32_t state = 1;
  int32_t cache = 0;  // 0x57D490 +0: EnableCache (0x57E110) / DisableCache (0x57E160)
};
RecoveryState& recovery() {
  static RecoveryState r;
  return r;
}

// ---- units (0x38740) --------------------------------------------------------------------
struct UnitHandle {
  const char* name;
  int32_t error;  // +48 of the list entry
  UnitImpl* impl = nullptr;
  int32_t (*internal_prepare)(const Event&) = nullptr;  // internal handles (async_storage)
};
UnitHandle& handle(UnitSlot s) {
  static UnitHandle app_cex{"app.db", static_cast<int32_t>(0x809C1054u)};
  static UnitHandle app_kratos{"app.db", static_cast<int32_t>(0x809C1054u)};
  static UnitHandle media{"media_event_listener", static_cast<int32_t>(0x809C107Cu)};
  switch (s) {
    case UnitSlot::AppDbCex: return app_cex;
    case UnitSlot::AppDbKratos: return app_kratos;
    default: return media;
  }
}
UnitHandle& async_storage_handle() {
  static UnitHandle h{"async_storage", static_cast<int32_t>(0x809C1058u), nullptr, async_storage_initialize};
  return h;
}

// 0x38700: CEX which 0 -> [app.db, media_event_listener, async_storage] (EBF280); CEX
// which 2 -> EBF2A0 (no unit); Kratos which 1 -> [app.db] (EBF2B0); Kratos which 2 ->
// [app.db (EBF1E0)] (EBF2C0).
std::vector<UnitHandle*> unit_list(int32_t cex_or_kratos, int32_t which) {
  if (cex_or_kratos == 0) {
    if (which == 0) return {&handle(UnitSlot::AppDbCex), &handle(UnitSlot::MediaEventListener), &async_storage_handle()};
    return {};
  }
  if (which == 1) return {&handle(UnitSlot::AppDbCex)};
  if (which == 2) return {&handle(UnitSlot::AppDbKratos)};
  return {};
}

// 0x38D10 / 0x38D80: impl vtable +16 / +24, 0 without an implementation.
int32_t unit_call(UnitHandle& h, const Event& ev, bool prepare) {
  if (h.internal_prepare) return prepare ? h.internal_prepare(ev) : 0;  // 0x38E40 / 0x39A20
  if (!h.impl) {
    if (&h == &handle(UnitSlot::MediaEventListener))
      host::note_unimplemented("eboot DbPreparation unit media_event_listener (MediaEventListener IPC, 0x7238A0) not reproduced");
    else
      host::note_unimplemented("eboot DbPreparation unit app.db: no implementation registered (AppDb client, eboot 0x520)");
    return 0;
  }
  return prepare ? h.impl->prepare(ev) : h.impl->terminate(ev);
}

// ---- notifier (EBF130, vtable 0xCC5440) ---------------------------------------------------
struct Notifier {
  std::recursive_mutex mutex;  // +8 "SceDbPreparationNotifier"
  std::list<Listener*> lists[2];
  bool notified = false;  // +96
};
Notifier& notifier() {
  static Notifier n;
  return n;
}

// ---- invalid users (EBF110, vtable 0xCC4EE8) ------------------------------------------------
struct InvalidUsers {
  std::recursive_mutex mutex;  // +64 "SceDbPreparationInvalidUser"
  std::list<void (*)(int32_t)> callbacks;
  std::vector<int32_t> users;
};
InvalidUsers& invalid_users() {
  static InvalidUsers u;
  return u;
}

// ---- manager (EBF118..EBF160) -------------------------------------------------------------
struct Preparer {
  std::recursive_mutex mutex;  // +8 "SceDbPreparationPreparer"
  bool initialized = false;    // +16
  int32_t prime_user_id = -1;  // +20
  int32_t recovery_error = 0;  // +24
  int32_t preparation_error = 0;  // +28
  Event event{};               // +0x28
};
struct Manager {
  std::recursive_mutex mutex;                  // EBF118 "SceDbPreparationManager"
  ssn::dbp::Thread preparation{mutex};         // EBF120
  Preparer preparer;                           // EBF128
  ssn::dbp::Thread notification{mutex};        // EBF138
  bool pre_notify_installed = false;           // EBF140/EBF160 (std::function)
};
Manager& mgr() {
  static Manager m;
  return m;
}

std::mutex g_client_mutex;  // EBF350 "SceDbPreparationClient"

// The DbPreparation client (0x39D70 connect, 0x39FD0 terminate around each call).
template <class F>
int32_t with_client(F&& f) {
  std::lock_guard lock(g_client_mutex);
  if (int32_t r = dbps::connect(); r != 0) {
    log::error("dbp", "[DBP Error][UI] initialize: DbPreparationClientImp::connect() : ret=0x{:08x}", static_cast<uint32_t>(r));
    return r;
  }
  return f();
}

const char* event_name(int32_t type) {
  static const char* const kNames[7] = {"Invalid", "Initialize", "Terminate", "CreateUser", "DestroyUser", "Login", "Logout"};
  const uint32_t i = static_cast<uint32_t>(type + 1);
  return i <= 6 ? kNames[i] : "Invalid";
}

// 0x36720 openErrorDialog: sceShellCoreUtilPostPsmEventToShellUI(104, {"errorCode",
// "errorDetail"}).
void open_error_dialog(int32_t code, const char* detail) {
  log::error("dbp", "[DBP][UI] openErrorDialog errorCode=0x{:08x} errorDetail={}", static_cast<uint32_t>(code), detail);
  host::note_unimplemented("eboot DbPreparation error dialog (sceShellCoreUtilPostPsmEventToShellUI 104) not reproduced");
}

// 0x36590 prepare(which, event, record).
void prepare(int32_t which, const Event& ev, bool record) {
  Preparer& p = mgr().preparer;
  const auto units = unit_list(ev.cex_or_kratos, which);
  if (ev.cex_or_kratos == -1 || ev.event_type == -1) {
    log::error("dbp", "[DBP Error][UI] prepare: Invalid argument!");
    if (p.preparation_error == 0) p.preparation_error = kErrInvalidArgument;
    open_error_dialog(kErrInvalidArgument, "[bug 135995] Invalid Argument");
    return;
  }
  const bool is_prepare = ev.event_type >= 0 && ev.event_type <= 4 && ((0x15 >> ev.event_type) & 1);
  for (UnitHandle* u : units) {
    const int32_t r = unit_call(*u, ev, is_prepare);
    if (r == 0) continue;
    // 0x35B30 sendTelemetry
    log::warn("dbp", "[DBP Warning][UI] sendTelemetry [bug 135995][{}][err:0x{:08X}][detail:0x{:08X}][{}]", u->name,
              static_cast<uint32_t>(u->error), static_cast<uint32_t>(r), event_name(ev.event_type));
    if (record) {
      if (p.preparation_error == 0) p.preparation_error = u->error;
    } else {
      open_error_dialog(u->error, "[bug 135995] Preparation Error");
    }
  }
}

// 0x3AF00 cexInitialize.
int32_t cex_initialize(const Event& ev) {
  uint8_t bad = 0;
  int32_t r = with_client([&] { return dbps::is_bad_power_down_detected(&bad); });  // IPC 0x160007
  if (r != 0) {
    log::error("dbp", "[DBP Error][UI] getBadPowerDownDetect: IpcClientStub::IsBadPowerDownDetected() fail. [{:#08x}]", static_cast<uint32_t>(r));
  } else if (bad) {
    recovery().bad_power_down = bad;  // 0x57E1C0
  }
  prepare(0, ev, true);
  host::note_unimplemented("eboot DbPreparation recovery check items (0x368D0, table 21EDFC0 of DbRecovery) not reproduced");
  // 0x3A400 BeginWaitForInitialization: IPC 0x160009 (async) + waitEventFlag + 0x16000A.
  int32_t init_result = 0;
  r = with_client([&] { return dbps::wait_for_initialization(&init_result); });
  if (r == 0) r = init_result;
  if (r != 0) log::error("dbp", "[DBP Error][UI] cexInitialize: BeginWaitForInitialization() failed. [{:#08x}]", static_cast<uint32_t>(r));
  return 0;
}

// 0x36A00 InitializeDb (preparer vtable +8).
int32_t initialize_db(const Event& ev) {
  Preparer& p = mgr().preparer;
  std::lock_guard lock(p.mutex);
  p.event = ev;
  log::info("dbp", "[DBP Message][UI] InitializeDb start. build = {}, event = {}", ev.cex_or_kratos, event_name(ev.event_type));
  int32_t r = kErrInvalidArgument;
  if (ev.cex_or_kratos == 1) {
    if (ev.event_type == ssn::dbp::kLogin) {
      host::note_unimplemented("eboot DbPreparation Kratos login (0x3B170 / 0x3B3A0) not reproduced");
      r = 0;
    }
  } else if (ev.cex_or_kratos == 0) {
    if (ev.event_type == ssn::dbp::kCreateUser) {
      // 0x3B090 cexCreateUser: IPC InitializeDbPreparation (0x160000), then prepare(2).
      const int32_t ir = with_client([&] { return dbps::initialize_db_preparation(ev); });
      if (ir != 0) log::error("dbp", "[DBP Error][UI] cexCreateUser: InitializeDbPreparation() failed. [{:#08x}]", static_cast<uint32_t>(ir));
      prepare(2, ev, true);
      r = 0;
    } else if (ev.event_type == ssn::dbp::kInitialize) {
      recovery().cache = 1;  // 0x57B500 EnableCache
      r = cex_initialize(ev);
    }
  }
  if (r == kErrInvalidArgument) log::error("dbp", "[DBP Error][UI] InitializeDb: invalid argument.");
  log::info("dbp", "[DBP Message][UI] InitializeDb end. 0x{:08x}", static_cast<uint32_t>(r));
  return r;
}

// 0x36BB0 TerminateDb (preparer vtable +16).
int32_t terminate_db(const Event& ev) {
  Preparer& p = mgr().preparer;
  std::lock_guard lock(p.mutex);
  p.event = ev;
  if (ev.cex_or_kratos == 0) {
    if (ev.event_type == ssn::dbp::kDestroyUser) {
      // 0x3B100 cexDestroyUser: IPC TerminateDbPreparation (0x160001), then prepare(2, terminate).
      const int32_t tr = with_client([&] { return dbps::terminate_db_preparation(ev); });
      if (tr != 0) log::error("dbp", "[DBP Error][UI] cexDestroyUser: TerminateDbPreparation() failed. [{:#08x}]", static_cast<uint32_t>(tr));
      prepare(2, ev, false);
      return 0;
    }
    if (ev.event_type == ssn::dbp::kTerminate) {
      // 0x3B030 cexTerminate ("not support"): recovery state reset, prepare(0, terminate).
      log::error("dbp", "[DBP Error][UI] cexTerminate: cexTerminate() is not support !!");
      recovery().state = 1;
      recovery().bad_power_down = 0;
      prepare(0, ev, false);
      recovery().cache = 0;  // 0x57B520
      return 0;
    }
  } else if (ev.cex_or_kratos == 1) {
    host::note_unimplemented("eboot DbPreparation Kratos logout (0x3B290 / 0x3B450) not reproduced");
    return 0;
  }
  log::error("dbp", "[DBP Error][UI] TerminateDb: invalid argument.");
  return kErrInvalidArgument;
}

// 0x36CA0 (preparer vtable +48): the error dialogs after the notification.
void report_errors() {
  Preparer& p = mgr().preparer;
  if (p.preparation_error != 0) open_error_dialog(p.preparation_error, "DbPreparation Error");
  else if (p.recovery_error != 0) open_error_dialog(p.recovery_error, "DbRecovery Error");
}

// Notifier 0x3AD20 NotifyDbInitialized: once; IPC 0x160002, then the listeners (0x35700).
bool notify_initialized(const Event& ev) {
  Notifier& n = notifier();
  std::lock_guard lock(n.mutex);
  if (n.notified) return false;
  const int32_t r = with_client([&] { return dbps::notify_db_initialized(ev); });
  if (r != 0) log::error("dbp", "[DBP Error][UI] NotifyDbInitialized: NotifyDbInitialized() failed. [{:#08x}]", static_cast<uint32_t>(r));
  for (auto& list : n.lists)
    for (Listener* l : list)
      if (l) l->on_initialized(ev);
  recovery().cache = 0;  // 0x57B520
  n.notified = true;
  return true;
}

// Notifier 0x3ADB0 NotifyDbTerminating.
bool notify_terminating(const Event& ev) {
  Notifier& n = notifier();
  std::lock_guard lock(n.mutex);
  if (!n.notified) return false;
  for (auto& list : n.lists)
    for (Listener* l : list)
      if (l) l->on_terminating(ev);
  n.notified = false;
  const int32_t r = with_client([&] { return dbps::notify_db_terminating(ev); });
  if (r != 0) log::error("dbp", "[DBP Error][UI] NotifyDbTerminating: NotifyDbTerminating() failed. [{:#08x}]", static_cast<uint32_t>(r));
  return true;
}

// 0x33E90 DeleteInvalidUser (EBF110 vtable +40).
int32_t delete_invalid_user() {
  InvalidUsers& iu = invalid_users();
  // 0x3A770 GetInvalidUsers: IPC 0x160004 / 0x160005 into the list.
  int32_t r = with_client([&] {
    int32_t num = 0;
    int32_t e = dbps::get_invalid_user_num(&num);
    if (e != 0) return e;
    for (int32_t i = 0; i < num; ++i) {
      int32_t uid = -1;
      if ((e = dbps::get_invalid_user(i, &uid)) != 0) return e;
      if (uid != -1) iu.users.push_back(uid);
    }
    return 0;
  });
  if (r != 0) {
    log::error("dbp", "[DBP Error][UI] DeleteInvalidUser: GetInvalidUsers() failed. [{:#x}]", static_cast<uint32_t>(r));
    return r;
  }
  r = with_client([] { return dbps::delete_invalid_user(); });  // IPC 0x160006
  if (r != 0) log::error("dbp", "[DBP Error][UI] DeleteInvalidUser: DeleteInvalidUser() failed. [{:#x}]", static_cast<uint32_t>(r));
  std::lock_guard lock(iu.mutex);
  if (iu.users.size() > 20) {
    log::error("dbp", "[DBP Error][UI] [bug 155288] exceed max of registered users. m_userIdList.size() = {}", iu.users.size());
    return 0;
  }
  for (auto cb : iu.callbacks) {
    int32_t prev = 0;
    for (int32_t uid : iu.users) {
      if (static_cast<uint32_t>(uid) - 0x10000000u >= 0x30000000u || uid <= prev) break;  // [bug 155288] checks
      cb(uid);
      prev = uid;
    }
  }
  iu.users.clear();
  return 0;
}

// 0x352B0: SceDbpNotificationThread.
void notification_thread(ssn::dbp::Thread* t) {
  delete_invalid_user();
  if (notify_initialized(t->event)) report_errors();
  t->status = 0;
}

// 0x35260 StartNotificationThread.
int32_t start_notification_thread() {
  Manager& m = mgr();
  if (m.pre_notify_installed)
    host::note_unimplemented("eboot DbPreparation pre-notification hook (EBF160 -> 0x5840 -> 0xE2D0(0x40000)) not traced");
  m.notification.event = m.preparer.event;  // the preparer's current event (+0x28)
  return m.notification.start(notification_thread, "SceDbpNotificationThread");
}

// 0x57DE10 DbRecovery CheckRequest: the 8-byte header of "/<word>/priv/mms/dbr.db-err" when
// its high 16 bits are 0xCAFE.
uint64_t check_recovery_request() {
  const std::filesystem::path p = std::filesystem::path(host::ctx().cache_dir) / "system_data" / "priv" / "mms" / "dbr.db-err";
  std::ifstream f(p, std::ios::binary);
  if (!f) return 0;
  uint64_t h = 0;
  f.read(reinterpret_cast<char*>(&h), 8);
  if (f.gcount() < 8) {
    log::error("dbp", "[DBR Error] CheckRequest: paf::LocalFile::Read() : size={}", f.gcount());
    return 0;
  }
  if ((h >> 48) != 0xCAFE) {
    log::error("dbp", "[DBR Error] CheckRequest: File Header Error : data=0x{:016x}", h);
    return 0;
  }
  return h;
}

// ---- icalls -------------------------------------------------------------------------------

// CexDbRecoveryStartNative(bool isPostAutoUpdateProcess) — 0x33500 -> preparer +0x20
// (0x3AE40 -> 0x64C8A0, the eboot's DbRecovery client).
int32_t CexDbRecoveryStartNative(uint8_t post_auto_update) {
  (void)post_auto_update;
  host::note_unimplemented("eboot DbRecovery (CexDbRecoveryStart, 0x64C8A0) not reproduced");
  return kErrNotImplemented;
}

// GetDbRecoveryStatusNative(out StatusAndProgressInternal) — 0x33560 -> 0x3AE50 -> 0x64C980:
// with the DbRecovery state 1 (idle) the record is the constant 0xB8B860 {0, 0, 0.0, 1, 0}.
int32_t GetDbRecoveryStatusNative(uint8_t* out) {
  if (!out) return kErrInvalidArgument;
  std::memset(out, 0, 20);
  if (recovery().state == 1) {
    const uint32_t remaining = 1;
    std::memcpy(out + 12, &remaining, 4);
    return 0;
  }
  if (recovery().state == 3 || recovery().state == 4) {
    // 0xB8C220 {1, 0, 100.0f, 0} (no DbRecovery object)
    const int32_t one = 1;
    const float hundred = 100.0f;
    std::memcpy(out, &one, 4);
    std::memcpy(out + 8, &hundred, 4);
    return 0;
  }
  return kErrState;
}

// CexCheckRequestNative(out bool isRequested) — 0x33460.
int32_t CexCheckRequestNative(uint8_t* requested) {
  if (!requested) {
    log::error("dbp", "[DBP Error][UI] CexCheckRequest: invalid argument.");
    return kErrInvalidArgument;
  }
  mgr().preparation.wait();  // 0x34F80 WaitThread(EBF120)
  if (check_recovery_request() != 0) {
    *requested = 1;
    return 0;
  }
  // 0x35250 -> preparer +0x40 (0x3B540 -> 0x64C800: DbRecovery IPC, state 4 when it answers).
  host::note_unimplemented("eboot DbRecovery state query after CheckRequest (0x64C800) not reproduced");
  return start_notification_thread();
}

// WaitNotificationThreadNative() — 0x35300 = WaitThread(EBF138).
int32_t WaitNotificationThreadNative() { return mgr().notification.wait(); }

// GetDbInitializedEventNative(out Event) — 0x35310: the notification thread object's event.
int32_t GetDbInitializedEventNative(Event* out) {
  if (!out) return kErrInvalidArgument;
  *out = mgr().notification.event;
  return 0;
}

// KratosLogInNative(out bool, int userId) — 0x33600.
int32_t KratosLogInNative(uint8_t* requested, int32_t user_id) {
  if (!requested || user_id == -1) {
    log::error("dbp", "[DBP Error][UI] KratosLogIn: invalid argument.");
    return kErrInvalidArgument;
  }
  Event ev{1, user_id, 1, {}, ssn::dbp::kLogin};
  const int32_t r = initialize_db(ev);
  if (r != 0) log::error("dbp", "[DBP Error][UI] KratosLogIn: DbPreparationManager::InitializeDbPreparation() : ret=0x{:08x}", static_cast<uint32_t>(r));
  if (check_recovery_request() != 0) {
    *requested = 1;
    return 0;
  }
  host::note_unimplemented("eboot DbPreparation Kratos user event callback (sceUserServiceRegisterEventCallback 0x33770) not reproduced");
  *requested = 0;
  return start_notification_thread();
}

// KratosLogOutNative(ref Event) — 0x337F0.
int32_t KratosLogOutNative(Event* ev) {
  if (!ev) return kErrInvalidArgument;
  if (ev->user_id != mgr().preparer.prime_user_id) {
    log::info("dbp", "[DBP Message][UI] KratosLogOut: [BUG 150784] unmatch user id. userId = {:x}, m_primeUserId = {:x}", ev->user_id,
              mgr().preparer.prime_user_id);
    return kErrInvalidArgument;
  }
  return terminate_db(*ev);
}

// KratosDbRecoveryStartNative(int isSafeMode, int userId, bool isPrimaryUser) — 0x33A60.
int32_t KratosDbRecoveryStartNative(int32_t safe_mode, int32_t user_id, uint8_t primary) {
  (void)safe_mode;
  (void)user_id;
  (void)primary;
  host::note_unimplemented("eboot DbRecovery Kratos start (0x64C8A0) not reproduced");
  return kErrNotImplemented;
}

// NotifyDbInitializedNative(int userId, bool isPrimaryUser) — 0x34FD0.
int32_t NotifyDbInitializedNative(int32_t user_id, uint8_t primary) {
  const int32_t cok = mgr().preparer.event.cex_or_kratos;  // preparer +0x38 (0x36D60)
  Event ev{cok, user_id, primary, {}, 4 * (cok != 0)};
  if (notify_initialized(ev)) report_errors();
  return 0;
}

// NotifyDbTerminatingNative(int userId, bool isPrimaryUser) — 0x35090.
int32_t NotifyDbTerminatingNative(int32_t user_id, uint8_t primary) {
  const int32_t cok = mgr().preparer.event.cex_or_kratos;
  Event ev{cok, user_id, primary, {}, 4 * (cok != 0) + 1};
  notify_terminating(ev);
  return 0;
}

// DeleteInvalidUserNative() — 0x33D00.
int32_t DeleteInvalidUserNative() { return delete_invalid_user(); }

SSN_ICALL("Sce.Vsh.DbPreparationWrapper::CexDbRecoveryStartNative", CexDbRecoveryStartNative);
SSN_ICALL("Sce.Vsh.DbPreparationWrapper::GetDbRecoveryStatusNative", GetDbRecoveryStatusNative);
SSN_ICALL("Sce.Vsh.DbPreparationWrapper::CexCheckRequestNative", CexCheckRequestNative);
SSN_ICALL("Sce.Vsh.DbPreparationWrapper::WaitNotificationThreadNative", WaitNotificationThreadNative);
SSN_ICALL("Sce.Vsh.DbPreparationWrapper::GetDbInitializedEventNative", GetDbInitializedEventNative);
SSN_ICALL("Sce.Vsh.DbPreparationWrapper::KratosLogInNative", KratosLogInNative);
SSN_ICALL("Sce.Vsh.DbPreparationWrapper::KratosLogOutNative", KratosLogOutNative);
SSN_ICALL("Sce.Vsh.DbPreparationWrapper::KratosDbRecoveryStartNative", KratosDbRecoveryStartNative);
SSN_ICALL("Sce.Vsh.DbPreparationWrapper::NotifyDbInitializedNative", NotifyDbInitializedNative);
SSN_ICALL("Sce.Vsh.DbPreparationWrapper::NotifyDbTerminatingNative", NotifyDbTerminatingNative);
SSN_ICALL("Sce.Vsh.DbPreparationWrapper::DeleteInvalidUserNative", DeleteInvalidUserNative);

}  // namespace

void register_unit_impl(UnitSlot slot, UnitImpl* impl) { handle(slot).impl = impl; }

void register_listener(Listener* l, int32_t which) {
  Notifier& n = notifier();
  std::lock_guard lock(n.mutex);
  for (auto& list : n.lists)
    if (std::find(list.begin(), list.end(), l) != list.end()) return;
  if (which == 0 || which == 1) n.lists[which].push_back(l);
}

void unregister_listener(Listener* l) {
  Notifier& n = notifier();
  std::lock_guard lock(n.mutex);
  for (auto& list : n.lists) {
    auto it = std::find(list.begin(), list.end(), l);
    if (it != list.end()) {
      list.erase(it);
      return;
    }
  }
}

int32_t register_invalid_user_callback(void (*cb)(int32_t)) {
  // 0x33B00: appends, dropping adjacent duplicates.
  if (!cb) return kErrInvalidArgument;
  InvalidUsers& iu = invalid_users();
  std::lock_guard lock(iu.mutex);
  iu.callbacks.push_back(cb);
  iu.callbacks.unique();
  return 0;
}

int32_t initialize_system_step() {
  // 0x56C0: the pre-notification std::function (EBF140), then on CEX 0x333D0 -> 0x34F20.
  Manager& m = mgr();
  m.pre_notify_installed = true;
  if (kKratos) return 0;
  m.preparation.event = Event{0, -1, 0, {}, ssn::dbp::kInitialize};
  return m.preparation.start([](ssn::dbp::Thread* t) { t->status = initialize_db(t->event); }, "SceDbpPreparationThread");
}

int32_t initialize_framework_dependent_step() {
  // 0x5850: (icall table registration 0x3B600 is the host boundary's) then on CEX
  // sceUserServiceRegisterEventCallback(0x33570): event 256 -> InitializeDb(CEX, uid,
  // CREATE_USER), 257 -> TerminateDb(CEX, uid, DESTROY_USER).
  if (kKratos) return 0;
  host::note_unimplemented("eboot DbPreparation user create/delete event callback (0x33570) not connected: the host user service has no event callbacks");
  return 0;
}

}  // namespace ssn::eboot::dbp
