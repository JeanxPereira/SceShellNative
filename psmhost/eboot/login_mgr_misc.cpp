// LoginMgrProxy / LoginMgrProxy2 entries of the eboot's native LoginMgr (FW 4.03) that read its
// static data, its controller-update manager and the user service.
//
// Static data (login_mgr.cpp): a JSON object map at LoginMgr +20704 (+8: the map) under a
// recursive mutex. initialize (0x1ED5B0) reads the ShellCore app data "loginmgr"
// (sceShellCoreUtilGetAppData, 2048 bytes) and parses it; with no stored data (size 0) or a
// parse error it applies the defaults of 0x1EF8E0: AutoAssignEnable true, FirstLogin true,
// ReadLotusButtonDisabled false, LogLevel 1, BootSequenceState true. Every setter (0x1F0F60
// bool, 0x1F1040 int) stores the value and queues SceLoginMgrWriteStaticDataJob (0x1F0CA0),
// which writes the object back to the app data.
// The host keeps the object in memory, starting from the 0x1EF8E0 defaults (a console with no
// stored "loginmgr" app data); the app-data round trip is not reproduced (noted once).
#include <cstdint>
#include <cstring>
#include <map>
#include <mutex>
#include <string>

#include "eboot/login_mgr.h"
#include "host/boundary.h"
#include "shellcore/user_service.h"

