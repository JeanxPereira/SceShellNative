// Sce.PlayStation.Core.Runtime.DiagnosticsNative (libScePsm.sprx, FW 4.03).
#include <windows.h>

#include <cstdint>
#include <cstdio>
#include <mutex>
#include <string>

#include "common/mono_util.h"
#include "host/boundary.h"
#include "host/context.h"
#include "libScePsm/graphics/objects.h"

namespace ssn::psm {
namespace {

// IsMainThreadNative() — libScePsm 0x300620: false while a global flag (0x53fc68) is
// set, otherwise scePthreadSelf() == the pthread recorded at startup (0x53fc70). The
// host's main thread is the one running the managed entry point.
bool IsMainThreadNative() { return GetCurrentThreadId() == host::ctx().main_thread_id; }
SSN_ICALL("Sce.PlayStation.Core.Runtime.DiagnosticsNative::IsMainThreadNative", IsMainThreadNative);

// FILETIME (100 ns units) -> microseconds.
uint64_t us(const FILETIME& f) { return ((uint64_t(f.dwHighDateTime) << 32) | f.dwLowDateTime) / 10; }

// GetSystemTime(out ulong user, out ulong system) — 0x300510: getrusage(RUSAGE_THREAD) of the
// calling thread; user = ru_utime, system = ru_stime, each sec * 1e6 + usec. The host takes the
// calling OS thread's user / kernel times.
void GetSystemTime(uint64_t* user, uint64_t* sys) {
  FILETIME c, e, k, u;
  if (GetThreadTimes(GetCurrentThread(), &c, &e, &k, &u)) {
    *user = us(u);
    *sys = us(k);
  } else {
    *user = *sys = 0;
  }
}
SSN_ICALL("Sce.PlayStation.Core.Runtime.DiagnosticsNative::GetSystemTime", GetSystemTime);

// GetSystemTimeThread(ulong thread, out ulong user, out ulong system) — 0x300590: both 0 first;
// thread 0 -> 0x80b30001; otherwise the same times of that native thread (import XkEs5rH1bSk),
// whose result is returned. The thread is the id GetNativeThreadId answers (the OS thread id on
// the host); an id the host cannot open answers ESRCH (0x80020003).
int32_t GetSystemTimeThread(uint64_t thread, uint64_t* user, uint64_t* sys) {
  *user = *sys = 0;
  if (thread == 0) return static_cast<int32_t>(0x80b30001u);
  HANDLE h = OpenThread(THREAD_QUERY_LIMITED_INFORMATION, FALSE, static_cast<DWORD>(thread));
  if (!h) return static_cast<int32_t>(0x80020003u);
  FILETIME c, e, k, u;
  const bool ok = GetThreadTimes(h, &c, &e, &k, &u) != 0;
  CloseHandle(h);
  if (!ok) return static_cast<int32_t>(0x80020003u);
  *user = us(u);
  *sys = us(k);
  return 0;
}
SSN_ICALL("Sce.PlayStation.Core.Runtime.DiagnosticsNative::GetSystemTimeThread", GetSystemTimeThread);

// GraphicsObjectStatistics (Core.dll): ShaderProgramCount, VertexBufferCount, PixelBufferCount,
// FrameBufferCount (four uint).
struct GraphicsObjectStatistics {
  uint32_t shader_programs, vertex_buffers, pixel_buffers, frame_buffers;
};

// GetGraphicsObjectStatistics(out GraphicsObjectStatistics) — 0x1f2440: for each handle table
// (0x527438 shader programs, 0x527440 vertex buffers, 0x527420 pixel buffers, 0x527448 frame
// buffers) lock its mutex (+32), read its entry count (+12: the element count the insert
// 0x1f2510 compares with the bucket count) and unlock; returns 0. The host keeps the same four
// tables (graphics/objects.h): an object is in its table from Create until its last reference is
// released, as in the firmware's tables.
int32_t GetGraphicsObjectStatistics(GraphicsObjectStatistics* s) {
  auto& o = gfx::Objects::get();
  std::lock_guard lock(o.mutex());
  s->shader_programs = static_cast<uint32_t>(o.shader_programs().map.size());
  s->vertex_buffers = static_cast<uint32_t>(o.vertex_buffers().map.size());
  s->pixel_buffers = static_cast<uint32_t>(o.pixel_buffers().map.size());
  s->frame_buffers = static_cast<uint32_t>(o.frame_buffers().map.size());
  return 0;
}
SSN_ICALL("Sce.PlayStation.Core.Runtime.DiagnosticsNative::GetGraphicsObjectStatistics", GetGraphicsObjectStatistics);

// Milliseconds of the time-stamp counter as DebugPrintTimeTick computes them:
// 1000 * sceKernelReadTsc() / sceKernelGetTscFrequency() in unsigned 64-bit arithmetic. The host's
// counter is the Windows performance counter (QueryPerformanceCounter / -Frequency), the PC's
// invariant TSC-derived clock counted from boot, as the console's TSC is.
uint64_t tsc_msec() {
  LARGE_INTEGER c, f;
  QueryPerformanceCounter(&c);
  QueryPerformanceFrequency(&f);
  return (1000ull * static_cast<uint64_t>(c.QuadPart)) / static_cast<uint64_t>(f.QuadPart);
}

// DebugPrintTimeTick(string) — 0x3006e0: s = ScePsmMonoStringToUtf8(str) (0x2f4be0; null string ->
// nothing printed); printf("PRINT_TIME_TICK#: %lu msec -- %s\n", ms, s); free(s).
void DebugPrintTimeTick(MonoString* str) {
  if (!str) return;
  const std::string s = utf8(str);
  std::printf("PRINT_TIME_TICK#: %llu msec -- %s\n", static_cast<unsigned long long>(tsc_msec()), s.c_str());
  std::fflush(stdout);
}
SSN_ICALL("Sce.PlayStation.Core.Runtime.DiagnosticsNative::DebugPrintTimeTick", DebugPrintTimeTick);

// DebugPrintTimeTickPrefix(string) — 0x300740: the same with printf("%s %lu msec\n", s, ms).
void DebugPrintTimeTickPrefix(MonoString* str) {
  if (!str) return;
  const std::string s = utf8(str);
  std::printf("%s %llu msec\n", s.c_str(), static_cast<unsigned long long>(tsc_msec()));
  std::fflush(stdout);
}
SSN_ICALL("Sce.PlayStation.Core.Runtime.DiagnosticsNative::DebugPrintTimeTickPrefix", DebugPrintTimeTickPrefix);

}  // namespace
}  // namespace ssn::psm
