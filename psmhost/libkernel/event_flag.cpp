#include "libkernel/event_flag.h"

#include <chrono>
#include <condition_variable>
#include <map>
#include <memory>
#include <mutex>
#include <vector>

namespace ssn::kernel {

struct EventFlag {
  std::string name;
  uint32_t attr = 0;
  uint64_t pattern = 0;
  std::mutex m;
  std::condition_variable cv;
};

namespace {
std::mutex g_registry_mutex;
std::map<std::string, std::unique_ptr<EventFlag>> g_named;
std::vector<std::unique_ptr<EventFlag>> g_anonymous;

bool satisfied(uint64_t pattern, uint64_t bits, uint32_t mode) {
  return (mode & kEvfWaitModeAnd) ? (pattern & bits) == bits : (pattern & bits) != 0;
}

void apply_clear(EventFlag& ef, uint64_t bits, uint32_t mode) {
  if (mode & kEvfWaitModeClearAll) ef.pattern = 0;
  else if (mode & kEvfWaitModeClearPat) ef.pattern &= ~bits;
}

bool valid_mode(uint32_t mode) {
  const uint32_t cond = mode & (kEvfWaitModeAnd | kEvfWaitModeOr);
  return cond == kEvfWaitModeAnd || cond == kEvfWaitModeOr;
}
}  // namespace

EventFlag* create_event_flag(const std::string& name, uint32_t attr, uint64_t init) {
  auto ef = std::make_unique<EventFlag>();
  ef->name = name;
  ef->attr = attr;
  ef->pattern = init;
  std::lock_guard lock(g_registry_mutex);
  EventFlag* p = ef.get();
  if (!name.empty() && !g_named.count(name)) g_named.emplace(name, std::move(ef));
  else g_anonymous.push_back(std::move(ef));
  return p;
}

EventFlag* open_event_flag(const std::string& name) {
  std::lock_guard lock(g_registry_mutex);
  auto it = g_named.find(name);
  return it == g_named.end() ? nullptr : it->second.get();
}

int32_t set_event_flag(EventFlag* ef, uint64_t bits) {
  if (!ef) return kErrorEsrch;
  {
    std::lock_guard lock(ef->m);
    ef->pattern |= bits;
  }
  ef->cv.notify_all();
  return 0;
}

int32_t clear_event_flag(EventFlag* ef, uint64_t bits) {
  if (!ef) return kErrorEsrch;
  std::lock_guard lock(ef->m);
  ef->pattern &= bits;
  return 0;
}

int32_t poll_event_flag(EventFlag* ef, uint64_t bits, uint32_t mode, uint64_t* result) {
  if (!ef) return kErrorEsrch;
  if (bits == 0 || !valid_mode(mode)) return kErrorEinval;
  std::lock_guard lock(ef->m);
  if (!satisfied(ef->pattern, bits, mode)) return kErrorEbusy;
  if (result) *result = ef->pattern;
  apply_clear(*ef, bits, mode);
  return 0;
}

int32_t wait_event_flag(EventFlag* ef, uint64_t bits, uint32_t mode, uint64_t* result, uint32_t* timeout_us) {
  if (!ef) return kErrorEsrch;
  if (bits == 0 || !valid_mode(mode)) return kErrorEinval;
  std::unique_lock lock(ef->m);
  auto pred = [&] { return satisfied(ef->pattern, bits, mode); };
  if (timeout_us) {
    const auto start = std::chrono::steady_clock::now();
    const bool ok = ef->cv.wait_for(lock, std::chrono::microseconds(*timeout_us), pred);
    const auto spent = std::chrono::duration_cast<std::chrono::microseconds>(std::chrono::steady_clock::now() - start).count();
    *timeout_us = spent >= *timeout_us ? 0 : static_cast<uint32_t>(*timeout_us - spent);
    if (!ok) {
      if (result) *result = ef->pattern;
      return kErrorEtimedout;
    }
  } else {
    ef->cv.wait(lock, pred);
  }
  if (result) *result = ef->pattern;
  apply_clear(*ef, bits, mode);
  return 0;
}

}  // namespace ssn::kernel
