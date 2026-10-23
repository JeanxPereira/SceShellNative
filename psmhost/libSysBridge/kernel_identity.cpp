// Console-class queries of libkernel (FW 4.03) as ShellUI reaches them:
//   "libSysBridgeKernel" (libSceSysBridge {name, fn} table: sceKernelIsCEX 0x787d0,
//   sceKernelIsDevKit 0x787b0, sceKernelIsCronos 0x788a0) and "libSceVshKernelWrapper.a"
//   (eboot table: sceKernelIsCEX 0xcef590, sceKernelIsDevKit 0xcef570) are all the libkernel
//   imports of the same meaning:
//   sceKernelIsCEX    8aCOCGoRkUI (libkernel_sys 0x1e190)
//   sceKernelIsDevKit QNjGUdj1HPM (0x1df90)
//   sceKernelIsCronos nQ5AHlyJghQ (0x1e460)
// Each reads sysctl "machdep.idps" (16 bytes) and tests the u16 at +4 (`code`):
//   IsCEX:    1 when the sysctl fails, code == 0xA102, or byteswap(code) - 0x283 <= 12;
//             otherwise (code == 0xA802), or the second sysctl's error.
//   IsDevKit: the sysctl's error when it fails; else 1 for code 0xA202 / 0x8102, else 0.
//   IsCronos: 1 when code == 0xA802 (or the sysctl fails); else (sysctl fails or code
//             0x8102 / 0xA202) && sub_1E3C0(109) == 1.
//
// HOST VALUE (console identity): the PC has no IDPS. The host answers as a retail console:
// machdep.idps product code bytes {0x02, 0x84} (code 0x8402, byteswap 0x0284, inside the
// 0x283..0x28F range IsCEX accepts), the same "retail console" stance as libScePsm's
// development-mode keys. Which retail model's code is not chosen beyond that range.
// Results: IsCEX 1, IsDevKit 0, IsCronos 0. (Before, the managed stubs answered 0 for all
// three, i.e. a console that is neither CEX nor devkit.)
#include <cstdint>

#include "host/boundary.h"

namespace ssn::sysbridge {
namespace {

constexpr uint16_t kHostIdpsCode = 0x8402;  // bytes {0x02, 0x84} at machdep.idps +4

uint16_t swap16(uint16_t v) { return static_cast<uint16_t>((v << 8) | (v >> 8)); }

void host_note() {
  static const bool noted = (host::note_unimplemented(
                                 "HOST VALUE machdep.idps: no console identity; product code 0x8402 (retail CEX class)"),
                             true);
  (void)noted;
}

int32_t is_cex() {  // 0x1e190
  host_note();
  const uint16_t c = kHostIdpsCode;
  if (c == 0xA102 || static_cast<uint16_t>(swap16(c) - 0x283) <= 0xC) return 1;
  return c == 0xA802 ? 1 : 0;
}

int32_t is_devkit() {  // 0x1df90
  host_note();
  const uint16_t c = kHostIdpsCode;
  return (c == 0xA202 || c == 0x8102) ? 1 : 0;
}

int32_t is_cronos() {  // 0x1e460
  host_note();
  const uint16_t c = kHostIdpsCode;
  if (c == 0xA802) return 1;
  if (c == 0x8102 || c == 0xA202) {
    host::note_unimplemented("libkernel sceKernelIsCronos sub_1E3C0(109) not traced");
    return 0;
  }
  return 0;
}

// sceKernelUseCronosKura 7lZJYy0_evk (libkernel_sys 0x1e520): IsCronos() && dipsw(110) == 1.
// sceKernelUseCronosStreaming ind6aqzel+Y (0x1e550): IsCronos() && dipsw(111) == 1.
// (libSceSysBridge table 0x788b0 / 0x788c0 -> the imports.) The dipsw read (sub_1E3C0: ioctl
// 0xC0088803 on /dev/dipsw) is only reached on a Cronos console; with the host's retail code
// IsCronos is 0 and both answer 0.
int32_t use_cronos_dipsw() {
  if (!is_cronos()) return 0;
  host::note_unimplemented("libkernel /dev/dipsw read (sub_1E3C0) not traced");
  return 0;
}

}  // namespace

SSN_PINVOKE("libSysBridgeKernel", "sceKernelIsCEX", int32_t, ssn_SysBridgeKernel_IsCEX, ()) { return is_cex(); }
SSN_PINVOKE("libSysBridgeKernel", "sceKernelIsDevKit", int32_t, ssn_SysBridgeKernel_IsDevKit, ()) { return is_devkit(); }
SSN_PINVOKE("libSysBridgeKernel", "sceKernelIsCronos", int32_t, ssn_SysBridgeKernel_IsCronos, ()) { return is_cronos(); }
SSN_PINVOKE("libSysBridgeKernel", "sceKernelUseCronosKura", int32_t, ssn_SysBridgeKernel_UseCronosKura, ()) {
  return use_cronos_dipsw();
}
SSN_PINVOKE("libSysBridgeKernel", "sceKernelUseCronosStreaming", int32_t, ssn_SysBridgeKernel_UseCronosStreaming, ()) {
  return use_cronos_dipsw();
}
SSN_PINVOKE("libSceVshKernelWrapper.a", "sceKernelIsCEX", int32_t, ssn_VshKernelWrapper_IsCEX, ()) { return is_cex(); }
SSN_PINVOKE("libSceVshKernelWrapper.a", "sceKernelIsDevKit", int32_t, ssn_VshKernelWrapper_IsDevKit, ()) { return is_devkit(); }

}  // namespace ssn::sysbridge
