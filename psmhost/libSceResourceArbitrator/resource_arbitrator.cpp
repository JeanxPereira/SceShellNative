// "ResourceArbitratorNativeCall" P/Invokes of Sce.Vsh.ResourceArbitrator (ShellUI eboot, FW 4.03).
//
// The eboot's {name, fn} table (0xcf1f40..0xcf1ff0) serves the event side of the resource
// arbitrator from a queue local to the eboot:
//   SceResourceArbitratorSetCallback  -> 0x4D8760: sceResourceArbitratorSetCallback(id, 0x4D8680, 0)
//                                        (libSceResourceArbitrator kJvjeet+f9I, 0xd20).
//   0x4D8680 (the library's event callback): under the mutex unk_EF3370, appends
//     {id, type, 184-byte data} to a 10-slot ring (records of 192 bytes at unk_EF3380, write
//     index dword_EF3378, read index dword_EF337C); when the next write index would reach the
//     read index it logs "[ResourceArbitrator]Queue max!" and drops the event.
//   SceResourceArbitratorCheckEvent   -> 0x4D8770: the number of queued events,
//     write - read (+10 when write < read).
//   SceResourceArbitratorGetEventData -> 0x4D87B0: empty -> 0x819B0006 (NOT_FOUND); otherwise
//     pops the oldest record into id, type and the 184-byte StructCallbackData; 0.
// The managed side (NativeCall.StartCheckTimer) polls CheckEvent once a second and fetches an
// event only when it returns > 0.
//
// The ring is reproduced as the eboot keeps it. Events reach it only through the library's
// callback, which the arbitrator server (not run on the host) drives; nothing on the host
// queues one, so CheckEvent answers 0 as the console does while no arbitration event is
// pending. SetCallback (the library's IPMI registration) is not reproduced here.
#include <cstdint>
#include <cstring>
#include <mutex>

#include "host/boundary.h"

namespace ssn::resource_arbitrator {
namespace {

constexpr int kSlots = 10;
constexpr size_t kDataSize = 184;  // StructCallbackData; record = id + type + data = 192 bytes
constexpr int32_t kErrNotFound = static_cast<int32_t>(0x819B0006u);  // SCE_RESOURCE_ARBITRATOR_ERROR_NOT_FOUND

struct Record {
  uint32_t id;
  int32_t type;
  uint8_t data[kDataSize];
};

std::mutex g_lock;  // unk_EF3370
Record g_ring[kSlots];
int g_write = 0;  // dword_EF3378
int g_read = 0;   // dword_EF337C

}  // namespace

// 0x4D8680 — the event callback the eboot registers with the library (no host caller yet).
void push_event(uint32_t id, int32_t type, const void* data) {
  std::lock_guard<std::mutex> lk(g_lock);
  const int next = (g_write + 1) % kSlots;
  if (next == g_read) return;  // "[ResourceArbitrator]Queue max!"
  g_ring[g_write].id = id;
  g_ring[g_write].type = type;
  std::memcpy(g_ring[g_write].data, data, kDataSize);
  g_write = next;
}

// int SceResourceArbitratorCheckEventInt() — 0x4D8770.
SSN_PINVOKE("ResourceArbitratorNativeCall", "SceResourceArbitratorCheckEvent", int32_t, ssn_ResourceArbitrator_CheckEvent, ()) {
  std::lock_guard<std::mutex> lk(g_lock);
  return g_write - g_read + (g_write >= g_read ? 0 : kSlots);
}

// int SceResourceArbitratorGetEventDataInt(ref uint id, ref type, ref StructCallbackData data) — 0x4D87B0.
SSN_PINVOKE("ResourceArbitratorNativeCall", "SceResourceArbitratorGetEventData", int32_t, ssn_ResourceArbitrator_GetEventData,
            (uint32_t* id, int32_t* type, void* data)) {
  std::lock_guard<std::mutex> lk(g_lock);
  if (g_read == g_write) return kErrNotFound;
  const Record& r = g_ring[g_read];
  *id = r.id;
  *type = r.type;
  std::memcpy(data, r.data, kDataSize);
  g_read = (g_read + 1) % kSlots;
  return 0;
}

}  // namespace ssn::resource_arbitrator
