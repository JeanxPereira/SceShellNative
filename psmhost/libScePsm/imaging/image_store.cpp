#include "libScePsm/imaging/image_store.h"

#include "libScePsm/imaging/image_core.h"

namespace ssn::psm::imaging {

// Snapshot for the texture side; the format choice is FromImageInstance's (0x1ee9b0):
// sub_296920(impl) == 1 (Alpha8) -> 10, == 9 (Rgba1010102) -> 26, otherwise 1.
std::optional<DecodedImage> find_image(int32_t handle) {
  ImageState s;
  if (!snapshot(static_cast<uint32_t>(handle), &s)) return std::nullopt;
  DecodedImage d;
  d.width = s.width;
  d.height = s.height;
  switch (s.internal_format) {
    case kFmtRgba8888: d.image_format = 0; break;
    case kFmtA8: d.image_format = 1; break;
    case kFmtDxt1: d.image_format = 2; break;
    case kFmtDxt3: d.image_format = 3; break;
    case kFmtDxt5: d.image_format = 4; break;
    case kFmtBc4U: d.image_format = 5; break;
    case kFmtBc5U: d.image_format = 6; break;
    case kFmtBc6U: d.image_format = 7; break;
    case kFmtBc7U: d.image_format = 8; break;
    case kFmtRgba1010102: d.image_format = 9; break;
    default: d.image_format = 255; break;
  }
  if (!s.has_impl) d.image_format = 255;
  d.texture_pixel_format = d.image_format == 1 ? 10u : d.image_format == 9 ? 26u : 1u;
  d.bytes_per_pixel = d.image_format == 1 ? 1u : 4u;
  d.shared_video_memory_type = s.shared_video_memory_type;
  d.alpha_channel_type = s.alpha_channel_type;
  d.pixels = s.pixels;
  return d;
}

std::optional<CompressedImage> link_compressed_image(int32_t handle) {
  CompressedImage c;
  if (!frame0(static_cast<uint32_t>(handle), &c.level0)) return std::nullopt;
  auto d = find_image(handle);
  if (!d) return std::nullopt;
  c.width = d->width;
  c.height = d->height;
  c.image_format = d->image_format;
  c.shared_video_memory_type = d->shared_video_memory_type;
  return c;
}

}  // namespace ssn::psm::imaging
