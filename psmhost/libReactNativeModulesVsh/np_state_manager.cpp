// ReactNative.Modules.Vsh.PSN.NpStateManager natives of libReactNative.Modules.Vsh.sprx (FW 4.03):
// the module's NP sign-in state listener list.
//
// The module keeps one listener object (guarded static at byte_143A8, built on first use with a
// destructor registered through __cxa_atexit, sub_2670): a vector of {callback, arg} pairs at
// ymmword_14388 under the mutex unk_14360, and the NP state callback id dword_14368 that
// NpStateManagerStart registers with libSceNpManager (qQJfO8HAiaY, callback sub_2E10, which
// forwards state changes to the listeners). Start / Stop are not reproduced here (they belong to
// the NpManager client); with no registration the listeners are never called, as on a console
// whose NP state does not change.
#include <cstdint>
#include <mutex>
#include <utility>
#include <vector>

#include "host/boundary.h"

namespace ssn::rnmv {
namespace {

std::mutex g_lock;                                    // unk_14360
std::vector<std::pair<void*, void*>> g_listeners;     // ymmword_14388 (+8 begin, +16 end)

}  // namespace

// NpStateManagerRegisterCallback(callback, arg) — 0x23d0 (NID D3CRTfQdhbA): null callback ->
// 0x81700504; otherwise, under the lock, the pair is appended unless the same {callback, arg} is
// already registered; returns 0.
SSN_PINVOKE("libReactNative.Modules.Vsh.sprx", "NpStateManagerRegisterCallback", int32_t,
            ssn_RNModulesVsh_NpStateManagerRegisterCallback, (void* callback, void* arg)) {
  if (!callback) return static_cast<int32_t>(0x81700504u);
  std::lock_guard<std::mutex> lk(g_lock);
  for (const auto& l : g_listeners)
    if (l.first == callback && l.second == arg) return 0;
  g_listeners.emplace_back(callback, arg);
  return 0;
}

// NpStateManagerUnregisterCallback(callback, arg) — 0x2520 (NID f5Zum3vudFM): under the lock, the
// first matching pair is removed (the later ones move down); returns 0, found or not.
SSN_PINVOKE("libReactNative.Modules.Vsh.sprx", "NpStateManagerUnregisterCallback", int32_t,
            ssn_RNModulesVsh_NpStateManagerUnregisterCallback, (void* callback, void* arg)) {
  std::lock_guard<std::mutex> lk(g_lock);
  for (auto it = g_listeners.begin(); it != g_listeners.end(); ++it) {
    if (it->first == callback && it->second == arg) {
      g_listeners.erase(it);
      break;
    }
  }
  return 0;
}

}  // namespace ssn::rnmv
