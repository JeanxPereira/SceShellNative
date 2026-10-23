// LoginMgrProxy _Login (eboot.bin, FW 4.03): ShellUI's in-process LoginMgr logging a user
// in (login_mgr\utility\user_service_utility.cpp Login 0x20FE40 / LoginProcess 0x210400,
// device_manager.cpp Assign 0x1D2F90), over the host's SceUserService
// (shellcore/user_service) and mbus (libSceMbus) models.
#include <cstdint>
#include <cstdio>
#include <cstring>

#include "common/mono_array.h"
#include "host/boundary.h"
#include "libSceMbus/mbus.h"
#include "shellcore/user_service.h"

namespace ssn::eboot {
namespace {

namespace users = shellcore::users;

constexpr int32_t kErrInvalidArgument = static_cast<int32_t>(0x80B60003u);

int32_t login_user_count() {
  int32_t ids[users::kMaxLogin];
  users::login_user_ids(ids);
  int32_t n = 0;
  for (int32_t id : ids) n += id != users::kUserIdInvalid;
  return n;
}

// The LoginMgr device object for `device_id` (0x1CE820): the devices it holds come from
// the mbus device list, so a device exists when the bus reports that id.
bool device_exists(int64_t device_id) {
  uint8_t cond[256];
  mbus::init_condition(cond, sizeof cond);
  for (int i = 0; i < 11; ++i) std::memcpy(cond + 24 + 16 * i, "\xff\xff\xff\xff", 4);
  std::memcpy(cond, &device_id, 8);
  uint8_t rec[mbus::kRecordSize];
  int32_t total = 0, gen = 0;
  return mbus::get_device_info_by_condition(cond, 0, rec, 1, &total, &gen, mbus::kRecordSize) > 0;
}

// LoginProcess (0x210400) for a user who is not logged in yet: sceUserServiceLogin, then
// the LoginMgr server's SetLoginFlag(mask 0x810, value 0) and SetUserStatus(mask and value
// (isTemporary << 4) | 2) (0x2109F0 / 0x210B60; failures are only logged), then the
// audio-device parameters 2212 / 2217.
int32_t login_process(int32_t user_id, int32_t login_type, bool is_temporary) {
  if (!users::is_logged_in(user_id)) {
    if ((login_type - 5u) >= 2u && login_user_count() == 0)
      host::note_unimplemented("LoginMgr first-login notification (SendNotification 254/35, 0x1EE240): no managed handler registered on the host");
    int32_t privacy = 0;
    if (users::get_int(user_id, users::kParamPlatformPrivacyWs1, &privacy) == 0 && privacy == -1)
      host::note_unimplemented("LoginMgr LoginProcess NP account-type test (_6miba_pcQt8) before SetPlatformPrivacyWs1(0) not traced; skipped");
    if (const int32_t r = users::login(user_id)) return r;
    host::note_unimplemented(
        "LoginMgr LoginProcess bookkeeping (0x1CD280 / 0x205450), account-id job (0x212650) not reproduced");
  }
  uint32_t out = 0;
  users::set_int_by_mask(user_id, users::kParamLoginFlag, 0x810, 0, &out);
  const uint32_t st = (static_cast<uint32_t>(is_temporary) << 4) | 2u;
  users::set_int_by_mask(user_id, users::kParamUserStatus, st, st, &out);
  host::note_unimplemented("LoginMgr LoginProcess audio-device parameters 2212 / 2217 (0x210400 tail) not applied");
  return 0;
}

// Assign (0x1D2F90) for a logged-in user: the device is bound to the user on the bus
// (0x1D2350 -> sceMbusBindDeviceWithUserId).
void assign_device(int64_t device_id, int32_t user_id) {
  if (!users::is_logged_in(user_id)) return;  // "[Assign] not login user."
  const int32_t r = mbus::bind_device_with_user_id(static_cast<uint64_t>(device_id), user_id);
  std::fprintf(stderr, "#LOGIN MGR# sceMbusBindDeviceWithUserId( 0x%016llx, 0x%08x ) : %d(0x%08x)\n",
               static_cast<unsigned long long>(device_id), static_cast<uint32_t>(user_id), r, static_cast<uint32_t>(r));
  host::note_unimplemented("LoginMgr device Assign: device-object state updates and notifications (0x1D2F90 vcalls) not reproduced");
}

}  // namespace

// int _Login(int userId, long deviceId, long controllerId, int loginType, ref int isLoginSuccess,
//            int isTryMorpheusInitialSetupFlow, int isDebugAutoLogin, bool isTemporaryLogin...)
// — 0x1C5070 -> 0x20FE40(userId, deviceId, loginType, &isLoginSuccess, isTryMorpheus == 1,
// isDebugAutoLogin == 1, isTemporary). With a device: no device object -> 0x80B60003;
// LoginProcess, isLoginSuccess = 1 on success, then the device is assigned to the user.
// Without a device (-1) the user is logged in alone. The notifications to the managed
// handler afterwards (NotifyOnDeviceAdded 0x1EDED0, first-login 0x2159B0) are not reproduced.
SSN_PINVOKE("LoginMgrProxy", "_Login", int32_t, ssn_LoginMgrProxy_Login,
            (int32_t user_id, int64_t device_id, int64_t controller_id, int32_t login_type, int32_t* is_login_success,
             int32_t is_try_morpheus, int32_t is_debug_auto_login, uint8_t is_temporary)) {
  (void)controller_id;
  (void)is_try_morpheus;
  (void)is_debug_auto_login;
  std::fprintf(stderr, "#LOGIN MGR# [Login] userId=0x%08x, isAlreadyLoggedin=%d\n", static_cast<uint32_t>(user_id),
               users::is_logged_in(user_id) ? 1 : 0);
  int32_t r;
  if (device_id == -1) {
    r = login_process(user_id, login_type, is_temporary != 0);
  } else {
    if (!device_exists(device_id)) return kErrInvalidArgument;  // "devicePtr is null"
    r = login_process(user_id, login_type, is_temporary != 0);
    if (r == 0) assign_device(device_id, user_id);
  }
  if (is_login_success) *is_login_success = r == 0 ? 1 : 0;
  if (r != 0) std::fprintf(stderr, "#LOGIN MGR# LoginProcess() failed. ret = 0x%x\n", static_cast<uint32_t>(r));
  else
    host::note_unimplemented("LoginMgr Login notifications to the managed send-notification handler (0x1EDED0 / 0x2159B0) not sent");
  return r;
}

namespace {

// Sce.Vsh.ShellUI.LoginMgrProxy.NativeCall::_GetOpenPsIdForSystem(byte[]) — eboot 0x1c7800:
// array length other than 16 -> 0x80B6000F; otherwise sceKernelGetOpenPsIdForSystem
// (ul57hvm6mBc) into a local and, on success, the 16 bytes into the array.
// HOST VALUE (as libSysBridge's sceKernelGetOpenPsIdForSystem): no console identity, 16
// zero bytes.
int32_t GetOpenPsIdForSystem(MonoArray* id) {
  const psm::ArrayView v = psm::view(id);
  if (v.bytes() != 16) return static_cast<int32_t>(0x80B6000Fu);
  host::note_unimplemented("HOST VALUE OpenPSID (machdep.openpsid_for_sys): no console identity; 16 zero bytes");
  std::memset(v.data, 0, 16);
  return 0;
}
SSN_ICALL("Sce.Vsh.ShellUI.LoginMgrProxy.NativeCall::_GetOpenPsIdForSystem", GetOpenPsIdForSystem);

}  // namespace
}  // namespace ssn::eboot
