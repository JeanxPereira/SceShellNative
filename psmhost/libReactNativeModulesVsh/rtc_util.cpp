// ReactNative.Modules.Vsh RtcUtil (FW 4.03): "currentNetworkTime" of the RN time module.
//
// RtcGetCurrentNetworkTickNative -> "libReactNative.Modules.Vsh.sprx" export m7od1ikoZpQ
// (0x4510), a thunk to the libSceRtc import zO9UL3qIINQ (sceRtcGetCurrentNetworkTick); see
// libSysBridge/rtc_network.cpp. RtcUtil.GetTimeFromNative only reports "msec" when the result
// is 0, so an unsynchronised network clock reaches JS as the error code.
#include <cstdint>

#include "host/boundary.h"
#include "libSysBridge/rtc_network.h"

namespace ssn::rnvsh {

SSN_PINVOKE("libReactNative.Modules.Vsh.sprx", "RtcGetCurrentNetworkTickNative", int32_t, ssn_RNVsh_RtcGetCurrentNetworkTickNative,
            (uint64_t* tick)) {
  return sysbridge::rtc_get_current_network_tick(tick);
}

}  // namespace ssn::rnvsh
