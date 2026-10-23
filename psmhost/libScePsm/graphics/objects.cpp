#include "libScePsm/graphics/objects.h"

#include "libScePsm/graphics/shader_program.h"

namespace ssn::psm::gfx {

Objects& Objects::get() {
  static Objects o;
  return o;
}

int32_t pixel_format_bits(uint32_t format) {
  // 0x3ef970: {bpp, ...} per PixelFormat 0..27.
  static constexpr uint8_t kBpp[28] = {0, 32, 64, 16, 16, 16, 16, 32, 8, 16, 8, 16, 16, 32, 32, 32, 4, 8, 8, 32, 8, 4, 8, 8, 8, 8, 32, 32};
  return format < 28 ? kBpp[format] : 0;
}

bool vk_format_for(uint32_t format, VkFormat& out, VkComponentMapping& sw) {
  using enum VkComponentSwizzle;
  sw = {VK_COMPONENT_SWIZZLE_IDENTITY, VK_COMPONENT_SWIZZLE_IDENTITY, VK_COMPONENT_SWIZZLE_IDENTITY, VK_COMPONENT_SWIZZLE_IDENTITY};
  switch (format) {
    case kPfRgba: out = VK_FORMAT_R8G8B8A8_UNORM; return true;
    case kPfBgra: out = VK_FORMAT_B8G8R8A8_UNORM; return true;
    case kPfRgbaH: out = VK_FORMAT_R16G16B16A16_SFLOAT; return true;
    case kPfRgba4444: out = VK_FORMAT_A4B4G4R4_UNORM_PACK16; return true;
    case kPfRgba5551: out = VK_FORMAT_A1B5G5R5_UNORM_PACK16_KHR; return true;
    case kPfRgb565: out = VK_FORMAT_B5G6R5_UNORM_PACK16; return true;
    case kPfLuminanceAlpha: out = VK_FORMAT_R8G8_UNORM; sw = {VK_COMPONENT_SWIZZLE_R, VK_COMPONENT_SWIZZLE_R, VK_COMPONENT_SWIZZLE_R, VK_COMPONENT_SWIZZLE_G}; return true;
    case kPfLuminanceAlphaH: out = VK_FORMAT_R16G16_SFLOAT; sw = {VK_COMPONENT_SWIZZLE_R, VK_COMPONENT_SWIZZLE_R, VK_COMPONENT_SWIZZLE_R, VK_COMPONENT_SWIZZLE_G}; return true;
    case kPfLuminance: out = VK_FORMAT_R8_UNORM; sw = {VK_COMPONENT_SWIZZLE_R, VK_COMPONENT_SWIZZLE_R, VK_COMPONENT_SWIZZLE_R, VK_COMPONENT_SWIZZLE_ONE}; return true;
    case kPfLuminanceH: out = VK_FORMAT_R16_SFLOAT; sw = {VK_COMPONENT_SWIZZLE_R, VK_COMPONENT_SWIZZLE_R, VK_COMPONENT_SWIZZLE_R, VK_COMPONENT_SWIZZLE_ONE}; return true;
    case kPfAlpha: out = VK_FORMAT_R8_UNORM; sw = {VK_COMPONENT_SWIZZLE_ZERO, VK_COMPONENT_SWIZZLE_ZERO, VK_COMPONENT_SWIZZLE_ZERO, VK_COMPONENT_SWIZZLE_R}; return true;
    case kPfAlphaH: out = VK_FORMAT_R16_SFLOAT; sw = {VK_COMPONENT_SWIZZLE_ZERO, VK_COMPONENT_SWIZZLE_ZERO, VK_COMPONENT_SWIZZLE_ZERO, VK_COMPONENT_SWIZZLE_R}; return true;
    case kPfRed: out = VK_FORMAT_R8_UNORM; return true;
    case kPfDxt1: out = VK_FORMAT_BC1_RGBA_UNORM_BLOCK; return true;
    case kPfDxt3: out = VK_FORMAT_BC2_UNORM_BLOCK; return true;
    case kPfDxt5: out = VK_FORMAT_BC3_UNORM_BLOCK; return true;
    case kPfBc4U: out = VK_FORMAT_BC4_UNORM_BLOCK; return true;
    case kPfBc5U: out = VK_FORMAT_BC5_UNORM_BLOCK; return true;
    case kPfBc6U: out = VK_FORMAT_BC6H_UFLOAT_BLOCK; return true;
    case kPfBc7U: out = VK_FORMAT_BC7_UNORM_BLOCK; return true;
    case kPfDepth16: out = VK_FORMAT_D16_UNORM; return true;
    case kPfDepth24:
    case kPfDepth16Stencil8:
    case kPfDepth24Stencil8: out = VK_FORMAT_D24_UNORM_S8_UINT; return true;
    case kPfRgba1010102: out = VK_FORMAT_A2B10G10R10_UNORM_PACK32; return true;
    case kPfAbgr2101010: out = VK_FORMAT_A2R10G10B10_UNORM_PACK32; return true;
  }
  return false;
}

namespace {
// 0x40fb40: bytes per component indexed by (VertexFormat >> 8) & 0xf.
constexpr uint32_t kComponentBytes[16] = {0, 4, 2, 1, 0, 0, 2, 2, 1, 1, 0, 0, 0, 0, 0, 0};
}  // namespace

uint32_t vertex_format_size(uint32_t f) {
  const uint32_t comp = kComponentBytes[(f >> 8) & 0xf];
  const uint32_t n = comp * ((f & 3) + 1) * (((f >> 4) & 3) + 1);
  return (n == 3 || n == 6) ? n * 4 / 3 : n;
}

bool vk_vertex_format(uint32_t f, VkFormat& out) {
  const uint32_t type = (f >> 8) & 0xf;
  const bool norm = (f & 0x1000) != 0;
  const uint32_t n = (f & 3) + 1;
  static constexpr VkFormat kFloat[4] = {VK_FORMAT_R32_SFLOAT, VK_FORMAT_R32G32_SFLOAT, VK_FORMAT_R32G32B32_SFLOAT, VK_FORMAT_R32G32B32A32_SFLOAT};
  static constexpr VkFormat kHalf[4] = {VK_FORMAT_R16_SFLOAT, VK_FORMAT_R16G16_SFLOAT, VK_FORMAT_R16G16B16_SFLOAT, VK_FORMAT_R16G16B16A16_SFLOAT};
  static constexpr VkFormat kShort[2][4] = {{VK_FORMAT_R16_SSCALED, VK_FORMAT_R16G16_SSCALED, VK_FORMAT_R16G16B16_SSCALED, VK_FORMAT_R16G16B16A16_SSCALED},
                                            {VK_FORMAT_R16_SNORM, VK_FORMAT_R16G16_SNORM, VK_FORMAT_R16G16B16_SNORM, VK_FORMAT_R16G16B16A16_SNORM}};
  static constexpr VkFormat kUShort[2][4] = {{VK_FORMAT_R16_USCALED, VK_FORMAT_R16G16_USCALED, VK_FORMAT_R16G16B16_USCALED, VK_FORMAT_R16G16B16A16_USCALED},
                                             {VK_FORMAT_R16_UNORM, VK_FORMAT_R16G16_UNORM, VK_FORMAT_R16G16B16_UNORM, VK_FORMAT_R16G16B16A16_UNORM}};
  static constexpr VkFormat kByte[2][4] = {{VK_FORMAT_R8_SSCALED, VK_FORMAT_R8G8_SSCALED, VK_FORMAT_R8G8B8_SSCALED, VK_FORMAT_R8G8B8A8_SSCALED},
                                           {VK_FORMAT_R8_SNORM, VK_FORMAT_R8G8_SNORM, VK_FORMAT_R8G8B8_SNORM, VK_FORMAT_R8G8B8A8_SNORM}};
  static constexpr VkFormat kUByte[2][4] = {{VK_FORMAT_R8_USCALED, VK_FORMAT_R8G8_USCALED, VK_FORMAT_R8G8B8_USCALED, VK_FORMAT_R8G8B8A8_USCALED},
                                            {VK_FORMAT_R8_UNORM, VK_FORMAT_R8G8_UNORM, VK_FORMAT_R8G8B8_UNORM, VK_FORMAT_R8G8B8A8_UNORM}};
  switch (type) {
    case 1: out = kFloat[n - 1]; return true;
    case 2: out = kHalf[n - 1]; return true;
    case 6: out = kShort[norm][n - 1]; return true;
    case 7: out = kUShort[norm][n - 1]; return true;
    case 8: out = kByte[norm][n - 1]; return true;
    case 9: out = kUByte[norm][n - 1]; return true;
  }
  return false;
}

}  // namespace ssn::psm::gfx
