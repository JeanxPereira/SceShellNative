// The JPEG codec of libScePsm.sprx (FW 4.03): factory sub_246190 type 2 (992-byte codec
// object, vtable off_4A1438, "ImageJpgMutex"), over the statically linked libjpeg-turbo
// 2.0.2 (third_party/jpeg.cmake). Compiled in when SSN_HAVE_PSM_JPEG is defined; without it
// available() is false and JPEG files keep failing as unsupported.
#pragma once
#include <cstddef>
#include <cstdint>
#include <vector>

namespace ssn::psm::imaging::jpeg {

bool available();

struct Header {
  int32_t width = 0, height = 0;  // cinfo.image_width / image_height (attribute +24)
  uint32_t orientation = 0;       // EXIF IFD0 tag 0x112 (attribute +112 = impl +184), 0 when absent
};

// SetAttribute 0x252800 -> SetAttributeInfo 0x24eb30: jpeg_read_header over the codec's
// memory source; with `apply_orientation` (PsmImageOption.IsApplyOrientation, impl +190) the
// APP1 markers are kept and the first one parsed for the EXIF orientation. Returns true on
// success; on failure `*codec_error` is the per-thread codec error the native leaves (0 when
// it sets none, which sub_246990 then turns into 0x80B35501).
bool read_header(const uint8_t* data, size_t size, bool apply_orientation, Header* out, uint32_t* codec_error);

// LoadImageFromBuffer 0x251d20 -> DecompressImage 0x250830 (full-size decode, scale 1/1):
// RGBA8888, R first, alpha 255, tight rows of 4 * width bytes. Grayscale is replicated,
// CMYK/YCCK (libjpeg's output: Adobe-inverted CMYK) becomes (32897 * c * k) >> 23 per
// channel; other output component counts fail with codec error 0x80B35500.
bool decode(const uint8_t* data, size_t size, int32_t width, int32_t height, std::vector<uint8_t>* rgba, uint32_t* codec_error);

}  // namespace ssn::psm::imaging::jpeg
