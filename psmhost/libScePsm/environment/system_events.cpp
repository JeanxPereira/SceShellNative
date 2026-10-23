// Sce.PlayStation.Core.Environment.SystemEvents (libScePsm.sprx, FW 4.03).
#include <cstdint>

#include "host/boundary.h"

namespace ssn::psm {
namespace {

// SystemEvents.InternalData { bool QuitRequired; bool Restored; }.
struct InternalData {
  uint8_t quit_required;
  uint8_t restored;
};

// CheckEventsNative(out InternalData) — 0x2d4310: zeroes the data, drains the three
// system-event queues (sub_17C670(0) -> sub_17C720(0..2): each queued entry goes to the
// queue's handler qword_4FD230[i]; an entry of type 6 that the handler does not take sets the
// application state to 3), then reads the application state dword_4FBE90 (sub_15D7F0):
// 3 -> QuitRequired; any other non-zero state -> the restore listeners (qword_53FC78) run and
// Restored is set. Returns 0.
// The state changes only through system-service events (focus, suspend/resume: sub_15D430 ->
// sub_17C270 type 6 / sub_15D800); the input layer's queue entries reach nobody unless a handler
// was installed in qword_4FD230 (sub_17C190/sub_17C200), which the host never does. HOST VALUE:
// the host delivers no system-service events, so the state stays 0.
int32_t CheckEventsNative(InternalData* data) {
  static const bool noted =
      (host::note_unimplemented("HOST VALUE libScePsm SystemEvents: no system-service events (application state 0)"), true);
  (void)noted;
  if (data) data->quit_required = data->restored = 0;
  return 0;
}
SSN_ICALL("Sce.PlayStation.Core.Environment.SystemEvents::CheckEventsNative", CheckEventsNative);

}  // namespace
}  // namespace ssn::psm
