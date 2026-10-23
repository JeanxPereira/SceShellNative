// Sce.PlayStation.Core.Imaging.Image icalls (libScePsm.sprx, FW 4.03): Mono glue over
// image_core.cpp. Addresses are the icall functions libScePsm registers.
#include <algorithm>
#include <cstdint>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <string>
#include <memory>
#include <vector>

#include <mono/metadata/object.h>

#include "common/mono_util.h"
#include "core/firmware.h"
#include "core/log.h"
#include "host/boundary.h"
#include "host/context.h"
#include "libScePsm/imaging/image_core.h"
#include "libScePsm/imaging/image_dds.h"
#include "libScePsm/imaging/psm_common.h"

namespace ssn::psm::imaging {
namespace {

// Sce.PlayStation.Core.UnmanagedBuffer: { ulong address; uint size; }.
struct UnmanagedBuffer {
  uint64_t address;
  uint32_t size;
};

const uint8_t* array_bytes(MonoArray* a) { return reinterpret_cast<const uint8_t*>(mono_array_addr_with_size(a, 1, 0)); }

// NewFromFileImageWithOptionNative(byte[], ref PsmImageOption, out int) — 0x2de7e0.
int32_t NewFromFileImageWithOptionNative(MonoArray* file_image, PsmImageOption* option, uint32_t* handle) {
  if (!file_image) return err::kArgumentNull;
  return create_from_file_image(array_bytes(file_image), mono_array_length(file_image), option, handle);
}
SSN_ICALL("Sce.PlayStation.Core.Imaging.Image::NewFromFileImageWithOptionNative", NewFromFileImageWithOptionNative);

// NewFromFileImage(byte[], out int) — 0x2dea80: the option-less form (option = null).
int32_t NewFromFileImage(MonoArray* file_image, uint32_t* handle) {
  return NewFromFileImageWithOptionNative(file_image, nullptr, handle);
}
SSN_ICALL("Sce.PlayStation.Core.Imaging.Image::NewFromFileImage", NewFromFileImage);

// A path the PSM file layer (0x201210) opens: console paths resolve against the firmware
// dump; anything not found there is taken as a host path (the drive-less form of
// host::managed_path that RN app assets arrive in).
std::string resolve_file(const std::string& path) {
  if (Firmware* fw = host::ctx().firmware) {
    const std::string mapped = fw->host_path(path);
    std::error_code ec;
    if (std::filesystem::is_regular_file(mapped, ec)) return mapped;
  }
  return path;
}

// NewFromFilenameWithOptionNative(string, ref PsmImageOption, out int) — 0x2dd6c0.
// null filename -> 0x80B30002. The file is opened through the PSM file layer (0x201210);
// failing that the image error is 0x80B30011 ("file='%s'"). With option.mode != 3 the
// whole file is read into memory (short read -> 0x80B30010, "Read failed") and the image is
// built from that file image exactly as NewFromFileImageWithOptionNative does (0x200710),
// then registered under a new handle. Mode 3 builds a file-backed source instead (format
// sniffed from the first 16 bytes, 0x2002d0); the host reads the same bytes into memory.
int32_t NewFromFilenameWithOptionNative(MonoString* filename, PsmImageOption* option, uint32_t* handle) {
  if (!filename) return err::kArgumentNull;
  const std::string name = ssn::psm::utf8(filename);
  if (option && (option->animation_decode_mode & 0xFF) == 3)
    host::note_unimplemented("libScePsm Image::NewFromFilename mode 3 file-backed source: read into memory on the host");
  std::ifstream f(std::filesystem::path(resolve_file(name)), std::ios::binary);
  if (!f) {
    log::error("psm.image", "file='{}'", name);
    return err::kFileNotFound;
  }
  std::vector<uint8_t> data((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
  const int32_t rc = create_from_file_image(data.data(), data.size(), option, handle);
  log::debug("psm.image", "NewFromFilename '{}' ({} bytes) -> 0x{:08x}", name, data.size(), static_cast<uint32_t>(rc));
  return rc;
}
SSN_ICALL("Sce.PlayStation.Core.Imaging.Image::NewFromFilenameWithOptionNative", NewFromFilenameWithOptionNative);

// NewFromFilename(string, out int) — 0x2de7d0: the same with option = null.
int32_t NewFromFilename(MonoString* filename, uint32_t* handle) { return NewFromFilenameWithOptionNative(filename, nullptr, handle); }
SSN_ICALL("Sce.PlayStation.Core.Imaging.Image::NewFromFilename", NewFromFilename);

// NewFromUnmanagedBufferFileImageWithOptionNative(ref UnmanagedBuffer, ref PsmImageOption,
// out int) — 0x2dea90: address 0 -> 0x80B30002, then the same path with UB.Size bytes.
int32_t NewFromUnmanagedBufferFileImageWithOptionNative(const UnmanagedBuffer* file_image, PsmImageOption* option,
                                                        uint32_t* handle) {
  if (!file_image || file_image->address == 0) return err::kArgumentNull;
  return create_from_file_image(reinterpret_cast<const uint8_t*>(static_cast<uintptr_t>(file_image->address)),
                                file_image->size, option, handle);
}
SSN_ICALL("Sce.PlayStation.Core.Imaging.Image::NewFromUnmanagedBufferFileImageWithOptionNative",
          NewFromUnmanagedBufferFileImageWithOptionNative);

// NewFromUnmanagedBufferFileImage(ref UnmanagedBuffer, out int) — 0x2ded10 (option = null).
int32_t NewFromUnmanagedBufferFileImage(const UnmanagedBuffer* file_image, uint32_t* handle) {
  return NewFromUnmanagedBufferFileImageWithOptionNative(file_image, nullptr, handle);
}
SSN_ICALL("Sce.PlayStation.Core.Imaging.Image::NewFromUnmanagedBufferFileImage", NewFromUnmanagedBufferFileImage);

// NewFromModeSizeBuffer(ImageMode, ref ImageSize, byte[], uint, out int) — 0x2df2a0.
int32_t NewFromModeSizeBuffer(uint32_t mode, const ImageSize* size, MonoArray* buffer, uint32_t buffer_format,
                              uint32_t* handle) {
  if (!buffer) return err::kArgumentNull;
  return create_from_mode_size_buffer(mode, *size, array_bytes(buffer), mono_array_length(buffer), buffer_format, handle);
}
SSN_ICALL("Sce.PlayStation.Core.Imaging.Image::NewFromModeSizeBuffer", NewFromModeSizeBuffer);

// NewFromModeSizeColor(ImageMode, ref ImageSize, ref ImageColor, out int) — 0x2ded20.
int32_t NewFromModeSizeColor(uint32_t mode, const ImageSize* size, const int32_t* color, uint32_t* handle) {
  return create_from_mode_size_color(mode, *size, color, handle);
}
SSN_ICALL("Sce.PlayStation.Core.Imaging.Image::NewFromModeSizeColor", NewFromModeSizeColor);

// AddRefNative(int) — 0x2df750.
void AddRefNative(uint32_t handle) { add_ref(handle); }
SSN_ICALL("Sce.PlayStation.Core.Imaging.Image::AddRefNative", AddRefNative);

// ReleaseNative(int) — 0x2df7c0.
void ReleaseNative(uint32_t handle) { release(handle); }
SSN_ICALL("Sce.PlayStation.Core.Imaging.Image::ReleaseNative", ReleaseNative);

// GetSize(int, out ImageSize) — 0x2df8b0.
int32_t GetSize(uint32_t handle, ImageSize* size) { return get_size(handle, size); }
SSN_ICALL("Sce.PlayStation.Core.Imaging.Image::GetSize", GetSize);

// SetDecodeSize(int, ref ImageSize) — 0x2df980.
int32_t SetDecodeSize(uint32_t handle, const ImageSize* size) { return set_decode_size(handle, *size); }
SSN_ICALL("Sce.PlayStation.Core.Imaging.Image::SetDecodeSize", SetDecodeSize);

// DecodeNative(int) — 0x2dfb00.
int32_t DecodeNative(uint32_t handle) { return decode(handle); }
SSN_ICALL("Sce.PlayStation.Core.Imaging.Image::DecodeNative", DecodeNative);

// GetFormatNative(int, out uint) — 0x2e63b0.
int32_t GetFormatNative(uint32_t handle, uint32_t* format) { return get_format(handle, format); }
SSN_ICALL("Sce.PlayStation.Core.Imaging.Image::GetFormatNative", GetFormatNative);

// GetFileFormatNative(int, out uint) — 0x2e6460.
int32_t GetFileFormatNative(uint32_t handle, uint32_t* format) { return get_file_format(handle, format); }
SSN_ICALL("Sce.PlayStation.Core.Imaging.Image::GetFileFormatNative", GetFileFormatNative);

// GetAlphaChannelTypeNative(int, out uint) — 0x2e6600.
int32_t GetAlphaChannelTypeNative(uint32_t handle, uint32_t* type) { return get_alpha_channel_type(handle, type); }
SSN_ICALL("Sce.PlayStation.Core.Imaging.Image::GetAlphaChannelTypeNative", GetAlphaChannelTypeNative);

// GetFrameCountNative(int, out int) — 0x2e6de0.
int32_t GetFrameCountNative(uint32_t handle, int32_t* count) { return get_frame_count(handle, count); }
SSN_ICALL("Sce.PlayStation.Core.Imaging.Image::GetFrameCountNative", GetFrameCountNative);

// GetSharedVideoMemoryTypeNative(int, out uint) — 0x2e68f0.
int32_t GetSharedVideoMemoryTypeNative(uint32_t handle, uint32_t* type) { return get_shared_video_memory_type(handle, type); }
SSN_ICALL("Sce.PlayStation.Core.Imaging.Image::GetSharedVideoMemoryTypeNative", GetSharedVideoMemoryTypeNative);

// SetSharedVideoMemoryTypeNative(int, uint) — 0x2e69a0.
int32_t SetSharedVideoMemoryTypeNative(uint32_t handle, uint32_t type) { return set_shared_video_memory_type(handle, type); }
SSN_ICALL("Sce.PlayStation.Core.Imaging.Image::SetSharedVideoMemoryTypeNative", SetSharedVideoMemoryTypeNative);

// ConvertNative(int, uint format, uint option, out int, int memoryBlockHandle) — 0x2e10a0.
int32_t ConvertNative(uint32_t handle, uint32_t format, uint32_t option, uint32_t* converted, int32_t memory_block) {
  return convert(handle, format, option, converted, memory_block);
}
SSN_ICALL("Sce.PlayStation.Core.Imaging.Image::ConvertNative", ConvertNative);

// ResizeNative(int, ref ImageSize, uint filter, out int) — 0x2dff70.
int32_t ResizeNative(uint32_t handle, const ImageSize* size, uint32_t filter, uint32_t* resized) {
  return resize(handle, *size, filter, resized);
}
SSN_ICALL("Sce.PlayStation.Core.Imaging.Image::ResizeNative", ResizeNative);

// Image format index of an image (sub_296920 over impl +72): 2..8 are the DXT/BC formats.
bool is_block_compressed(const ImageState& s) {
  if (!s.has_impl) return false;
  switch (s.internal_format) {
    case kFmtDxt1: case kFmtDxt3: case kFmtDxt5: case kFmtBc4U: case kFmtBc5U: case kFmtBc6U: case kFmtBc7U: return true;
    default: return false;
  }
}

uint32_t image_format_of(const ImageState& s) {
  switch (s.internal_format) {
    case kFmtDxt1: return 2;
    case kFmtDxt3: return 3;
    case kFmtDxt5: return 4;
    case kFmtBc4U: return 5;
    case kFmtBc5U: return 6;
    case kFmtBc6U: return 7;
    case kFmtBc7U: return 8;
    default: return 255;
  }
}

// GetPixelDataSize(int, out uint) — 0x2dfe60: unknown handle -> 0x80B30005. For a
// non-block-compressed image the size is the decoded pixel slot's size (state +24; 0 when
// not decoded). For DXT/BC images sub_296990 links frame 0 and the size is the DDS header
// (128 bytes, 148 with the DX10 header for Bc4U..Bc7U) plus that entry's size ("[PSM] Dxt
// image data is too large" 0x80B30004 when the sum overflows 64 bits; stored as 32 bits).
int32_t GetPixelDataSize(uint32_t handle, uint32_t* size) {
  ImageState s;
  if (!snapshot(handle, &s)) return err::kObjectDisposed;
  if (is_block_compressed(s)) {
    std::shared_ptr<const std::vector<uint8_t>> level0;
    if (!frame0(handle, &level0)) return err::kObjectDisposed;
    const uint64_t header = image_format_of(s) < 5 ? 128u : 148u;
    *size = static_cast<uint32_t>(header + (level0 ? level0->size() : 0));
    return err::kOk;
  }
  *size = s.pixels ? static_cast<uint32_t>(s.pixels->size()) : 0u;
  return err::kOk;
}
SSN_ICALL("Sce.PlayStation.Core.Imaging.Image::GetPixelDataSize", GetPixelDataSize);

// GetPixelData(int, byte[], uint bufferSize) — 0x2dfba0: unknown handle -> 0x80B30005.
// Non-block-compressed: the slot size must fit both bufferSize and the array length,
// else "[PSM] Buffer is too small to store image" and 0x80B30004; the decoded bytes are
// copied as they are. DXT/BC: sub_296990's frame 0 behind the DDS header image_dds.h
// write_header produces (size = impl +96); min(array length, bufferSize) must hold
// header + data, else "[PSM] Buffer is too small to store DXT image" 0x80B30004.
int32_t GetPixelData(uint32_t handle, MonoArray* buffer, uint32_t buffer_size) {
  ImageState s;
  if (!snapshot(handle, &s)) return err::kObjectDisposed;
  const size_t cap = buffer ? mono_array_length(buffer) : 0;
  uint8_t* dst = buffer ? reinterpret_cast<uint8_t*>(mono_array_addr_with_size(buffer, 1, 0)) : nullptr;
  if (is_block_compressed(s)) {
    std::shared_ptr<const std::vector<uint8_t>> level0;
    if (!frame0(handle, &level0)) return err::kObjectDisposed;
    uint8_t header[148];
    const uint32_t hl = dds::write_header(image_format_of(s), s.width, s.height, header);
    const size_t n = level0 ? level0->size() : 0;
    if (std::min<size_t>(cap, buffer_size) < hl + n) return err::kInvalidOperation;
    std::memcpy(dst, header, hl);
    if (n) std::memcpy(dst + hl, level0->data(), n);
    return err::kOk;
  }
  const size_t n = s.pixels ? s.pixels->size() : 0;
  if (n > buffer_size || cap < n) return err::kInvalidOperation;
  if (n) std::memcpy(dst, s.pixels->data(), n);
  return err::kOk;
}
SSN_ICALL("Sce.PlayStation.Core.Imaging.Image::GetPixelData", GetPixelData);

}  // namespace
}  // namespace ssn::psm::imaging
