// The LoginMgr server (FW 4.03) behind ReactNative.Modules.Vsh's LoginMgrModule.
//
// LoginMgrModule's P/Invokes (libReactNative.Modules.Vsh.sprx exports LoginMgrInitialize
// 0x15e0, LoginMgrTerminate 0x1610, LoginMgrCreateUser 0x1640, LoginMgrDestroyUser 0x1650,
// LoginMgrSetLoginFlag 0x1660, LoginMgrSetUserStatus 0x1670, LoginMgrSendTelemetryOfCreateUser
// 0x1680) are thunks into libSceLoginMgrServer's client facade
// (login_mgr_server_ipc_facade.cpp, one connection object at 0xC018 with a use count at +16),
// which sends IPMI requests to the "SceLoginMgrServer" server inside SceShellCore
// (login_mgr_server_ipc_server.cpp; sync-method switch 0x61E80). The handlers there call
// SceShellCore's SceUserService, modelled in shellcore/user_service.
#include <cstdint>
#include <format>
#include <mutex>

#include "host/boundary.h"
#include "shellcore/user_service.h"

namespace ssn::shellcore {
namespace {

// SCE_LOGIN_MGR_SERVER_ERROR_* as the facade returns them.
constexpr int32_t kErrNotConnected = static_cast<int32_t>(0x81360001u);  // use count 0
constexpr int32_t kErrInvalidArg = static_cast<int32_t>(0x81360002u);
constexpr int32_t kErrInternal = static_cast<int32_t>(0x81360005u);      // any non-0x13E facility error
constexpr int32_t kErrInvalidUserId = static_cast<int32_t>(0x81360006u);

struct Facade {
  std::mutex mu;
  int32_t uses = 0;  // 0xC018 +16
};
Facade& facade() {
  static Facade f;
  return f;
}

// Server errors keep their code only inside the LoginMgrServer facility (0x13E); the facade
// turns every other negative result into 0x81360005 (0x3200 / 0x33E0 / 0x34F0 / 0x3300).
int32_t facade_result(int32_t r) {
  if (r < 0 && (static_cast<uint32_t>(r) & 0x0FFF0000u) != 0x013E0000u) return kErrInternal;
  return r;
}

// The facade's range test on a user id: id - 0x10000000 <= 0x2FFFFFFF.
bool user_id_in_range(int32_t id) { return static_cast<uint32_t>(id) - 0x10000000u <= 0x2FFFFFFFu; }

// SceLoginMgrServerUserType -> the SceUserService user kind (dword_1880748, SceShellCore):
// persistent 0, 1 and Kratos 6 -> 0; guest / guest-by-UI / remote-play guest -> 1;
// share-play client -> 2.
constexpr int32_t kUserKind[7] = {0, 0, 1, 1, 1, 2, 0};

}  // namespace

// LoginMgrInitialize (0x15e0): loads system module 0x80000045 when it is not loaded, then
// the facade's connect (0x1800): the IPMI client is created on first use and the use count
// goes up. The host server is always reachable.
SSN_PINVOKE("libReactNative.Modules.Vsh.sprx", "LoginMgrInitialize", int32_t, ssn_LoginMgr_Initialize, ()) {
  Facade& f = facade();
  std::lock_guard l(f.mu);
  ++f.uses;
  return 0;
}

// LoginMgrTerminate (0x1610 -> 0x1A00): use count 0 -> 0x81360001; otherwise it goes down
// (the IPMI client is destroyed at 0).
SSN_PINVOKE("libReactNative.Modules.Vsh.sprx", "LoginMgrTerminate", int32_t, ssn_LoginMgr_Terminate, ()) {
  Facade& f = facade();
  std::lock_guard l(f.mu);
  if (f.uses == 0) return kErrNotConnected;
  --f.uses;
  return 0;
}

// LoginMgrCreateUser(name, userType, isIncomplete, hasShownEula, out newUserId)
// (0x1640 -> 0x3200, IPMI 0x3001F {type, isIncomplete, hasShownEula} + name):
// not connected -> 0x81360001; userType > 6, no name or no out -> 0x81360002. Server
// 0x495B00 -> 0x4936B0: CreateUser(name, out, kind[userType], 0, isIncomplete) (0x6FB800);
// hasShownEula is not used; a complete user also gets the create-user telemetry (0x493730).
SSN_PINVOKE("libReactNative.Modules.Vsh.sprx", "LoginMgrCreateUser", int32_t, ssn_LoginMgr_CreateUser,
            (const char* name, int32_t user_type, int32_t is_incomplete, int32_t has_shown_eula, int32_t* new_user_id)) {
  (void)has_shown_eula;
  {
    Facade& f = facade();
    std::lock_guard l(f.mu);
    if (f.uses == 0) return kErrNotConnected;
  }
  if (user_type > 6 || !name || !new_user_id) return kErrInvalidArg;
  if (user_type < 0) return 0;  // 0x4936B0 creates only for (unsigned)type <= 6, else result 0
  const bool incomplete = (is_incomplete & 0xFF) != 0;
  int32_t id = users::kUserIdInvalid;
  const int32_t r = users::create_user(name, kUserKind[user_type], incomplete, &id);
  if (r == 0) {
    *new_user_id = id;
    if (!incomplete) host::note_unimplemented("LoginMgrServer create-user telemetry (SceShellCore 0x493730): no telemetry service on the host");
  }
  return facade_result(r);
}

// LoginMgrDestroyUser(userId) (0x1650 -> 0x3300, IPMI 0x30020): user id out of range ->
// 0x81360006; server 0x495B70 -> 0x493C40 -> SceUserService DestroyUser (0x6FC360).
SSN_PINVOKE("libReactNative.Modules.Vsh.sprx", "LoginMgrDestroyUser", int32_t, ssn_LoginMgr_DestroyUser, (int32_t user_id)) {
  {
    Facade& f = facade();
    std::lock_guard l(f.mu);
    if (f.uses == 0) return kErrNotConnected;
  }
  if (!user_id_in_range(user_id)) return kErrInvalidUserId;
  return facade_result(users::destroy_user(user_id));
}

// LoginMgrSetLoginFlag(userId, mask, value, out newLoginFlag) (0x1660 -> 0x33E0, IPMI
// 0x30021): no out -> 0x81360002; id out of range -> 0x81360006; server 0x495BB0 -> 0x493C60
// on user parameter 6 (login_flag).
SSN_PINVOKE("libReactNative.Modules.Vsh.sprx", "LoginMgrSetLoginFlag", int32_t, ssn_LoginMgr_SetLoginFlag,
            (int32_t user_id, uint32_t mask, uint32_t value, uint32_t* new_flag)) {
  {
    Facade& f = facade();
    std::lock_guard l(f.mu);
    if (f.uses == 0) return kErrNotConnected;
  }
  if (!new_flag) return kErrInvalidArg;
  if (!user_id_in_range(user_id)) return kErrInvalidUserId;
  return facade_result(users::set_int_by_mask(user_id, users::kParamLoginFlag, mask, value, new_flag));
}

// LoginMgrSetUserStatus(userId, mask, value, out newUserStatus) (0x1670 -> 0x34F0, IPMI
// 0x30022): as above, server 0x495C10 -> 0x493D00 on user parameter 2091 (user_status).
SSN_PINVOKE("libReactNative.Modules.Vsh.sprx", "LoginMgrSetUserStatus", int32_t, ssn_LoginMgr_SetUserStatus,
            (int32_t user_id, uint32_t mask, uint32_t value, uint32_t* new_status)) {
  {
    Facade& f = facade();
    std::lock_guard l(f.mu);
    if (f.uses == 0) return kErrNotConnected;
  }
  if (!new_status) return kErrInvalidArg;
  if (!user_id_in_range(user_id)) return kErrInvalidUserId;
  return facade_result(users::set_int_by_mask(user_id, users::kParamUserStatus, mask, value, new_status));
}

// LoginMgrSendTelemetryOfCreateUser(userId, userType) (0x1680 -> 0x3600, IPMI 0x30023):
// id out of range -> 0x81360006; server 0x495C70 -> 0x493730 builds and posts the
// create-user telemetry event. HOST VALUE: there is no telemetry service; the request
// succeeds without sending anything.
SSN_PINVOKE("libReactNative.Modules.Vsh.sprx", "LoginMgrSendTelemetryOfCreateUser", int32_t,
            ssn_LoginMgr_SendTelemetryOfCreateUser, (int32_t user_id, int32_t user_type)) {
  (void)user_type;
  {
    Facade& f = facade();
    std::lock_guard l(f.mu);
    if (f.uses == 0) return kErrNotConnected;
  }
  if (!user_id_in_range(user_id)) return kErrInvalidUserId;
  host::note_unimplemented("HOST VALUE LoginMgrServer create-user telemetry (SceShellCore 0x493730): not sent");
  return 0;
}

}  // namespace ssn::shellcore
