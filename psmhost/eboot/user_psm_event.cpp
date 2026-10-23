// User-service events as PSM events (eboot.bin, FW 4.03): Sce.Vsh.UserServiceWrapper
// _EnablePsmEvent / _DisablePsmEvent. initializeFrameworkDependentModules (0xD050) calls
// _EnablePsmEvent (through the thunk 0xDF30) right after its first step; no managed code
// calls it in this firmware.
#include <cstdint>
#include <string>

#include "host/boundary.h"
#include "libScePsm/runtime/event_hub.h"
#include "shellcore/user_service.h"

namespace ssn::eboot {
namespace {

// 0x4A6A30: the user-service event callback. eventType 0 -> PSM event 40 (OnUserLogin),
// 1 -> 41, 256 -> 42, 257 -> 43, 512 -> 177, 768 -> 212, 1280 -> 295; other types are
// dropped. The event value is an sce::Json object {"userId": (uint64) userId, "eventType":
// (int64) eventType}, posted with 7W6jR0LyiSg (written in the object's key order).
void on_user_event(const int32_t* ev, void*) {
  int32_t id;
  switch (ev[0]) {
    case 0: id = 40; break;
    case 1: id = 41; break;
    case 256: id = 42; break;
    case 257: id = 43; break;
    case 512: id = 177; break;
    case 768: id = 212; break;
    case 1280: id = 295; break;
    default: return;
  }
  const uint64_t user = static_cast<uint64_t>(static_cast<int64_t>(ev[1]));
  psm::events::post(id, "{\"eventType\":" + std::to_string(ev[0]) + ",\"userId\":" + std::to_string(user) + "}");
}

// _EnablePsmEvent (0x4A6A20): sceUserServiceRegisterEventCallback(0x4A6A30, nullptr).
int32_t EnablePsmEvent() { return shellcore::users::register_event_callback(&on_user_event, nullptr); }
SSN_ICALL("Sce.Vsh.UserServiceWrapper::_EnablePsmEvent", EnablePsmEvent);

}  // namespace

// initializeFrameworkDependentModules step (0xD050 -> 0xDF30 -> 0x4A6A20).
int32_t enable_user_psm_events() { return EnablePsmEvent(); }

}  // namespace ssn::eboot
