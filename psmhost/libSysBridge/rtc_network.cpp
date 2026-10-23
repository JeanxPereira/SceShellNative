// libSceRtc network clock and RFC 3339 formatting (FW 4.03) as ShellUI reaches them:
//   "libSysBridgeRtc" sceRtcGetCurrentNetworkTick  libSceSysBridge table 0x78bc0 -> zO9UL3qIINQ
//   "libSysBridgeRtc" sceRtcFormatRFC3339          table 0x78bf0 -> WJ3rqFwymew
//   "libReactNative.Modules.Vsh.sprx" RtcGetCurrentNetworkTickNative  export m7od1ikoZpQ
//       (0x4510): a thunk to zO9UL3qIINQ
//   Sce.Vsh.ShellUI.Settings.Rtc.RtcWrapper::RtcGetCurrentNetworkTickNative  eboot icall
//       0x4d8a70: a thunk to zO9UL3qIINQ
//
// sceRtcGetCurrentNetworkTick (libSceRtc 0x590): null -> 0x80B50002; clock_gettime(16)
// (lLMT9vJAck0); on failure errno 5 (EIO) -> 0x80B50001, any other errno -> errno +
// 0x80020000 (libkernel 0x16130); on success tick = sec * 1000000 + nsec / 1000 +
// 62135596800000000.
//
// Clock 16 in the kernel (kern_clock_gettime 0xFFFFFFFF808AA650 -> 0xFFFFFFFF80B39070, 7.00
// kernel): when no network time is held in memory (+128 of the RTC softc, set by a network
// time sync this session) the kernel reads the saved network-clock offset from the registry
// (key 0x051A0000 + (id << 16), 8-byte binary, 0xFFFFFFFF80B38E60; id 0 = DATE_rtc_net2,
// id 1 = DATE_rtc_net_dbg2): a registry failure ->
// ENXIO; the value 0x7FFFFFFFFFFFFFFF ("never synchronised", also what the clear path
// 0xFFFFFFFF80B38D70 stores, and the db0 default of all three keys) -> EIO. For a caller
// whose ucred+88 authority class is 1 the kernel first tries id 1 (0x051B0000), then id 0;
// the host reads id 0 (ShellUI's class is not in hand; both keys default to the sentinel).
//
// Nothing on the host synchronises network time (no network), so the in-memory time is never
// set. A non-sentinel saved offset (only the kernel's network-time set path writes these
// keys) would need the RTC base the kernel adds (+88/+96 of the softc, flag +104), which is not
// traced: that case is logged and answered as unsynchronised.
#include "libSysBridge/rtc_network.h"

#include <chrono>
#include <cstring>

#include "host/boundary.h"
#include "libSceRegMgr/regmgr.h"

