// "NpCommonApi" P/Invokes of Sce.Vsh.Np (ShellUI eboot, FW 4.03).
#include <cstdint>

#include "host/boundary.h"

namespace ssn::np {

// sceNpCheckAllCallbacksForVsh — eboot 0x4B61B0: runs the NP manager's callback checks
// (imports 3Zl8BePTh9Y, then JELHf4xPufo) and returns the first one's error, else the second's
// result. They only dispatch callbacks that were registered and have pending notifications;
// the host's NP registration entries are stubs, so nothing is registered and there is nothing
// to dispatch: 0.
SSN_PINVOKE("NpCommonApi", "sceNpCheckAllCallbacksForVsh", int32_t, ssn_NpCommon_CheckAllCallbacksForVsh, ()) {
  return 0;
}

}  // namespace ssn::np
