// Sce.PlayStation.Orbis.Graphics.CanvasTexture2DNative icalls (libScePsm.sprx, FW 4.03) and the
// host side of sampling a canvas texture (pixel-buffer type 3).
//
// The managed CanvasTexture2D creates a type-3 pixel buffer (0x0, Rgba, mipmap) and binds it
// to a compositor canvas with SetId. On the console the compositor supplies the descriptor of
// the canvas's current frame and libScePsm samples it directly; the host copies the newest
// frame of the host canvas table (libSceComposite/canvas.h, a declared host value) into the
// texture's own image right before a draw samples it.
#include <cstdint>

#include "core/log.h"
#include "host/boundary.h"
#include "libSceComposite/canvas.h"
#include "libScePsm/error.h"
#include "libScePsm/graphics/objects.h"
#include "libScePsm/graphics/types.h"

namespace ssn::psm::gfx {
namespace {

// 0x1fbf60: resolves the bound id to a canvas handle (+148). For id type 2 (CanvasHandle) the
// handle is the id itself; type 1 (AppId: the canvas of an application process) asks the
// compositor for that application's canvas, which the host does not have.
void resolve(PixelBuffer& pb) {
  if (pb.canvas_id_type == 2) {
    pb.canvas_handle = pb.canvas_id;
  } else {
    host::note_unimplemented("libScePsm CanvasTexture2D by AppId (0x1fbf60): no application canvases on the host");
    pb.canvas_handle = 0xFFFFFFFFu;
  }
}

// int SetId(int handle, uint id, int idType) — 0x1f1dc0: unknown handle -> 0x80b30005; else
// +140 = idType, +144 = id, 0x1fbf60, result 0.
int32_t SetId(int32_t h, uint32_t id, int32_t id_type) {
  PixelBuffer* pb = Objects::get().pixel_buffer(h);
  if (!pb) return kErrorObjectDisposed;
  pb->canvas_id_type = static_cast<uint32_t>(id_type);
  pb->canvas_id = id;
  resolve(*pb);
  pb->canvas_generation = 0;
  return kOk;
}

// int IsAvailable(int handle, out int available) — 0x1f1ef0: *available = 0; unknown handle
// -> 0x80b30005; an AppId texture is resolved again; available when the compositor knows
// the canvas (libSceComposite NID SpfMcnnpfDA succeeds with a state other than -1).
int32_t IsAvailable(int32_t h, int32_t* available) {
  if (available) *available = 0;
  PixelBuffer* pb = Objects::get().pixel_buffer(h);
  if (!pb) return kErrorObjectDisposed;
  if (pb->canvas_id_type == 1) resolve(*pb);
  if (available && composite::canvas_exists(pb->canvas_handle)) *available = 1;
  return kOk;
}

// int IsVirtualCanvas(int handle, out int isVirtual) — 0x1f2070: 1 only for a known canvas the
// compositor reports as virtual (NID Bt2J_lHKAbQ); host canvases are plain surfaces.
int32_t IsVirtualCanvas(int32_t h, int32_t* is_virtual) {
  if (is_virtual) *is_virtual = 0;
  PixelBuffer* pb = Objects::get().pixel_buffer(h);
  if (!pb) return kErrorObjectDisposed;
  return kOk;
}

}  // namespace

void sync_canvas_texture(PixelBuffer& pb) {
  int32_t w = 0, h = 0;
  std::vector<uint8_t> rgba;
  if (!composite::canvas_take(pb.canvas_handle, pb.canvas_generation, w, h, rgba)) return;
  auto& dev = ssn::gfx::device();
  if (!pb.image.valid() || static_cast<int32_t>(pb.image.width) != w || static_cast<int32_t>(pb.image.height) != h) {
    if (pb.image.valid()) dev.destroy_image(pb.image);
    VkFormat vf;
    VkComponentMapping sw;
    vk_format_for(kPfRgba, vf, sw);
    pb.image = dev.create_image(w, h, 1, vf, VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT);
    if (!pb.image.valid()) return;
  }
  dev.upload_image(pb.image, 0, 0, 0, w, h, rgba.data(), rgba.size());
}

SSN_ICALL("Sce.PlayStation.Orbis.Graphics.CanvasTexture2DNative::SetId", SetId);
SSN_ICALL("Sce.PlayStation.Orbis.Graphics.CanvasTexture2DNative::IsAvailable", IsAvailable);
SSN_ICALL("Sce.PlayStation.Orbis.Graphics.CanvasTexture2DNative::IsVirtualCanvas", IsVirtualCanvas);

}  // namespace ssn::psm::gfx
