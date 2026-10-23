// Sce.Vsh.UserServiceWrapper per-user parameter getters (FW 4.03).
//
// The eboot registers these internal calls straight onto libSceUserService imports (the icall
// is the import slot). The client functions call the client's parameter accessors in the table
// at 0x1C220 (filled by 0x4450; IPMI client methods of the "SceUserService" server in
// SceShellCore, dispatcher 0x1A73E0):
//   +0x00 int getter    (0x3B70 -> IPMI 0x30008 -> 0x713210 -> 0x6ECA70 -> 0x6EAB70 / 0x6EADE0)
//   +0x10 64-bit getter (0x3BC0 -> IPMI 0x30006 -> 0x7131F0 -> 0x6ECA20 -> 0x6E9DC0 / 0x6EA090)
//   +0x30 bin getter    (0x3C70)
// Both integer getters return the entry's 32-bit member (the 64-bit one zero-extended), check
// the parameter against 0x70EDB0's table (0x7124A0), and answer 0x80960105 for user id -1 or
// an unknown user and 0x80960005 for a null output. The host's parameter store
// (shellcore/user_service) serves them from the users' registry slots.
#include <cstdint>

#include <mono/metadata/appdomain.h>
#include <mono/metadata/object.h>

#include "host/boundary.h"
#include "shellcore/user_service.h"

