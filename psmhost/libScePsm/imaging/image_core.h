// Sce.PlayStation.Core.Imaging.Image — the native object model of libScePsm.sprx
// (FW 4.03), without the Mono glue (image_icalls.cpp) so it can be exercised standalone.
//
// libScePsm keeps two layers per Image handle:
//   * the image object (40 bytes, vtable off_4A23B0) with a 64-byte state block:
//       +0 impl pointer, +16/+24 decoded pixel pointer/size, +48 status, +56 shared
//       video memory type;
//   * the decoder-side "impl" (296 bytes, built by sub_1FFF30 / sub_200910 / sub_200EB0):
//       +72 pixel format, +96 width/height, +136 file type, +156 frame count,
//       +184 orientation, +189 animation decode mode, +190 apply-orientation,
//       +216 alpha channel type.
// Handles come from the shared generator (psm_common.h) and live in the image table
// (xmmword_5273E0), separate from the other object tables.
#pragma once
#include <cstddef>
#include <cstdint>
#include <memory>
#include <vector>

namespace ssn::psm::imaging {

// Internal pixel formats (impl +72), as mapped to ImageFormat by sub_296920.
enum InternalFormat : uint32_t {
  kFmtRgba8888 = 4, kFmtBgra8888 = 6, kFmtA8 = 14, kFmtDxt1 = 16, kFmtDxt3 = 18, kFmtDxt5 = 20,
  kFmtBc4U = 33, kFmtBc5U = 34, kFmtBc6U = 35, kFmtBc7U = 36, kFmtRgba1010102 = 103,
};

// Internal file types (impl +136) as returned by the signature sniffer sub_2002D0.
enum FileType : uint32_t {
  kFileNone = 0, kFilePng = 1, kFileJpg = 2, kFileGif = 4, kFileBmp = 5, kFileDds = 7, kFileSvg = 8, kFileWebP = 9, kFileJxr = 10,
};

// Managed structs, bit-for-bit (Core.dll).
struct PsmImageOption {  // Sce.PlayStation.Core.Imaging.PsmImageOption
  int32_t animation_decode_mode;
  int32_t is_apply_orientation;
};
struct ImageSize {  // Sce.PlayStation.Core.Imaging.ImageSize
  int32_t width, height;
};

// Image::NewFromFileImageWithOptionNative (0x2de7e0) / NewFromUnmanagedBufferFileImage-
// WithOptionNative (0x2dea90): copy the encoded file, sniff and open it (sub_295880).
int32_t create_from_file_image(const uint8_t* data, size_t size, const PsmImageOption* option, uint32_t* handle);
// Image::NewFromModeSizeBuffer (0x2df2a0).
int32_t create_from_mode_size_buffer(uint32_t mode, const ImageSize& size, const uint8_t* buffer, size_t buffer_length,
                                     uint32_t buffer_format, uint32_t* handle);
// Image::NewFromModeSizeColor (0x2ded20). color = ImageColor {R, G, B, A}.
int32_t create_from_mode_size_color(uint32_t mode, const ImageSize& size, const int32_t color[4], uint32_t* handle);

void add_ref(uint32_t handle);   // AddRefNative 0x2df750
void release(uint32_t handle);   // ReleaseNative 0x2df7c0

int32_t get_size(uint32_t handle, ImageSize* size);                    // GetSize 0x2df8b0
int32_t set_decode_size(uint32_t handle, const ImageSize& size);        // SetDecodeSize 0x2df980
int32_t decode(uint32_t handle);                                        // DecodeNative 0x2dfb00
int32_t get_format(uint32_t handle, uint32_t* format);                  // GetFormatNative 0x2e63b0
int32_t get_file_format(uint32_t handle, uint32_t* format);             // GetFileFormatNative 0x2e6460
int32_t get_alpha_channel_type(uint32_t handle, uint32_t* type);        // GetAlphaChannelTypeNative 0x2e6600
int32_t get_frame_count(uint32_t handle, int32_t* count);               // GetFrameCountNative 0x2e6de0
int32_t get_shared_video_memory_type(uint32_t handle, uint32_t* type);  // 0x2e68f0
int32_t set_shared_video_memory_type(uint32_t handle, uint32_t type);   // 0x2e69a0
// ConvertNative 0x2e10a0 (format = ImageFormat, option = ImageConvertOption).
int32_t convert(uint32_t handle, uint32_t format, uint32_t option, uint32_t* converted, int32_t memory_block);
// ResizeNative 0x2dff70 (filter = ImageResizeFilter).
int32_t resize(uint32_t handle, const ImageSize& size, uint32_t filter, uint32_t* resized);

// Header-only open of an encoded image, as CxmlResources::GetTextureResource (0x2fbf38)
// does to compare candidate sizes: true and the impl size (+96) when sub_295880 succeeds.
bool probe_file_image_size(const uint8_t* data, size_t size, int32_t* width, int32_t* height);

// sub_296990 — entry 0 of the image's frame table, running the codec decode first when no
// entry is populated yet. For a DDS image this links the frames to the file and yields mip
// level 0 as stored in the file (the block-compressed data for ImageFormat Dxt1..Bc7U).
// `out` is null when there is no such entry. False for an unknown handle.
bool frame0(uint32_t handle, std::shared_ptr<const std::vector<uint8_t>>* out);

// Snapshot for image_store.h.
struct ImageState {
  bool has_impl = false;
  uint32_t internal_format = 0;
  int32_t width = 0, height = 0;
  uint32_t shared_video_memory_type = 0;
  uint32_t alpha_channel_type = 0;  // impl +216 (1 = Premultiplied: SVG)
  std::shared_ptr<const std::vector<uint8_t>> pixels;  // state +16/+24 (null until decoded)
};
bool snapshot(uint32_t handle, ImageState* out);

// Image::DrawTextNative (0x2e34e0) works directly on the image's decoded pixel slot
// (state +16 pointer / +24 size) and reads the impl format (+72) and size (+96).
// draw_into_pixels runs `draw(target, ctx)` with the image locked. `pixels` is null when
// the image has no decoded pixels (the layout routine then only measures). Snapshots
// taken earlier (image_store.h) keep the old pixels. Unknown handle -> 0x80B30005.
struct PixelTarget {
  uint8_t* pixels = nullptr;
  size_t size = 0;
  bool has_impl = false;
  uint32_t internal_format = 0;   // impl +72
  int32_t width = 0, height = 0;  // impl +96
};
int32_t draw_into_pixels(uint32_t handle, int32_t (*draw)(const PixelTarget& target, void* ctx), void* ctx);

}  // namespace ssn::psm::imaging
