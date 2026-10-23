// SceShellCore's SceUserService (FW 4.03) and the libSceUserService client calls ShellUI
// makes through Sce.Vsh.UserServiceWrapper. See user_service.h for the model.
//
// Managed entry points: Sce.Vsh.UserServiceWrapper internal calls registered by the ShellUI
// eboot ({name, fn} table 0xced420..); several are registered straight onto
// libSceUserService imports, whose client either answers from its cache or asks the server.
#include "shellcore/user_service.h"

#include <condition_variable>
#include <cstdio>
#include <cstring>
#include <deque>
#include <filesystem>
#include <format>
#include <mutex>
#include <random>
#include <string>
#include <thread>
#include <vector>

#include <mono/metadata/appdomain.h>
#include <mono/metadata/object.h>

#include "common/mono_array.h"
#include "host/boundary.h"
#include "host/context.h"
#include "libSceRegMgr/regmgr.h"

namespace ssn::shellcore::users {
namespace {

namespace fs = std::filesystem;

struct ParamDef {
  int32_t param;
  uint32_t key;  // USER_01 key; slot s uses key + s * 0x10000 (0x70A0D0: (slot << 16) + key)
  uint8_t type;  // 0 int, 1 str, 2 bin (sceRegMgrGet{Int,Str,Bin}InitVal in 0x70EDB0)
  uint16_t size;
};

constexpr ParamDef kParams[] = {
#include "shellcore/user_params.inc"
};

const ParamDef* find_param(int32_t param) {
  for (const ParamDef& p : kParams)
    if (p.param == param) return &p;
  return nullptr;
}

uint32_t slot_key(uint32_t key, int slot) { return key + (static_cast<uint32_t>(slot) << 16); }

// SCE_REGMGR_ENT_KEY_USER_max_used_home_u — the last persistent user id handed out
// (SceUserService +216, saved by 0x7125E0; INI -1).
constexpr uint32_t kKeyMaxUsedHomeUserId = 0x07020000;

struct State {
  std::recursive_mutex mu;           // SceUserService +0 lock (sub_B0B680 / sub_B0B6B0)
  std::vector<int32_t> login;        // +56 vector of logged-in users
  int32_t foreground = kUserIdInvalid;  // +88, -1 from the constructor 0x6E5790
  bool order_loaded = false;
  int32_t login_order = 0;           // +256 login-order counter
};

State& st() {
  static State s;
  return s;
}

int32_t slot_user_id(int slot) {
  int32_t id = kUserIdInvalid;
  if (regmgr::get_int(slot_key(0x07800100, slot), &id) != 0) return kUserIdInvalid;
  return id;
}

// 0x6E66D0(a1, userId, 0): the persistent slot holding `user_id`, or 0x80960105.
int find_slot(int32_t user_id) {
  if (user_id == kUserIdInvalid) return -1;
  for (int s = 0; s < kSlots; ++s)
    if (slot_user_id(s) == user_id) return s;
  return -1;
}

// HOST VALUE: the console directories ShellCore keeps per user live under
// <cache>/users/<console path>: /system_data/priv/home/%08x (0x6E6940), /user/home/%08x
// (0x6E68F0).
fs::path host_dir(const char* console_dir) {
  fs::path p = fs::path(host::ctx().cache_dir) / "users";
  p /= fs::path(console_dir).relative_path();
  return p;
}

std::string hex8(int32_t id) { return std::format("{:08x}", static_cast<uint32_t>(id)); }

// 0x6F7DD0: the highest persistent user id (0x1xxxxxxx) among the directory names of
// /system_data/priv/home, 0x0FFFFFFF when there is none.
int32_t max_home_dir_id() {
  int32_t best = 0x0FFFFFFF;
  std::error_code ec;
  for (const auto& d : fs::directory_iterator(host_dir("/system_data/priv/home"), ec)) {
    if (!d.is_directory(ec)) continue;
    const std::string n = d.path().filename().string();
    char* end = nullptr;
    const unsigned long long v = std::strtoull(n.c_str(), &end, 16);
    if ((v & 0xFFFFFFFFF0000000ull) == 0x10000000ull && static_cast<int64_t>(v) > best) best = static_cast<int32_t>(v);
  }
  return best;
}

// 0x6FB4F0 for a persistent user (kind 0) on a retail unit (the kiosk test nQ5AHlyJghQ —
// IDPS product code 0xA802 — is false, so the fixed id 0x10000000 is not used): the next id
// after the counter, unless that falls outside [0x10010000, 0x1FFFFFFF); then a random id
// v10 + rand % (v9 - v10) in [0x10010000, 0x1EFFFFFF) that no slot uses.
int32_t allocate_user_id(int32_t counter) {
  const int32_t lo = 0x10010000, hi_rand = 0x1EFFFFFF;
  const int32_t next = counter + 1;
  if (!(next < lo || counter >= 0x1FFFFFFF)) return next;
  // HOST VALUE: the random source (w1o05aHJT4c on the console) is the host's.
  static std::mt19937 rng{std::random_device{}()};
  for (;;) {
    const int32_t id = lo + static_cast<int32_t>(rng() % static_cast<uint32_t>(hi_rand - lo));
    if (find_slot(id) < 0) return id;
  }
}

// 0x6F5E30 (directory part): /user/home/%08x (mode 0755) and /system_data/priv/home/%08x.
void create_home_directories(int32_t id) {
  std::error_code ec;
  fs::create_directories(host_dir("/user/home") / hex8(id), ec);
  fs::create_directories(host_dir("/system_data/priv/home") / hex8(id), ec);
  host::note_unimplemented(
      "SceShellCore CreateHomeDirectory (0x6F5E30): only the two home directories are created (no symlinks, "
      "eap rodata, config.dat 0x6F7410)");
}

void load_login_order(State& s) {
  if (s.order_loaded) return;
  s.order_loaded = true;
  // HOST VALUE: the boot value of the login-order counter (+256) is not traced; the host
  // starts it at the highest last_login_order of the registered users.
  host::note_unimplemented("HOST VALUE SceUserService login-order counter (+256) boot value: max last_login_order of the users");
  const ParamDef* p = find_param(kParamLastLoginOrder);
  for (int sl = 0; sl < kSlots; ++sl) {
    if (slot_user_id(sl) == kUserIdInvalid) continue;
    int32_t v = 0;
    if (regmgr::get_int(slot_key(p->key, sl), &v) == 0 && v > s.login_order) s.login_order = v;
  }
}

}  // namespace

int32_t create_user(const char* name, int32_t kind, bool incomplete, int32_t* user_id) {
  if (!name || !user_id) return kErrInvalidArgument;
  if (kind != 0) {
    host::note_unimplemented(std::format("SceShellCore CreateUser kind {} (guest / share-play entries) not modelled", kind));
    return kErrInvalidArgument;
  }
  State& s = st();
  std::lock_guard l(s.mu);
  // The first slot whose user id is -1 (entries +392, 16 of them).
  int slot = -1;
  for (int i = 0; i < kSlots && slot < 0; ++i)
    if (slot_user_id(i) == kUserIdInvalid) slot = i;
  if (slot < 0) return kErrNoFreeUserId;

  // The counter (+216) is raised to the highest home directory id first (0x6F7DD0).
  int32_t counter = -1;
  regmgr::get_int(kKeyMaxUsedHomeUserId, &counter);
  const int32_t dir_max = max_home_dir_id();
  if (counter < dir_max) counter = dir_max;
  const int32_t id = allocate_user_id(counter);

  create_home_directories(id);

  // 0x70ED80 loads every parameter's InitVal into the cleared entry, then the entry gets the
  // user id (+0), the name (+4, 17 bytes) and, for an incomplete user, user_status (+92) = 1;
  // 0x70A0D0(slot, -1) writes all of it to USER_<slot+1>.
  std::vector<uint32_t> keys;
  keys.reserve(std::size(kParams));
  for (const ParamDef& p : kParams) keys.push_back(slot_key(p.key, slot));
  regmgr::reset_vals(keys.data(), keys.size());
  char nm[17] = {};
  std::strncpy(nm, name, 16);
  regmgr::set_str(slot_key(0x07800200, slot), nm, sizeof nm);
  if (incomplete) regmgr::set_int(slot_key(0x07800F00, slot), 1);
  regmgr::set_int(slot_key(0x07800100, slot), id);

  // The rest of 0x6FB800 for a retail unit: region-dependent parameters (0x6FB5F0), the
  // login-order parameter, created_version, config.dat, events to the clients.
  host::note_unimplemented(
      "SceShellCore CreateUser: region parameters (0x6FB5F0), last_login_order / created_version updates, "
      "config.dat (0x6F7920) and the vshctl notification not reproduced");

  // +216 = the new id, saved by 0x7125E0.
  regmgr::set_int(kKeyMaxUsedHomeUserId, id);
  *user_id = id;
  // 0x6FBDBB: the user-created event 0x6FC0F0(this, 256, id) to the clients, right before the
  // "[SceUserService] User \"%s\" (0x%x) created" line.
  post_event(256, id);
  std::fprintf(stderr, "[SceUserService] User \"%s\" (0x%x) created\n", nm, static_cast<uint32_t>(id));
  return 0;
}

int32_t destroy_user(int32_t user_id) {
  State& s = st();
  std::lock_guard l(s.mu);
  const int slot = find_slot(user_id);
  if (slot < 0) return kErrUserNotFound;
  host::note_unimplemented("SceShellCore DestroyUser (0x6FC360): slot reset to defaults only (home directories, events kept)");
  std::vector<uint32_t> keys;
  for (const ParamDef& p : kParams) keys.push_back(slot_key(p.key, slot));
  regmgr::reset_vals(keys.data(), keys.size());
  std::erase(s.login, user_id);
  return 0;
}

// Two parameters have no USER_nn key: 0x70EDB0 answers them from the system's parental
// settings unless the entry has an NP account id (+256, parameter 256) or its byte +392 set
// (+392 not traced; the host takes it as 0):
//   2193 (game age limit): account -> 21; else SECURITY_PARENTAL_game_age_limit
//     (0x03800600), 21 when the read fails.
//   2245 (its region, 3-byte string): account -> ""; else
//     SECURITY_PARENTAL_game_age_limit_region (0x03800700), "" when the read fails.
namespace {

constexpr uint32_t kKeyGameAgeLimit = 0x03800600;
constexpr uint32_t kKeyGameAgeLimitRegion = 0x03800700;

bool has_np_account(int32_t user_id) {
  uint64_t account = 0;
  return get_bin(user_id, kParamAccountId, &account, sizeof account) == 0 && account != 0;
}

}  // namespace

int32_t get_int(int32_t user_id, int32_t param, int32_t* value) {
  if (!value) return kErrInvalidArgument;
  if (user_id == kUserIdInvalid) return kErrUserNotFound;
  if (param == kParamGameAgeLimit) {
    std::lock_guard l(st().mu);
    if (find_slot(user_id) < 0) return kErrUserNotFound;
    *value = 21;
    int32_t v = 0;
    if (!has_np_account(user_id) && regmgr::get_int(kKeyGameAgeLimit, &v) == 0) *value = v;
    return 0;
  }
  const ParamDef* p = find_param(param);
  if (!p || p->type != 0) {
    host::note_unimplemented(std::format("SceUserService parameter {} (int) not in 0x70EDB0's table", param));
    return kErrInvalidArgument;
  }
  std::lock_guard l(st().mu);
  const int slot = find_slot(user_id);
  if (slot < 0) return kErrUserNotFound;
  return regmgr::get_int(slot_key(p->key, slot), value);
}

int32_t set_int(int32_t user_id, int32_t param, int32_t value) {
  if (user_id == kUserIdInvalid) return kErrUserNotFound;
  const ParamDef* p = find_param(param);
  if (!p || p->type != 0) {
    host::note_unimplemented(std::format("SceUserService parameter {} (int) not in 0x70EDB0's table", param));
    return kErrInvalidArgument;
  }
  std::lock_guard l(st().mu);
  const int slot = find_slot(user_id);
  if (slot < 0) return kErrUserNotFound;
  return regmgr::set_int(slot_key(p->key, slot), value);
}

int32_t get_str(int32_t user_id, int32_t param, char* buf, size_t size) {
  if (!buf) return kErrInvalidArgument;
  if (user_id == kUserIdInvalid) return kErrUserNotFound;
  std::vector<char> tmp;
  if (param == kParamGameAgeLimitRegion) {
    std::lock_guard l(st().mu);
    if (find_slot(user_id) < 0) return kErrUserNotFound;
    tmp.assign(4, 0);
    if (has_np_account(user_id) || regmgr::get_str(kKeyGameAgeLimitRegion, tmp.data(), 3) != 0) tmp.assign(4, 0);
  } else {
    const ParamDef* p = find_param(param);
    if (!p || p->type != 1) return kErrInvalidArgument;
    std::lock_guard l(st().mu);
    const int slot = find_slot(user_id);
    if (slot < 0) return kErrUserNotFound;
    tmp.assign(p->size + 1, 0);
    const int32_t r = regmgr::get_str(slot_key(p->key, slot), tmp.data(), p->size);
    if (r != 0) return r;
  }
  const size_t n = std::min(size - 1, std::strlen(tmp.data()));
  std::memcpy(buf, tmp.data(), n);
  buf[n] = 0;
  return 0;
}

int32_t get_bin(int32_t user_id, int32_t param, void* buf, size_t size) {
  if (!buf) return kErrInvalidArgument;
  if (user_id == kUserIdInvalid) return kErrUserNotFound;
  const ParamDef* p = find_param(param);
  if (!p || p->type != 2 || p->size != size) return kErrInvalidArgument;
  std::lock_guard l(st().mu);
  const int slot = find_slot(user_id);
  if (slot < 0) return kErrUserNotFound;
  return regmgr::get_bin(slot_key(p->key, slot), buf, size);
}

int32_t set_int_by_mask(int32_t user_id, int32_t param, uint32_t mask, uint32_t value, uint32_t* out) {
  std::lock_guard l(st().mu);
  *out = 0;
  int32_t old = 0;
  if (const int32_t r = get_int(user_id, param, &old)) return r;
  const uint32_t v = value | (static_cast<uint32_t>(old) & ~mask);
  if (const int32_t r = set_int(user_id, param, static_cast<int32_t>(v))) return r;
  *out = v;
  return 0;
}

void registered_user_ids(int32_t out[kListSize]) {
  std::lock_guard l(st().mu);
  for (int i = 0; i < kListSize; ++i) out[i] = kUserIdInvalid;
  int n = 0;
  for (int s = 0; s < kSlots; ++s) {
    const int32_t id = slot_user_id(s);
    if ((static_cast<uint32_t>(id) & 0xF0000000u) == 0x10000000u) out[n++] = id;
  }
}

void registered_home_user_ids(int32_t out[kSlots], int32_t* count) {
  std::lock_guard l(st().mu);
  *count = 0;
  for (int s = 0; s < kSlots; ++s) {
    out[s] = kUserIdInvalid;
    const int32_t id = slot_user_id(s);
    if ((static_cast<uint32_t>(id) & 0xF0000000u) == 0x10000000u) {
      out[s] = id;
      ++*count;
    }
  }
}

int32_t login(int32_t user_id) {
  State& s = st();
  std::lock_guard l(s.mu);
  if (user_id < 0) return kErrInvalidArgument;
  const int slot = find_slot(user_id);
  if (slot < 0) return kErrUserNotFound;
  if (std::find(s.login.begin(), s.login.end(), user_id) != s.login.end()) return 0;
  if (s.login.size() > 3) return kErrMaxLoginUsers;
  s.login.push_back(user_id);
  char nm[17] = {};
  regmgr::get_str(slot_key(0x07800200, slot), nm, sizeof nm);
  std::fprintf(stderr, "[SceUserService] User \"%s\" (0x%x) logged in\n", nm, static_cast<uint32_t>(user_id));
  // last_login_order (param 5): when it is 0 or not the counter, the counter advances and
  // the user takes the new value.
  load_login_order(s);
  const uint32_t order_key = slot_key(find_param(kParamLastLoginOrder)->key, slot);
  int32_t order = 0;
  regmgr::get_int(order_key, &order);
  if (order != s.login_order || order == 0) {
    ++s.login_order;
    regmgr::set_int(order_key, s.login_order);
  }
  host::note_unimplemented(
      "SceShellCore Login (0x6FF420): user number, sceMbusUserLogin, audio-in and region parameters not reproduced");
  // The login event (0x6FC0F0, type 0) to the user-service clients.
  post_event(0, user_id);
  return 0;
}

// ---- client events -------------------------------------------------------------------
// SceShellCore queues each user event for every SceUserService client (0x6FC0F0); the
// client library (libSceUserService) reads them on its "SceUserServiceGetEvent_%08x" thread
// (sub_21E0 -> sceUserServiceGetEventOne) and calls every callback registered with
// sceUserServiceRegisterEventCallback (wuI7c7UNk0A, 0x32E0) with the event
// {eventType, userId}.
namespace {
struct ClientEvents {
  std::mutex mu;
  std::condition_variable cv;
  std::deque<std::pair<int32_t, int32_t>> queue;
  std::vector<std::pair<EventCallback, void*>> callbacks;
  bool thread = false;
};
ClientEvents& client_events() {
  static ClientEvents c;
  return c;
}
void client_event_thread() {
  ClientEvents& c = client_events();
  for (;;) {
    std::pair<int32_t, int32_t> ev;
    std::vector<std::pair<EventCallback, void*>> cbs;
    {
      std::unique_lock l(c.mu);
      c.cv.wait(l, [&] { return !c.queue.empty(); });
      ev = c.queue.front();
      c.queue.pop_front();
      cbs = c.callbacks;
    }
    const int32_t rec[2] = {ev.first, ev.second};
    for (auto& [fn, arg] : cbs) fn(rec, arg);
  }
}
}  // namespace

void post_event(int32_t type, int32_t user_id) {
  ClientEvents& c = client_events();
  {
    std::lock_guard l(c.mu);
    if (c.callbacks.empty()) return;  // no client registered: nothing is delivered
    c.queue.emplace_back(type, user_id);
  }
  c.cv.notify_all();
}

int32_t register_event_callback(EventCallback fn, void* arg) {
  if (!fn) return kErrInvalidArgument;  // "sceUserServiceRegisterEventCallback: NULL"
  ClientEvents& c = client_events();
  std::lock_guard l(c.mu);
  c.callbacks.emplace_back(fn, arg);
  if (!c.thread) {
    c.thread = true;
    std::thread(client_event_thread).detach();
  }
  return 0;
}

bool is_logged_in(int32_t user_id) {
  State& s = st();
  std::lock_guard l(s.mu);
  return std::find(s.login.begin(), s.login.end(), user_id) != s.login.end();
}

void login_user_ids(int32_t out[kMaxLogin]) {
  State& s = st();
  std::lock_guard l(s.mu);
  for (int i = 0; i < kMaxLogin; ++i) out[i] = i < static_cast<int>(s.login.size()) ? s.login[i] : kUserIdInvalid;
}

int32_t foreground_user() {
  State& s = st();
  std::lock_guard l(s.mu);
  return s.foreground;
}

void set_foreground_user(int32_t user_id) {
  State& s = st();
  std::lock_guard l(s.mu);
  s.foreground = user_id;
}

namespace {

// ---------------------------------------------------------------------------------------
// Sce.Vsh.UserServiceWrapper internal calls

// _GetLoginUserIdList(int[]) — eboot 0x4a6bf0: array shorter than 4 -> -1; otherwise
// sceUserServiceGetLoginUserIdList(data) (fPhymKNvK-A, libSceUserService 0x2a50): the
// client copies the first four entries of its cached login vector (filled from the server's
// login events), every missing slot SCE_USER_SERVICE_USER_ID_INVALID, result 0.
int32_t GetLoginUserIdList(MonoArray* list) {
  const psm::ArrayView v = psm::view(list);
  if (v.length < 4) return -1;
  login_user_ids(reinterpret_cast<int32_t*>(v.data));
  return 0;
}
SSN_ICALL("Sce.Vsh.UserServiceWrapper::_GetLoginUserIdList", GetLoginUserIdList);

// _GetForegroundUser(int&) — registered straight onto sceUserServiceGetForegroundUser
// (eNb53LQJmIM, libSceUserService 0x2f30 -> 0x3f70): null -> 0x80960005; a cached foreground
// user other than -1 is returned directly, otherwise IPMI 0x3000C asks ShellCore, which
// returns +88 under its lock with result 0 (0x7001c0).
int32_t GetForegroundUser(int32_t* user_id) {
  if (!user_id) return kErrInvalidArgument;
  *user_id = foreground_user();
  return 0;
}
SSN_ICALL("Sce.Vsh.UserServiceWrapper::_GetForegroundUser", GetForegroundUser);

// Sce.Vsh.ProfileCache.CacheReader::_GetLocalUserInfoNative(int, out string name,
// ref bool isSignedUp, ref bool isGuest, ref bool isSharePlayVisitor) — eboot 0x4f4110:
// isSignedUp = account id (parameter 256) read and non-zero; name = sceUserServiceGetUserName
// (17 bytes); only when that fails or is empty, NiTGNLkBc-Q fills name and account id;
// isGuest = (id & 0xE0000000) == 0x20000000 (FnWkLNOmJXw, client 0x32a0); isSharePlayVisitor
// = (id & 0xF0000000) == 0x30000000 (7XgCm-Ewkrs, client 0x32c0). Returns that fallback's
// result, else 0.
int32_t GetLocalUserInfoNative(int32_t user_id, MonoString** name, uint8_t* signed_up, uint8_t* guest,
                               uint8_t* share_play) {
  uint64_t account = 0;
  const int32_t ra = get_bin(user_id, kParamAccountId, &account, sizeof account);
  *signed_up = ra == 0 && account != 0;
  char buf[17] = {};
  int32_t r = 0;
  if (get_str(user_id, kParamUserName, buf, sizeof buf) != 0 || buf[0] == 0) {
    host::note_unimplemented("libSceUserService NiTGNLkBc-Q (name + account id fallback of 0x4f4110) not traced");
    r = kErrUserNotFound;
    *signed_up = false;
  }
  *guest = (static_cast<uint32_t>(user_id) & 0xE0000000u) == 0x20000000u;
  *share_play = (static_cast<uint32_t>(user_id) & 0xF0000000u) == 0x30000000u;
  *name = mono_string_new(mono_domain_get(), buf);
  return r;
}
SSN_ICALL("Sce.Vsh.ProfileCache.CacheReader::_GetLocalUserInfoNative", GetLocalUserInfoNative);

// _IsLoggedIn(int) — registered straight onto sceUserServiceIsLoggedIn (MZxH8029+Wg,
// libSceUserService 0x36d0): 1 when the id is in the client's cached login vector, else 0.
int32_t IsLoggedIn(int32_t user_id) { return is_logged_in(user_id) ? 1 : 0; }
SSN_ICALL("Sce.Vsh.UserServiceWrapper::_IsLoggedIn", IsLoggedIn);

// _SetForegroundUser(int) — sceUserServiceSetForegroundUser (MgBIXUkGtpE): ShellCore stores
// the id the getter above returns.
int32_t SetForegroundUser(int32_t user_id) {
  host::note_unimplemented("SceShellCore SetForegroundUser handler not traced: stores the id read back by 0x7001c0");
  set_foreground_user(user_id);
  return 0;
}
SSN_ICALL("Sce.Vsh.UserServiceWrapper::_SetForegroundUser", SetForegroundUser);

// _GetRegisteredUserIdList(int[]) — eboot 0x4a6c30: array shorter than 20 -> -1; otherwise
// sceUserServiceGetRegisteredUserIdList (5EiQCnL2G1Y, libSceUserService 0x2b30 -> 0x3fd0 ->
// 0x8b0: IPMI 0x3000F, 80-byte output) -> ShellCore 0x6E69E0: 20 ids preset to -1, then the
// persistent users (id & 0xF0000000 == 0x10000000) in slot order, then the guests.
int32_t GetRegisteredUserIdList(MonoArray* list) {
  const psm::ArrayView v = psm::view(list);
  if (v.length < kListSize) return -1;
  registered_user_ids(reinterpret_cast<int32_t*>(v.data));
  return 0;
}
SSN_ICALL("Sce.Vsh.UserServiceWrapper::_GetRegisteredUserIdList", GetRegisteredUserIdList);

// _GetRegisteredHomeUserIdList(int[], ref int) — eboot 0x4a6c70: array shorter than 16 ->
// -1; otherwise sceUserServiceGetRegisteredHomeUserIdList (OVdVBcejvmQ) -> ShellCore
// 0x6E6DB0: 16 ids preset to -1, each persistent user at its slot index, count = users.
int32_t GetRegisteredHomeUserIdList(MonoArray* list, int32_t* count) {
  const psm::ArrayView v = psm::view(list);
  if (v.length < kSlots) return -1;
  if (!count) return kErrInvalidArgument;
  registered_home_user_ids(reinterpret_cast<int32_t*>(v.data), count);
  return 0;
}
SSN_ICALL("Sce.Vsh.UserServiceWrapper::_GetRegisteredHomeUserIdList", GetRegisteredHomeUserIdList);

// _IsValidRealUserIdRange(int) — eboot 0x4a97f0.
bool IsValidRealUserIdRange(int32_t id) { return static_cast<uint32_t>(id - 0x10000000) < 0x30000000u; }
SSN_ICALL("Sce.Vsh.UserServiceWrapper::_IsValidRealUserIdRange", IsValidRealUserIdRange);

// _IsValidAvailableUserIdRange(int) — eboot 0x4a9800.
bool IsValidAvailableUserIdRange(int32_t id) {
  const uint32_t a = static_cast<uint32_t>(id);
  return a != 0 && (a < 0x100 || a - 0x10000000u < 0x30000000u);
}
SSN_ICALL("Sce.Vsh.UserServiceWrapper::_IsValidAvailableUserIdRange", IsValidAvailableUserIdRange);

// _GetUserName(int, out string) — eboot 0x4a6cb0: sceUserServiceGetUserName(id, buf, 17)
// (1xxcMiGu2fo: string parameter 1, at most 1024 bytes); on error the name is "".
int32_t GetUserName(int32_t user_id, MonoString** name) {
  char buf[17] = {};
  const int32_t r = get_str(user_id, kParamUserName, buf, sizeof buf);
  if (r != 0) buf[0] = 0;
  if (name) *name = mono_string_new(mono_domain_get(), buf);
  return r;
}
SSN_ICALL("Sce.Vsh.UserServiceWrapper::_GetUserName", GetUserName);

// Integer parameters registered straight onto their libSceUserService getters/setters
// (client 0x4cb0 / 0x4d10 / 0x4d30 / 0x4d90: parameter 6 / 2091 through the client's
// int get/set; null out -> 0x80960005).
int32_t GetLoginFlag(int32_t user_id, uint32_t* v) {
  if (!v) return kErrInvalidArgument;
  int32_t x = 0;
  const int32_t r = get_int(user_id, kParamLoginFlag, &x);
  *v = static_cast<uint32_t>(x);
  return r;
}
SSN_ICALL("Sce.Vsh.UserServiceWrapper::_GetLoginFlag", GetLoginFlag);

int32_t SetLoginFlag(int32_t user_id, uint32_t v) { return set_int(user_id, kParamLoginFlag, static_cast<int32_t>(v)); }
SSN_ICALL("Sce.Vsh.UserServiceWrapper::_SetLoginFlag", SetLoginFlag);

int32_t GetUserStatus(int32_t user_id, uint32_t* v) {
  if (!v) return kErrInvalidArgument;
  int32_t x = 0;
  const int32_t r = get_int(user_id, kParamUserStatus, &x);
  *v = static_cast<uint32_t>(x);
  return r;
}
SSN_ICALL("Sce.Vsh.UserServiceWrapper::_GetUserStatus", GetUserStatus);

int32_t SetUserStatus(int32_t user_id, uint32_t v) { return set_int(user_id, kParamUserStatus, static_cast<int32_t>(v)); }
SSN_ICALL("Sce.Vsh.UserServiceWrapper::_SetUserStatus", SetUserStatus);

// _SetPlatformPrivacyWs1(int, int) — ZF6EzkpOI08 (client 0x9f20): parameter 2204.
int32_t SetPlatformPrivacyWs1(int32_t user_id, int32_t v) { return set_int(user_id, kParamPlatformPrivacyWs1, v); }
SSN_ICALL("Sce.Vsh.UserServiceWrapper::_SetPlatformPrivacyWs1", SetPlatformPrivacyWs1);

// _GetPlatformPrivacyWs1Internal(int, out int) — Iem2arLgE-I (client 0x9e90): the raw
// parameter 2204.
int32_t GetPlatformPrivacyWs1Internal(int32_t user_id, int32_t* v) {
  if (!v) return kErrInvalidArgument;
  return get_int(user_id, kParamPlatformPrivacyWs1, v);
}
SSN_ICALL("Sce.Vsh.UserServiceWrapper::_GetPlatformPrivacyWs1Internal", GetPlatformPrivacyWs1Internal);

// _GetLastLoginOrder(int, out uint) — parameter 5.
int32_t GetLastLoginOrder(int32_t user_id, uint32_t* v) {
  if (!v) return kErrInvalidArgument;
  int32_t x = 0;
  const int32_t r = get_int(user_id, kParamLastLoginOrder, &x);
  *v = static_cast<uint32_t>(x);
  return r;
}
SSN_ICALL("Sce.Vsh.UserServiceWrapper::_GetLastLoginOrder", GetLastLoginOrder);

}  // namespace
}  // namespace ssn::shellcore::users
