// libSceRtc network clock (FW 4.03), shared by every P/Invoke / icall that reaches
// sceRtcGetCurrentNetworkTick (libSysBridge, libReactNative.Modules.Vsh, eboot RtcWrapper).
#pragma once
#include <cstdint>

namespace ssn::sysbridge {

// sceRtcGetCurrentNetworkTick (zO9UL3qIINQ, libSceRtc 0x590). See rtc_network.cpp.
int32_t rtc_get_current_network_tick(uint64_t* tick);

}  // namespace ssn::sysbridge
