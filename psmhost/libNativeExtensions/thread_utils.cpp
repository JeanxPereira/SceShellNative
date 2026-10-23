// ReactNative.Bridge.Queue.MessageQueueThread natives of libNativeExtensions.sprx (FW 4.03):
// ThreadUtils_SetThreadName / ThreadUtils_SetSchedulePolicy, called on the message-queue thread
// itself (MessageQueueThread enqueues them).
#include <windows.h>

#include <cstdint>
#include <string>

#include "host/boundary.h"

namespace ssn::native_extensions {

// ThreadUtils_SetThreadName(string name) — 0x153b0 (NID J4I3Ue9vQ+E):
// pthread_set_name_np(pthread_self(), name) (libkernel oxMp8uPqa+U / EotR8a3ASf4): renames the
// calling thread. The host names the calling Windows thread (SetThreadDescription), the thread
// name debuggers and the OS tools show, as the kernel thread name is on the console.
SSN_PINVOKE("libNativeExtensions.sprx", "ThreadUtils_SetThreadName", void, ssn_NativeExtensions_ThreadUtils_SetThreadName,
            (const char* name)) {
  if (!name) return;
  const int n = MultiByteToWideChar(CP_ACP, 0, name, -1, nullptr, 0);
  if (n <= 0) return;
  std::wstring w(static_cast<size_t>(n), L'\0');
  MultiByteToWideChar(CP_ACP, 0, name, -1, w.data(), n);
  SetThreadDescription(GetCurrentThread(), w.c_str());
}

// ThreadUtils_SetSchedulePolicy(SchedulePolicy policy) — 0x15330 (NID KGAA02fSVmk):
//   r = pthread_getschedparam(pthread_self(), &cur_policy, &param) (FIs3-UQT9sg); if r == 0:
//     policy == 2 (SCHED_OTHER): r = ymCrwbFD4fk(self, param) (libkernel 0xc960 -> 0xc710 with
//                                 policy 2, the kernel's rtprio_thread),
//     otherwise:                 r = pthread_setschedparam(self, policy, &param) (Xs9hdiD7sAA);
//   returns r (MessageQueueThread throws RNPSException when it is not 0).
// HOST POLICY (thread scheduling): the PS5 kernel's schedule policies are not applied to Windows
// threads; the call answers 0, the result of the kernel calls for the policies the shell passes
// (SCHED_FIFO 1 / SCHED_OTHER 2 / SCHED_RR 3), without changing Windows scheduling. Logged once
// with the other thread-scheduling calls.
SSN_PINVOKE("libNativeExtensions.sprx", "ThreadUtils_SetSchedulePolicy", int32_t, ssn_NativeExtensions_ThreadUtils_SetSchedulePolicy,
            (int32_t policy)) {
  (void)policy;
  static const bool noted = (host::note_unimplemented(
                                 "HOST POLICY thread scheduling: PS5 thread priorities / schedule policies are not applied to Windows threads (calls succeed)"),
                             true);
  (void)noted;
  return 0;
}

}  // namespace ssn::native_extensions