namespace ssn::eboot {
namespace {

constexpr int32_t kErrInvalidArgument = static_cast<int32_t>(0x80B60003u);

struct StaticData {
  std::recursive_mutex mu;
  std::map<std::string, int64_t> values;  // the object's bool / integer members
  StaticData() {                           // 0x1EF8E0
    values["AutoAssignEnable"] = 1;
    values["FirstLogin"] = 1;
    values["ReadLotusButtonDisabled"] = 0;
    values["LogLevel"] = 1;
    values["BootSequenceState"] = 1;
  }
};

StaticData& static_data() {
  static StaticData d;
  return d;
}

void note_static_data() {
  static const bool noted = (host::note_unimplemented(
                                 "LoginMgr static data: ShellCore app data 'loginmgr' (0x1ED5B0 read, SceLoginMgrWriteStaticDataJob "
                                 "0x1F0CA0 write) not reproduced; the 0x1EF8E0 defaults, kept in memory"),
                             true);
  (void)noted;
}

int64_t get_value(const char* key) {  // 0x1F0EA0 (int) / 0x1F0DE0 (bool)
  note_static_data();
  StaticData& d = static_data();
  std::lock_guard l(d.mu);
  return d.values[key];
}

void set_value(const char* key, int64_t v) {  // 0x1F1040 (int) / 0x1F0F60 (bool)
  note_static_data();
  StaticData& d = static_data();
  std::lock_guard l(d.mu);
  d.values[key] = v;
}

}  // namespace

// int _GetLogLevel — {name, fn} entry 0xcd4070 -> 0x1C5FF0: static data "LogLevel".
SSN_PINVOKE("LoginMgrProxy", "_GetLogLevel", int32_t, ssn_LoginMgrProxy_GetLogLevel, ()) {
  return static_cast<int32_t>(get_value("LogLevel"));
}

// void _SetLogLevel(int) — {name, fn} entry 0xcd4060 -> 0x1C5FC0: static data "LogLevel" = value.
SSN_PINVOKE("LoginMgrProxy", "_SetLogLevel", void, ssn_LoginMgrProxy_SetLogLevel, (int32_t level)) {
  set_value("LogLevel", level);
}

// int _GetMinControllerUpdateBatteryLevel — {name, fn} entry 0xcd3fa0 -> 0x1C5DC0: return 10.
SSN_PINVOKE("LoginMgrProxy", "_GetMinControllerUpdateBatteryLevel", int32_t,
            ssn_LoginMgrProxy_GetMinControllerUpdateBatteryLevel, ()) {
  return 10;
}

// int _IsBondUpdating — {name, fn} entry 0xcd3f40 -> 0x1C5C50: the controller-update manager
// singleton (0x1DE110, created on first use: "SceLoginMgrControllerFwUpdate", +32 = 0) and its
// "bond updating" byte +32 under its mutex (0x1DEEB0). Its only writers are the constructor
// (0), UpdateStart (0x1DF070, set to 1 for a type 22..24 device on bus 0/1; reached from the
// P/Invoke entry 0x1C5BC0, which the host does not serve) and 0x1DF690 (0). With UpdateStart
// never run on the host the byte stays 0.
SSN_PINVOKE("LoginMgrProxy", "_IsBondUpdating", int32_t, ssn_LoginMgrProxy_IsBondUpdating, ()) {
  return 0;
}

// void _GetLotusWakeOnButton(ref int wakeOnType, ref byte partnerCode) — {name, fn} entry
// 0xcd3e10 -> 0x1C5990 -> 0x1F0590: null arguments -> logged, nothing written; otherwise both
// 0; the first time (static data "ReadLotusButtonDisabled" false) the flag is set and the
// LoginMgr device search (0x1F98F0) looks for one type-26 ("Lotus") device; only when one is
// found are the wake-on type and partner code read from it (wxmyc_71ijI / eqldkyw7GAo).
SSN_PINVOKE("LoginMgrProxy", "_GetLotusWakeOnButton", void, ssn_LoginMgrProxy_GetLotusWakeOnButton,
            (int32_t* wake_on_type, uint8_t* partner_code)) {
  if (!wake_on_type || !partner_code) return;  // "GetLotusPartnerButton() : invalid parameter."
  *wake_on_type = 0;
  *partner_code = 0;
  if (get_value("ReadLotusButtonDisabled") != 0) return;
  set_value("ReadLotusButtonDisabled", 1);
  uint8_t cond[256];
  init_device_condition(cond);
  const int32_t type = 26;
  std::memcpy(cond + 12, &type, 4);
  uint8_t rec[256];
  if (device_search(cond, rec, 1, nullptr, 0, 0, false, false) == 1)
    host::note_unimplemented("LoginMgr GetLotusPartnerButton (0x1F0590): Lotus device wake-on query (wxmyc_71ijI) not reproduced");
}

// LoginMgrProxy (v1) _GetLoginFlag / _GetUserStatus(int userId, out uint) — eboot icalls 0x1C81E0
// / 0x1C8250 -> 0x2121D0 / 0x2122C0 (user_service_utility.cpp): null -> 0x80B60003; under the
// LoginMgr mutex (+20584) sceUserServiceGetLoginFlag (QNk7qD4dlD4, parameter 6) /
// sceUserServiceGetUserStatus (RJX7T4sjNgI, parameter 2091); on an error the value is 0 and the
// error is returned.
namespace {

int32_t get_user_param(int32_t user_id, uint32_t* out, int32_t param) {
  if (!out) return kErrInvalidArgument;
  int32_t v = 0;
  const int32_t r = shellcore::users::get_int(user_id, param, &v);
  *out = r == 0 ? static_cast<uint32_t>(v) : 0u;
  return r;
}

int32_t GetLoginFlagV1(int32_t user_id, uint32_t* out) {
  return get_user_param(user_id, out, shellcore::users::kParamLoginFlag);
}
SSN_ICALL("Sce.Vsh.ShellUI.LoginMgrProxy.NativeCall::_GetLoginFlag", GetLoginFlagV1);

int32_t GetUserStatusV1(int32_t user_id, uint32_t* out) {
  return get_user_param(user_id, out, shellcore::users::kParamUserStatus);
}
SSN_ICALL("Sce.Vsh.ShellUI.LoginMgrProxy.NativeCall::_GetUserStatus", GetUserStatusV1);

}  // namespace

}  // namespace ssn::eboot
