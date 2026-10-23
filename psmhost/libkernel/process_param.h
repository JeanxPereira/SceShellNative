// The ShellUI process's process parameter (PT_SCE_PROCPARAM of its eboot), which the
// kernel exposes to libkernel through sysctl.
#pragma once
#include <cstdint>

namespace ssn::kernel {

// The 8-byte sdk_version field at +0x10 of the ShellUI eboot's process parameter
// (system_ex/app/NPXS40087/eboot.bin; 0x0403000009040001 in the 4.03 dump).
uint64_t procparam_sdk_version();

// sceKernelGetProsperoCompiledSdkVersion(uint32_t*) — libkernel_sys 0x1c8d0
// (NID GGeRJk1XdWc): null -> 0x80020016; otherwise sysctl {CTL_KERN, KERN_PROC(14),
// 68, getpid()} into an 8-byte value (cached) and returns its high dword, i.e. the
// high half of the process parameter's sdk_version (0x04030000 for 4.03).
int32_t get_prospero_compiled_sdk_version(uint32_t* out);

}  // namespace ssn::kernel
