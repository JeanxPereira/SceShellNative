// Sce.Vsh.SysBridge.Kernel event-flag P/Invokes into libSysBridgeKernel, which exposes
// the kernel's sceKernel*EventFlag calls to managed code.
#include <cstdint>

#include "common/mono_util.h"
#include "core/log.h"
#include "host/boundary.h"
#include "libkernel/event_flag.h"

namespace ssn::sysbridge {

using kernel::EventFlag;

SSN_PINVOKE("libSysBridgeKernel", "sceKernelPollEventFlag", int32_t, ssn_SysBridgeKernel_sceKernelPollEventFlag,
            (void* ef, uint64_t bits, uint32_t mode, uint64_t* result)) {
  return kernel::poll_event_flag(static_cast<EventFlag*>(ef), bits, mode, result);
}

// Both managed overloads (ref IntPtr / IntPtr timeout) land here: the timeout argument
// is a pointer to the microsecond count, or null for an infinite wait.
SSN_PINVOKE("libSysBridgeKernel", "sceKernelWaitEventFlag", int32_t, ssn_SysBridgeKernel_sceKernelWaitEventFlag,
            (void* ef, uint64_t bits, uint32_t mode, uint64_t* result, uint32_t* timeout)) {
  return kernel::wait_event_flag(static_cast<EventFlag*>(ef), bits, mode, result, timeout);
}

SSN_PINVOKE("libSysBridgeKernel", "sceKernelSetEventFlag", int32_t, ssn_SysBridgeKernel_sceKernelSetEventFlag, (void* ef, uint64_t bits)) {
  return kernel::set_event_flag(static_cast<EventFlag*>(ef), bits);
}

// Named flags belong to other processes (ShellCore, daemons); only flags the host
// itself created can be opened, anything else reports ESRCH like a missing object.
SSN_PINVOKE("libSysBridgeKernel", "sceKernelOpenEventFlag", int32_t, ssn_SysBridgeKernel_sceKernelOpenEventFlag,
            (void** ef, const char* name)) {
  EventFlag* f = kernel::open_event_flag(name ? name : "");
  if (!f) {
    log::warn("kernel", "sceKernelOpenEventFlag(\"{}\"): no such event flag on the host", name ? name : "");
    return kernel::kErrorEsrch;
  }
  *ef = f;
  return 0;
}

SSN_PINVOKE("libSysBridgeKernel", "sceKernelCloseEventFlag", int32_t, ssn_SysBridgeKernel_sceKernelCloseEventFlag, (void* ef)) {
  return ef ? 0 : kernel::kErrorEsrch;
}

}  // namespace ssn::sysbridge
