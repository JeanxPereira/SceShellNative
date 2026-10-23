// The DDS codec of libScePsm.sprx (FW 4.03), image_dds.cpp in the native build
// ("W:\Build\...\psm\src\misc\src\image\image\image_dds.cpp").
//
// Factory sub_246190 type 7 builds the codec object (824 bytes, vtable off_4A1138) when the
// data starts with "DDS "; sub_200710 wraps it in the DDS wrapper sub_25F7B0 (vtable
// off_4A1748) instead of the generic one. The codec never decodes: its header (+64,
// sub_245EB0) parses the file (sub_2458E0) and its "decode" (wrapper sub_25FA10 -> codec +16,
// sub_2455E0 "LinkImageFromBuffer") points the image's frame table at the mip levels inside
// the file buffer. Block-compressed images stay compressed: the texture side uploads them as
// they are (PsmTexture::FromImageInstance compressed branch).
#pragma once
#include <cstddef>
#include <cstdint>

namespace ssn::psm::imaging::dds {

// The 804-byte parse block sub_2458E0 fills (codec +16). Offsets are relative to the
// block; `level_offset` replaces the native pointers into the file with offsets.
struct Spec {
  bool valid = false;        // +0 data pointer != null
  int32_t format = -1;       // +8 internal pixel format (impl +72 values: 4, 6, 16, 18, 20, 26, 33..36)
  int32_t components = 0;    // +12
  uint32_t width = 0;        // +16 (header dwWidth)
  uint32_t height = 0;       // +20 (header dwHeight)
  uint32_t mips = 0;         // +24 (dwMipMapCount, 0 -> 1)
  uint32_t faces = 0;        // +28 (6 with DDSCAPS2_CUBEMAP, else 1)
  size_t level_offset[6][16] = {};  // +32: face f, level l (16 slots per face)
  size_t level_size[16] = {};       // sub_245E10 size per level (same for every face)
};

// sub_2458E0 — parse a DDS file. On any error the block is reset (valid = false, format = -1)
// and the reason is what the native prints (returned through `error` when non-null).
Spec parse(const uint8_t* data, size_t size, const char** error = nullptr);

// sub_245E10 — byte size of one w x h level of internal format `format` (0 when unknown).
uint32_t level_size(uint32_t width, uint32_t height, int32_t format);

// The DDS header libScePsm writes in front of the block-compressed data of an image (Image
// GetPixelData 0x2dfba0, PsmTexture::FromImageInstance 0x1ee9b0): "DDS ", dwSize 124, flags
// 0x1007, height, width, mip count 1, pixel format {32, DDPF_FOURCC, FourCC}, caps 0x1000;
// FourCC DXT1/DXT3/DXT5 for ImageFormat Dxt1/Dxt3/Dxt5, otherwise "DX10" with the DXGI
// format at +128 (Bc4U 80, Bc5U 83, Bc6U 95, Bc7U 98). Returns the header length: 128 for
// ImageFormat < 5, 148 otherwise. `out` must hold 148 bytes; it is zero-filled first.
uint32_t write_header(uint32_t image_format, int32_t width, int32_t height, uint8_t out[148]);

}  // namespace ssn::psm::imaging::dds
