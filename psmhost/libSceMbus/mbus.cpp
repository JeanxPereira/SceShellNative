#include "libSceMbus/mbus.h"

#include <condition_variable>
#include <cstring>
#include <deque>
#include <mutex>
#include <vector>

#include "host/boundary.h"
#include "libScePsm/input/pss_input.h"
#include "platform/input.h"

namespace ssn::mbus {
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

constexpr int32_t kEntries = 32;  // DeviceManager table: 32 x 256 bytes (0x7b590 stops at 32)

// One SceSysCore DeviceManager entry (256 bytes). Offsets as 0x7b3e0 / 0x7b590 / 0x75500
// read them: +8 deviceId (-1 = free slot), +16 bus/connection, +20 device type, +24 number
// of functions, +28 user id, +40 BT address, +48 + 8*i function {type, port, state},
// +136, +140 VID, +142 PID, +144..147, +168/+172 power state cache, +244, +246..+248.
struct Entry {
  uint8_t raw[256];
};

struct Table {
  std::mutex mu;
  Entry e[kEntries];
  uint32_t generation = 0;  // DeviceManager +24 (0x7b9c0)
};

// The server's per-client event queues (one per sceMbusEventCreate handle).
struct Events {
  std::mutex mu;
  std::condition_variable cv;
  std::vector<std::deque<Event>> queues;
};
Events& events() {
  static Events e;
  return e;
}
void post_event(const Event& ev) {
  Events& e = events();
  {
    std::lock_guard<std::mutex> l(e.mu);
    for (auto& q : e.queues) q.push_back(ev);
  }
  e.cv.notify_all();
}

// HOST VALUE: the device the kernel would report for the emulated DualSense of the input
// layer (psmhost/libScePsm/input), shaped the way SceSysCore's addDevice (0x8ca30) builds a
// DualSense: HID kind 6 with VID 1356 / PID 3302 (the ids 0x8ca30 itself tests), a kernel
// connection value other than 1/2 so the bus field stays 0 (USB; 0 is also
// ConnectionType_Usb), device type 22, one function (type 0, the controller) with state 0,
// and no extra functions: the optional ones (9 from sub_5D2D0, 17/18 from the HID
// capability bits 0x200/0x800, 24 for feature 0x40) describe hardware the host pad does
// not have. The device id is the input layer's (kHostDeviceId); no user owns the pad
// (kHostUserId = -1); BT address 0 (USB). The entry exists from the moment the pad is on
// (platform/input power); each attach bumps the generation.
void fill_host_pad(uint8_t* d) {
  wr<uint64_t>(d, 8, psm::input::kHostDeviceId);
  wr<int32_t>(d, 16, 0);   // bus: USB
  wr<int32_t>(d, 20, 22);  // DualSense
  wr<int32_t>(d, 24, 1);   // one function
  wr<int32_t>(d, 28, psm::input::kHostUserId);
  wr<uint64_t>(d, 40, 0);  // BT address
  d[48] = 0;               // function 0: controller
  d[49] = 0;
  d[50] = 0;               // state 0 (enabled)
  wr<uint16_t>(d, 140, 1356);
  wr<uint16_t>(d, 142, 3302);
}

Table& table() {
  static Table* t = [] {
    auto* t = new Table();
    std::memset(t->e, 0, sizeof t->e);
    for (auto& x : t->e) wr<int64_t>(x.raw, 8, -1);
    host::note_unimplemented("HOST VALUE mbus: one USB DualSense (deviceId 0x10001, VID 1356 PID 3302, type 22, controller function only, no owner), attached when it is turned on");
    return t;
  }();
  return *t;
}

// Power listener of the emulated pad (platform/input), installed at static init; a pad
// that is on from boot is attached by the input stack's start (pss_input ensure_started).
struct PowerHook {
  PowerHook() { platform::input::set_power_on_listener(&attach_host_pad); }
} g_power_hook;

// 0x7b260: the function-list part of a condition (cond +20 count, +24 + 16*i {type, mode});
// 0 = match. Mode 4 asks for a function in state 4, any other mode for state 0. Type 2
// matches only with the 0x5001 flag (cond +228) and an owner of 0 or 255; type 10 checks
// the VID/PID class against the flag.
int match_functions(const uint8_t* cond, const uint8_t* e) {
  const int32_t n = rd<int32_t>(cond, 20);
  const uint16_t flag = rd<uint16_t>(cond, 228);
  for (int32_t i = 0; i < n; ++i) {
    const int32_t type = rd<int32_t>(cond, 24 + 16 * i);
    if (type == -1) continue;
    const int32_t count = rd<int32_t>(e, 24);
    if (count <= 0) return 1;
    const int32_t mode = rd<int32_t>(cond, 32 + 16 * i);
    bool found = false;
    for (int32_t k = 0; k < count && !found; ++k) {
      const int8_t ft = static_cast<int8_t>(e[48 + 8 * k]);
      const uint8_t st = e[50 + 8 * k];
      found = type == ft && st == (mode == 4 ? 4 : 0);
    }
    if (!found) return 1;
    if (type == 2) {
      if (flag != 20481) continue;
      const int32_t user = rd<int32_t>(e, 28);
      if (user != 0 && user != 255) continue;
      return 1;
    }
    if (type == 10) {
      const uint16_t vid = rd<uint16_t>(e, 140), pid = rd<uint16_t>(e, 142);
      uint16_t want = 4097;
      if ((vid != 5141 || (pid & 0xFFDF) != 0) && (vid != 1133 || pid != 2563)) want = (vid == 0x12BA && pid == 0xFF) ? 8193 : 0;
      if (want != flag) return 1;
    }
  }
  return 0;
}

// The common predicate of 0x7b3e0 (count) and 0x7b590 (fetch).
bool matches(const uint8_t* cond, const uint8_t* e) {
  const int64_t id = rd<int64_t>(e, 8);
  if (id == -1) return false;
  const int64_t cid = rd<int64_t>(cond, 0);
  if (cid != -1 && cid != id) return false;
  const int32_t a = rd<int32_t>(cond, 8), type = rd<int32_t>(cond, 12);
  if (a != -1 && rd<int32_t>(e, 16) != a) return false;
  if (type != -1 && rd<int32_t>(e, 20) != type) return false;
  if (match_functions(cond, e) != 0) return false;
  if (rd<uint16_t>(cond, 228) == 32769) {
    if (rd<uint16_t>(e, 140) != rd<uint16_t>(cond, 230)) return false;
    const uint16_t pid = rd<uint16_t>(e, 142);
    if (pid < rd<uint16_t>(cond, 232) || pid > rd<uint16_t>(cond, 234)) return false;
  }
  const int32_t user = rd<int32_t>(cond, 200);
  if (user != -1 && rd<int32_t>(e, 28) != user) return false;
  return true;
}

// 0x75500 (flag 0): server entry -> client record; then 0x7b590 clears out +16.
void to_record(uint8_t* e, uint8_t* out) {
  std::memset(out, 0, kRecordSize);
  wr<uint64_t>(out, 0, rd<uint64_t>(e, 8));
  wr<int32_t>(out, 8, rd<int32_t>(e, 16));
  const uint32_t type = rd<uint32_t>(e, 20);
  wr<uint32_t>(out, 12, type);
  wr<int32_t>(out, 20, rd<int32_t>(e, 24));
  for (int i = 0; i < 11; ++i) {
    wr<int32_t>(out, 24 + 16 * i, static_cast<int8_t>(e[48 + 8 * i]));
    wr<int32_t>(out, 28 + 16 * i, static_cast<int8_t>(e[49 + 8 * i]));
    int8_t st = static_cast<int8_t>(e[50 + 8 * i]);
    if (st == 2) st = 1;
    wr<int32_t>(out, 32 + 16 * i, st);
  }
  wr<int32_t>(out, 200, rd<int32_t>(e, 28));
  // Types in mask 96565260 ask the power-state source (sub_868c0 / sub_86ee0) for the
  // charge state; on failure the record gets state 5 and level 0. HOST VALUE: the host
  // has no such source, so the failure path is taken.
  if (type <= 0x1A && ((96565260u >> type) & 1)) {
    wr<int32_t>(out, 216, 5);
    wr<int32_t>(out, 220, 0);
  } else {
    wr<uint64_t>(out, 216, 5);
  }
  uint8_t b = e[244];
  if (uint8_t(b - 1) >= 3) b = 0;
  out[17] = b;
  wr<uint64_t>(out, 208, rd<uint64_t>(e, 40));
  wr<uint32_t>(out, 224, rd<uint32_t>(e, 136));
  wr<uint16_t>(out, 228, rd<uint16_t>(e, 140));
  wr<uint16_t>(out, 230, rd<uint16_t>(e, 142));
  for (int i = 0; i < 4; ++i) out[232 + i] = e[144 + i];
  out[236] = e[246];
  out[237] = e[247];
  out[18] = e[248];
  out[16] = 0;
}

}  // namespace

