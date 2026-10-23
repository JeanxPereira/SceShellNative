// SceShellCore's SceUserService server (FW 4.03) — the user table the host stands in for.
//
// On the console ShellCore keeps 16 persistent user entries of 0x4000 bytes (SceUserService
// +392 + slot * 0x4000) plus guest entries, mirrors every per-user parameter of a persistent
// entry in the registry slot USER_<slot+1> (0x70A0D0 writes them, 0x70EDB0 reads the InitVal
// defaults), creates the user's home directories, and keeps the logged-in users in a vector
// (+56). Clients (libSceUserService in ShellUI) reach it over IPMI; the LoginMgr server in
// the same process (login_mgr_server_ipc_server.cpp) calls it directly.
//
// The host keeps no copy of the entries: every parameter is read from and written to its
// registry key through psmhost/libSceRegMgr, so the users persist in the launcher cache's
// registry store exactly where the console persists them. Only persistent users are
// modelled; guest / share-play users (entries +262536..) are not.
#pragma once
#include <cstddef>
#include <cstdint>

namespace ssn::shellcore::users {

constexpr int32_t kUserIdInvalid = -1;                                  // SCE_USER_SERVICE_USER_ID_INVALID
constexpr int32_t kErrNotInitialized = static_cast<int32_t>(0x80960002u);
constexpr int32_t kErrInvalidArgument = static_cast<int32_t>(0x80960005u);
constexpr int32_t kErrMaxLoginUsers = static_cast<int32_t>(0x80960101u);
constexpr int32_t kErrNoFreeUserId = static_cast<int32_t>(0x80960102u);
constexpr int32_t kErrUserNotFound = static_cast<int32_t>(0x80960105u);

constexpr int kSlots = 16;      // persistent entries
constexpr int kMaxLogin = 4;    // login vector limit (0x6FF420: > 3 -> 0x80960101)
constexpr int kListSize = 20;   // registered list: 16 persistent + 4 guest (0x6E69E0, 80 bytes)

// Per-user parameter ids used by the shell (SceUserService parameter numbers).
enum Param : int32_t {
  kParamUserId = 0,
  kParamUserName = 1,
  kParamLastLoginOrder = 5,
  kParamLoginFlag = 6,
  kParamAccountId = 256,
  kParamUserStatus = 2091,
  kParamGameAgeLimit = 2193,
  kParamPlatformPrivacyWs1 = 2204,
  kParamPapcWhitelist = 2215,
  kParamGameAgeLimitRegion = 2245,
};

// CreateUser (0x6FB800) for a persistent user (kind 0): the first free slot, a new user id
// (0x6FB4F0), the slot's parameters set to their defaults, name and user id stored,
// user_status = 1 for an incomplete user. Guest kinds (1, 2) return kErrInvalidArgument.
int32_t create_user(const char* name, int32_t kind, bool incomplete, int32_t* user_id);

// DestroyUser (0x6FC360): the user's slot back to its defaults (user id -1).
int32_t destroy_user(int32_t user_id);

// Parameter access by user id (0x6E9DC0 get / 0x6EA4D0 set). Integers: 4 bytes.
int32_t get_int(int32_t user_id, int32_t param, int32_t* value);
int32_t set_int(int32_t user_id, int32_t param, int32_t value);
int32_t get_str(int32_t user_id, int32_t param, char* buf, size_t size);
int32_t get_bin(int32_t user_id, int32_t param, void* buf, size_t size);

// The LoginMgr server's get-modify-set of an integer parameter (0x493C60 login flag,
// 0x493D00 user status): *out = 0; new = value | (old & ~mask); stored; *out = new.
int32_t set_int_by_mask(int32_t user_id, int32_t param, uint32_t mask, uint32_t value, uint32_t* out);

// sceUserServiceGetRegisteredUserIdList (0x6E69E0): kListSize ids, -1 padded.
void registered_user_ids(int32_t out[kListSize]);
// sceUserServiceGetRegisteredHomeUserIdList (0x6E6DB0): ids at their slot index, count.
void registered_home_user_ids(int32_t out[kSlots], int32_t* count);

// Login (0x6FF420) / the login vector.
int32_t login(int32_t user_id);
bool is_logged_in(int32_t user_id);
void login_user_ids(int32_t out[kMaxLogin]);

// Foreground user (SceUserService +88; 0x7001c0 reads it).
int32_t foreground_user();
void set_foreground_user(int32_t user_id);

// Client events (user_service.cpp): SceShellCore's event to the clients (0x6FC0F0: type 0
// login, 1 logout, 256 create, 257 destroy) and libSceUserService's
// sceUserServiceRegisterEventCallback (wuI7c7UNk0A, 0x32E0). Callbacks receive the event
// record {eventType, userId} on the client's event thread.
using EventCallback = void (*)(const int32_t* event, void* arg);
int32_t register_event_callback(EventCallback fn, void* arg);
void post_event(int32_t type, int32_t user_id);

}  // namespace ssn::shellcore::users
