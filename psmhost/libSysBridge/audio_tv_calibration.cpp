// TV audio calibration registry queries — "libSysBridgeAudioTvCalibration" (libSceSysBridge
// {name, fn} table; FW 4.03, same code in both dumps):
//   sceAudioTvCalibrationRegistryIsEnable  entry 0x78030 -> 0x4F20: 0x57D0() != 0, where 0x57D0
//     reads registry 0x0B480100 (sceRegMgrGetInt): the read's error when it fails, else value != 0;
//   sceAudioTvCalibrationRegistryIsValid   entry 0x78040 -> 0x4F80: 0x58A0() != 0, where 0x58A0
//     reads registry 0x0B480200: the read's error when it fails, else 7 when the value is non-zero,
//     0 otherwise.
// A failed read therefore answers 1 (the error is non-zero), as in the firmware.
#include <cstdint>

#include "host/boundary.h"
#include "libSceRegMgr/regmgr.h"

namespace ssn::sysbridge {
namespace {

int32_t is_enable() {  // 0x57D0
  int32_t v = 0;
  const int32_t r = regmgr::get_int(0x0B480100u, &v);
  return r < 0 ? r : (v != 0 ? 1 : 0);
}

int32_t is_valid() {  // 0x58A0
  int32_t v = 0;
  const int32_t r = regmgr::get_int(0x0B480200u, &v);
  return r < 0 ? r : (v != 0 ? 7 : 0);
}

}  // namespace

SSN_PINVOKE("libSysBridgeAudioTvCalibration", "sceAudioTvCalibrationRegistryIsEnable", int32_t,
            ssn_SysBridgeAudioTvCalibration_RegistryIsEnable, ()) {
  return is_enable() != 0 ? 1 : 0;
}

SSN_PINVOKE("libSysBridgeAudioTvCalibration", "sceAudioTvCalibrationRegistryIsValid", int32_t,
            ssn_SysBridgeAudioTvCalibration_RegistryIsValid, ()) {
  return is_valid() != 0 ? 1 : 0;
}

}  // namespace ssn::sysbridge
