// Sce.PlayStation.Core.Graphics.PsmGraphicsContext (libScePsm.sprx, FW 4.03).
#include <cstring>
#include <thread>

#include "common/mono_array.h"
#include "core/log.h"
#include "gfx/vk_device.h"
#include "host/boundary.h"
#include "libScePsm/error.h"
#include "libScePsm/graphics/core.h"
#include "libScePsm/graphics/draw.h"
#include "platform/window.h"

namespace ssn::psm::gfx {
namespace {

// Create(w, h, colorFormat, depthFormat, multiSample, out result) — 0x1e5d50.
//   Only one context exists (global 0x53f4c0); a second Create returns
//   0x80b30021 with result unchanged. w/h > 0x1000 -> 0x80b30003; formats > 0x1b or
//   multisample > 2 -> 0x80b30001. result = 1 once the context exists.
int32_t Create(int32_t w, int32_t h, uint32_t color, uint32_t depth, uint32_t msaa, int32_t* result) {
  Core& c = Core::get();
  if (c.created()) return kErrorGraphicsSystem;
  if (uint32_t(w) > 0x1000 || uint32_t(h) > 0x1000) return kErrorOutOfRange;
  if (color > 0x1b || msaa > 2 || depth >= 0x1c) return kErrorInvalidParameter;
  const int32_t rc = c.create();
  *result = c.created() ? 1 : 0;
  return rc == 0 ? kOk : kErrorGraphicsSystem;
}
SSN_ICALL("Sce.PlayStation.Core.Graphics.PsmGraphicsContext::Create", Create);

int32_t Delete(int32_t) {
  Core::get().destroy();
  return kOk;
}
SSN_ICALL("Sce.PlayStation.Core.Graphics.PsmGraphicsContext::Delete", Delete);

// GetScreenSizes(ImageSize[] sizes, out count) — 0x1e74a0: 1920x1080, plus 3840x2160
// when the 4K query (NID rNRtm1uioyY) reports support; count = 1 or 2. Entries past
// the supported ones are zeroed. The host presents a 1080p video out.
int32_t GetScreenSizes(MonoArray* sizes, int32_t* count) {
  constexpr int32_t kSizes[1][2] = {{1920, 1080}};
  *count = 1;
  if (sizes) {
    ArrayView v = view(sizes);
    auto* out = reinterpret_cast<int32_t*>(v.data);
    for (size_t i = 0; i < v.length; ++i) {
      out[2 * i] = i < 1 ? kSizes[i][0] : 0;
      out[2 * i + 1] = i < 1 ? kSizes[i][1] : 0;
    }
  }
  return kOk;
}
SSN_ICALL("Sce.PlayStation.Core.Graphics.PsmGraphicsContext::GetScreenSizes", GetScreenSizes);

// GetScreenInfo(handle, out w, out h, out color, out depth, out msaa) — 0x1e75e0:
// the display buffer size, colour format = 1 (Rgba, byte 0x53f5ac set by Create),
// depth = None, multisample = None.
int32_t GetScreenInfo(int32_t, int32_t* w, int32_t* h, uint32_t* color, uint32_t* depth, uint32_t* msaa) {
  Core& c = Core::get();
  if (!c.created()) return kErrorGraphicsSystem;
  *w = c.screen_width();
  *h = c.screen_height();
  *color = kPfRgba;
  *depth = kPfNone;
  *msaa = 0;
  return kOk;
}
SSN_ICALL("Sce.PlayStation.Core.Graphics.PsmGraphicsContext::GetScreenInfo", GetScreenInfo);

// GetCaps(handle, out GraphicsCapsState) — 0x1e76e0: 72 bytes assembled by Create from
// constants 0x34fe40 (32 bytes), 0x34edc0 (16), the value 16, 0x34e9f0 (16) and 16.0f.
int32_t GetCaps(int32_t, uint8_t* caps) {
  if (!Core::get().created()) return kErrorGraphicsSystem;
  static constexpr int32_t kHead[8] = {0xffff, 4096, 4096, 4096, 2048, 4096, 256, 224};  // 0x34fe40
  static constexpr int32_t kMid[4] = {16, 15, 32, 16};                                   // 0x34edc0
  static constexpr float kTail[4] = {16.0f, 0.125f, 8.0f, 1.0f};                         // 0x34e9f0
  const int32_t sixteen = 16;
  const float sixteen_f = 16.0f;
  std::memcpy(caps, kHead, 32);
  std::memcpy(caps + 32, kMid, 16);
  std::memcpy(caps + 48, &sixteen, 4);
  std::memcpy(caps + 52, kTail, 16);
  std::memcpy(caps + 68, &sixteen_f, 4);
  return kOk;
}
SSN_ICALL("Sce.PlayStation.Core.Graphics.PsmGraphicsContext::GetCaps", GetCaps);

// Update(handle, GraphicsUpdate, ref GraphicsState, int[] handles) — 0x1e6c50 -> 0x1f32a0
// (applies the dirty state and handle slots; handles must hold >= 24 entries).
int32_t Update(int32_t, uint32_t update, const GraphicsState* state, MonoArray* handles) {
  Core& c = Core::get();
  if (!c.created()) return kErrorGraphicsSystem;
  ArrayView v = view(handles);
  if (!v.data) return kErrorArgumentNull;
  if (v.length < 24) return kErrorOutOfRange;
  c.apply(update, state, reinterpret_cast<const int32_t*>(v.data), static_cast<int32_t>(v.length));
  return kOk;
}
SSN_ICALL("Sce.PlayStation.Core.Graphics.PsmGraphicsContext::Update", Update);

// Clear(handle, ClearMask) — 0x1e7020: mask > 7 -> 0x80b30001; colour clears the bound
// target with the state's clear colour (0x7e10); depth/stencil clear the depth target
// with ClearDepth / ClearStencil (0xa1a0). The native clear takes no rectangle.
int32_t Clear(int32_t, uint32_t mask) {
  Core& c = Core::get();
  if (!c.created()) return kErrorGraphicsSystem;
  if (mask > 7) return kErrorInvalidParameter;
  if (!c.bind_frame_buffer()) return kErrorInvalidOperation;
  return clear_target(mask);
}
SSN_ICALL("Sce.PlayStation.Core.Graphics.PsmGraphicsContext::Clear", Clear);

// DrawArrays(handle, mode, first, count, repeat, update, ref state, handles) — 0x1e7280.
int32_t DrawArrays(int32_t, uint32_t mode, int32_t first, int32_t count, int32_t repeat, uint32_t update, const GraphicsState* state,
                   MonoArray* handles) {
  Core& c = Core::get();
  if (!c.created()) return kErrorGraphicsSystem;
  ArrayView v = view(handles);
  if (!v.data) return kErrorArgumentNull;
  if (v.length < 24) return kErrorOutOfRange;
  c.apply(update, state, reinterpret_cast<const int32_t*>(v.data), static_cast<int32_t>(v.length));
  return draw_arrays(mode, first, count, repeat);
}
SSN_ICALL("Sce.PlayStation.Core.Graphics.PsmGraphicsContext::DrawArrays", DrawArrays);

// BeginRender(handle) — 0x1e7de0: runs the registered per-frame callbacks; 0.
int32_t BeginRender(int32_t) { return Core::get().created() ? kOk : kErrorGraphicsSystem; }
SSN_ICALL("Sce.PlayStation.Core.Graphics.PsmGraphicsContext::BeginRender", BeginRender);

// SwapBuffers(handle, repeatSubmitCount) — 0x1e6d40: submits the frame and flips the
// display buffer. The host presents the screen target to the window (FIFO) and pumps
// window messages.
int32_t SwapBuffers(int32_t, int32_t) {
  Core& c = Core::get();
  if (!c.created()) return kErrorGraphicsSystem;
  ssn::gfx::device().present(c.screen(), c.capture_path_for_next_frame());
  platform::main_window().pump();
  return kOk;
}
SSN_ICALL("Sce.PlayStation.Core.Graphics.PsmGraphicsContext::SwapBuffers", SwapBuffers);

// WaitVBlank(handle, waitCount, withCallback) — 0x1e77b0. Without the context or with
// waitCount <= 0: 0x80b30021. Otherwise, waitCount times:
//  - while the post-swap-buffer list holds an object (qword_527340, read under its lock): with
//    withCallback the objects' vtable +24 hook (0x1e4470); a repeat submit of the display buffer
//    (0x2bc0(ctx, 0), 0x78f0(buffer, 4), 0x2e20 — the calls SwapBuffers 0x1e6d40 makes for each
//    repeatSubmitCount), or 0x2dd0 in virtual-canvas mode; the end-of-frame state SwapBuffers
//    also sets (+328 = 1, +332 = 0, byte 0x53f658 = 1); with withCallback the vtable +16 hook
//    (0x1e4390). Result 0.
//  - otherwise: drains the vblank equeue qword_5273C0 with zero timeouts, then waits for the
//    next vblank with a timeout of 1000 * (int)frame_ms + 1000 us (frame_ms: float 0x4fa6dc,
//    16.683 at 59.94 Hz; 200000 us when negative); a timeout answers the remaining time / 1000,
//    a failure is printed as "[PG ERR] WaitVBlank ret=%x".
// The managed renderer calls it on the frames that swap nothing (UIRenderer.End ->
// ScreenRenderer.Render(swap: false), and End's VBlankSkip waits), so it is what paces the main
// loop (UISystem.Update: input, FrameTaskManager.Update, scenes) on those frames, exactly as
// SwapBuffers does on the others. The ShellUI registers its BGLayer root in the list at boot
// (eboot InitializeNative 0x8DB30), so from then on the repeat submit is taken.
// Host: the repeat submit is presenting the screen image again (the host's flip, paced like
// SwapBuffers); the registrants' hooks are not called (see Core::add_post_swap_object). The
// vblank wait uses the host's vblank clock (next_vblank, graphics_device.cpp).
int32_t WaitVBlank(int32_t, int32_t wait_count, uint8_t with_callback) {
  (void)with_callback;
  Core& c = Core::get();
  if (wait_count <= 0 || !c.created()) return kErrorGraphicsSystem;
  for (int32_t i = 0; i < wait_count; ++i) {
    if (c.has_post_swap_objects()) {
      ssn::gfx::device().present(c.screen(), c.capture_path_for_next_frame());
      platform::main_window().pump();
    } else {
      std::this_thread::sleep_until(next_vblank());
    }
  }
  return kOk;
}
SSN_ICALL("Sce.PlayStation.Core.Graphics.PsmGraphicsContext::WaitVBlank", WaitVBlank);

// IsVirtualCanvasScreen(handle) — 0x1e8ba0: bit 1 of the device mode flags (+268);
// the host device never runs in virtual-canvas mode.
int32_t IsVirtualCanvasScreen(int32_t h) {
  if (h == 0 || !Core::get().created()) return kErrorGraphicsSystem;
  return 0;
}
SSN_ICALL("Sce.PlayStation.Core.Graphics.PsmGraphicsContext::IsVirtualCanvasScreen", IsVirtualCanvasScreen);

// UpdateResolution(handle, w, h) — 0x1e8470: w in 1..0xf00 and h in 1..0x86f, else
// 0x80b30003; with the compositor flag set only 3840x2160 / 2560x1440 / 1920x1080 are
// accepted (0x80b30001 otherwise); then the render resolution is changed (0x1d50).
int32_t UpdateResolution(int32_t, uint32_t w, uint32_t h) {
  Core& c = Core::get();
  if (!c.created()) return kErrorGraphicsSystem;
  if (w - 1 > 0xeff || h - 1 >= 0x870) return kErrorOutOfRange;
  if (int32_t(w) != c.screen_width() || int32_t(h) != c.screen_height())
    host::note_unimplemented("libScePsm PsmGraphicsContext::UpdateResolution to a size other than the display buffer");
  return kOk;
}
SSN_ICALL("Sce.PlayStation.Core.Graphics.PsmGraphicsContext::UpdateResolution", UpdateResolution);

// UpdateGlobalConstantBuffer<T>(handle, ref T value, int size) — 0x1f1c80: size < 0 ->
// 0x80b30001; when size fits the buffer, its first `size` bytes are overwritten
// (otherwise nothing happens, still 0). The buffer is the globalConstantBuffer every
// embedded shader reads, bound to slot 7 of both stages by 0x1f32a0.
int32_t UpdateGlobalConstantBuffer(int32_t, const uint8_t* value, int32_t size) {
  Core& c = Core::get();
  if (size < 0) return kErrorInvalidParameter;
  auto& g = c.global_constants();
  if (value && size_t(size) <= g.size()) std::memcpy(g.data(), value, size);
  return kOk;
}
SSN_ICALL("Sce.PlayStation.Core.Graphics.PsmGraphicsContext::UpdateGlobalConstantBuffer", UpdateGlobalConstantBuffer);

}  // namespace
}  // namespace ssn::psm::gfx
