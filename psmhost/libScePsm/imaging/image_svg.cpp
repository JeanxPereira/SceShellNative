// The SVG image codec of libScePsm.sprx (FW 4.03). See image_svg.h.
#include "libScePsm/imaging/image_svg.h"

#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <mutex>

#include "lodepng/lodepng.h"

#if defined(__clang__)
#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Wunused-function"
#pragma clang diagnostic ignored "-Wmissing-field-initializers"
#endif
#define NANOSVG_ALL_COLOR_KEYWORDS  // libScePsm's colour table off_4A1780 holds all 147 keywords
#define NANOSVG_IMPLEMENTATION
#include "nanosvg/nanosvg_sce.h"
#define NANOSVGRAST_IMPLEMENTATION
#include "nanosvg/nanosvgrast_sce.h"
#if defined(__clang__)
#pragma clang diagnostic pop
#endif

// The <image> data decoder of libScePsm (sub_26BA60): PNG only (the first 8 bytes must be
// the PNG signature), decoded by the static libpng 1.6.35 with palette->RGB, tRNS->alpha,
// gray->RGB, 1/2/4-bit gray expansion, strip_16 and filler 0xFF after: 8-bit RGBA, straight
// alpha, no gamma handling. lodepng_decode32 yields the same pixels for valid files; error
// handling of damaged files (CRC / zlib checks) is lodepng's, not libpng's.
extern "C" unsigned char* nsvg__sceDecodeImage(const unsigned char* data, int len, int* width, int* height, int* channels) {
  static const unsigned char kSig[8] = {0x89, 'P', 'N', 'G', 0x0D, 0x0A, 0x1A, 0x0A};
  // (The native compares 8 bytes without looking at the length.)
  if (!data || len < 8 || std::memcmp(data, kSig, 8) != 0) return nullptr;
  unsigned char* out = nullptr;
  unsigned w = 0, h = 0;
  if (lodepng_decode32(&out, &w, &h, data, size_t(len)) != 0) {
    std::free(out);
    return nullptr;
  }
  *width = int(w);
  *height = int(h);
  *channels = 4;
  return out;  // lodepng's default allocator is malloc; nsvgDelete frees it with free()
}

namespace ssn::psm::imaging::svg {

struct Codec {
  std::mutex mutex;                   // +16 "ImageSvgMutex"
  NSVGimage* image = nullptr;         // +24
  NSVGrasterizer* rasterizer = nullptr;  // +32
  ~Codec() {                          // vtable[0] destructor (sub_26F340 / sub_26F390)
    nsvgDelete(image);
    nsvgDeleteRasterizer(rasterizer);
  }
};

std::shared_ptr<Codec> create_codec() { return std::make_shared<Codec>(); }

namespace {

// The parse both entry points share: the encoded file copied into a NUL-terminated
// allocation (nsvgParse writes into its input), parsed, the copy freed; the result is
// kept in the codec.
NSVGimage* parsed(Codec& c, const uint8_t* data, size_t size) {
  if (c.image) return c.image;
  char* text = static_cast<char*>(std::malloc(size + 1));
  if (!text) return nullptr;
  std::memcpy(text, data, size);
  text[size] = 0;
  c.image = nsvgParse(text);
  std::free(text);
  return c.image;
}

// vcvttss2si
int32_t cvtt(float v) {
  if (!(v > -2147483904.0f && v < 2147483648.0f)) return INT32_MIN;
  return int32_t(v);
}

}  // namespace

bool read_header(Codec& c, const uint8_t* data, size_t size, int32_t* width, int32_t* height) {
  std::lock_guard<std::mutex> lock(c.mutex);
  if (!data) return false;
  NSVGimage* image = parsed(c, data, size);
  if (!image) return false;
  *width = cvtt(image->width);
  *height = cvtt(image->height);
  return true;
}

bool decode(Codec& c, const uint8_t* data, size_t size, int32_t header_width, int32_t header_height, int32_t width,
            int32_t height, std::vector<uint8_t>* out) {
  std::lock_guard<std::mutex> lock(c.mutex);
  if (!data) return false;
  // Format 4, row alignment 1 (attribute +76): stride = 4 * width.
  const int32_t stride = width * 4;
  out->assign(size_t(uint32_t(stride)) * size_t(uint32_t(height)), 0);
  NSVGimage* image = parsed(c, data, size);
  if (!image) return false;
  if (!c.rasterizer) {
    c.rasterizer = nsvgCreateRasterizer();  // 128 bytes, tessTol 0.25, distTol 0.01
    if (!c.rasterizer) return false;
  }
  // scale = minss(sy, sx) with the NaN case of sx falling back to sy (vcmpunordss/vblendvps).
  const float sx = float(width) / float(header_width);
  const float sy = float(height) / float(header_height);
  const float scale = std::isnan(sx) ? sy : (sy < sx ? sy : sx);
  nsvgRasterize(c.rasterizer, image, 0.0f, 0.0f, scale, out->data(), width, height, stride);
  return true;
}

}  // namespace ssn::psm::imaging::svg
