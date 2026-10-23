// Sce.PlayStation.Core.Graphics.PsmFrameBuffer (libScePsm.sprx, FW 4.03).
#include "host/boundary.h"
#include "libScePsm/error.h"
#include "libScePsm/graphics/core.h"
#include "libScePsm/graphics/objects.h"

namespace ssn::psm::gfx {
namespace {

// Create(out handle) — 0x1f0b60: needs the graphics context (0x80b30021 otherwise).
int32_t FrameBuffer_Create(int32_t* out) {
  if (!Core::get().created()) return kErrorGraphicsSystem;
  *out = Objects::get().add_frame_buffer(std::make_shared<FrameBuffer>());
  return kOk;
}
SSN_ICALL("Sce.PlayStation.Core.Graphics.PsmFrameBuffer::Create", FrameBuffer_Create);

int32_t FrameBuffer_Delete(int32_t h) {  // 0x1f0de0
  auto& o = Objects::get();
  o.release(o.frame_buffers(), h);
  return kOk;
}
SSN_ICALL("Sce.PlayStation.Core.Graphics.PsmFrameBuffer::Delete", FrameBuffer_Delete);

int32_t FrameBuffer_AddRef(int32_t h) {  // 0x1f0e80
  auto& o = Objects::get();
  return o.add_ref(o.frame_buffers(), h) ? kOk : kErrorObjectDisposed;
}
SSN_ICALL("Sce.PlayStation.Core.Graphics.PsmFrameBuffer::AddRef", FrameBuffer_AddRef);

// SetColorTarget(fb, pixelBuffer (0 = detach), level, cubeFace, out status) — 0x1f0f90:
// attaches through 0x1fc250 and returns the frame buffer's status byte (+72). The
// status computation inside 0x1fc250 is not traced yet: the host reports 1 while a
// colour target is attached.
int32_t FrameBuffer_SetColorTarget(int32_t h, int32_t pb, int32_t level, int32_t face, int32_t* status) {
  FrameBuffer* fb = Objects::get().frame_buffer(h);
  if (!fb) return kErrorObjectDisposed;
  (void)face;
  fb->color = pb ? Objects::get().pixel_buffer_ref(pb) : nullptr;
  fb->color_level = level;
  *status = fb->color ? 1 : 0;
  return kOk;
}
SSN_ICALL("Sce.PlayStation.Core.Graphics.PsmFrameBuffer::SetColorTarget", FrameBuffer_SetColorTarget);

// SetDepthTarget(fb, pixelBuffer, level, cubeFace, out status) — 0x1f11a0 (via 0x1fc4e0).
int32_t FrameBuffer_SetDepthTarget(int32_t h, int32_t pb, int32_t level, int32_t face, int32_t* status) {
  FrameBuffer* fb = Objects::get().frame_buffer(h);
  if (!fb) return kErrorObjectDisposed;
  (void)face;
  fb->depth = pb ? Objects::get().pixel_buffer_ref(pb) : nullptr;
  fb->depth_level = level;
  *status = fb->color ? 1 : 0;
  return kOk;
}
SSN_ICALL("Sce.PlayStation.Core.Graphics.PsmFrameBuffer::SetDepthTarget", FrameBuffer_SetDepthTarget);

}  // namespace
}  // namespace ssn::psm::gfx
