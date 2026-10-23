// DbPreparation library pieces shared by the ShellUI eboot and SceShellCore (FW 4.03): both
// link the same library (vsh/common_lib/db_preparation), so the event record and the
// preparation/notification thread object are the same code in both modules.
#pragma once
#include <cstdint>
#include <functional>
#include <mutex>
#include <string>
#include <thread>

namespace ssn::dbp {

// DbPreparation::Event (16 bytes; the managed Sce.Vsh.DbPreparationWrapper.Event layout and
// the IPMI argument of methods 0x160000..0x160003).
struct Event {
  int32_t cex_or_kratos = -1;  // 0 CEX, 1 Kratos
  int32_t user_id = -1;
  uint8_t is_primary_user = 0;
  uint8_t pad[3] = {};
  int32_t event_type = -1;  // 0 INITIALIZE, 1 TERMINATE, 2 CREATE_USER, 3 DESTROY_USER, 4 LOGIN, 5 LOGOUT
};
static_assert(sizeof(Event) == 16);

enum EventType : int32_t { kInitialize = 0, kTerminate = 1, kCreateUser = 2, kDestroyUser = 3, kLogin = 4, kLogout = 5 };

// SCE_DBP errors.
constexpr int32_t kErrInvalidArgument = static_cast<int32_t>(0x809C1000u);
constexpr int32_t kErrNoMemory = static_cast<int32_t>(0x809C1005u);
constexpr int32_t kErrNotImplemented = static_cast<int32_t>(0x809C103Bu);
constexpr int32_t kErrState = static_cast<int32_t>(0x809C113Bu);  // thread object "not started" status
// scePthreadJoin of a thread that was never created: the libkernel status is not traced;
// ESRCH (0x80020003) stands in (only its non-zero-ness reaches the callers).
constexpr int32_t kErrJoinNoThread = static_cast<int32_t>(0x80020003u);

// The 80-byte thread object (ctor in 0x35470 / 0x778A90): the event (+0), the status
// (+16, initially 0x809C113B), the manager mutex (+24), the pthread (+32), the name (+48).
class Thread {
 public:
  // `manager_mutex`: the manager's mutex both thread objects lock (eboot unk_EBF118,
  // "SceDbPreparationManager"; ShellCore unk_1AB4850).
  explicit Thread(std::recursive_mutex& manager_mutex) : mutex_(manager_mutex) {}
  // Host: a thread nobody waited for is left running at process exit.
  ~Thread() {
    if (thread_.joinable()) thread_.detach();
  }

  Event event{};
  int32_t status = kErrState;

  // StartThread (eboot 0x34BD0): fails with 0x809C113B when a thread is still held.
  int32_t start(std::function<void(Thread*)> fn, const char* name) {
    std::lock_guard lock(mutex_);
    if (!fn || !name) return kErrInvalidArgument;
    if (thread_.joinable()) return kErrState;
    name_ = name;
    thread_ = std::thread([this, fn = std::move(fn)] { fn(this); });
    return 0;
  }

  // WaitThread (eboot 0x34D90): when a thread is held or the status is still 0x809C113B,
  // joins it; the result is the first dword the thread function returns through its exit
  // value — the object itself, i.e. event.cex_or_kratos — and the pthread is released.
  // Otherwise the stored status.
  int32_t wait() {
    std::lock_guard lock(mutex_);
    if (thread_.joinable() || status == kErrState) {
      if (!thread_.joinable()) return kErrJoinNoThread;
      thread_.join();
      return event.cex_or_kratos;
    }
    return status;
  }

  const std::string& name() const { return name_; }

 private:
  std::recursive_mutex& mutex_;
  std::thread thread_;
  std::string name_;
};

}  // namespace ssn::dbp
