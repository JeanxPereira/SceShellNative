// LoginMgr PSVR managers (eboot.bin, FW 4.03, login_mgr psvr_service.cpp / morpheus_mgr.cpp) as
// the LoginMgrProxy2 P/Invokes reach them.
//
// The LoginMgr (qword_EC6730, built by 0x1F1240) holds a PsvrService at +20920 with two
// managers made by 0x1F1690(manager, mbus device type): the PSVR ("Morpheus") manager at
// +20920 itself (type 15, vtable 0xCDAB28) and the next-generation VR ("Caesar") manager at
// +21024 (type 28, vtable 0xCDACB0). PsvrService::GetPsvrMgr (0x205390) returns the first for
// 12 and the second for 26. +21200 points at a null manager at +21112 (type -1, vtable
// 0xCDA3B0); 0x1F1240 is the only writer of +21200.
//
// Manager user id (vtable +40 = 0x203760, both vtables): n = vtable +32 (0x2036D0), the
// LoginMgr device search (0x1F98F0 -> 0x20C4A0) for the manager's type (+48) with one result
// slot; n <= 0 -> -1; otherwise the found device's owner (+200) when 0x20CF60 accepts it
// (> 0 and != 255), else -1.
#include <cstdint>
#include <cstring>

#include "eboot/login_mgr.h"
#include "host/boundary.h"

namespace ssn::eboot {
namespace {

int32_t manager_user_id(int32_t device_type) {  // 0x203760 over 0x2036D0
  uint8_t cond[256];
  init_device_condition(cond);
  std::memcpy(cond + 12, &device_type, 4);
  uint8_t rec[256];
  const int32_t n = device_search(cond, rec, 1, nullptr, 0, 0, false, false);
  if (n <= 0) return -1;
  int32_t owner;
  std::memcpy(&owner, rec + 200, 4);
  return owner > 0 && owner != 255 ? owner : -1;  // 0x20CF60
}

}  // namespace

// _GetCaesarUserId — {name, fn} entry 0xcd3d40 -> 0x1C5520: GetPsvrMgr(26) vtable +40, the
// type-28 manager. As Settings > Sound asks for it (Peripheral.getCaesarUser -> LoginMgr
// Utility.GetCaesarUserId). The host bus holds no type-28 device: -1
// (SCE_USER_SERVICE_USER_ID_INVALID). The managed stub's default 0 made Settings > Sound pass 0
// as excludedUserId and fail its volume page load.
SSN_PINVOKE("LoginMgrProxy", "_GetCaesarUserId", int32_t, ssn_LoginMgrProxy_GetCaesarUserId, ()) {
  return manager_user_id(28);
}

// _GetMorpheusUserId — {name, fn} entry 0xcd3d30 -> 0x1C54F0 (also the LoginMgrProxy v1 icall):
// GetPsvrMgr(12) vtable +40, the type-15 manager. No type-15 device on the host bus: -1.
SSN_PINVOKE("LoginMgrProxy", "_GetMorpheusUserId", int32_t, ssn_LoginMgrProxy_GetMorpheusUserId, ()) {
  return manager_user_id(15);
}

// long _GetMorpheusDeviceId — {name, fn} entry 0xcd3d10 -> 0x1C54B0: the manager at LoginMgr
// +21200, vtable +48. +21200 only ever holds the null manager (0xCDA3B0), whose +48 is
// 0x1F1820: return -1. (The managed stub answered 0.)
SSN_PINVOKE("LoginMgrProxy", "_GetMorpheusDeviceId", int64_t, ssn_LoginMgrProxy_GetMorpheusDeviceId, ()) {
  return -1;
}

}  // namespace ssn::eboot
