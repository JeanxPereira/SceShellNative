// Read access to Sce.PlayStation.Core.Imaging.Image objects for the graphics side
// (PsmTexture::FromImageInstance 0x1ee9b0 / SetPixelsFromImage 0x1f07b0).
//
// What libScePsm uploads for an Image (traced in FromImageInstance, uncompressed branch):
//   * size        = impl +96 (Image.Size),
//   * pixel bytes = the image state's decoded pixel slot (+16 pointer / +24 size) as is —
//                   no premultiplication, no swizzle, no row padding,
//   * format      = the PixelFormat argument when it is not None; when it is None,
//                   ImageFormat Alpha8 -> PixelFormat.Alpha (10), Rgba1010102 -> Rgba1010102
//                   (26), anything else -> PixelFormat.Rgba (1). Only None or Bgra (20) are
//                   accepted as the argument ("... Image" error otherwise).
//   * when SharedVideoMemoryType != None and the texture reused a record of the shared
//     system-asset table (texture +184 == 0, set by sub_1F5420; that memory already holds
//     the pixels) the pixel upload is skipped; otherwise it is done (see pixel_buffer.cpp).
// Decoded PNG data is 8-bit R,G,B,A per pixel (byte order R first), straight alpha.
// Decoded SVG data (image_svg.cpp, codec sub_26F470) is 8-bit R,G,B,A, byte order R first,
// PREMULTIPLIED alpha, and the image reports ImageAlphaChannelType.Premultiplied (impl +216
// = 1, set by the SVG header sub_2708A0); the managed side reads that through
// Image.AlphaChannelType (UIImage -> ImageObject -> renderer BlendMode.Premultiplied).
// Alpha8 images (ConvertNative) are 1 byte per pixel holding the source alpha.
// The pixel slot is empty until Image.Decode() / DecodeNative ran (UIImage.FilterIfNeeded
// decodes non-compressed images before the texture is created).
#pragma once
#include <cstdint>
#include <memory>
#include <optional>
#include <vector>

namespace ssn::psm::imaging {

struct DecodedImage {
  int32_t width = 0;
  int32_t height = 0;
  // Sce.PlayStation.Core.Imaging.ImageFormat (0 Rgba8888, 1 Alpha8, ... 9 Rgba1010102;
  // 255 = no impl / not representable).
  uint32_t image_format = 255;
  // Sce.PlayStation.Core.Graphics.PixelFormat FromImageInstance picks when the managed
  // side passes PixelFormat.None (Texture2D(Image, bool) does): 1 Rgba, 10 Alpha, 26 Rgba1010102.
  uint32_t texture_pixel_format = 1;
  // Bytes per pixel of `pixels` (4 for Rgba8888, 1 for Alpha8).
  uint32_t bytes_per_pixel = 4;
  // ImageSharedVideoMemoryType (0 None, 1 SystemAsset, 2 HighResoAsset).
  uint32_t shared_video_memory_type = 0;
  // ImageAlphaChannelType of the image (impl +216): 0 Straight, 1 Premultiplied (SVG).
  // Informational: the pixel bytes are uploaded as they are either way.
  uint32_t alpha_channel_type = 0;
  // Tightly packed rows, top row first: width * height * bytes_per_pixel bytes. Null when
  // the image has not been decoded. Immutable; safe to keep after the Image is released.
  std::shared_ptr<const std::vector<uint8_t>> pixels;
};

// The block-compressed side of FromImageInstance (ImageFormat Dxt1..Bc7U, i.e. DDS images):
// the texture is created from a DDS header libScePsm synthesises (imaging/image_dds.h
// write_header: size = impl +96, one mip level) followed by entry 0 of the image's frame
// table, which sub_296990 links first (the file's mip level 0, block data as stored).
struct CompressedImage {
  int32_t width = 0, height = 0;  // impl +96
  uint32_t image_format = 255;    // ImageFormat 2..8
  uint32_t shared_video_memory_type = 0;
  std::shared_ptr<const std::vector<uint8_t>> level0;  // null when nothing could be linked
};
// std::nullopt for an unknown handle.
std::optional<CompressedImage> link_compressed_image(int32_t handle);

// Snapshot of the live Image `handle` (Image.Handle, the int the managed object holds);
// std::nullopt when no such image exists (disposed / never created).
std::optional<DecodedImage> find_image(int32_t handle);

}  // namespace ssn::psm::imaging
