// "NpManagerApi" P/Invokes of Sce.Vsh.Np.Manager (ShellUI, FW 4.03): libSceNpManager client
// over IPMI to SceShellCore's NP manager server ("SceNpMgrIpc").
//
// Client (libSceNpManager):
//   sceNpManagerIntGetSigninState  6miba-pcQt8 (0xc720): hook table qword_55310 (unset) ->
//     client (0xBD80: IPMI "SceNpMgrIpc" + event flag; failure -> 0x80550002); null state ->
//     0x80550003; else sync method 14 (0x7CE0), 4-byte state out, the server's result.
//   sceNpManagerIntGetCachedAccountInfo mZV+Pm4hvbI (0xd910): same client; user -1 or null ->
//     0x80550003; else sync method 26 (0x8360), 112-byte SceNpCachedAccountInfo out.
// Server (SceShellCore, sync dispatch 0x8E9D70):
//   14 -> 0x8E4D40: user -1 / null -> 0x80550003; 0x8AB3D0: the NP user context of the user
//     (list at the NpManager singleton +104) -> none: 0x80550007; else 0x8B3830: refresh the
//     state from the account data (0x8C5C50 / 0x8B64C0: no account data at context +24+104 ->
//     0x8055000A -> state 0 via 0x8B2450) and map it: 0 -> NOT_SIGNED_UP (1), 1..2 ->
//     SIGNED_OUT (2), 3..8 -> SIGNED_IN (3); result 0.
//   26 -> 0x8E50D0 -> 0x8AC020: same context lookup (none: 0x80550007) -> 0x8B3E00 ->
//     0x8C5970: no account data -> 0x8055000A (output untouched).
// Contexts: 0x8B1480 creates one (0x8B0600 -> 0x8B2120 / 0x8B2280) for every id of
// sceUserServiceGetRegisteredUserIdList (0x6E69E0). 0x8B2280 loads the user's NP account
// (0x8C54E0 -> 0x8C8740 into +24+104).
//
// HOST VALUE (identity/service): the host has no PSN accounts (README "No console identity /
// services": no NP); every registered user's NP account data is absent, as for a local user
// that never signed up for PSN.
#include <cstdint>

#include "host/boundary.h"
#include "shellcore/user_service.h"

namespace ssn::np_manager {
namespace {

constexpr int32_t kErrInvalidArgument = static_cast<int32_t>(0x80550003u);
constexpr int32_t kErrUserNotFound = static_cast<int32_t>(0x80550007u);
constexpr int32_t kErrNoAccount = static_cast<int32_t>(0x8055000Au);
constexpr int32_t kSigninStateNotSignedUp = 1;  // SCE_NP_SIGNIN_STATE_NOT_SIGNED_UP

void host_note() {
  static const bool noted =
      (host::note_unimplemented("HOST VALUE NP manager: no PSN account for any user (NP user context without account data)"), true);
  (void)noted;
}

// 0x8AB3D0 / 0x8AC020: is there an NP user context for this user (a registered user)?
bool has_context(int32_t user_id) {
  int32_t ids[shellcore::users::kListSize];
  shellcore::users::registered_user_ids(ids);
  for (int32_t id : ids)
    if (id != shellcore::users::kUserIdInvalid && id == user_id) return true;
  return false;
}

}  // namespace

SSN_PINVOKE("NpManagerApi", "sceNpManagerIntGetSigninState", int32_t, ssn_NpManager_GetSigninState, (int32_t user_id, int32_t* state)) {
  if (!state || user_id == -1) return kErrInvalidArgument;
  if (!has_context(user_id)) return kErrUserNotFound;
  host_note();
  *state = kSigninStateNotSignedUp;  // context state 0 (no account data)
  return 0;
}

SSN_PINVOKE("NpManagerApi", "sceNpManagerIntGetCachedAccountInfo", int32_t, ssn_NpManager_GetCachedAccountInfo,
            (int32_t user_id, void* info)) {
  if (!info || user_id == -1) return kErrInvalidArgument;
  if (!has_context(user_id)) return kErrUserNotFound;
  host_note();
  return kErrNoAccount;
}

}  // namespace ssn::np_manager
