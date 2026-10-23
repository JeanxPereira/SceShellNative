// SceUserService accessibility key-remap parameters (FW 4.03), as Settings > Accessibility >
// Controllers > Custom Button Assignments reads them (ReactNative.Modules.Vsh UserInfoUtil
// ACCESSIBILITY_KEYREMAP_* -> Sce.Vsh.UserServiceWrapper).
//
// libSceUserService:
//   sceUserServiceGetAccessibilityKeyremapEnable (xrtki9sUopg, 0x8380): the client's
//     int-parameter getter (fn ptr 0x1C220) with parameter 2056;
//   sceUserServiceGetAccessibilityKeyremapData (g6ojqW3c8Z4, 0x83c0): the client's
//     binary-parameter getter (fn ptr 0x1C240+16) with parameter 2057 and size 16
//     (ScePadRemappingInfo: buttons[14], reserved, bSwapSticks).
// Both are served by SceShellCore's per-user parameter table (0x70EDB0: 2056 -> USER_01 key
// 0x07806c02 int, 2057 -> 0x07806c03 bin[16]), modelled in shellcore/user_service.
//
// Without these the managed P/Invoke stub left ScePadRemappingInfo.buttons null and
// UserInfoUtil.Read threw in Array.TrueForAll (fatal NativeModule exception).
#include <cstdint>

#include "host/boundary.h"
#include "shellcore/user_service.h"

namespace ssn::shellcore {
namespace {

constexpr int32_t kParamKeyremapEnable = 2056;
constexpr int32_t kParamKeyremapData = 2057;

// _GetAccessibilityKeyremapEnable(int userId, out int) — xrtki9sUopg (0x8380).
int32_t GetAccessibilityKeyremapEnable(int32_t user_id, int32_t* enable) {
  return users::get_int(user_id, kParamKeyremapEnable, enable);
}
SSN_ICALL("Sce.Vsh.UserServiceWrapper::_GetAccessibilityKeyremapEnable", GetAccessibilityKeyremapEnable);

}  // namespace

// _GetAccessibilityKeyremapData(int userId, out ScePadRemappingInfo) — g6ojqW3c8Z4 (0x83c0).
SSN_PINVOKE("libSceUserService", "sceUserServiceGetAccessibilityKeyremapData", int32_t,
            ssn_UserService_GetAccessibilityKeyremapData, (int32_t user_id, uint8_t* data)) {
  return users::get_bin(user_id, kParamKeyremapData, data, 16);
}

}  // namespace ssn::shellcore
