// Kernel event flags (sceKernel*EventFlag) as the PS4/PS5 kernel provides them: a
// 64-bit pattern that threads wait on with AND/OR conditions and optional clearing.
#pragma once
#include <cstdint>
#include <string>

namespace ssn::kernel {

constexpr int32_t kEvfWaitModeAnd = 0x01;
constexpr int32_t kEvfWaitModeOr = 0x02;
constexpr int32_t kEvfWaitModeClearAll = 0x10;
constexpr int32_t kEvfWaitModeClearPat = 0x20;

constexpr int32_t kErrorEsrch = static_cast<int32_t>(0x80020003u);      // SCE_KERNEL_ERROR_ESRCH
constexpr int32_t kErrorEbusy = static_cast<int32_t>(0x80020010u);      // SCE_KERNEL_ERROR_EBUSY
constexpr int32_t kErrorEinval = static_cast<int32_t>(0x80020016u);     // SCE_KERNEL_ERROR_EINVAL
constexpr int32_t kErrorEtimedout = static_cast<int32_t>(0x8002003cu);  // SCE_KERNEL_ERROR_ETIMEDOUT

struct EventFlag;

EventFlag* create_event_flag(const std::string& name, uint32_t attr, uint64_t init);
EventFlag* open_event_flag(const std::string& name);  // nullptr when no flag has that name
int32_t set_event_flag(EventFlag* ef, uint64_t bits);
int32_t clear_event_flag(EventFlag* ef, uint64_t bits);
int32_t poll_event_flag(EventFlag* ef, uint64_t bits, uint32_t mode, uint64_t* result);
// timeout_us: nullptr = infinite; on return holds the remaining time.
int32_t wait_event_flag(EventFlag* ef, uint64_t bits, uint32_t mode, uint64_t* result, uint32_t* timeout_us);

}  // namespace ssn::kernel
