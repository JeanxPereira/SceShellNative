// Console hardware queries ShellUI reaches (FW 4.03), each logged once:
//
// sceKernelGetHwSerialNumber / sceKernelGetHwModelName — "libSysBridgeKernel" (libSceSysBridge
// {name, fn} table 0x78760 / 0x78770 -> the libkernel_sys imports PgGve9EvhPE (0x24C30) /
// JC7I7J1bllQ (0x24CF0)): null buffer -> 0x80020016; sysctlbyname("machdep.icc.hw_info", 17 bytes)
// / ("machdep.icc.hw_model", 32 bytes); on failure errno + 0x80020000 (buffer not written), else
// the printable prefix of the answer copied and terminated, 0. The kernel handlers (FW 7.00 kernel,
// the one in hand: 0xFFFFFFFF80C02BB0 / 0xFFFFFFFF80C02CD0) pass the caller check (the host's
// ShellUI has the system attribute, see kernel_modes.cpp), read the board id (0xFFFFFFFF808C43D0;
// failure -> "failed to get boardid", ENXIO; board id 0x0301010101010120 -> ENXIO), then ask the
// ICC (0xFFFFFFFF80D12740, command 2 / 0x10 or 0x30) and copy its answer out.
// HOST VALUE (console identity): the PC has no ICC and no board id, so the handlers take their
// board-id failure path: ENXIO, i.e. 0x80020006, and the buffers are not written (DebugInfoView
// shows "N/A").
//
// sceKernelIsDevelopmentMode / sceKernelIsAssistMode — "libSceVshKernelWrapper.a" (eboot {name, fn}
// entries 0xcef5a0 / 0xcef5b0; devkit build 0xcf35c0 / 0xcf35d0) -> libSceDipsw UtO0OHMCgmI (0x7B0)
// / PKg59uLpHNI (0x7C0) = dipsw switch 0 / 2 (0x380): open("/dev/dipsw"); on failure the switch's
// bit in the table that the module init (0x6A0) filled from /dev/dipsw (ioctl 0x8010440A) after
// zeroing it — all zero when the device could not be opened there either; otherwise ioctl
// 0xC0088803 for that switch.
// HOST VALUE (console hardware): the PC has no /dev/dipsw, so both answer 0 (the zeroed table),
// "Release Check Mode: Release".
#include <cstdint>

#include "host/boundary.h"

namespace ssn::sysbridge {
namespace {

constexpr int32_t kErrInval = static_cast<int32_t>(0x80020016u);
constexpr int32_t kErrNxio = static_cast<int32_t>(0x80020006u);

void icc_note() {
  static const bool noted = (host::note_unimplemented(
                                 "HOST VALUE machdep.icc.hw_info / hw_model: no ICC or board id; the kernel's board-id failure (ENXIO)"),
                             true);
  (void)noted;
}

void dipsw_note() {
  static const bool noted =
      (host::note_unimplemented("HOST VALUE /dev/dipsw: no such device; libSceDipsw answers its zeroed switch table"), true);
  (void)noted;
}

}  // namespace

SSN_PINVOKE("libSysBridgeKernel", "sceKernelGetHwSerialNumber", int32_t, ssn_SysBridgeKernel_GetHwSerialNumber,
            (uint8_t* serial)) {
  if (!serial) return kErrInval;
  icc_note();
  return kErrNxio;
}

SSN_PINVOKE("libSysBridgeKernel", "sceKernelGetHwModelName", int32_t, ssn_SysBridgeKernel_GetHwModelName, (uint8_t* model)) {
  if (!model) return kErrInval;
  icc_note();
  return kErrNxio;
}

SSN_PINVOKE("libSceVshKernelWrapper.a", "sceKernelIsDevelopmentMode", int32_t, ssn_VshKernelWrapper_IsDevelopmentMode, ()) {
  dipsw_note();
  return 0;  // dipsw switch 0
}

SSN_PINVOKE("libSceVshKernelWrapper.a", "sceKernelIsAssistMode", int32_t, ssn_VshKernelWrapper_IsAssistMode, ()) {
  dipsw_note();
  return 0;  // dipsw switch 2
}

}  // namespace ssn::sysbridge
