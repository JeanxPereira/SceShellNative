#include "libScePsm/runtime/event_hub.h"

#include <deque>
#include <map>
#include <mutex>
#include <vector>

#include <mono/metadata/appdomain.h>
#include <mono/metadata/object.h>

#include "host/boundary.h"

namespace ssn::psm::events {
namespace {

struct Hub {
  std::recursive_mutex map_mutex;    // "ScePsmEventReceiverMapMutex"
  std::map<int32_t, std::vector<Receiver>> by_id;
  std::mutex list_mutex;             // "ScePsmEventDataListMutex"
  std::deque<std::pair<int32_t, std::string>> posted;
  std::recursive_mutex global_mutex;  // "ScePsmEventReceiverListMutex"
  uint32_t delegator = 0;            // GC handle of the managed ProxyHandler (qword_5409C0 slot)
};

Hub& hub() {
  static Hub h;
  return h;
}

// sub_2D2AB0, the global receiver installed with the delegator slot: the event value is
// serialised to a string and the managed ProxyHandler is called with {id, value}
// (EventProxy.ProcessEvent -> PsmEventDispatcher.Send). The host invokes the delegate through
// the runtime instead of a native thunk; the managed side receives the same ProxyData.
void call_delegator(int32_t id, const std::string& json) {
  uint32_t handle;
  {
    std::lock_guard<std::recursive_mutex> lk(hub().global_mutex);
    handle = hub().delegator;
  }
  if (!handle) return;
  MonoObject* del = mono_gchandle_get_target(handle);
  if (!del) return;
  struct ProxyData {  // Sce.PlayStation.Orbis.Framework.EventProxy.ProxyData (managed layout)
    int32_t m_id;
    MonoString* m_value;
  } data{id, mono_string_new(mono_domain_get(), json.c_str())};
  void* args[1] = {&data};
  MonoObject* exc = nullptr;
  mono_runtime_delegate_invoke(del, args, &exc);
}

void dispatch(int32_t id, const std::string& json) {  // sub_2D1A60
  std::vector<Receiver> receivers;
  {
    std::lock_guard<std::recursive_mutex> lk(hub().map_mutex);
    auto it = hub().by_id.find(id);
    if (it != hub().by_id.end()) receivers = it->second;
  }
  for (auto& r : receivers) r(id, json);
  call_delegator(id, json);
}

// The managed ProxyData passed by reference to the Post/Send icalls.
struct ManagedProxyData {
  int32_t m_id;
  MonoString* m_value;
};

std::string utf8(MonoString* s) {
  if (!s) return {};
  char* c = mono_string_to_utf8(s);
  std::string r = c ? c : "";
  mono_free(c);
  return r;
}

// RegisterDelegator(ProxyHandler) — 0x2d2b30: the delegate's function pointer into the slot.
void RegisterDelegator(MonoObject* handler) {
  std::lock_guard<std::recursive_mutex> lk(hub().global_mutex);
  if (hub().delegator) mono_gchandle_free(hub().delegator);
  hub().delegator = handler ? mono_gchandle_new(handler, false) : 0;
}
SSN_ICALL("Sce.PlayStation.Orbis.Framework.EventProxy::RegisterDelegator", RegisterDelegator);

// UnregisterDelegator(ProxyHandler) — 0x2d2b50: clears the slot.
void UnregisterDelegator(MonoObject*) {
  std::lock_guard<std::recursive_mutex> lk(hub().global_mutex);
  if (hub().delegator) mono_gchandle_free(hub().delegator);
  hub().delegator = 0;
}
SSN_ICALL("Sce.PlayStation.Orbis.Framework.EventProxy::UnregisterDelegator", UnregisterDelegator);

// Post(ref ProxyData) — 0x2d2b60: the value is parsed as JSON and queued (7W6jR0LyiSg).
// (The host keeps the text as given; the native round trip through sce::Json re-serialises it.)
void Post(ManagedProxyData* e) { post(e->m_id, utf8(e->m_value)); }
SSN_ICALL("Sce.PlayStation.Orbis.Framework.EventProxy::Post", Post);

// Send(ref ProxyData) — 0x2d2c10: the same, dispatched at once (sub_2D1A60).
void Send(ManagedProxyData* e) { send(e->m_id, utf8(e->m_value)); }
SSN_ICALL("Sce.PlayStation.Orbis.Framework.EventProxy::Send", Send);

}  // namespace

void post(int32_t id, const std::string& json) {
  std::lock_guard<std::mutex> lk(hub().list_mutex);
  hub().posted.emplace_back(id, json);
}

void send(int32_t id, const std::string& json) { dispatch(id, json); }

void add_receiver(int32_t id, Receiver r) {
  std::lock_guard<std::recursive_mutex> lk(hub().map_mutex);
  hub().by_id[id].push_back(std::move(r));
}

void pump() {
  for (;;) {
    std::pair<int32_t, std::string> e;
    {
      std::lock_guard<std::mutex> lk(hub().list_mutex);
      if (hub().posted.empty()) return;
      e = std::move(hub().posted.front());
      hub().posted.pop_front();
    }
    dispatch(e.first, e.second);
  }
}

}  // namespace ssn::psm::events
