// "libSysBridgeRtc" sceRtcGetCurrentClockLocalTime (FW 4.03), as Settings > System > Date and
// Time and other pages read the console's local clock.
//
// libSceSysBridge {name, fn} table entry 0x78bb0 -> the libSceRtc import ZPD1YOKI-Kw
// (libSceRtc 0x320):
//   null -> 0x80B50002;
//   clock_gettime(CLOCK_REALTIME) (QBi7HCK03hw) -> tick = sec * 1000000 + nsec / 1000 +
//     62135596800000000 (microseconds since 0001-01-01);
//   sceKernelConvertUtcToLocaltime (o5uEDpN-oY, libkernel_sys 0x164a0: gettimeofday's
//     struct timezone {minutes west, dst minutes}) -> tick += (tz + dst) minutes
//     (sceRtcTickAddMinutes mn-tf4QiFzk);
//   sceRtcSetTick (ueega6v3GUw) -> SceRtcDateTime {u16 year, month, day, hour, minute,
//     second; u32 microsecond}; result 0.
// The managed stub used to leave the structure zeroed, which the pages showed as 12/30/1899.
//
// HOST VALUE (the kernel's timezone state): ShellCore's path that loads the console time
// zone (registry DATE_time_zone / DATE_summer_time) into the kernel is not traced. The host
// kernel's timezone is the PC's current UTC offset — the same local time the desktop BCL
// gives the shell's managed clock — so the Settings pages agree with the home-screen clock.
#include <chrono>
#include <cstdint>
#include <cstring>
#include <ctime>

#include "host/boundary.h"

namespace ssn::sysbridge {
namespace {

constexpr int32_t kErrInvalidPointer = static_cast<int32_t>(0x80B50002u);
constexpr int64_t kEpochTick = 62135596800000000LL;  // 1970-01-01 in microseconds since 0001-01-01

// Days since 0001-01-01 -> civil date (proleptic Gregorian), as sceRtcSetTick computes it.
void civil_from_days(int64_t z, int64_t& y, unsigned& m, unsigned& d) {
  z += 306;  // shift so the era starts 0000-03-01 (day 0 = 0001-01-01 is day 306 of year 0)
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

int64_t host_utc_offset_minutes() {
  static const bool noted = (host::note_unimplemented(
                                 "HOST VALUE kernel timezone (gettimeofday tz): the PC's UTC offset; ShellCore's time-zone load not traced"),
                             true);
  (void)noted;
  const std::time_t now = std::time(nullptr);
  std::tm local{}, utc{};
  localtime_s(&local, &now);
  gmtime_s(&utc, &now);
  utc.tm_isdst = local.tm_isdst;  // compare like with like; DST is part of the local offset
  return static_cast<int64_t>(std::difftime(std::mktime(&local), std::mktime(&utc))) / 60;
}

}  // namespace

SSN_PINVOKE("libSysBridgeRtc", "sceRtcGetCurrentClockLocalTime", int32_t, ssn_SysBridgeRtc_GetCurrentClockLocalTime,
            (uint8_t* dt)) {
  if (!dt) return kErrInvalidPointer;
  const auto now = std::chrono::system_clock::now().time_since_epoch();
  int64_t tick = std::chrono::duration_cast<std::chrono::microseconds>(now).count() + kEpochTick;
  tick += host_utc_offset_minutes() * 60 * 1000000LL;
  const int64_t days = tick / 86400000000LL;
  int64_t rem = tick % 86400000000LL;
  int64_t y;
  unsigned m, d;
  civil_from_days(days, y, m, d);
  const uint16_t f[6] = {static_cast<uint16_t>(y), static_cast<uint16_t>(m), static_cast<uint16_t>(d),
                         static_cast<uint16_t>(rem / 3600000000LL), static_cast<uint16_t>(rem / 60000000LL % 60),
                         static_cast<uint16_t>(rem / 1000000LL % 60)};
  const uint32_t us = static_cast<uint32_t>(rem % 1000000LL);
  std::memcpy(dt, f, 12);
  std::memcpy(dt + 12, &us, 4);
  return 0;
}

}  // namespace ssn::sysbridge
