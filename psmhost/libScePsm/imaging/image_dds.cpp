// libScePsm DDS codec (FW 4.03). See image_dds.h.
#include "libScePsm/imaging/image_dds.h"

#include <cstring>

namespace ssn::psm::imaging::dds {
namespace {

uint32_t rd32(const uint8_t* p) { return uint32_t(p[0]) | (uint32_t(p[1]) << 8) | (uint32_t(p[2]) << 16) | (uint32_t(p[3]) << 24); }
void wr32(uint8_t* p, uint32_t v) {
  p[0] = uint8_t(v), p[1] = uint8_t(v >> 8), p[2] = uint8_t(v >> 16), p[3] = uint8_t(v >> 24);
}

constexpr uint32_t kFourDx10 = 0x30315844u;  // "DX10" 808540228
constexpr uint32_t kFourDxt1 = 0x31545844u;  // "DXT1" 827611204
constexpr uint32_t kFourDxt3 = 0x33545844u;  // "DXT3" 861165636
constexpr uint32_t kFourDxt5 = 0x35545844u;  // "DXT5" 894720068
constexpr uint32_t kFourBc4u = 0x55344342u;  // "BC4U" 1429488450
constexpr uint32_t kFourBc5u = 0x55354342u;  // "BC5U" 1429553986

Spec reset(const char** error, const char* why) {
  if (error) *error = why;
  return Spec{};
}

}  // namespace

// sub_245E10 (unsigned 32-bit arithmetic as in the native).
uint32_t level_size(uint32_t w, uint32_t h, int32_t format) {
  switch (format) {
    case 3: case 4: case 5: case 6: return 4u * w * h;
    case 14: case 15: return w * h;
    case 16: case 33: return ((h + 3u) >> 2) * ((2u * w + 6u) & 0xFFFFFFF8u);
    case 18: case 20: case 34: case 35: case 36: return ((h + 3u) >> 2) * ((4u * w + 12u) & 0xFFFFFFF0u);
    case 25: case 26: return 3u * w * h;
    default: return 0;  // "unable to determine image size"
  }
}

// sub_2458E0. Header fields read (file offsets): +12 dwHeight, +16 dwWidth, +24 dwDepth,
// +28 dwMipMapCount, +80 ddspf.dwFlags, +84 FourCC, +88 dwRGBBitCount, +92 dwRBitMask
// (low byte only), +112 dwCaps2; DX10 header +128 dxgiFormat.
Spec parse(const uint8_t* d, size_t n, const char** error) {
  if (error) *error = nullptr;
  if (!d || n < 128 || std::memcmp(d, "DDS ", 4) != 0) return reset(error, "this file is not dds format.");
  const uint32_t fourcc = rd32(d + 84);
  size_t data = 128;
  const uint8_t* dx10 = nullptr;
  if (fourcc == kFourDx10) {
    if (n <= 147) return reset(error, "ERROR: unexpected eof");
    dx10 = d + 128;
    data = 148;
  }
  const uint32_t caps2 = rd32(d + 112);
  if ((caps2 & 0x200000u) && rd32(d + 24) != 0) return reset(error, "ERROR: <?> is a volume texture");

  Spec s;
  int32_t format = -1, components = 0;
  const uint32_t pf_flags = rd32(d + 80);
  if (pf_flags & 4u) {  // DDPF_FOURCC
    switch (fourcc) {
      case kFourDxt1: format = 16, components = 3; break;
      case kFourDxt3: format = 18, components = 4; break;
      case kFourDxt5: format = 20, components = 4; break;
      case kFourBc4u: format = 33, components = 1; break;
      case kFourBc5u: format = 34, components = 2; break;
      case kFourDx10: {
        const uint32_t dxgi = rd32(dx10);
        if (dxgi - 71u <= 1u) format = 16, components = 3;         // BC1_UNORM(_SRGB)
        if ((dxgi & ~1u) == 74u) format = 18, components = 4;      // BC2_UNORM(_SRGB)
        else if (dxgi - 77u <= 1u) format = 20, components = 4;    // BC3_UNORM(_SRGB)
        if (dxgi == 95u) format = 35, components = 3;              // BC6H_UF16
        else if (dxgi == 83u) format = 34, components = 2;         // BC5_UNORM
        else if (dxgi == 80u) format = 33, components = 1;         // BC4_UNORM
        else if ((dxgi & ~1u) == 98u) format = 36, components = 4; // BC7_UNORM(_SRGB)
        break;
      }
      default: break;
    }
    // Anything else: "this type dds image not supported in ImageSpec" (the codec error
    // 0x80B35500 it stores at +800 is cleared again by the reset), "ERROR: ImageSpec() failed".
    if (format == -1) return reset(error, "ERROR: ImageSpec() failed");
  } else {
    const uint32_t bits = rd32(d + 88);
    if ((pf_flags == 65u && bits == 32u) || (pf_flags == 64u && bits == 32u)) {  // DDPF_RGB(|ALPHAPIXELS), 32 bit
      format = d[92] != 0 ? 4 : 6, components = 4;  // R mask in the low byte -> RGBA8888, else BGRA8888
    } else if (pf_flags == 64u && bits == 24u) {
      format = 26, components = 3;
    } else {
      return reset(error, "ERROR: ImageSpec() failed");
    }
  }
  uint32_t h = rd32(d + 12), w = rd32(d + 16);
  if (w > 0x1000u || h >= 0x1001u) return reset(error, "ERROR: image size is invalid");
  uint32_t mips = rd32(d + 28);
  if (mips == 0) {
    mips = 1;  // the native also writes 1 back into the header
  } else if (mips >= 16) {
    return reset(error, "ERROR: ImageSpec() failed : dwMipMapCount is not supported.");
  }
  s.format = format;
  s.components = components;
  s.width = w;
  s.height = h;
  s.mips = mips;
  s.faces = (caps2 & 0x200u) ? 6u : 1u;  // DDSCAPS2_CUBEMAP
  uint64_t cursor = data;
  for (uint32_t f = 0; f < s.faces; ++f) {
    uint32_t lw = w, lh = h;
    for (uint32_t l = 0; l < mips; ++l) {
      s.level_offset[f][l] = size_t(cursor);
      s.level_size[l] = level_size(lw, lh, format);
      cursor += s.level_size[l];
      if (lw != 1) lw >>= 1;
      if (lh != 1) lh >>= 1;
    }
  }
  if (cursor > n) return reset(error, "ERROR: unexpected eof");
  s.valid = true;
  return s;
}

// GetPixelData 0x2dfba0 / FromImageInstance 0x1ee9b0 header block.
uint32_t write_header(uint32_t image_format, int32_t width, int32_t height, uint8_t out[148]) {
  const uint32_t len = image_format < 5 ? 128u : 148u;
  std::memset(out, 0, 148);
  std::memcpy(out, "DDS ", 4);
  wr32(out + 4, 0x7C);
  wr32(out + 8, 0x1007);  // CAPS | HEIGHT | WIDTH | PIXELFORMAT
  wr32(out + 12, uint32_t(height));
  wr32(out + 16, uint32_t(width));
  wr32(out + 28, 1);
  wr32(out + 76, 32);
  wr32(out + 80, 4);
  wr32(out + 108, 0x1000);
  switch (image_format) {
    case 2: wr32(out + 84, kFourDxt1); break;
    case 3: wr32(out + 84, kFourDxt3); break;
    case 4: wr32(out + 84, kFourDxt5); break;
    default:
      wr32(out + 84, kFourDx10);
      switch (image_format) {
        case 5: wr32(out + 128, 80); break;
        case 6: wr32(out + 128, 83); break;
        case 7: wr32(out + 128, 95); break;
        case 8: wr32(out + 128, 98); break;
        default: break;
      }
      break;
  }
  return len;
}

}  // namespace ssn::psm::imaging::dds
