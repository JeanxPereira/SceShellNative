// Sce.PlayStation.Core.Graphics.PsmGraphicsDevice (libScePsm.sprx, FW 4.03).
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstring>
#include <thread>

#include "host/boundary.h"
#include "libScePsm/error.h"
#include "libScePsm/graphics/core.h"

namespace ssn::psm::gfx {

std::atomic<void (*)()> g_vblank_callback{nullptr};  // qword_5273D0
// XmlFvKZlm7k (0x1e4050) / m7QCwDRxED4 (0x1e4060).
void set_vblank_callback(void (*cb)()) { g_vblank_callback = cb; }

namespace {

bool g_device = false;  // libScePsm 0x53f4d0

// Create() — 0x1e8bd0: creates the singleton and returns its pointer as the handle
// (a non-zero value); a second Create returns 0x80b30021.
int32_t Create() {
  if (g_device) return kErrorGraphicsSystem;
  g_device = true;
  return 1;
}
SSN_ICALL("Sce.PlayStation.Core.Graphics.PsmGraphicsDevice::Create", Create);

// Delete(handle) — 0x1e8c10.
int32_t Delete(int32_t h) {
  if (h != 0) g_device = false;
  return kOk;
}
SSN_ICALL("Sce.PlayStation.Core.Graphics.PsmGraphicsDevice::Delete", Delete);

// GetVideoOutStatus(handle, ref GraphicsDeviceVideoOutStatus) — 0x1e8db0:
//   +0 width, +4 height  (sceVideoOutGetResolutionStatus of port 0; 1920x1080 on failure)
//   +8 refresh rate      (from the status' refresh-rate code: 1 23.976, 2 50, 3 59.94,
//                         4 24, 5 30, 6 29.97, 9 60, 13 119.88, 35 89.91; unknown codes
//                         fall back to 59.94 — floats at 0x350364.. in the module)
//   +12 VR refresh rate  (initialised to 119.88; set from port 1 when the pipeline is VR)
//   +16 mode flags       (1 system default, 2 VR, 4/8 pipeline layout / HMD2)
// The host video out is a non-VR 1080p port in its system default mode.
int32_t GetVideoOutStatus(int32_t h, uint8_t* st) {
  if (h == 0 || !g_device) return kErrorGraphicsSystem;
  const VideoOut& vo = Core::get().video_out;
  float rate = 59.94005584716797f;
  switch (vo.refresh_code) {
    case 1: rate = 23.976022720336914f; break;
    case 2: rate = 50.0f; break;
    case 4: rate = 24.0f; break;
    case 5: rate = 30.0f; break;
    case 6: rate = 29.970027923583984f; break;
    case 9: rate = 60.0f; break;
    case 13: rate = 119.88011169433594f; break;
    case 35: rate = 89.91008758544922f; break;
  }
  const float vr_rate = 119.88011169433594f;  // 0x42efc29e
  std::memcpy(st, &vo.width, 4);
  std::memcpy(st + 4, &vo.height, 4);
  std::memcpy(st + 8, &rate, 4);
  std::memcpy(st + 12, &vr_rate, 4);
  st[16] = 1;
  return kOk;
}
SSN_ICALL("Sce.PlayStation.Core.Graphics.PsmGraphicsDevice::GetVideoOutStatus", GetVideoOutStatus);

// GetVideoOutMode(handle, ref mode) — 0x1e8d50: bit 0 system-default, bit 1 VR.
int32_t GetVideoOutMode(int32_t h, uint32_t* mode) {
  if (h == 0 || !g_device) return kErrorGraphicsSystem;
  *mode = 1;
  return 0;  // the native returns the VR query's result (0: not VR)
}
SSN_ICALL("Sce.PlayStation.Core.Graphics.PsmGraphicsDevice::GetVideoOutMode", GetVideoOutMode);

// IsInitializedVBlankEvent() — 0x1e8c40: both vblank equeues exist. The host has a
// vblank source (presentation is FIFO-paced).
int32_t IsInitializedVBlankEvent() { return 1; }
SSN_ICALL("Sce.PlayStation.Core.Graphics.PsmGraphicsDevice::IsInitializedVBlankEvent", IsInitializedVBlankEvent);

}  // namespace

// The vblank event of the video out port (sceVideoOutSysAddVblankEvent2 on port 0, added at
// 0x2ce0e0 to the equeues qword_5273C0 "ScePsmVBlankMainThr" and qword_5273C8
// "ScePsmVBlankCheckerThr"). HOST VALUE: the PC has no PS5 video out; its vblanks are a clock at
// the port's refresh rate (the rate GetVideoOutStatus reports), counted from the first use.
std::chrono::steady_clock::time_point next_vblank() {
  using namespace std::chrono;
  static const steady_clock::time_point epoch = steady_clock::now();
  const VideoOut& vo = Core::get().video_out;
  double hz = 59.94005584716797;
  switch (vo.refresh_code) {
    case 1: hz = 23.976022720336914; break;
    case 2: hz = 50.0; break;
    case 4: hz = 24.0; break;
    case 5: hz = 30.0; break;
    case 6: hz = 29.970027923583984; break;
    case 9: hz = 60.0; break;
    case 13: hz = 119.88011169433594; break;
    case 35: hz = 89.91008758544922; break;
  }
  const duration<double> period(1.0 / hz);
  const auto since = duration<double>(steady_clock::now() - epoch);
  const double n = std::floor(since / period) + 1.0;
  return epoch + duration_cast<steady_clock::duration>(period * n);
}

namespace {

// WaitVBlank(timeout_ms) — 0x1e8c60: drains the vblank equeue (sceKernelWaitEqueue with a
// zero timeout until ETIMEDOUT), then waits for the next vblank event with a timeout of
// timeout_ms (200 ms when <= 0, capped at 0x7FFFFFFF us); a timeout returns the remaining
// time / 1000. Then the vblank callback qword_5273D0, which only the compositor-shared
// paths install (none on the host).
int32_t WaitVBlank(int32_t timeout_ms) {
  using namespace std::chrono;
  (void)timeout_ms;  // the host's next vblank is always within one refresh period (< 200 ms)
  static const bool noted = (host::note_unimplemented("HOST VALUE video-out vblank: a clock at the port's refresh rate (no PS5 video out)"), true);
  (void)noted;
  std::this_thread::sleep_until(next_vblank());
  if (void (*cb)() = g_vblank_callback.load()) cb();
  return 0;
}
SSN_ICALL("Sce.PlayStation.Core.Graphics.PsmGraphicsDevice::WaitVBlank", WaitVBlank);

// ReceiveSetModeEvent(handle) — 0x1e9080: without the device 0x80b30021; otherwise the
// set-mode equeue (qword_53F500) is polled with a zero timeout: 1 when an event is a
// set-mode (-13) event, 0 when none is pending. The host video out never changes mode.
int32_t ReceiveSetModeEvent(int32_t h) {
  (void)h;
  if (!g_device) return kErrorGraphicsSystem;
  return 0;
}
SSN_ICALL("Sce.PlayStation.Core.Graphics.PsmGraphicsDevice::ReceiveSetModeEvent", ReceiveSetModeEvent);

// IsInitializedSharedVideoMemory(handle, type) — 0x1e92d0: 0 when the shared DMEM
// record exists and is initialised, 0x80b30021 otherwise. The host has no compositor
// shared video memory.
int32_t IsInitializedSharedVideoMemory(int32_t h, uint32_t type) {
  (void)h;
  (void)type;
  return kErrorGraphicsSystem;
}
SSN_ICALL("Sce.PlayStation.Core.Graphics.PsmGraphicsDevice::IsInitializedSharedVideoMemory", IsInitializedSharedVideoMemory);

// Lock/TryLock/UnlockSharedVideoMemory(handle, type) — 0x1e9180/0x1e91f0/0x1e9260:
// handle == 0 or type > 2 -> 0x80b30021; without the compositor's lock callbacks 0.
int32_t LockSharedVideoMemory(int32_t h, uint32_t type) { return (h == 0 || type > 2) ? kErrorGraphicsSystem : kOk; }
SSN_ICALL("Sce.PlayStation.Core.Graphics.PsmGraphicsDevice::LockSharedVideoMemory", LockSharedVideoMemory);
SSN_ICALL("Sce.PlayStation.Core.Graphics.PsmGraphicsDevice::TryLockSharedVideoMemory", LockSharedVideoMemory);
SSN_ICALL("Sce.PlayStation.Core.Graphics.PsmGraphicsDevice::UnlockSharedVideoMemory", LockSharedVideoMemory);

}  // namespace
}  // namespace ssn::psm::gfx
