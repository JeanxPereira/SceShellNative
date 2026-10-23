// NpPbtcLib (eboot.bin, FW 4.03): the play-time-control client the ShellUI eboot links and
// publishes to managed code as "NpPbtcApi" ({name, fn} table 0xcf0cb0: sceNpPbtcStart
// 0x4C25D0, Stop 0x4C25E0, GetDisplayInfo 0x4C25F0, ConvertToIndicatorInfo 0x4C2610,
// ConvertToDialogInfo 0x4C2630, IsLoginAllowed 0x4C2650, IsTarget 0x4C2670). One object
// (unk_EE2E08) holds 20 per-user entries of 40 bytes (+8), the "SceNpPbtcLibMtx" mutex
// (+808), the event flag / "SceNpPbtcLibThr" thread (+816..+848), +856 initialised, +857 UT.
//
// The table is filled from SceShellCore's NpPbtc manager (bTmtBchzFps, a SceShellCoreUtil
// call into SceShellCore's NpPbtcUserMgr 0x900EB0), which is not reproduced: the host table
// keeps its initial entries (user -1), so every user gets the defaults.
#include <cstdint>
#include <cstring>
#include <mutex>

#include "host/boundary.h"

namespace ssn::eboot {
namespace {

constexpr int32_t kErrNotInitialized = static_cast<int32_t>(0x8055BC01u);
constexpr int32_t kErrInvalidArgument = static_cast<int32_t>(0x8055BC02u);

// One entry (40 bytes, 0x4C2C70 initialiser): +0 userId, +4 flag, +8 qword, +16 day settings
// 0xFFFF05A00000FFFF, +24 0x05A00000, +32 qword.
struct Entry {
  int32_t user_id = -1;
  uint8_t raw[36] = {};
};

struct Pbtc {
  std::mutex mu;          // +808 "SceNpPbtcLibMtx"
  Entry entries[20];      // +8
  bool initialized = false;  // +856
  bool ut = false;           // +857
};

Pbtc& pbtc() {
  static Pbtc p;
  return p;
}

#pragma pack(push, 1)
// Sce.Vsh.Np.Pbtc.SceNpPbtcDisplayInfo (24 bytes).
struct DisplayInfo {
  uint8_t is_limited, block_mode, pad_[2];
  uint32_t login_second, current_second;
  uint16_t today[3], tomorrow[3];  // playableDurationMinute, playableHoursStart/EndMinute
};
#pragma pack(pop)
static_assert(sizeof(DisplayInfo) == 24);

// sub_4C3000 defaults: zero, today / tomorrow = {-1 (unset), 0, 1440}.
void default_display_info(DisplayInfo* d) {
  std::memset(d, 0, sizeof *d);
  d->today[0] = d->tomorrow[0] = 0xFFFF;
  d->today[2] = d->tomorrow[2] = 1440;
}

// 0x4C32E0: the entry of a user (under +808), copied out; found = false when none.
bool find_entry(int32_t user, Entry* out) {
  Pbtc& p = pbtc();
  std::lock_guard<std::mutex> l(p.mu);
  for (const Entry& e : p.entries)
    if (e.user_id == user) {
      *out = e;
      return true;
    }
  return false;
}

// GetDisplayInfo body (0x4C3000).
int32_t get_display_info(int32_t user, DisplayInfo* d) {
  if (!pbtc().initialized) return kErrNotInitialized;
  if (user == -1 || !d) return kErrInvalidArgument;
  default_display_info(d);
  Entry e;
  if (!find_entry(user, &e)) return 0;
  // A found entry with its limit flag set makes 0x4C3000 read the RTC (Ot1DE3gif84,
  // mn_tf4QiFzk with the entry's time-zone offset, ueega6v3GUw) to place login and current
  // seconds in the user's day. The host table holds no entries, so this is never reached.
  host::note_unimplemented("eboot NpPbtcLib GetDisplayInfo for a user with a play-time entry (0x4C3000 RTC path) not reproduced");
  return 0;
}

bool valid(const DisplayInfo* d) {
  return d->login_second <= 0x15180 && d->current_second <= 0x15180 && uint16_t(d->today[0] + 1) <= 0x5A1u &&
         d->today[1] <= 0x5A0 && d->today[2] <= 0x5A0 && uint16_t(d->tomorrow[0] + 1) <= 0x5A1u && d->tomorrow[1] <= 0x5A0 &&
         d->tomorrow[2] <= 0x5A0;
}

uint32_t sat_sub(uint32_t a, uint32_t b) { return a < b ? 0 : a - b; }

}  // namespace

// sceNpPbtcStart (0x4C25D0 -> 0x4C2EC0): initialised -> (not UT) PSM event 252 receiver
// 0x4C2E90 (m3YkcPqYBzQ), wjn0_e9fz3A(0) (SceShellCoreUtil, libSceSystemService 0x14470 ->
// IPC 0x18540), then the table refresh 0x4C2920 (bTmtBchzFps into a 20-entry list, copied
// under +808, PSM event 253 posted). Not initialised -> 0x8055BC01.
SSN_PINVOKE("NpPbtcApi", "sceNpPbtcStart", int32_t, ssn_NpPbtc_Start, ()) {
  if (!pbtc().initialized) return kErrNotInitialized;
  host::note_unimplemented(
      "eboot NpPbtcLib Start/Stop: SceShellCoreUtil wjn0_e9fz3A and the SceShellCore NpPbtc user list (bTmtBchzFps) not "
      "reproduced; table unchanged, PSM events 252/253 not used");
  return 0;
}

// sceNpPbtcStop (0x4C25E0 -> 0x4C2F20): not initialised -> 0x8055BC01; wjn0_e9fz3A(1) and the
// refresh 0x4C2920; 0.
SSN_PINVOKE("NpPbtcApi", "sceNpPbtcStop", int32_t, ssn_NpPbtc_Stop, ()) {
  if (!pbtc().initialized) return kErrNotInitialized;
  host::note_unimplemented(
      "eboot NpPbtcLib Start/Stop: SceShellCoreUtil wjn0_e9fz3A and the SceShellCore NpPbtc user list (bTmtBchzFps) not "
      "reproduced; table unchanged, PSM events 252/253 not used");
  return 0;
}

// sceNpPbtcGetDisplayInfo(userId, ref SceNpPbtcDisplayInfo) — 0x4C25F0 -> 0x4C3000.
SSN_PINVOKE("NpPbtcApi", "sceNpPbtcGetDisplayInfo", int32_t, ssn_NpPbtc_GetDisplayInfo, (int32_t user, DisplayInfo* info)) {
  return get_display_info(user, info);
}

// sceNpPbtcConvertToIndicatorInfo(ref DisplayInfo, ref SceNpPbtcIndicatorInfo) — 0x4C2610.
// IndicatorInfo: +0 isLimited, +1 blockMode, +4 state, +8 remainingSecond, +12 playableSecond.
SSN_PINVOKE("NpPbtcApi", "sceNpPbtcConvertToIndicatorInfo", int32_t, ssn_NpPbtc_ConvertToIndicatorInfo,
            (const DisplayInfo* d, uint8_t* o)) {
  if (!d || !o || !valid(d)) return kErrInvalidArgument;
  const uint16_t dur = d->today[0];
  const uint32_t start = d->today[1], end = d->today[2], login = d->login_second, cur = d->current_second;
  o[0] = d->is_limited;
  o[1] = d->block_mode;
  int32_t state;
  if (!d->is_limited) state = 0;  // UNLIMITED
  else if (dur == 0xFFFF && start == 0 && end == 1440) state = 4;  // UNLIMITED_TODAY
  else state = cur < 60 * start ? 2 : (cur >= 60 * end ? 3 : 1);   // BEFORE_START / AFTER_END / IN_PLAYABLE_HOURS
  std::memcpy(o + 4, &state, 4);
  const uint32_t hours = sat_sub(60 * end, 60 * start);
  uint32_t remaining = sat_sub(60 * end, cur);
  if (hours < remaining) remaining = hours;
  if (dur != 0xFFFF) {
    const uint32_t left = sat_sub(60u * dur, login);
    if (remaining > left) remaining = left;
  }
  std::memcpy(o + 8, &remaining, 4);
  const uint32_t limit = 60u * ((dur & 0x8000) ? 1440u : dur);
  const uint32_t playable = limit < hours ? limit : hours;
  std::memcpy(o + 12, &playable, 4);
  return 0;
}

// sceNpPbtcConvertToDialogInfo(ref DisplayInfo, ref SceNpPbtcDialogInfo) — 0x4C2630.
// DialogInfo: +0 isLimited, +1 blockMode, +2 isPlayableDurationOver, +4 state, +8
// loginSecond, +12 today, +18 tomorrow.
SSN_PINVOKE("NpPbtcApi", "sceNpPbtcConvertToDialogInfo", int32_t, ssn_NpPbtc_ConvertToDialogInfo,
            (const DisplayInfo* d, uint8_t* o)) {
  if (!d || !o || !valid(d)) return kErrInvalidArgument;
  const uint16_t dur = d->today[0];
  const uint32_t start = d->today[1], end = d->today[2], login = d->login_second, cur = d->current_second;
  o[0] = d->is_limited;
  o[1] = d->block_mode;
  o[2] = dur == 0xFFFF ? 0 : (60u * dur <= login ? 1 : 0);
  int32_t state;
  if (!d->is_limited) state = 0;
  else if (dur == 0xFFFF && start == 0 && end == 1440) state = 4;
  else state = cur < 60 * start ? 2 : (cur >= 60 * end ? 3 : 1);
  std::memcpy(o + 4, &state, 4);
  std::memcpy(o + 8, &d->login_second, 4);
  std::memcpy(o + 12, d->today, 6);
  std::memcpy(o + 18, d->tomorrow, 6);
  return 0;
}

// sceNpPbtcIsLoginAllowed(userId, ref bool) — 0x4C2650 -> 0x4C37C0.
SSN_PINVOKE("NpPbtcApi", "sceNpPbtcIsLoginAllowed", int32_t, ssn_NpPbtc_IsLoginAllowed, (int32_t user, uint8_t* allowed)) {
  if (!pbtc().initialized) return kErrNotInitialized;
  if (user == -1 || !allowed) return kErrInvalidArgument;
  *allowed = 1;
  DisplayInfo d;
  default_display_info(&d);
  if (const int32_t r = get_display_info(user, &d); r < 0) return r;
  if (!d.is_limited || !d.block_mode) return 0;
  const uint16_t dur = d.today[0];
  const uint32_t from = 60u * d.today[1], to = 60u * d.today[2], cur = d.current_second;
  uint32_t left = sat_sub(to, cur);
  const uint32_t hours = sat_sub(to, from);
  if (hours < left) left = hours;
  if (dur != 0xFFFF) {
    const uint32_t budget = sat_sub(60u * ((dur & 0x8000) ? 1440u : dur), d.login_second);
    if (left > budget) left = budget;
  } else if (d.today[1] == 0 && d.today[2] == 1440) {
    return 0;
  }
  *allowed = cur >= from && cur < to && left != 0;
  return 0;
}

// sceNpPbtcIsTarget(userId, ref bool) — 0x4C2670 -> 0x4C3900: the found entry's limit flag
// (entry +12), false when the user has no entry.
SSN_PINVOKE("NpPbtcApi", "sceNpPbtcIsTarget", int32_t, ssn_NpPbtc_IsTarget, (int32_t user, uint8_t* target)) {
  if (!pbtc().initialized) return kErrNotInitialized;
  if (user == -1 || !target) return kErrInvalidArgument;
  *target = 0;
  Entry e;
  if (find_entry(user, &e)) *target = e.raw[8];
  return 0;
}

// The Np initialisation of BackgroundInit(InitNetwork) (sub_A790) ends with the library
// init 0x4C2590 -> 0x4C2C70(obj, UT = 0): the 20 entries reset (user -1), the mutex
// "SceNpPbtcLibMtx", the event flag "SceNpPbtcLibEvf" and thread "SceNpPbtcLibThr" (0x4C26C0;
// the thread refreshes the table on bit 2, set by the PSM event 252 receiver), then +856 = 1.
void np_pbtc_initialize() {
  Pbtc& p = pbtc();
  std::lock_guard<std::mutex> l(p.mu);
  if (p.initialized) return;
  for (Entry& e : p.entries) e = Entry{};
  p.ut = false;
  p.initialized = true;
  host::note_unimplemented("eboot NpPbtcLib event flag / SceNpPbtcLibThr refresh thread (0x4C26C0 / 0x4C2800) not created on the host");
}

}  // namespace ssn::eboot