void init_condition(uint8_t* cond, uint32_t size) {
  std::memset(cond, 0, size);
  std::memset(cond, 0xFF, 16);
  wr<int32_t>(cond, 200, -1);
}

int32_t get_device_info_by_condition(const uint8_t* cond, int32_t start, uint8_t* out, int32_t max, int32_t* total,
                                     int32_t* generation, int32_t record_size) {
  Table& t = table();
  std::lock_guard<std::mutex> l(t.mu);
  // Server 0x67ff0: total = number of matches; records from index `start`, at most the
  // client's 16 (0x4a90 passes 16 to 0x1690).
  int32_t count = 0;
  for (auto& e : t.e)
    if (matches(cond, e.raw)) ++count;
  if (total) *total = count;
  uint8_t buf[16][kRecordSize];
  int32_t got = 0;
  int32_t idx = 0;
  for (auto& e : t.e) {
    if (!matches(cond, e.raw)) continue;
    if (idx++ < start) continue;
    if (got >= 16) break;
    to_record(e.raw, buf[got++]);
  }
  if (generation) *generation = static_cast<int32_t>(t.generation);
  // Client 0x4a90: copies min(got, max) records when recordSize == 256 and returns that
  // count; any other record size returns 0x809B0080 without copying (0x4a90: result
  // preset to 2157641856 before the size test).
  int32_t n = got < max ? got : max;
  if (record_size != kRecordSize) return static_cast<int32_t>(0x809B0080u);
  std::memcpy(out, buf, size_t(n) * kRecordSize);
  return n;
}

