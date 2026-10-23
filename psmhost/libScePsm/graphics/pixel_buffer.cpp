// Sce.PlayStation.Core.Graphics.PsmPixelBuffer / PsmTexture (libScePsm.sprx, FW 4.03).
#include <algorithm>
#include <bit>
#include <cstring>

#include "common/mono_array.h"
#include "common/mono_util.h"
#include "core/log.h"
#include "host/boundary.h"
#include "libScePsm/error.h"
#include "libScePsm/graphics/core.h"
#include "libScePsm/graphics/objects.h"

namespace ssn::psm::gfx {
namespace {

constexpr int32_t kNotFound = static_cast<int32_t>(0x80b30005u);  // unknown handle (native: -2135752699)

int32_t mip_levels(int32_t w, int32_t h) { return 1 + std::bit_width(static_cast<uint32_t>(std::max(w, h))) - 1; }

// Create(PixelBufferType, w, h, mipmap, PixelFormat, PixelBufferOption, InternalOption,
//        memoryBlock, out handle) — libScePsm 0x1ece20: type 1 Texture2D (0x1f52f0),
// 2 TextureCube, 3/4 other kinds, anything else 0x80b30003; requires the graphics
// context (0x80b30021 otherwise). The texture is created by 0x1f5420.
int32_t PixelBuffer_Create(uint32_t type, int32_t w, int32_t h, bool mipmap, uint32_t format, uint32_t option, uint32_t option2,
                           int32_t memory_block, int32_t* out) {
  if (!Core::get().created()) return kErrorGraphicsSystem;
  if (type < 1 || type > 4) return kErrorOutOfRange;
  if (type == 3) {
    // Canvas texture (0x1ece20 case 3, 152-byte object): no storage of its own; its pixels
    // are the compositor canvas it is bound to (CanvasTexture2DNative.SetId), pulled when drawn.
    auto pb = std::make_shared<PixelBuffer>();
    pb->type = type;
    pb->width = w;
    pb->height = h;
    pb->format = format;
    pb->option = option;
    pb->levels = 1;
    *out = Objects::get().add_pixel_buffer(std::move(pb));
    return kOk;
  }
  if (type != 1) {
    host::note_unimplemented("libScePsm PsmPixelBuffer::Create type " + std::to_string(type));
    return kErrorNotSupported;
  }
  (void)option2;
  (void)memory_block;
  auto pb = std::make_shared<PixelBuffer>();
  pb->type = type;
  pb->width = w;
  pb->height = h;
  pb->format = format;
  pb->option = option;
  pb->levels = mipmap ? mip_levels(w, h) : 1;
  VkFormat vf;
  VkComponentMapping sw;
  if (w <= 0 || h <= 0 || !vk_format_for(format, vf, sw)) {
    log::warn("psm.gfx", "PixelBuffer::Create unsupported {}x{} format {}", w, h, format);
    return kErrorOutOfRange;
  }
  const bool depth = format >= kPfDepth16 && format <= kPfDepth24Stencil8;
  VkImageUsageFlags usage = VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT;
  if (option & 1) usage |= depth ? VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT : VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT;
  pb->image = ssn::gfx::device().create_image(w, h, pb->levels, vf, usage);
  if (!pb->image.valid()) return kErrorOutOfMemory;
  if (!depth) {
    // Apply the format's component mapping through a dedicated view.
    VkImageViewCreateInfo vi{VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO};
    vi.image = pb->image.image;
    vi.viewType = VK_IMAGE_VIEW_TYPE_2D;
    vi.format = vf;
    vi.components = sw;
    vi.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, static_cast<uint32_t>(pb->levels), 0, 1};
    vkDestroyImageView(ssn::gfx::device().device(), pb->image.view, nullptr);
    vkCreateImageView(ssn::gfx::device().device(), &vi, nullptr, &pb->image.view);
  }
  *out = Objects::get().add_pixel_buffer(std::move(pb));
  return kOk;
}
SSN_ICALL("Sce.PlayStation.Core.Graphics.PsmPixelBuffer::Create", PixelBuffer_Create);
}  // namespace

