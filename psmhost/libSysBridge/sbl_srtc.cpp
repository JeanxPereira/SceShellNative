// "libSysBridgeSbl" sceSblSrtcGetCurrentSecureTick (FW 4.03), reached by the RN SrtcUtil
// ("currentSecureTime").
//
// libSceSysBridge table 0x78c60 -> its own sub_FC60:
//   null -> 0x800F050E;
//   open("/dev/srtc", O_RDWR) (wuCroIGjt2g); failure -> 0x800F0513, tick untouched;
//   ioctl(fd, 0x40105305, {sec, ..., status}): 0 -> result = status, tick = sec * 1000000 +
//     63460972800000000; ioctl failure -> 0x800F0405 with tick = 63460972800000000; close.
//
// The host has no secure RTC device (/dev/srtc), the same stance as libScePsm's
// SystemParameters key 19 (IsSecureClockSynced): the open fails, so the answer is 0x800F0513
// and the RN module reports the error instead of a time.
#include <cstdint>

#include "host/boundary.h"

namespace ssn::sysbridge {

SSN_PINVOKE("libSysBridgeSbl", "sceSblSrtcGetCurrentSecureTick", int32_t, ssn_SysBridgeSbl_SrtcGetCurrentSecureTick, (uint64_t* tick)) {
  if (!tick) return static_cast<int32_t>(0x800F050Eu);
  static const bool noted = (host::note_unimplemented("HOST VALUE no /dev/srtc (secure RTC): sceSblSrtcGetCurrentSecureTick -> 0x800F0513"), true);
  (void)noted;
  return static_cast<int32_t>(0x800F0513u);
}

}  // namespace ssn::sysbridge