namespace ssn::sysbridge {
namespace {

constexpr int32_t kErrNotInitialized = static_cast<int32_t>(0x80B50001u);  // EIO from clock 16
constexpr int32_t kErrInvalidPointer = static_cast<int32_t>(0x80B50002u);
constexpr int32_t kErrInvalidValue = static_cast<int32_t>(0x80B50003u);
constexpr int32_t kErrInvalidYear = static_cast<int32_t>(0x80B50008u);
constexpr uint64_t kNeverSynced = 0x7FFFFFFFFFFFFFFFull;
constexpr int32_t kEnxio = 6;

// Days since 0001-01-01 -> proleptic Gregorian date (sceRtcSetTick ueega6v3GUw).
void civil_from_days(int64_t z, int64_t& y, unsigned& m, unsigned& d) {
  z += 306;
  const int64_t era = (z >= 0 ? z : z - 146096) / 146097;
  const unsigned doe = static_cast<unsigned>(z - era * 146097);
  const unsigned yoe = (doe - doe / 1460 + doe / 36524 - doe / 146096) / 365;
  y = static_cast<int64_t>(yoe) + era * 400;
  const unsigned doy = doe - (365 * yoe + yoe / 4 - yoe / 100);
  const unsigned mp = (5 * doy + 2) / 153;
  d = doy - (153 * mp + 2) / 5 + 1;
  m = mp < 10 ? mp + 3 : mp - 9;
  y += (m <= 2);
}

}  // namespace

int32_t rtc_get_current_network_tick(uint64_t* tick) {
  if (!tick) return kErrInvalidPointer;
  uint64_t offset = 0;
  const int32_t r = regmgr::get_bin(0x051A0000u, &offset, sizeof offset);
  if (r != 0) return static_cast<int32_t>(0x80020000u + kEnxio);
  if (offset != kNeverSynced)
    host::note_unimplemented("kernel network clock with a saved offset (RTC softc base +88/+96) not traced; answered as unsynchronised");
  return kErrNotInitialized;
}

SSN_PINVOKE("libSysBridgeRtc", "sceRtcGetCurrentNetworkTick", int32_t, ssn_SysBridgeRtc_GetCurrentNetworkTick, (uint64_t* tick)) {
  return rtc_get_current_network_tick(tick);
}

int32_t RtcWrapper_RtcGetCurrentNetworkTickNative(uint64_t* tick) { return rtc_get_current_network_tick(tick); }
SSN_ICALL("Sce.Vsh.ShellUI.Settings.Rtc.RtcWrapper::RtcGetCurrentNetworkTickNative", RtcWrapper_RtcGetCurrentNetworkTickNative);

// sceRtcFormatRFC3339(char* out, const SceRtcTick* utc, int tzMinutes) — WJ3rqFwymew
// (libSceRtc 0x2010) = lja0nNPWojg(out, utc, tz, 2) (0x1b20, two fraction digits):
//   out null -> 0x80B50002; utc null -> the current tick (18B2NS1y9UU);
//   tz outside -1439..1439 -> 0x80B50003;
//   tick += tz minutes: dword_C014 is the caller's compiled SDK version (sub_D0:
//     sceKernelGetCompiledSdkVersion); ShellUI's (4.03) is >= 0x3000000, so the
//     overflow-checked add 0x3440 (-> 0x80B50003 on overflow) rather than mn-tf4QiFzk 0x3420;
//   sceRtcSetTick -> date; sceRtcCheckValid (lPEBYdVX0XQ 0x24e0: year 1..9999 -> else 0x80B50008);
//   "YYYY-MM-DDTHH:MM:SS.ff" (ff = microsecond / 10000), then "Z" for tz 0 or "+HH:MM" /
//   "-HH:MM", NUL-terminated; 0.
SSN_PINVOKE("libSysBridgeRtc", "sceRtcFormatRFC3339", int32_t, ssn_SysBridgeRtc_FormatRFC3339,
            (char* out, const uint64_t* utc, int32_t tz)) {
  if (!out) return kErrInvalidPointer;
  uint64_t tick;
  if (utc) {
    std::memcpy(&tick, utc, 8);
  } else {  // 18B2NS1y9UU (0x1d0): clock_gettime(CLOCK_REALTIME) as a tick
    const auto now = std::chrono::system_clock::now().time_since_epoch();
    tick = static_cast<uint64_t>(std::chrono::duration_cast<std::chrono::microseconds>(now).count()) + 62135596800000000ull;
  }
  if (static_cast<uint32_t>(tz + 1439) > 0xB3E) return kErrInvalidValue;
  const int64_t add = 60000000LL * tz;
  if (add < 0) {
    if (tick < static_cast<uint64_t>(-add)) return kErrInvalidValue;
  } else if (tick + static_cast<uint64_t>(add) < tick) {
    return kErrInvalidValue;
  }
  tick += static_cast<uint64_t>(add);
  int64_t y;
  unsigned mo, d;
  civil_from_days(static_cast<int64_t>(tick / 86400000000ull), y, mo, d);
  const uint64_t rem = tick % 86400000000ull;
  const unsigned h = static_cast<unsigned>(rem / 3600000000ull), mi = static_cast<unsigned>(rem / 60000000ull % 60),
                 s = static_cast<unsigned>(rem / 1000000ull % 60), us = static_cast<unsigned>(rem % 1000000ull);
  const uint16_t year = static_cast<uint16_t>(y);
  if (static_cast<uint16_t>(year - 1) > 0x270E) return kErrInvalidYear;
  auto two = [](char* p, unsigned v) {
    p[0] = static_cast<char>('0' + v / 10 % 10);
    p[1] = static_cast<char>('0' + v % 10);
  };
  out[0] = static_cast<char>('0' + year / 1000 % 10);
  out[1] = static_cast<char>('0' + year / 100 % 10);
  out[2] = static_cast<char>('0' + year / 10 % 10);
  out[3] = static_cast<char>('0' + year % 10);
  out[4] = '-';
  two(out + 5, mo);
  out[7] = '-';
  two(out + 8, d);
  out[10] = 'T';
  two(out + 11, h);
  out[13] = ':';
  two(out + 14, mi);
  out[16] = ':';
  two(out + 17, s);
  out[19] = '.';
  two(out + 20, us / 10000);
  char* p = out + 22;
  if (tz == 0) {
    *p++ = 'Z';
  } else {
    unsigned a = static_cast<unsigned>(tz < 0 ? -tz : tz);
    *p++ = tz < 0 ? '-' : '+';
    p[0] = static_cast<char>('0' + a / 600 % 10);
    p[1] = static_cast<char>('0' + a / 60 % 10);
    p[2] = ':';
    p[3] = static_cast<char>('0' + a % 60 / 10);
    p[4] = static_cast<char>('0' + a % 60 % 10);
    p += 5;
  }
  *p = '\0';
  return 0;
}

}  // namespace ssn::sysbridge