int32_t PixelBuffer_CreateForImage(int32_t w, int32_t h, uint32_t pf, int32_t* out) {
  return PixelBuffer_Create(1, w, h, false, pf, 0, 0, 0, out);
}

namespace {

// Delete(handle) — 0x1ed650: drops one reference (sub_1f27a0 with the pixel-buffer table); always 0.
int32_t PixelBuffer_Delete(int32_t h) {
  auto& o = Objects::get();
  o.release(o.pixel_buffers(), h);
  return kOk;
}
SSN_ICALL("Sce.PlayStation.Core.Graphics.PsmPixelBuffer::Delete", PixelBuffer_Delete);

// AddRef(handle) — 0x1ed6f0.
int32_t PixelBuffer_AddRef(int32_t h) {
  auto& o = Objects::get();
  return o.add_ref(o.pixel_buffers(), h) ? kOk : kNotFound;
}
SSN_ICALL("Sce.PlayStation.Core.Graphics.PsmPixelBuffer::AddRef", PixelBuffer_AddRef);

// SetName(handle, name) — 0x1ed800 (debug name of the GPU object).
int32_t PixelBuffer_SetName(int32_t h, MonoString* name) {
  PixelBuffer* pb = Objects::get().pixel_buffer(h);
  if (!pb) return kNotFound;
  pb->name = utf8(name);
  return kOk;
}
SSN_ICALL("Sce.PlayStation.Core.Graphics.PsmPixelBuffer::SetName", PixelBuffer_SetName);

// GetInfo(handle, out type, out w, out h, out levels, out format, out option) — 0x1ede20.
int32_t PixelBuffer_GetInfo(int32_t h, uint32_t* type, int32_t* w, int32_t* hh, int32_t* level, uint32_t* format, uint32_t* option) {
  PixelBuffer* pb = Objects::get().pixel_buffer(h);
  if (!pb) return kNotFound;
  *type = pb->type;
  *w = pb->width;
  *hh = pb->height;
  *level = pb->levels;
  *format = pb->format;
  *option = pb->option;
  return kOk;
}
SSN_ICALL("Sce.PlayStation.Core.Graphics.PsmPixelBuffer::GetInfo", PixelBuffer_GetInfo);

// GetMemorySize(handle, out size) — 0x1edcd0. The native value is the GPU allocation
// (tiled); the host reports the untiled size of all levels.
int32_t PixelBuffer_GetMemorySize(int32_t h, int32_t* size) {
  PixelBuffer* pb = Objects::get().pixel_buffer(h);
  if (!pb) return kNotFound;
  int64_t total = 0;
  for (int32_t l = 0; l < pb->levels; ++l)
    total += int64_t(std::max(pb->width >> l, 1)) * std::max(pb->height >> l, 1) * pixel_format_bits(pb->format) / 8;
  *size = static_cast<int32_t>(total);
  return kOk;
}
SSN_ICALL("Sce.PlayStation.Core.Graphics.PsmPixelBuffer::GetMemorySize", PixelBuffer_GetMemorySize);

// SetFilter(handle, ref TextureFilter{mag, min, mip}) — 0x1efda0.
int32_t Texture_SetFilter(int32_t h, const uint8_t* filter) {
  PixelBuffer* pb = Objects::get().pixel_buffer(h);
  if (!pb) return kNotFound;
  pb->mag = filter[0];
  pb->min = filter[1];
  pb->mip = filter[2];
  return kOk;
}
SSN_ICALL("Sce.PlayStation.Core.Graphics.PsmTexture::SetFilter", Texture_SetFilter);

// SetWrap(handle, ref TextureWrap{s, t}) — 0x1eff80.
int32_t Texture_SetWrap(int32_t h, const uint8_t* wrap) {
  PixelBuffer* pb = Objects::get().pixel_buffer(h);
  if (!pb) return kNotFound;
  pb->wrap_s = wrap[0];
  pb->wrap_t = wrap[1];
  return kOk;
}
SSN_ICALL("Sce.PlayStation.Core.Graphics.PsmTexture::SetWrap", Texture_SetWrap);

int32_t Texture_SetMaxAnisotropy(int32_t h, float a) {
  PixelBuffer* pb = Objects::get().pixel_buffer(h);
  if (!pb) return kNotFound;
  pb->max_anisotropy = a;
  return kOk;
}
SSN_ICALL("Sce.PlayStation.Core.Graphics.PsmTexture::SetMaxAnisotropy", Texture_SetMaxAnisotropy);

}  // namespace

