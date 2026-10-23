// LoginMgrProxy (eboot.bin) — device list P/Invokes of Sce.Vsh.ShellUI.LoginMgrProxy2.NativeCall
// ({name, fn} table at 0xcd3b60 for "_GetDeviceInfoList"), over the mbus model
// (psmhost/libSceMbus). Reproduced from the eboot's mbus_utility.cpp / device classes.
#include <cstdint>
#include <cstring>
#include <vector>

#include "eboot/login_mgr.h"
#include "host/boundary.h"
#include "libSceMbus/mbus.h"

namespace ssn::eboot {
namespace {

template <class T>
T rd(const uint8_t* p, size_t off) {
  T v;
  std::memcpy(&v, p + off, sizeof v);
  return v;
}
template <class T>
void wr(uint8_t* p, size_t off, T v) {
  std::memcpy(p + off, &v, sizeof v);
}

constexpr size_t kInfoSize = 352;            // DeviceInfoFull
constexpr size_t kListCountOffset = 0x16000;  // DeviceInfoList.num (256 x 352)

// Sce.Vsh.ShellUI.LoginMgrProxy.DeviceInfoFullParam { int userId; long nativeDeviceType2; }.
struct DeviceInfoFullParam {
  int32_t user_id;
  int64_t native_device_type2;
};

// GetDeviceList (0x20c4a0): one sceMbusGetDeviceInfoByCondition(cond, 0, recs, 16) query;
// records with +17 == 1 (unless type 15) are skipped; with `assigned_only` (a8) a record
// needs an owner > 0 and != 255; with a function filter (a4/a5, unused by the callers
// here) the record's functions are compared. Returns the number copied into `out`
// (0x80B60004 "Not enough buffer" past `max`), or the mbus error.
int32_t get_device_list(const uint8_t* cond, uint8_t* out, int32_t max) {
  if (!out || max > 256) return static_cast<int32_t>(0x80B60003u);
  std::vector<uint8_t> recs(16 * mbus::kRecordSize);
  int32_t total = 0, gen = 0;
  const int32_t n = mbus::get_device_info_by_condition(cond, 0, recs.data(), 16, &total, &gen, mbus::kRecordSize);
  if (n < 0) return n;  // "sceMbusGetDeviceInfoByCondition : %d(0x%08x)"
  int32_t got = 0;
  for (int32_t i = 0; i < n && i < 16; ++i) {
    const uint8_t* r = recs.data() + size_t(i) * mbus::kRecordSize;
    if (r[17] == 1 && rd<int32_t>(r, 12) != 15) continue;
    if (got >= max) return static_cast<int32_t>(0x80B60004u);  // "Not enough buffer"
    std::memcpy(out + size_t(got++) * mbus::kRecordSize, r, mbus::kRecordSize);
  }
  return got;
}

// The device classes the factory 0x1ce9a0 builds per record type. The host bus only ever
// holds the DualSense (type 22, bus != 4 -> class 0x1e74d0, vtable 0xcd77d8); any other
// record is reported once and left out.
struct Device {
  const uint8_t* rec;  // object +8: the 256-byte record
  int64_t type2() const { return 22 - (rd<int32_t>(rec, 8) == 0 ? 1 : 0); }  // 0x1e76a0 (vtable +528), 0x1e7710
  int64_t subtype() const { return 3 * (type2() != 1) - 1; }                  // 0x1e76c0 (+536)
  bool disconnectable() const { return true; }                                 // 0x1e7700 (+224)
};
bool make_device(const uint8_t* rec, Device* d) {
  if (rd<int32_t>(rec, 12) == 22 && rd<int32_t>(rec, 8) != 4) {
    d->rec = rec;
    return true;
  }
  host::note_unimplemented("LoginMgrProxy device class for mbus type " + std::to_string(rd<int32_t>(rec, 12)) + " bus " +
                           std::to_string(rd<int32_t>(rec, 8)));
  return false;
}

// 0x1d4620: function bits of the record's functions (enabled: state 0 only; builtin: all).
uint64_t function_bits(const uint8_t* rec, bool builtin) {
  const int32_t n = rd<int32_t>(rec, 20);
  uint64_t r = 0;
  for (int32_t i = 0; i < n && i <= 10; ++i) {
    if (rd<int32_t>(rec, 32 + 16 * i) != 0 && !builtin) continue;
    switch (rd<int32_t>(rec, 24 + 16 * i)) {
      case 0: case 0xB: r |= 1; break;
      case 1: r |= 4; break;
      case 2: r |= 8; break;
      case 4: r |= 0x400; break;
      case 5: r |= 0x40; break;
      case 6: r |= 2; break;
      case 7: r |= 0x10; break;
      case 8: r |= 0x20; break;
      case 9: r |= 0x200; break;
      case 0xC: r |= 0x80; break;
      case 0xD: case 0x15: r |= 0x100; break;
      case 0x12: r |= 0x1000; break;
      case 0x13: r |= 0x2000; break;
      case 0x14: r |= 0x4000; break;
      case 0x18: r |= 0x8000; break;
      default: break;
    }
  }
  return r;
}

// 0x1f8f80 / 0x1f92c0 for a record whose charge state (+216) is not 6 ("waiting"): battery
// state / level / status. The host's records carry state 5 -> -1 / -1 / -1.
void battery(const uint8_t* rec, int32_t* state, int32_t* level, int32_t* status) {
  const uint32_t cs = rd<uint32_t>(rec, 216), lv = rd<uint32_t>(rec, 220);
  const int32_t l = lv < 0x65 ? int32_t(lv) : -1;
  *level = -1;
  switch (cs) {
    case 0: *state = -(l == -1); *level = l; break;
    case 1: *state = 1; host::note_unimplemented("LoginMgrProxy battery level gate 0x1f8e90"); break;
    case 2: *state = 2; break;
    case 5: *state = -1; *level = -1; break;
    default: *state = 3; break;
  }
  switch (cs) {
    case 0: *status = lv < 6 ? 0 : lv - 6 < 0x23 ? 1 : lv - 41 < 0x1E ? 2 : 4 * (lv - 71 < 0x1E) - 1; break;
    case 1: *status = 4; break;
    case 2: *status = 3; break;
    case 4: *status = 5; break;
    default: *status = -1; break;
  }
}

// 0x1d3fa0: DeviceInfoFull (352 bytes) of a device object.
void fill_info(const Device& d, uint8_t* o) {
  const uint8_t* r = d.rec;
  std::memset(o, 0, kInfoSize);
  wr<uint64_t>(o, 0, rd<uint64_t>(r, 0));
  wr<int32_t>(o, 16, rd<int32_t>(r, 8));
  const int64_t type2 = d.type2();
  wr<int64_t>(o, 288, type2);
  wr<int64_t>(o, 296, d.subtype());
  wr<uint64_t>(o, 304, function_bits(r, false));
  wr<uint64_t>(o, 312, function_bits(r, true));
  const int32_t type = rd<int32_t>(r, 12);
  wr<int32_t>(o, 8, type != 18 ? rd<int32_t>(r, 200) : -1);
  const uint64_t bus_type = rd<uint64_t>(r, 8);
  if (rd<uint32_t>(r, 8) < 2u || bus_type == 0x100000004ull) {
    wr<uint16_t>(o, 12, rd<uint16_t>(r, 228));  // vtable +568 -> object +236
    wr<uint16_t>(o, 14, rd<uint16_t>(r, 230));  // vtable +576 -> object +238
  }
  const bool pad = type == 2 || type == 22;  // 0x20cc40
  uint64_t bt = 0;
  if (pad || (type <= 0x18 && ((0x1845008u >> type) & 1)) ||
      (rd<int32_t>(r, 8) == 1 && type <= 0x1A && ((67117440u >> type) & 1)))
    bt = rd<uint64_t>(r, 208);
  wr<uint64_t>(o, 280, bt);
  if (pad || (type <= 0x18 && ((0x1805808u >> type) & 1))) {
    int32_t st, lv, status;
    battery(r, &st, &lv, &status);
    wr<int32_t>(o, 320, st);
    wr<int32_t>(o, 324, lv);
    wr<int32_t>(o, 328, status);
  } else {
    wr<int64_t>(o, 320, -1);
    wr<int32_t>(o, 328, -1);
  }
  if (type == 15 || type == 11) {
    host::note_unimplemented("LoginMgrProxy DeviceInfoFull audio fields for mbus type " + std::to_string(type));
  } else {
    wr<int64_t>(o, 332, -1);
    wr<int32_t>(o, 340, -1);
    wr<int32_t>(o, 348, 0);
  }
  // Product name (sub_20d870) only for DeviceType2 15/17/18, or for non-USB-composite
  // devices with a function in 0xC and type2 in {1..8} \ {0,3}: not the host's DualSense.
  const uint64_t builtin = rd<uint64_t>(o, 312);
  if ((type2 >= 0 && type2 <= 0x12 && ((0x68000ull >> type2) & 1)) ||
      (bus_type != 0x100000004ull && (builtin & 0xC) != 0 && type2 >= 0 && type2 <= 8 && ((502ull >> type2) & 1)))
    host::note_unimplemented("LoginMgrProxy product name (sub_20d870)");
}

// The HDMI pseudo device appended to the all-devices list (0x1ce820(-2) -> 0x1d3fa0's
// type-14 branch): deviceId -2, owner 255, connection 2 (HDMI), type2 14
// (TVorAVAmplifier), subtype -1, functions 4, battery/audio fields -1.
void fill_hdmi(uint8_t* o) {
  std::memset(o, 0, kInfoSize);
  wr<int64_t>(o, 0, -2);
  wr<int64_t>(o, 8, 255);
  wr<int32_t>(o, 16, 2);
  wr<uint64_t>(o, 280, 0);  // ymmword 0xb8d1a0
  wr<int64_t>(o, 288, 14);
  wr<int64_t>(o, 296, -1);
  wr<uint64_t>(o, 304, 4);
  wr<uint64_t>(o, 312, 4);
  std::memset(o + 320, 0xFF, 16);
  wr<int64_t>(o, 336, -1);
  wr<int64_t>(o, 344, 0);
}

}  // namespace

uint64_t device_function_bits(const uint8_t* rec, bool builtin) { return function_bits(rec, builtin); }

// int _GetDeviceInfoList(DeviceInfoListType type, DeviceInfoFullParam param, ref DeviceInfoList list)
// — 0x1c4fa0 -> 0x20ec90 (mbus_utility.cpp GetDeviceInfoList). null list -> 0x80B60003.
// The condition is libSceMbus's default (+24..+184 function types -1) with the owner
// (+200) = param.userId for every type but AllDevice. Each record (deviceId != -1, bus
// != 5, bus 2 only for type 13) becomes a device object; AllDevice/UserDevice/
// SpecificDeviceType (type2 == param.nativeDeviceType2) take every device;
// UserBtDevice/DisconnectableDevice take VID 1356 / PID 1071 devices, and otherwise
// Disconnectable takes types 15/28 or disconnectable devices, and both take BT (bus 1)
// devices. DisconnectableDevice then adds the BT HID devices (type 18, up to 4); the
// all-devices list ends with the HDMI pseudo device.
SSN_PINVOKE("LoginMgrProxy", "_GetDeviceInfoList", int32_t, ssn_LoginMgrProxy_GetDeviceInfoList,
            (int32_t type, DeviceInfoFullParam param, uint8_t* list)) {
  if (!list) return static_cast<int32_t>(0x80B60003u);
  wr<int32_t>(list, kListCountOffset, 0);
  uint8_t cond[256];
  mbus::init_condition(cond, sizeof cond);
  for (int i = 0; i < 11; ++i) wr<int32_t>(cond, 24 + 16 * i, -1);
  if (type != 0) wr<int32_t>(cond, 200, param.user_id);
  std::vector<uint8_t> recs(256 * mbus::kRecordSize);
  for (size_t i = 0; i < 256; ++i) {
    uint8_t* r = recs.data() + i * mbus::kRecordSize;
    mbus::init_condition(r, mbus::kRecordSize);
    for (int k = 0; k < 11; ++k) wr<int32_t>(r, 24 + 16 * k, -1);
  }
  const int32_t n = get_device_list(cond, recs.data(), 256);
  uint32_t count = 0;
  for (int32_t i = 0; i < n && i < 256; ++i) {
    const uint8_t* r = recs.data() + size_t(i) * mbus::kRecordSize;
    if (rd<int64_t>(r, 0) == -1) continue;
    const int32_t bus = rd<int32_t>(r, 8), rtype = rd<int32_t>(r, 12);
    if (bus == 5 || (bus == 2 && rtype != 13)) continue;
    Device d;
    if (!make_device(r, &d)) continue;
    bool take;
    if (type == 4) {
      take = param.native_device_type2 == d.type2();
    } else if ((type & ~1) != 2 || (rd<uint16_t>(r, 228) == 1356 && rd<uint16_t>(r, 230) == 1071)) {
      take = true;
    } else if (type == 3 && (rtype == 15 || rtype == 28 || d.disconnectable())) {
      take = true;
    } else {
      take = (type != 3 || rtype != 18) && bus == 1;
    }
    if (!take) continue;
    fill_info(d, list + size_t(count++) * kInfoSize);
    if (count >= uint32_t(n) || count > 0xFF) break;
  }
  if (type == 3) {
    // The BT HID query (type 18, at most 4 records) — the host bus holds no BT HID device.
    uint8_t c[256];
    mbus::init_condition(c, sizeof c);
    for (int i = 0; i < 11; ++i) wr<int32_t>(c, 24 + 16 * i, -1);
    wr<int32_t>(c, 12, 18);
    std::vector<uint8_t> hid(4 * mbus::kRecordSize);
    const int32_t m = get_device_list(c, hid.data(), 4);
    for (int32_t i = 0; i < m && i < 4; ++i) {
      Device d;
      if (make_device(hid.data() + size_t(i) * mbus::kRecordSize, &d)) fill_info(d, list + size_t(count++) * kInfoSize);
    }
  }
  if (count <= 0xFF && (type == 0 || (type == 1 && param.user_id == 255) || (type == 4 && param.native_device_type2 == 14)))
    fill_hdmi(list + size_t(count++) * kInfoSize);
  wr<uint32_t>(list, kListCountOffset, count);
  return 0;
}

void init_device_condition(uint8_t* cond) {  // 0x20C280
  mbus::init_condition(cond, 256);
  for (int i = 0; i < 11; ++i) wr<int32_t>(cond, 24 + 16 * i, -1);
}

int32_t device_search(const uint8_t* cond, uint8_t* out, int32_t max, const uint8_t* filter, int32_t nfilter, int32_t any,
                      bool match_state, bool assigned_only) {  // 0x20C4A0
  if (!out || max > 256) return static_cast<int32_t>(0x80B60003u);
  if (max != 0) {
    const uint32_t n = (static_cast<uint32_t>(max) - 1u) < 0xFFu ? static_cast<uint32_t>(max) : 256u;
    for (uint32_t i = 0; i < n; ++i) init_device_condition(out + size_t(i) * 256);
  }
  std::vector<uint8_t> recs(16 * 256);
  for (int i = 0; i < 16; ++i) init_device_condition(recs.data() + size_t(i) * 256);
  uint8_t c[256];
  std::memcpy(c, cond, sizeof c);
  int32_t total = 0, gen = 0;
  const int32_t n = mbus::get_device_info_by_condition(c, 0, recs.data(), 16, &total, &gen, 256);
  if (n < 0) return n;  // "sceMbusGetDeviceInfoByCondition : %d(0x%08x)"
  int32_t count = 0;
  const int32_t last = n < 16 ? n : 16;
  for (int32_t i = 0; i < last; ++i) {
    const uint8_t* r = recs.data() + size_t(i) * 256;
    if (r[17] == 1 && rd<int32_t>(r, 12) != 15) continue;
    const int32_t owner = rd<int32_t>(r, 200);
    if (assigned_only && !(owner > 0 && owner != 255)) continue;
    bool take = false;
    if (!filter || nfilter == 0) {
      take = true;
    } else if (nfilter > 0) {
      const int32_t fcount = rd<int32_t>(r, 20);
      int32_t kmax = fcount < 11 ? fcount : 11;
      if (kmax <= 0) kmax = 1;
      bool matched = false;  // v38
      for (int32_t j = 0; j < nfilter; ++j) {
        bool miss = true;  // v36
        matched = false;
        if (fcount > 0) {
          for (int32_t k = 0; k < kmax; ++k) {
            if (rd<int32_t>(r, 24 + 16 * k) != rd<int32_t>(filter, 16 * j)) continue;
            matched = true;
            if (match_state) matched = rd<int32_t>(filter, 16 * j + 8) == (rd<int32_t>(r, 32 + 16 * k) != 0 ? 1 : 0);
            miss = !matched;
            break;
          }
          if (any == 1 && matched) break;
        }
        if (miss && any == 0) break;
      }
      take = matched;
    }
    if (!take) continue;
    if (count >= max) return static_cast<int32_t>(0x80B60004u);  // "Not enough buffer"
    std::memcpy(out + size_t(count++) * 256, r, 256);
  }
  return count;
}

// int _IsPersonalAudioDeviceInUse(int type, int otherThanUserId) — {name, fn} entry 0xcd3cd0 ->
// 0x1C5390 -> 0x1EF450 (managed: == 1 means in use):
//   n = 0x1FCDE0(type, recs[4]): a device search (0x20C4A0, 16 slots, any = 1, assigned only)
//     whose condition asks for one function of type 1 (+20 = 1, +24 = 1) when type != 0, or
//     with the function filter {type 2, type 18} when type == 0; n > 15 or 0 -> 0; the found
//     records (deviceId != -1) are copied into recs (at most 4, else 0x80B60004);
//   n < 0 -> n; n == 0 -> 0; otherwise 1 when a record's owner (+200) is not otherThanUserId,
//   its type (+12) is not 15/28, and the Caesar manager (PsvrService 26, vtable +40 / +80)
//   does not cover it.
// The host bus holds only the DualSense, whose one function is type 0 (libSceMbus model), so
// neither query matches and the answer is 0.
SSN_PINVOKE("LoginMgrProxy", "_IsPersonalAudioDeviceInUse", int32_t, ssn_LoginMgrProxy_IsPersonalAudioDeviceInUse,
            (int32_t type, int32_t other_than_user_id)) {
  uint8_t cond[256];
  init_device_condition(cond);
  uint8_t filter[32] = {};
  int32_t nfilter = 0;
  if (type != 0) {
    wr<int32_t>(cond, 20, 1);
    wr<int32_t>(cond, 24, 1);
  } else {
    wr<int32_t>(filter, 0, 2);
    wr<int32_t>(filter, 16, 18);
    nfilter = 2;
  }
  std::vector<uint8_t> recs(16 * 256);
  const int32_t m = device_search(cond, recs.data(), 16, filter, nfilter, 1, false, true);
  if (m < 0) return m;
  if (m > 15 || m == 0) return 0;
  int32_t found = 0;
  for (int32_t i = 0; i < m; ++i) {
    if (rd<int64_t>(recs.data() + size_t(i) * 256, 0) == -1) continue;
    if (found >= 4) return static_cast<int32_t>(0x80B60004u);
    ++found;
  }
  if (found == 0) return 0;
  (void)other_than_user_id;
  host::note_unimplemented(
      "LoginMgr IsPersonalAudioDeviceInUse (0x1EF450) with a personal audio device on the bus: Caesar manager state "
      "(vtable +40 / +80) not reproduced; answered 0");
  return 0;
}

}  // namespace ssn::eboot
