// libScePsm's PSM event hub (qword_5409B8, created by TGsos8ZCV8c 0x2d2880): per-id and global
// receivers, the posted-event list drained by PsmFramework::Update, and the EventProxy icalls
// that connect it to the managed PsmEventDispatcher (Sce.PlayStation.Orbis.Framework).
#pragma once
#include <cstdint>
#include <functional>
#include <string>

namespace ssn::psm::events {

using Receiver = std::function<void(int32_t id, const std::string& json)>;

// 7W6jR0LyiSg (0x2d19a0): queue an event; it is dispatched by the next PsmFramework::Update.
void post(int32_t id, const std::string& json);
// xmqLf_a8vYY -> sub_2D1A60: dispatch now, on the calling thread.
void send(int32_t id, const std::string& json);
// m3YkcPqYBzQ (0x2d1c50): a native receiver for one event id.
void add_receiver(int32_t id, Receiver r);
// sub_2D2630, the update callback the hub registers: drain the posted list.
void pump();

}  // namespace ssn::psm::events