// Shared by SetPixels and the image paths: 0x1f4910.
//   level < levels, cubeFace <= 5, srcFormat <= 27 (0 = the texture's format);
//   the source must be the texture's format (or Rgba into any non-1010102 texture,
//   converted by 0x1f4e60 — not reproduced yet), pitch defaults to w*bpp/8, the rect
//   must lie inside the level and the source must hold pitch*(h-1)+offset+w*bpp/8 bytes.
int32_t upload_pixels(PixelBuffer& pb, int32_t level, int32_t face, const uint8_t* src, size_t src_bytes, uint32_t src_format,
                      int32_t offset, int32_t pitch, int32_t dx, int32_t dy, int32_t dw, int32_t dh) {
  if (!src) return kErrorArgumentNull;
  if (level < 0 || level >= pb.levels) return kErrorOutOfRange;
  if (face > 5 || src_format > 27) return kErrorInvalidParameter;
  if (pb.option & 8) return kErrorInvalidOperation;
  if (src_format == 0) src_format = pb.format;
  if (src_format != pb.format) {
    host::note_unimplemented("libScePsm Texture upload format conversion " + std::to_string(src_format) + "->" + std::to_string(pb.format));
    return kOk;
  }
  const int32_t bpp = pixel_format_bits(src_format);
  const int32_t row = dw * bpp / 8;
  if (pitch == 0) pitch = row;
  const int32_t lw = std::max(pb.width >> level, 1), lh = std::max(pb.height >> level, 1);
  if ((dx | dy | dw | dh | offset) < 0 || dx + dw > lw || dy + dh > lh || pitch < row) return kErrorOutOfRange;
  if (int64_t(pitch) * (dh - 1) + offset + row > int64_t(src_bytes)) return kErrorInvalidOperation;
  if (dw == 0 || dh == 0) return kOk;
  std::vector<uint8_t> packed(size_t(row) * dh);
  for (int32_t y = 0; y < dh; ++y) std::memcpy(packed.data() + size_t(y) * row, src + offset + size_t(y) * pitch, row);
  ssn::gfx::device().upload_image(pb.image, level, dx, dy, dw, dh, packed.data(), packed.size());
  return kOk;
}

namespace {
// SetPixels(handle, level, cubeFace, Array pixels, PixelFormat, offset, pitch, dx, dy, dw, dh) — 0x1f0270.
int32_t Texture_SetPixels(int32_t h, int32_t level, int32_t face, MonoArray* pixels, uint32_t format, int32_t offset, int32_t pitch,
                          int32_t dx, int32_t dy, int32_t dw, int32_t dh) {
  PixelBuffer* pb = Objects::get().pixel_buffer(h);
  if (!pb) return kNotFound;
  ArrayView v = view(pixels);
  // A negative width or height means the whole level; the array must then hold
  // exactly one level of texels in the texture's format.
  if ((dw | dh) < 0) {
    dw = std::max(pb->width >> std::max(level, 0), 1);
    dh = std::max(pb->height >> std::max(level, 0), 1);
    if (pixels && int64_t(pixel_format_bits(pb->format)) * dw * dh / 8 != int64_t(v.bytes())) return kErrorInvalidOperation;
  }
  return upload_pixels(*pb, level, face, v.data, v.bytes(), format, offset, pitch, dx, dy, dw, dh);
}
SSN_ICALL("Sce.PlayStation.Core.Graphics.PsmTexture::SetPixels", Texture_SetPixels);
}  // namespace

}  // namespace ssn::psm::gfx

#include "libScePsm/imaging/image_dds.h"
#include "libScePsm/imaging/image_store.h"

