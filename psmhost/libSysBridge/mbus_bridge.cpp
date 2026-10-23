// Sce.Vsh.SysBridge.Mbus P/Invokes ([DllImport("libSysBridgeMbus")], FW 4.03) that are
// pure libSceMbus client code.
//
// libSceSysBridge.sprx publishes "libSysBridgeMbus" as a {name, fn} table; the entry
// "sceMbusConvertToLocalDeviceId2" (table 0x784e0) is the libSceMbus import
// wxmyc+71ijI (libSceMbus 0x64e0).
#include <cstdint>

#include "host/boundary.h"

namespace ssn::sysbridge {

// int sceMbusConvertToLocalDeviceId2(long deviceId, SceMbusKmodSystem system, ref ulong
// localDeviceId) — libSceMbus 0x64e0. A device id carries its kernel-module system in
// bits 48..63: when they equal `system`, the local id is the low 48 bits (bzhi 0x30) and
// the result is 0. Otherwise the client asks the mbus server (0x2590, IPC with the
// caller's thread id from HoLVWNanBBc) to translate between systems; that server path is
// not traced. The out value is written only for a non-negative result.
SSN_PINVOKE("libSysBridgeMbus", "sceMbusConvertToLocalDeviceId2", int32_t, ssn_SysBridgeMbus_ConvertToLocalDeviceId2,
            (int64_t device_id, uint32_t system, uint64_t* local_device_id)) {
  const uint64_t id = static_cast<uint64_t>(device_id);
  if ((id >> 48) == system) {
    if (local_device_id) *local_device_id = id & ((1ull << 48) - 1);
    return 0;
  }
  host::note_unimplemented("libSceMbus sceMbusConvertToLocalDeviceId2 cross-system translation (0x2590 server IPC) not traced");
  // Placeholder status for the untraced server answer (marked; no out value written).
  return static_cast<int32_t>(0x80020022u);
}

}  // namespace ssn::sysbridge