namespace ssn::shellcore {
namespace {

// Client function -> parameter, through the int getter (+0x00): "return getter(id, param, out)".
#define SSN_USER_INT_PARAM(Method, nid, addr, param)                                    \
  int32_t Method(int32_t user_id, int32_t* value) {                                     \
    return users::get_int(user_id, param, value);                                       \
  }                                                                                     \
  SSN_ICALL("Sce.Vsh.UserServiceWrapper::_" #Method, Method)

SSN_USER_INT_PARAM(GetJapaneseInputType, "u-dCVE6fQAU", 0x5c50, 1280);
SSN_USER_INT_PARAM(GetTraditionalChineseInputType, "oXVAQutr3Ns", 0x5c10, 1281);
SSN_USER_INT_PARAM(GetKeyboardType, "Ta52bXx5Tek", 0x5bd0, 1282);
SSN_USER_INT_PARAM(GetKeyRepeatStartingTime, "iWpzXixD0UE", 0x5c90, 1283);
SSN_USER_INT_PARAM(GetKeyRepeatSpeed, "XUT7ad-BUMc", 0x5cd0, 1284);
SSN_USER_INT_PARAM(GetVibrationStrength, "HTl8p6AEWFw", 0x5d90, 516);
SSN_USER_INT_PARAM(GetVolumeForSidetone, "3UZADLBXpiA", 0x5b30, 517);
SSN_USER_INT_PARAM(GetLightBarBaseBrightness, "q+7UTGELzj4", 0x5e10, 518);
SSN_USER_INT_PARAM(GetMouseHandType, "sukPd-xBDjM", 0x5e50, 519);
SSN_USER_INT_PARAM(GetMousePointerSpeed, "Y5zgw69ndoE", 0x5e90, 520);
SSN_USER_INT_PARAM(GetGlsLiveQuality2, "Z+dzNaClq7w", 0x6c90, 1539);
SSN_USER_INT_PARAM(GetGlsBroadcastService, "HxNRiCWfVFw", 0x6f20, 1545);
SSN_USER_INT_PARAM(GetGlsCameraSize, "zBLxX8JRMoo", 0x7320, 1558);
SSN_USER_INT_PARAM(GetGlsCameraBgFilter, "Y5U66nk0bUc", 0x7360, 1559);
SSN_USER_INT_PARAM(GetAccessibilityZoom, "1zDEFUmBdoo", 0x8300, 2055);
SSN_USER_INT_PARAM(GetVolumeForMorpheusSidetone, "r2QuHIT8u9I", 0x5b70, 2069);
SSN_USER_INT_PARAM(GetNpAccountUpgradeFlag, "Veo1PbQZzG4", 0x57a0, 2119);
SSN_USER_INT_PARAM(GetHoldAudioOutDevice, "VjLkKY0CQew", 0x5ed0, 2121);
SSN_USER_INT_PARAM(GetTriggerEffectStrength, "sq-XofudYqA", 0x5dd0, 2190);
SSN_USER_INT_PARAM(GetWebBrowserEnableCookie, "BdzmN4JP3JM", 0x9b10, 2194);
SSN_USER_INT_PARAM(GetWebBrowserEnableJavaScript, "WGYLbMB95kM", 0x9b50, 2195);
SSN_USER_INT_PARAM(GetAccessibilityLongPressTime, "hKzuaakSOLE", 0x8400, 2206);
SSN_USER_INT_PARAM(GetWebBrowserEnablePreventCrossSiteTracking, "L9NSPGwLZTQ", 0x9b90, 2213);
SSN_USER_INT_PARAM(GetIsHrtfEnabled, "g0Iqe+IHNH8", 0x9f80, 2217);
SSN_USER_INT_PARAM(GetIsUmmStatusMutedWhenLoggedIn, "1fV-P4lyYAM", 0xa000, 2218);
SSN_USER_INT_PARAM(GetIsUmmStatusMutedWhenChatBroadcast, "6erDJ4-MGCs", 0xa040, 2219);
SSN_USER_INT_PARAM(GetAccessibilityZoomSetFocus, "iRkGgbFAhcw", 0x8340, 2246);

#undef SSN_USER_INT_PARAM

// Client functions over the 64-bit getter (+0x10) into a local, then `*out = (int)local`:
// null out -> 0x80960005 without a call. The value is the entry's 32-bit member (0x6EA090).
int32_t get_through_64(int32_t user_id, int32_t param, uint32_t* out) {
  if (!out) return users::kErrInvalidArgument;
  int32_t v = 0;
  const int32_t r = users::get_int(user_id, param, &v);
  *out = static_cast<uint32_t>(v);  // the client stores its local whatever the result
  return r;
}

// _GetCaptureStatus(int, out uint) — y5JnCLW7ccU (0x7d50): parameter 2207.
int32_t GetCaptureStatus(int32_t user_id, uint32_t* out) { return get_through_64(user_id, 2207, out); }
SSN_ICALL("Sce.Vsh.UserServiceWrapper::_GetCaptureStatus", GetCaptureStatus);

// _GetGlsUserFlag(int, out uint) — tPbEIE0wYCM (0x7a90): parameter 2227.
int32_t GetGlsUserFlag(int32_t user_id, uint32_t* out) { return get_through_64(user_id, 2227, out); }
SSN_ICALL("Sce.Vsh.UserServiceWrapper::_GetGlsUserFlag", GetGlsUserFlag);

// _GetHintShowFlag(int, out uint) — eZES7x+e-Mg (0xa1c0): parameter 2235.
int32_t GetHintShowFlag(int32_t user_id, uint32_t* out) { return get_through_64(user_id, 2235, out); }
SSN_ICALL("Sce.Vsh.UserServiceWrapper::_GetHintShowFlag", GetHintShowFlag);

// _GetNpAccountId(int, out ulong) — 6dfDreosXGY (0x4780): the 64-bit getter with parameter 256
// (0x6EA090: the entry's 64-bit account id, registry binary of 8 bytes).
int32_t GetNpAccountId(int32_t user_id, uint64_t* out) {
  if (!out) return users::kErrInvalidArgument;
  return users::get_bin(user_id, users::kParamAccountId, out, sizeof *out);
}
SSN_ICALL("Sce.Vsh.UserServiceWrapper::_GetNpAccountId", GetNpAccountId);

// _GetGlsIsMuteEnabled(int, out bool) — W3neFYAvZss (0x6e10): null -> 0x80960005; the int getter
// with parameter 1542 into a local; *out = local > 0.
int32_t GetGlsIsMuteEnabled(int32_t user_id, uint8_t* out) {
  if (!out) return users::kErrInvalidArgument;
  int32_t v = 0;
  const int32_t r = users::get_int(user_id, 1542, &v);
  *out = v > 0 ? 1 : 0;
  return r;
}
SSN_ICALL("Sce.Vsh.UserServiceWrapper::_GetGlsIsMuteEnabled", GetGlsIsMuteEnabled);

// _GetPasscode(int, out string) — eboot 0x4A6E10: a 16-byte buffer, sceUserServiceGetPasscode
// (X5rJZNDZ2Ss, 0x4730: the string getter +0x20 with parameter 2, size min(16, 1024)); on an
// error the buffer is "" — the string is made from it either way, the result returned.
int32_t GetPasscode(int32_t user_id, MonoString** out) {
  char buf[16] = {};
  const int32_t r = users::get_str(user_id, 2, buf, sizeof buf);
  if (r != 0) buf[0] = 0;
  if (out) *out = mono_string_new(mono_domain_get(), buf);
  return r;
}
SSN_ICALL("Sce.Vsh.UserServiceWrapper::_GetPasscode", GetPasscode);

// _GetAutoLoginEnabled(int, out bool) — eboot icall 0x4A8F70 (devkit build 0x4AA320):
// sceUserServiceGetAutoLoginEnabled (nqDEnj7M0QE, 0x48F0: the int getter +0x00 with parameter 3,
// registry USER_nn 0x07800400) into a local; on result 0 *out = (local == 1), otherwise *out is
// not written; the result is returned.
int32_t GetAutoLoginEnabled(int32_t user_id, uint8_t* out) {
  int32_t v = 0;
  const int32_t r = users::get_int(user_id, 3, &v);
  if (r == 0) *out = v == 1 ? 1 : 0;
  return r;
}
SSN_ICALL("Sce.Vsh.UserServiceWrapper::_GetAutoLoginEnabled", GetAutoLoginEnabled);

// _IsGuestUser(int) — FnWkLNOmJXw (0x32a0): (id & 0xE0000000) == 0x20000000.
bool IsGuestUser(int32_t user_id) { return (static_cast<uint32_t>(user_id) & 0xE0000000u) == 0x20000000u; }
SSN_ICALL("Sce.Vsh.UserServiceWrapper::_IsGuestUser", IsGuestUser);

// _IsSharePlayClientUser(int) — -7XgCmEwKrs (0x32c0): (id & 0xF0000000) == 0x30000000.
bool IsSharePlayClientUser(int32_t user_id) { return (static_cast<uint32_t>(user_id) & 0xF0000000u) == 0x30000000u; }
SSN_ICALL("Sce.Vsh.UserServiceWrapper::_IsSharePlayClientUser", IsSharePlayClientUser);

}  // namespace

// _GetControlCenterFunctionControlSettings(int, byte[]) — sceUserServiceGetControlCenterFunction
// ControlSettings (1z7XMEnjelI, 0xa080): the bin getter (+0x30) with parameter 2222, 64 bytes.
SSN_PINVOKE("libSceUserService", "sceUserServiceGetControlCenterFunctionControlSettings", int32_t,
            ssn_UserService_GetControlCenterFunctionControlSettings, (int32_t user_id, uint8_t* data)) {
  return users::get_bin(user_id, 2222, data, 64);
}

}  // namespace ssn::shellcore