int32_t bind_device_with_user_id(uint64_t device_id, int32_t user_id) {
  Table& t = table();
  Event ev;
  {
    std::lock_guard<std::mutex> l(t.mu);
    uint8_t* e = nullptr;
    for (auto& x : t.e)
      if (rd<uint64_t>(x.raw, 8) == device_id) e = x.raw;
    if (!e) return -10;
    if (rd<int32_t>(e, 28) == user_id) return 0;  // 0x7B770: already the owner
    if (user_id == 0) {                          // 0x77470
      const int32_t prev = rd<int32_t>(e, 28);
      if (prev != 0) wr<int32_t>(e, 32, prev);
    }
    wr<int32_t>(e, 28, user_id);
    ev.id = kEventOwnerChanged;
    ev.device_id = device_id;
    ev.type = rd<int32_t>(e, 20);
    ev.user_id = user_id;
  }
  // 0x63350 then binds the device's functions (pad port 0x7EF80, audio routing 0x63FD0 /
  // 0x64530) and queues a user event (0x8C1C0 code 5). The clients see the new owner as
  // SCE_MBUS_EVENT_DEVICE_OWNER_CHANGED (16), the id libScePsm's scanner (0x188AF0
  // " OWNERCHG") and the eboot LoginMgr (0x1CF9B0 case 16) handle; where the server turns
  // the bind into that event is not traced.
  host::note_unimplemented(
      "SceSysCore mbus bind (0x63350): owner stored and OWNER_CHANGED queued; per-function binding (pad/audio ports) "
      "and the server-side event post site not reproduced");
  post_event(ev);
  return 0;
}

void attach_host_pad() {
  Table& t = table();
  Event ev;
  {
    std::lock_guard<std::mutex> l(t.mu);
    for (auto& x : t.e)
      if (rd<uint64_t>(x.raw, 8) == psm::input::kHostDeviceId) return;  // already attached
    uint8_t* d = nullptr;
    for (auto& x : t.e)
      if (rd<int64_t>(x.raw, 8) == -1) {
        d = x.raw;
        break;
      }
    if (!d) return;
    std::memset(d, 0, 256);
    fill_host_pad(d);
    ++t.generation;
    ev.id = kEventDeviceAdded;
    ev.device_id = psm::input::kHostDeviceId;
    ev.type = 22;
    ev.user_id = psm::input::kHostUserId;
  }
  // The event's subType (printed by the eboot LoginMgr, 0x1CF9B0) is not traced: 0.
  post_event(ev);
}

int32_t create_event_handle() {
  Events& e = events();
  std::lock_guard<std::mutex> l(e.mu);
  e.queues.emplace_back();
  return static_cast<int32_t>(e.queues.size() - 1);
}

int32_t receive_event(int32_t handle, Event* out, bool block) {
  Events& e = events();
  std::unique_lock<std::mutex> l(e.mu);
  if (handle < 0 || size_t(handle) >= e.queues.size() || !out) return 0;  // host handles only
  if (block) e.cv.wait(l, [&] { return !e.queues[handle].empty(); });
  if (e.queues[handle].empty()) return 0;
  *out = e.queues[handle].front();
  e.queues[handle].pop_front();
  return 1;
}

}  // namespace ssn::mbus