namespace ssn::psm::gfx {
int32_t upload_pixels(PixelBuffer& pb, int32_t level, int32_t face, const uint8_t* src, size_t src_bytes, uint32_t src_format,
                      int32_t offset, int32_t pitch, int32_t dx, int32_t dy, int32_t dw, int32_t dh);
int32_t PixelBuffer_CreateForImage(int32_t w, int32_t h, uint32_t pf, int32_t* out);
namespace {

// FromImageInstance(type, imageHandle, mipmap, PixelFormat, out handle) — 0x1ee9b0:
//   unknown image -> 0x80b30005; mipmap -> 0x80b30004 ("...not supported"); type other
//   than Texture2D -> 0x80b30004; a format other than None/Bgra -> 0x80b30004. The texture
//   gets the image's size and, for None, Rgba / Alpha / Rgba1010102 by the image format;
//   the decoded pixels are uploaded as they are (see imaging/image_store.h).
//   Images with a block-compressed ImageFormat (2..8: Dxt1..Bc7U, i.e. DDS) take the
//   compressed branch (from_compressed_image below) instead.
//
//   Shared video memory: the image's SharedVideoMemoryType (state +56; SystemAsset for the
//   ui3 resource textures) becomes the texture creation options (1 -> 4/2, 2 -> 4/4), and
//   0x1ee9b0 skips the upload only when that type is set AND the texture's +184 is 0.
//   sub_1F5420 sets +184 = 0 only when it reuses an entry of the shared system-asset table
//   (xmmword_527360: {w,h,GPU address} records, next entry dword_527374) whose memory already
//   holds the pixels from an earlier process; with an empty or absent table it allocates
//   through the shared allocator (qword_527398, +184 = 1) or falls back to ordinary memory
//   (+184 = 1, or 2 when the table had no matching record), and the pixels are uploaded.
//   The host has no shared asset table (nothing has populated one), so it always takes the
//   upload path.
int32_t from_compressed_image(const imaging::CompressedImage& img, int32_t* out);

int32_t Texture_FromImageInstance(uint32_t type, int32_t image, bool mipmap, uint32_t format, int32_t* out) {
  if (!Core::get().created()) return kErrorGraphicsSystem;
  auto img = imaging::find_image(image);
  if (!img) return kErrorObjectDisposed;
  if (mipmap || type != 1 || (format != kPfNone && format != kPfBgra)) return kErrorInvalidOperation;
  if (img->image_format - 2u <= 6u) {
    auto c = imaging::link_compressed_image(image);
    if (!c) return kErrorObjectDisposed;
    return from_compressed_image(*c, out);
  }
  const uint32_t pf = format == kPfNone ? img->texture_pixel_format : format;
  int32_t h = 0;
  int32_t rc = PixelBuffer_CreateForImage(img->width, img->height, pf, &h);
  if (rc != kOk) return rc;
  if (img->pixels) {
    PixelBuffer* pb = Objects::get().pixel_buffer(h);
    upload_pixels(*pb, 0, 0, img->pixels->data(), img->pixels->size(), pf, 0, 0, 0, 0, img->width, img->height);
  }
  *out = h;
  return kOk;
}
SSN_ICALL("Sce.PlayStation.Core.Graphics.PsmTexture::FromImageInstance", Texture_FromImageInstance);

// sub_1F6B60 — the DDS texture loader, run on the header FromImageInstance writes
// (imaging/image_dds.h write_header) and the image's level-0 data. FourCC DXT1 -> PixelFormat
// Dxt1 (16, when header dwFlags has DDSD_CAPS, as the written header does), DXT3 -> Dxt3 (17),
// DXT5 -> Dxt5 (18), BC4U -> Bc4U (21), BC5U -> Bc5U (22), DX10 with DXGI 71/74/77/80/83/95/98
// -> 16/17/18/21/22/23/24; block bytes 8 for Dxt1/Bc4U, 16 for Dxt3/Dxt5/Bc5U, 2 x the
// 0x3ef970 bpp entry for DX10 formats. Size (width-1 | height-1) <= 0xFFF, 1..13 mip levels,
// non-empty data and level bytes ((w+3)/4 * block * (h+3)/4 per level) inside the data, else
// 0x80b30012. Returns 0 and fills `pf`/`bytes` (level 0).
int32_t parse_dds_texture(const uint8_t* header, const uint8_t* data, size_t size, uint32_t* pf, size_t* bytes) {
  auto rd = [](const uint8_t* p) { return uint32_t(p[0]) | (uint32_t(p[1]) << 8) | (uint32_t(p[2]) << 16) | (uint32_t(p[3]) << 24); };
  if (std::memcmp(header, "DDS ", 4) != 0) return -1;
  const uint8_t* fourcc = header + 84;
  uint32_t format = 0, block = 16;
  if (std::memcmp(fourcc, "DXT1", 4) == 0) {
    block = 8;
    format = (rd(header + 8) & 1) ? kPfDxt1 : 0;
  } else if (std::memcmp(fourcc, "DXT3", 4) == 0) {
    format = kPfDxt3;
  } else if (std::memcmp(fourcc, "DXT5", 4) == 0) {
    format = kPfDxt5;
  } else if (std::memcmp(fourcc, "BC4U", 4) == 0) {
    format = kPfBc4U, block = 8;
  } else if (std::memcmp(fourcc, "BC5U", 4) == 0) {
    format = kPfBc5U;
  } else if (std::memcmp(fourcc, "DX10", 4) == 0) {
    switch (rd(header + 128)) {
      case 71: format = kPfDxt1; break;
      case 74: format = kPfDxt3; break;
      case 77: format = kPfDxt5; break;
      case 80: format = kPfBc4U; break;
      case 83: format = kPfBc5U; break;
      case 95: format = kPfBc6U; break;
      case 98: format = kPfBc7U; break;
      default: format = 0; break;
    }
    block = 2u * uint32_t(pixel_format_bits(format));
  } else {
    return kErrorFileLoad;
  }
  const uint32_t w = rd(header + 16), h = rd(header + 12);
  const uint32_t levels = rd(header + 28) ? rd(header + 28) : 1u;
  if (((w - 1u) | (h - 1u)) > 0xFFFu || size == 0) return kErrorFileLoad;
  if (levels - 1u > 0xCu) return kErrorFileLoad;
  uint64_t total = 0;
  for (uint32_t l = 0, lw = w, lh = h; l < levels; ++l) {
    const uint64_t n = uint64_t((lw + 3) >> 2) * block * ((lh + 3) >> 2);
    if (l == 0) *bytes = size_t(n);
    total += n;
    lw = lw <= 1 ? 1 : lw >> 1;
    lh = lh <= 1 ? 1 : lh >> 1;
  }
  if (total > size) return kErrorFileLoad;
  *pf = format;
  return kOk;
}

// FromImageInstance, compressed branch (0x1ee9b0, image format 2..8): the header block
// (148 bytes, see imaging/image_dds.h) + sub_296990's level-0 data go through sub_1F6B60; on
// success the texture is created by sub_1F5420 with the loader's format (PixelFormat None
// -> the loaded one), no mipmap (one level) and the level uploaded as it is (block data, no
// conversion). No level data -> texture error 0x80b30002; a loader error is the texture's
// error. The shared-video-memory option bits FromImageInstance derives (state +56) only
// select GPU memory pools and are not modelled.
int32_t from_compressed_image(const imaging::CompressedImage& img, int32_t* out) {
  if (!img.level0 || img.level0->empty()) return kErrorArgumentNull;
  uint8_t header[148];
  imaging::dds::write_header(img.image_format, img.width, img.height, header);
  uint32_t pf = 0;
  size_t bytes = 0;
  const int32_t rc = parse_dds_texture(header, img.level0->data(), img.level0->size(), &pf, &bytes);
  if (rc != kOk) return rc;
  int32_t h = 0;
  const int32_t rc2 = PixelBuffer_CreateForImage(img.width, img.height, pf, &h);
  if (rc2 != kOk) return rc2;
  PixelBuffer* pb = Objects::get().pixel_buffer(h);
  ssn::gfx::device().upload_image(pb->image, 0, 0, 0, uint32_t(img.width), uint32_t(img.height), img.level0->data(), bytes);
  *out = h;
  return kOk;
}

}  // namespace
}  // namespace ssn::psm::gfx
