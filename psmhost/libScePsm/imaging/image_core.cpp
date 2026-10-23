// Sce.PlayStation.Core.Imaging.Image native object model (libScePsm.sprx, FW 4.03).
// See image_core.h for the object layout this mirrors.
#include "libScePsm/imaging/image_core.h"

#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <mutex>
#include <new>
#include <string>
#include <unordered_map>
#include <vector>

#include "host/boundary.h"
#include "libScePsm/imaging/image_dds.h"
#include "libScePsm/imaging/image_gtc.h"
#include "libScePsm/imaging/image_jpeg.h"
#include "libScePsm/imaging/image_svg.h"
#include "libScePsm/imaging/psm_common.h"
#include "lodepng/lodepng.h"

namespace ssn::psm::imaging {
namespace {

// The decoder-side image ("impl", 296 bytes in libScePsm). Fields keep the native offsets
// in their comments; the defaults are the ones sub_1FFF30 / sub_200EB0 initialise.
struct Impl {
  uint32_t format = 0;             // +72 (codec attribute block +0; the block starts at +72)
  int32_t header_width = 0, header_height = 0;  // +88 (attribute +16: the size the codec header reported)
  int32_t width = 0, height = 0;   // +96 (attribute +24: the decode size; SetDecodeSize changes it)
  uint32_t file_type = kFileNone;  // +136
  int32_t frame_count = 1;         // +156 (rbx[19] = 0x100000001)
  uint32_t orientation = 0;        // +184
  uint8_t anim_mode = 0;           // +189 (low byte of PsmImageOption.AnimationDecodeMode)
  bool apply_orientation = false;  // +190
  uint32_t alpha_type = 0;         // +216 (attribute +144; ImageAlphaChannelType.Straight)
  bool has_decoder = false;        // impl +0 (codec object) still alive
  bool apng = false;               // host bookkeeping: an acTL chunk was seen
  std::shared_ptr<svg::Codec> svg; // the SVG codec object (file type 8) while has_decoder
  dds::Spec dds;                   // the DDS codec's parse block (file type 7, codec +16)
  uint32_t frame_slots = 1;        // frame table size (impl +24 * +28): mip count for DDS
  std::vector<uint8_t> source;     // the encoded file the codec reads from
  std::shared_ptr<const std::vector<uint8_t>> frame0;  // decoded frame 0 (impl frames array)
};

// The image object + its 64-byte state block.
struct Object {
  std::mutex mutex;
  std::shared_ptr<Impl> impl;                           // state +0
  std::shared_ptr<const std::vector<uint8_t>> pixels;   // state +16 / +24
  int32_t status = 0;                                   // state +48
  uint32_t shared_vm_type = 0;                          // state +56
};

struct Entry {
  std::shared_ptr<Object> object;
  int32_t refs = 0;  // object +8
};

std::mutex g_table_mutex;  // X6AKrGoJOU0_h_A
std::unordered_map<uint32_t, Entry> g_table;  // xmmword_5273E0

std::shared_ptr<Object> lookup(uint32_t handle) {
  std::lock_guard<std::mutex> lock(g_table_mutex);
  auto it = g_table.find(handle);
  return it == g_table.end() ? nullptr : it->second.object;
}

// The insertion every constructor icall ends with: generate a handle, store, refs = 1.
uint32_t register_object(std::shared_ptr<Object> obj) {
  std::lock_guard<std::mutex> lock(g_table_mutex);
  const uint32_t h = generate_handle([](uint32_t k) { return g_table.count(k) != 0; });
  g_table[h] = Entry{std::move(obj), 1};
  return h;
}

std::string hex32(uint32_t v) {
  char b[16];
  std::snprintf(b, sizeof b, "0x%08x", v);
  return b;
}

uint32_t rd_be32(const uint8_t* p) { return (uint32_t(p[0]) << 24) | (uint32_t(p[1]) << 16) | (uint32_t(p[2]) << 8) | p[3]; }
uint32_t rd_le32(const uint8_t* p) { return uint32_t(p[0]) | (uint32_t(p[1]) << 8) | (uint32_t(p[2]) << 16) | (uint32_t(p[3]) << 24); }

// sub_26F3E0 — SVG signature test: optional UTF-8 BOM, then "<?xml" or "<svg". (The
// native adds 3 to the remaining length after skipping the BOM instead of subtracting.)
bool sniff_svg(const uint8_t* data, size_t size) {
  if (!data || size < 3) return false;
  const uint8_t* p = data;
  size_t n = size;
  if (data[0] == 0xEF && data[1] == 0xBB && data[2] == 0xBF) {
    p = data + 3;
    n = size + 3;
  }
  if (n < 5) {
    if (n != 4) return false;
  } else if (std::memcmp(p, "<?xml", 5) == 0) {
    return true;
  }
  return std::memcmp(p, "<svg", 4) == 0;
}

// sub_2002D0 — file signature sniffer (the order is the native one). `tls` receives the
// per-thread codec error the native sets when nothing matches (0x80B35500).
uint32_t sniff(const uint8_t* d, size_t n, uint32_t* tls) {
  if (!d) return kFileNone;
  static const uint8_t kPng[8] = {0x89, 'P', 'N', 'G', 0x0D, 0x0A, 0x1A, 0x0A};
  if (n >= 8 && std::memcmp(d, kPng, 8) == 0) return kFilePng;
  if (n >= 2 && d[0] == 0xFF && d[1] == 0xD8) return kFileJpg;
  if (n >= 6 && (std::memcmp(d, "GIF87a", 6) == 0 || std::memcmp(d, "GIF89a", 6) == 0)) return kFileGif;
  if (n >= 2 && d[0] == 'B' && d[1] == 'M') return kFileBmp;
  if (n >= 4 && rd_le32(d) == 0x20534444u) return kFileDds;  // "DDS "
  // The native tests n >= 2 but reads 4 bytes ("II\xBC\x01"); the host needs n >= 4.
  if (n >= 4 && rd_le32(d) == 0x01BC4949u) return kFileJxr;
  if (sniff_svg(d, n)) return kFileSvg;
  // Last: the WebP probe sub_205EF0 (libwebp feature parse). Not reproduced; data that
  // reaches this point is treated as the native treats non-WebP data.
  host::note_unimplemented("libScePsm Image sniff: WebP probe sub_205EF0 not reproduced (data treated as unsupported)");
  *tls = 0x80B35500u;
  return kFileNone;
}

// The codec error -> PSM result mapping of sub_295880 (with the exception messages).
int32_t map_codec_error(uint32_t tls) {
  switch (tls) {
    case 0x80B35500u: set_exception_info("Image unsupported"); return err::kNotSupported;
    case 0x80B35501u: set_exception_info("Unknown error"); return err::kImageSystem;
    case 0x80B35502u: set_exception_info("Format mismatch"); return err::kNotSupported;
    case 0x80B35503u: return err::kFileNotFound;
    case 0x80B35504u: set_exception_info("Seek failed"); return err::kIo;
    case 0x80B35505u: set_exception_info("Read failed"); return err::kIo;
    case 0x80B35506u: set_exception_info("Decode cancelled"); return err::kImageSystem;
    case 0x80B35507u: return err::kOutOfMemory;
    default:
      set_exception_info("Unknown error 'default' case : errorcode=" + hex32(tls));
      return err::kImageSystem;
  }
}

// ImagePng::SetAttribute 0x25B830 -> ReadAttributeInfo 0x2575C0 (png_read_info) ->
// SetAttributeInfo 0x25BA00. The PNG impl is always format 4 (RGBA8888), file type 1,
// size from IHDR, frame count from acTL.num_frames (1 without acTL). Returns the
// per-thread codec error (0 = success); SetAttribute clears it first, so a libpng error
// (setjmp path) leaves 0, which sub_246990 turns into 0x80B35501 ("Unknown error").
uint32_t open_png(Impl& impl) {
  unsigned w = 0, h = 0;
  LodePNGState st;
  lodepng_state_init(&st);
  const unsigned e = lodepng_inspect(&w, &h, &st, impl.source.data(), impl.source.size());
  lodepng_state_cleanup(&st);
  if (e != 0) return 0;  // libpng error -> ReadAttributeInfo() failed, tls left 0

  // acTL is honoured by the APNG-patched libpng 1.6.35 when it precedes IDAT.
  uint32_t frames = 1;
  const uint8_t* d = impl.source.data();
  const size_t n = impl.source.size();
  for (size_t pos = 8; pos + 12 <= n;) {
    const uint32_t len = rd_be32(d + pos);
    if (std::memcmp(d + pos + 4, "IDAT", 4) == 0) break;
    if (std::memcmp(d + pos + 4, "acTL", 4) == 0 && len == 8 && pos + 16 <= n) {
      frames = rd_be32(d + pos + 8);
      impl.apng = true;
      break;
    }
    if (len > n - pos - 12) break;
    pos += 12 + size_t(len);
  }

  impl.format = kFmtRgba8888;  // *(a3) = 4
  impl.width = int32_t(w);
  impl.height = int32_t(h);
  impl.header_width = impl.width;    // sub_1FFF30 copies +96 to +88 after the header
  impl.header_height = impl.height;
  impl.file_type = kFilePng;   // a3+64 = 1
  impl.frame_count = int32_t(frames);
  // Animated PNG limits, checked only when frames >= 2 and the decode mode is <= 1.
  if (frames >= 2 && impl.anim_mode <= 1) {
    if (int64_t(impl.width) * int64_t(impl.height) >= 0x15F91) return 0x80B35500u;  // "Image size is too large"
    if (frames >= 0x97) return 0x80B35500u;                                       // "Image frame count is too large"
  }
  if (frames == 0) return 0x80B35501u;  // "image array initialize error"
  impl.has_decoder = true;
  return 0;
}

// The SVG codec (factory sub_246190 type 8) and its header sub_2708A0, through sub_1FFF30:
// attribute block (impl +72) <- format 4, +16/+24/+40 = (int)image->width/height, +32 = 0,
// +64 file type 8, +8 = 1, +68 = 0x100000002, +76 = 0x00010001 (row alignment 1),
// +144 = 1 (impl +216, the alpha channel type: Premultiplied); frame table 1x1.
// sub_1FFF30 then copies +96 to +88/+112/+128. A failing header (no data, nsvgParse out of
// memory) leaves the per-thread codec error untouched (0 here: the SVG paths never set it),
// which sub_246990 turns into 0x80B35501 ("Unknown error").
uint32_t open_svg(Impl& impl) {
  auto codec = svg::create_codec();
  int32_t w = 0, h = 0;
  if (!svg::read_header(*codec, impl.source.data(), impl.source.size(), &w, &h)) return 0;
  impl.format = kFmtRgba8888;
  impl.width = w;
  impl.height = h;
  impl.header_width = w;
  impl.header_height = h;
  impl.file_type = kFileSvg;
  impl.frame_count = 1;
  impl.alpha_type = 1;  // ImageAlphaChannelType.Premultiplied
  impl.svg = std::move(codec);
  impl.has_decoder = true;
  return 0;
}

// The DDS codec header sub_245EB0 (factory type 7, wrapper sub_25F7B0): parse the file
// (sub_2458E0); a failed parse (format -1) is "SetAttributeInfo() error". Attribute block
// (impl +72): +0 format, +4 = -1, +8 = 1, +12 = 0, +24 = (width, height), +64 file type 7,
// +68 = 0, +76 = 0x00010001; the frame table (impl +8) gets mips x 1 empty entries. The
// alpha channel type (+144) is left Straight. sub_1FFF30 then copies +96 to +88/+112/+128.
uint32_t open_dds(Impl& impl) {
  const dds::Spec s = dds::parse(impl.source.data(), impl.source.size());
  if (!s.valid || s.mips == 0) return 0;  // "SetAttributeInfo() error" / "dds mips=0"
  impl.format = uint32_t(s.format);
  impl.width = int32_t(s.width);
  impl.height = int32_t(s.height);
  impl.header_width = impl.width;
  impl.header_height = impl.height;
  impl.file_type = kFileDds;
  impl.frame_count = 1;
  impl.frame_slots = s.mips;
  impl.dds = s;
  impl.has_decoder = true;
  return 0;
}

// The DDS "decode": wrapper sub_25FA10 -> codec +16 sub_2455E0 (LinkImageFromBuffer). The
// file is parsed again; the frame table must be mips x 1 and the texture a plain 2D one
// (cube maps: "Texture3D and TextureCubeMap not supported yet", codec error 0x80B35500,
// nothing linked); then entry i = {file + level offset, sub_245E10(level size)}, the levels
// halving down to 1. The entries point into the file buffer the image state keeps (+32), so
// nothing is copied natively; the host keeps a copy of level 0 (the only entry it reads).
bool link_dds(Impl& impl) {
  if (impl.source.empty()) return false;  // "images or image attribute not exist in LinkImageFromBuffer"
  const dds::Spec s = dds::parse(impl.source.data(), impl.source.size());
  if (!s.valid || s.mips != impl.frame_slots) return false;  // "images array size failed" (0x80B35501)
  if (s.faces != 1) return false;                            // cube map (0x80B35500)
  const uint8_t* level0 = impl.source.data() + s.level_offset[0][0];
  impl.frame0 = std::make_shared<const std::vector<uint8_t>>(level0, level0 + s.level_size[0]);
  return true;
}

// The JPEG codec header (factory type 2, codec +64 = SetAttribute 0x252800 ->
// SetAttributeInfo 0x24eb30): attribute block (impl +72) <- format 4 (RGBA8888), +8 = 1,
// +24/+40 = (image_width, image_height), +32 = 0, +64 file type 2, +68 = 0x100000002,
// +76 = 0x00010001, +112 (impl +184) = the EXIF orientation when the option asks for it;
// frame table 1x1. SetAttribute clears the per-thread codec error first.
uint32_t open_jpeg(Impl& impl) {
  if (!jpeg::available()) {
    host::note_unimplemented("libScePsm Image JPEG codec (libjpeg-turbo not linked: third_party/jpeg.cmake)");
    return 0x80B35500u;
  }
  jpeg::Header h;
  uint32_t err = 0;
  if (!jpeg::read_header(impl.source.data(), impl.source.size(), impl.apply_orientation, &h, &err)) return err;
  impl.format = kFmtRgba8888;
  impl.width = h.width;
  impl.height = h.height;
  impl.header_width = h.width;
  impl.header_height = h.height;
  impl.file_type = kFileJpg;
  impl.frame_count = 1;
  impl.orientation = h.orientation;
  impl.has_decoder = true;
  return 0;
}

// sub_295880 — build the impl of an encoded image: sniff, open the codec for the type.
// Returns the state status (0 or a negative PSM result). A codec header that fails without
// setting the per-thread codec error gets 0x80B35501 from sub_246990 ("Unknown error").
int32_t open_file_image(Impl& impl) {
  uint32_t tls = 0;
  const uint32_t type = sniff(impl.source.data(), impl.source.size(), &tls);
  if (type == kFilePng || type == kFileSvg || type == kFileDds || type == kFileJpg) {
    tls = type == kFilePng   ? open_png(impl)
          : type == kFileSvg ? open_svg(impl)
          : type == kFileDds ? open_dds(impl)
                             : open_jpeg(impl);
    if (tls == 0 && impl.has_decoder) return 0;
    if (tls == 0) tls = 0x80B35501u;
    return map_codec_error(tls);
  }
  if (type == kFileNone) return map_codec_error(tls);
  // Recognised by the native sniffer but its codec is not reproduced here (GIF, BMP,
  // JPEG XR). Host placeholder result.
  static const char* const kNames[] = {"none", "PNG", "JPEG", "?", "GIF", "BMP", "?", "DDS", "SVG", "WebP", "JPEG XR"};
  host::note_unimplemented(std::string("libScePsm Image codec for ") + kNames[type] + " (file type " + std::to_string(type) + ")");
  set_exception_info("Image unsupported");
  return err::kNotSupported;
}

// The codec decode step for a PNG impl (ImagePng::LoadImage 0x257230): libpng with the
// SetAttributeInfo transform flags — palette/tRNS expansion, gray->RGB, 1/2/4-bit gray
// expansion, strip_16, filler 0xFF after — i.e. 8-bit RGBA, R,G,B,A byte order, straight
// (non-premultiplied) alpha, rows tightly packed (row pitch 4*width).
bool decode_png(Impl& impl) {
  if (impl.width != impl.header_width || impl.height != impl.header_height) {
    // Image.SetDecodeSize on a PNG: what the PNG codec wrapper does with a decode size
    // other than the header size is not traced.
    host::note_unimplemented("libScePsm Image PNG decode at a SetDecodeSize size (scaled codec decode)");
    return false;
  }
  unsigned char* out = nullptr;
  unsigned w = 0, h = 0;
  const unsigned e = lodepng_decode32(&out, &w, &h, impl.source.data(), impl.source.size());
  if (e != 0 || !out || int32_t(w) != impl.width || int32_t(h) != impl.height) {
    std::free(out);
    return false;
  }
  if (impl.apng) host::note_unimplemented("libScePsm Image APNG frame decode (frame 0 taken from the IDAT default image)");
  impl.frame0 = std::make_shared<const std::vector<uint8_t>>(out, out + size_t(w) * h * 4);
  std::free(out);
  return true;
}

// The codec decode (+8) into the impl's frame table, as sub_295D00 (and the converter
// framework reading a source impl's frames) run it: only when no frame is populated yet,
// the codec is alive and the sizes at +112 (the decode size) and +128 (the header size)
// are positive.
void decode_frames(Impl& impl) {
  const bool sizes_ok = impl.width > 0 && impl.height > 0 && impl.header_width > 0 && impl.header_height > 0;
  if (impl.frame0 || !impl.has_decoder || !sizes_ok) return;
  if (impl.file_type == kFileDds) {
    // DDS: the codec "decode" links the frames to the file data (for RGBA8888: 4 bytes per
    // pixel, R first, tight rows, as stored in the file).
    link_dds(impl);
  } else if (impl.file_type == kFilePng) {
    decode_png(impl);
  } else if (impl.file_type == kFileJpg) {
    // Codec kind 2 (codec +8): the generic wrapper's sub_247C80 decodes at the header size
    // straight into the frame (LoadImageFromBuffer 0x251d20 -> DecompressImage 0x250830)
    // when there is no subrect and the decode size is the header size.
    if (impl.width != impl.header_width || impl.height != impl.header_height) {
      host::note_unimplemented("libScePsm Image JPEG decode at a SetDecodeSize size (sub_247C80 resize / DecompressSubImage 0x250db0)");
      return;
    }
    auto px = std::make_shared<std::vector<uint8_t>>();
    uint32_t err = 0;
    if (jpeg::decode(impl.source.data(), impl.source.size(), impl.width, impl.height, px.get(), &err)) impl.frame0 = std::move(px);
  } else if (impl.file_type == kFileSvg && impl.svg) {
    // sub_26F470 into the frame buffer (width x height, 4 bytes per pixel, tight rows).
    auto px = std::make_shared<std::vector<uint8_t>>();
    if (svg::decode(*impl.svg, impl.source.data(), impl.source.size(), impl.header_width, impl.header_height, impl.width,
                    impl.height, px.get()))
      impl.frame0 = std::move(px);
  }
}

// sub_295D00 — decode the impl into the object's pixel slot (state +16/+24).
int32_t decode_object(Object& obj) {
  if (!obj.impl) return 0;
  Impl& impl = *obj.impl;
  if (uint32_t(impl.width) > 0x1000 || uint32_t(impl.height) >= 0x1001) {
    set_exception_info("impl_size.w=" + std::to_string(impl.width) + ", impl_size.h=" + std::to_string(impl.height));
    obj.status = err::kOutOfRange;
    return err::kOutOfRange;
  }
  std::shared_ptr<const std::vector<uint8_t>> pixels;
  if (impl.format == kFmtRgba8888 || impl.format == kFmtRgba1010102 || impl.format == kFmtA8) {
    // Frames already decoded (impl +32 / +64 non-zero) are reused; otherwise the codec runs
    // when the frame table is non-empty and the sizes at +112 (the decode size) and +128
    // (the header size) are positive.
    decode_frames(impl);
    pixels = impl.frame0;
  } else {
    // Other impl formats (DDS BGRA8888 / RGB888 and the block-compressed DXT/BC formats) are
    // first converted to RGBA8888 by a new impl (sub_200EB0 with converter off_4A0E30, whose
    // per-frame step sub_23C320 -> sub_23E080 holds the DXT decompressor) that replaces the
    // image's impl. Not reproduced: the image is left as it was (codec kept, so the texture
    // side can still link its frames), with no pixels.
    host::note_unimplemented("libScePsm Image decode of impl format " + std::to_string(impl.format) +
                             " (RGBA8888 conversion sub_200EB0/off_4A0E30, DXT decompressor sub_23E080)");
  }
  // Decoders of non-animated images are released after decoding (+189 <= 1). The encoded
  // file stays with the image state (+32): DDS frames point into it.
  const bool native_impl = impl.format == kFmtRgba8888 || impl.format == kFmtRgba1010102 || impl.format == kFmtA8;
  if (native_impl && impl.anim_mode <= 1 && impl.has_decoder) {
    impl.has_decoder = false;
    impl.svg.reset();
    if (impl.file_type != kFileDds) {
      impl.source.clear();
      impl.source.shrink_to_fit();
    }
  }
  obj.pixels = pixels;
  if (obj.pixels) {
    if (impl.orientation >= 2) host::note_unimplemented("libScePsm Image orientation transform (impl +184 >= 2)");
    return 0;
  }
  set_exception_info("Unknown error 'default' case : errorcode=0x00000000");
  return err::kImageSystem;
}

// sub_296920 — impl pixel format -> ImageFormat (255 without impl / unknown).
uint32_t to_image_format(const Impl* impl) {
  if (!impl) return 255;
  switch (impl->format) {
    case kFmtRgba8888: return 0;
    case kFmtA8: return 1;
    case kFmtDxt1: return 2;
    case kFmtDxt3: return 3;
    case kFmtDxt5: return 4;
    case kFmtBc4U: return 5;
    case kFmtBc5U: return 6;
    case kFmtBc6U: return 7;
    case kFmtBc7U: return 8;
    case kFmtRgba1010102: return 9;
    default: return 255;
  }
}

// The raw-pixel impl sub_200910 builds (file type 0, one frame, straight alpha), already
// decoded by the sub_295D00 call that NewFromModeSize* make right after.
std::shared_ptr<Object> raw_object(uint32_t format, const ImageSize& size, std::vector<uint8_t> pixels) {
  auto impl = std::make_shared<Impl>();
  impl->format = format;
  impl->width = size.width;
  impl->height = size.height;
  impl->frame0 = std::make_shared<const std::vector<uint8_t>>(std::move(pixels));
  auto obj = std::make_shared<Object>();
  obj->impl = std::move(impl);
  obj->pixels = obj->impl->frame0;
  return obj;
}

}  // namespace

// NewFromFileImageWithOptionNative 0x2de7e0 / NewFromUnmanagedBufferFileImageWithOptionNative
// 0x2dea90 (and their option-less forms 0x2dea80 / 0x2ded10, which pass option = null):
// null data -> 0x80B30002; the file is copied into an allocator block (failure 0x80B30013),
// sub_295880 opens it; a negative state status is returned as is, otherwise a handle.
int32_t create_from_file_image(const uint8_t* data, size_t size, const PsmImageOption* option, uint32_t* handle) {
  if (!data) return err::kArgumentNull;
  auto obj = std::make_shared<Object>();
  auto impl = std::make_shared<Impl>();
  try {
    impl->source.assign(data, data + size);
  } catch (const std::bad_alloc&) {
    return err::kOutOfMemory;
  }
  if (option) {  // v41: low byte = AnimationDecodeMode, high byte = IsApplyOrientation != 0
    impl->anim_mode = uint8_t(option->animation_decode_mode);
    impl->apply_orientation = option->is_apply_orientation != 0;
  }
  obj->status = open_file_image(*impl);
  if (obj->status < 0) return obj->status;
  obj->impl = std::move(impl);
  *handle = register_object(std::move(obj));
  return 0;
}

// NewFromModeSizeBuffer 0x2df2a0: (mode, bufferFormat) -> impl format / bytes per pixel:
// (Rgba, Rgba8888) -> 4/4, (Rgba, Bgra8888) -> 6/4, (A, A8) -> 14/1, else "mode=%u
// bufferFormat=%u" 0x80B30003; width > 4096 or height > 4096 -> 0x80B30003; the array
// must hold w*h*bpp bytes ("current buffer size=%zu, required buffer size=%d" 0x80B30013).
// The pixels are taken over by the raw impl and decoded immediately (copied).
int32_t create_from_mode_size_buffer(uint32_t mode, const ImageSize& size, const uint8_t* buffer, size_t buffer_length,
                                     uint32_t buffer_format, uint32_t* handle) {
  if (!buffer) return err::kArgumentNull;
  uint32_t bpp = 0, format = 0;
  if (mode == 1 && buffer_format == 1) {
    bpp = 1, format = kFmtA8;
  } else if (mode == 0 && buffer_format == 0) {
    bpp = 4, format = kFmtRgba8888;
  } else if (mode == 0 && buffer_format == 2) {
    bpp = 4, format = kFmtBgra8888;
  } else {
    set_exception_info("mode=" + std::to_string(mode) + " bufferFormat=" + std::to_string(buffer_format));
    return err::kOutOfRange;
  }
  if (uint32_t(size.width) > 0x1000 || uint32_t(size.height) >= 0x1001) {
    set_exception_info("size.width=" + std::to_string(size.width) + ", size.height=" + std::to_string(size.height));
    return err::kOutOfRange;
  }
  const uint64_t need = uint64_t(uint32_t(size.height)) * bpp * uint32_t(size.width);
  if (buffer_length < need) {
    set_exception_info("current buffer size=" + std::to_string(buffer_length) + ", required buffer size=" + std::to_string(need));
    return err::kOutOfMemory;
  }
  if (format == kFmtBgra8888) {
    // Decoding a BGRA raw impl runs the generic converter (off_4A0E30 -> sub_23E080),
    // which is not reproduced.
    host::note_unimplemented("libScePsm Image::NewFromModeSizeBuffer with ImageBufferFormat.Bgra8888 (converter sub_23E080)");
    set_exception_info("Image unsupported");
    return err::kNotSupported;
  }
  std::vector<uint8_t> px(buffer, buffer + need);
  *handle = register_object(raw_object(format, size, std::move(px)));
  return 0;
}

// NewFromModeSizeColor 0x2ded20: mode Rgba -> format 4 (4 bytes), A -> 14 (1 byte), else
// "mode=%d" 0x80B30003; size limits as NewFromModeSizeBuffer; every channel is clamped to
// [0, 255]; Rgba fills R,G,B,A, A fills the alpha value only.
int32_t create_from_mode_size_color(uint32_t mode, const ImageSize& size, const int32_t color[4], uint32_t* handle) {
  uint32_t bpp = 0, format = 0;
  if (mode == 0) {
    bpp = 4, format = kFmtRgba8888;
  } else if (mode == 1) {
    bpp = 1, format = kFmtA8;
  } else {
    set_exception_info("mode=" + std::to_string(int32_t(mode)));
    return err::kOutOfRange;
  }
  if (uint32_t(size.width) > 0x1000 || uint32_t(size.height) >= 0x1001) {
    set_exception_info("size.width=" + std::to_string(size.width) + ", size.height=" + std::to_string(size.height));
    return err::kOutOfRange;
  }
  auto clamp = [](int32_t v) -> uint8_t { v = v < 0 ? 0 : v; return v >= 255 ? 255 : uint8_t(v); };
  const uint8_t r = clamp(color[0]), g = clamp(color[1]), b = clamp(color[2]), a = clamp(color[3]);
  const size_t count = size_t(uint32_t(size.width)) * uint32_t(size.height);
  std::vector<uint8_t> px(count * bpp);
  for (size_t i = 0; i < count; ++i) {
    if (bpp == 1) {
      px[i] = a;
    } else {
      px[i * 4 + 0] = r, px[i * 4 + 1] = g, px[i * 4 + 2] = b, px[i * 4 + 3] = a;
    }
  }
  *handle = register_object(raw_object(format, size, std::move(px)));
  return 0;
}

// AddRefNative 0x2df750: refcount +1 under the table lock (unknown handles ignored).
void add_ref(uint32_t handle) {
  std::lock_guard<std::mutex> lock(g_table_mutex);
  auto it = g_table.find(handle);
  if (it != g_table.end()) ++it->second.refs;
}

// ReleaseNative 0x2df7c0: refcount -1; at zero the object is destroyed and its table
// entry removed.
void release(uint32_t handle) {
  std::lock_guard<std::mutex> lock(g_table_mutex);
  auto it = g_table.find(handle);
  if (it != g_table.end() && --it->second.refs == 0) g_table.erase(it);
}

// GetSize 0x2df8b0: impl +96 (0,0 without impl). Unknown handle -> 0x80B30005.
int32_t get_size(uint32_t handle, ImageSize* size) {
  auto obj = lookup(handle);
  if (!obj) return err::kObjectDisposed;
  std::lock_guard<std::mutex> lock(obj->mutex);
  *size = obj->impl ? ImageSize{obj->impl->width, obj->impl->height} : ImageSize{0, 0};
  return 0;
}

// SetDecodeSize 0x2df980 (the Image.DecodeSize setter; UIImage.FilterIfNeeded uses it for
// SVG files instead of Image.Resize). Unknown handle -> 0x80B30005; no impl -> 0; width >
// 4096 or height > 4096 (unsigned compares) -> "size.width=%d, size.height=%d", status and
// result 0x80B30003; width or height 0 -> per-thread codec error 0x80B30003, returned and
// stored as status; impl file type 0, 6 or 7 (raw images, DDS) -> status and result
// 0x80B30004 (the native returns the per-thread codec error instead when one is pending,
// which the host does not track); otherwise impl +96 = +112 = size, +104 = 0, +152 = 1.
int32_t set_decode_size(uint32_t handle, const ImageSize& size) {
  auto obj = lookup(handle);
  if (!obj) return err::kObjectDisposed;
  std::lock_guard<std::mutex> lock(obj->mutex);
  if (!obj->impl) return 0;
  Impl& impl = *obj->impl;
  const uint32_t w = uint32_t(size.width), h = uint32_t(size.height);
  if (w > 0x1000 || h >= 0x1001) {
    set_exception_info("size.width=" + std::to_string(size.width) + ", size.height=" + std::to_string(size.height));
    obj->status = err::kOutOfRange;
    return err::kOutOfRange;
  }
  if (size.width <= 0 || size.height <= 0) {
    obj->status = err::kOutOfRange;
    return err::kOutOfRange;
  }
  if (impl.file_type <= 7 && ((193u >> impl.file_type) & 1u)) {
    obj->status = err::kInvalidOperation;
    return err::kInvalidOperation;
  }
  impl.width = size.width;
  impl.height = size.height;
  return 0;
}

// DecodeNative 0x2dfb00 -> sub_295D00.
int32_t decode(uint32_t handle) {
  auto obj = lookup(handle);
  if (!obj) return err::kObjectDisposed;
  std::lock_guard<std::mutex> lock(obj->mutex);
  return decode_object(*obj);
}

// GetFormatNative 0x2e63b0: sub_296920(impl).
int32_t get_format(uint32_t handle, uint32_t* format) {
  auto obj = lookup(handle);
  if (!obj) return err::kObjectDisposed;
  std::lock_guard<std::mutex> lock(obj->mutex);
  *format = to_image_format(obj->impl.get());
  return 0;
}

// GetFileFormatNative 0x2e6460: impl +136 mapped through dword_417170 for the types in
// bitmask 971 (1 PNG->Png, 2 JPEG->Jpg, 4 GIF->Gif, 7 DDS->Dds, 8 SVG->Svg, 9 WebP->WebP,
// 10 JXR->JpegXr); anything else (BMP, raw images, no impl) -> None.
int32_t get_file_format(uint32_t handle, uint32_t* format) {
  static const uint32_t kTable[10] = {1, 2, 1, 3, 1, 1, 4, 5, 6, 7};
  auto obj = lookup(handle);
  if (!obj) return err::kObjectDisposed;
  std::lock_guard<std::mutex> lock(obj->mutex);
  uint32_t v = 0;
  if (obj->impl) {
    const uint32_t i = obj->impl->file_type - 1;
    if (i <= 9 && ((971u >> i) & 1u)) v = kTable[i];
  }
  *format = v;
  return 0;
}

// GetAlphaChannelTypeNative 0x2e6600: impl +216 (0 without impl).
int32_t get_alpha_channel_type(uint32_t handle, uint32_t* type) {
  auto obj = lookup(handle);
  if (!obj) return err::kObjectDisposed;
  std::lock_guard<std::mutex> lock(obj->mutex);
  *type = obj->impl ? obj->impl->alpha_type : 0;
  return 0;
}

// GetFrameCountNative 0x2e6de0: impl +156.
int32_t get_frame_count(uint32_t handle, int32_t* count) {
  auto obj = lookup(handle);
  if (!obj) return err::kObjectDisposed;
  std::lock_guard<std::mutex> lock(obj->mutex);
  *count = obj->impl ? obj->impl->frame_count : 0;  // the native dereferences the impl unchecked
  return 0;
}

// GetSharedVideoMemoryTypeNative 0x2e68f0: state +56.
int32_t get_shared_video_memory_type(uint32_t handle, uint32_t* type) {
  auto obj = lookup(handle);
  if (!obj) return err::kObjectDisposed;
  std::lock_guard<std::mutex> lock(obj->mutex);
  *type = obj->shared_vm_type;
  return 0;
}

// SetSharedVideoMemoryTypeNative 0x2e69a0: type <= 2 stored at state +56, else 0x80B30003.
int32_t set_shared_video_memory_type(uint32_t handle, uint32_t type) {
  auto obj = lookup(handle);
  if (!obj) return err::kObjectDisposed;
  std::lock_guard<std::mutex> lock(obj->mutex);
  if (type > 2) return err::kOutOfRange;
  obj->shared_vm_type = type;
  return 0;
}

// ConvertNative 0x2e10a0. Images whose impl is not RGBA8888/A8 are decoded first. With a
// CPU option (option != DXTCompressByGPU) the target must be in bitmask 575 (Rgba8888,
// Alpha8, Dxt1, Dxt3, Dxt5, Bc4U, Rgba1010102), else "Format has to be ... image mode=%d"
// 0x80B30004. The target maps through dword_417148 {4,14,16,18,20,33,16,16,16,103}; for
// Alpha8 (14) the converter off_4A0FC0 (sub_23DCC0) takes byte 3 (alpha) of each RGBA8888
// pixel. The result is a new, decoded image (file type None, straight alpha, one frame,
// shared video memory type 0) with its own handle.
int32_t convert(uint32_t handle, uint32_t format, uint32_t option, uint32_t* converted, int32_t memory_block) {
  (void)memory_block;  // only consulted by the GPU compression paths
  auto obj = lookup(handle);
  if (!obj) return err::kObjectDisposed;
  std::lock_guard<std::mutex> lock(obj->mutex);
  if (!obj->impl) {
    host::note_unimplemented("libScePsm Image::ConvertNative on an image without impl");
    return err::kImageSystem;
  }
  if (obj->impl->format != kFmtRgba8888 && obj->impl->format != kFmtA8) {
    const int32_t r = decode_object(*obj);
    if (r < 0) return r;
  }
  if (option == 4) {
    // DXTCompressByGPU: the target ImageFormat 2..8 selects the block format (Dxt1 16, Dxt3
    // 18, Dxt5 20, Bc4U 33, Bc5U 34, Bc6U 35, Bc7U 36); a raw impl of that format over the size
    // rounded up to a multiple of 4 (sub_200910) receives the blocks the GPU texture compressor
    // (sub_296B70) writes from the RGBA8888 frame. Failure: 0x80B30004.
    static const uint32_t kTarget[9] = {0, 0, kFmtDxt1, kFmtDxt3, kFmtDxt5, kFmtBc4U, kFmtBc5U, kFmtBc6U, kFmtBc7U};
    if (format < 2 || format > 8) {
      set_exception_info("Format has to be ImageFormat.Dxt1 or ImageFormat.Dxt3 or ImageFormat.Dxt5: image mode=" +
                         std::to_string(obj->impl->format));
      return err::kInvalidOperation;
    }
    if (obj->impl->format != kFmtRgba8888) {
      host::note_unimplemented("libScePsm Image::ConvertNative DXTCompressByGPU from impl format " + std::to_string(obj->impl->format));
      return err::kInvalidOperation;
    }
    decode_frames(*obj->impl);
    const auto frame = obj->impl->frame0;
    const int32_t w = obj->impl->width, h = obj->impl->height;
    std::vector<uint8_t> blocks;
    if (!frame || frame->size() < size_t(uint32_t(w)) * uint32_t(h) * 4 || !gtc::compress(format, frame->data(), w, h, &blocks)) {
      set_exception_info("GPU texture compression failed");
      return err::kInvalidOperation;
    }
    *converted = register_object(raw_object(kTarget[format], ImageSize{(w + 3) & ~3, (h + 3) & ~3}, std::move(blocks)));
    return 0;
  }
  if (format >= 10 || !((575u >> format) & 1u)) {
    set_exception_info("Format has to be ImageFormat.Dxt1 or ImageFormat.Dxt3 or ImageFormat.Dxt5: image mode=" +
                       std::to_string(obj->impl->format));
    return err::kInvalidOperation;
  }
  if (format != 1) {
    host::note_unimplemented("libScePsm Image::ConvertNative to ImageFormat " + std::to_string(format));
    return err::kInvalidOperation;
  }
  // Alpha8: the converter requires an RGBA8888 source (sub_23DB90). ConvertNative case 14
  // builds the converted impl over the source impl (sub_200EB0, converter off_4A0FC0), and
  // the converter framework reads the source frames through the source codec's decode, so
  // an image that was never decoded is decoded here (its pixel slot is left untouched).
  if (obj->impl->format == kFmtRgba8888) decode_frames(*obj->impl);
  if (obj->impl->format != kFmtRgba8888 || !obj->impl->frame0) {
    if (obj->impl->format != kFmtRgba8888)
      host::note_unimplemented("libScePsm Image::ConvertNative Alpha8 from impl format " + std::to_string(obj->impl->format));
    set_exception_info("ConvertMode failed");
    return err::kImageSystem;
  }
  const std::vector<uint8_t>& src = *obj->impl->frame0;
  const size_t count = size_t(uint32_t(obj->impl->width)) * uint32_t(obj->impl->height);
  std::vector<uint8_t> a8(count);
  for (size_t i = 0; i < count && i * 4 + 3 < src.size(); ++i) a8[i] = src[i * 4 + 3];
  *converted = register_object(raw_object(kFmtA8, ImageSize{obj->impl->width, obj->impl->height}, std::move(a8)));
  return 0;
}

// ---- Image::ResizeNative (0x2dff70) -------------------------------------------------------
// The resize filter (vtable off_4A1638) built over the source impl by sub_200EB0 with the
// target size (+36) and the filter mode (+44 = ImageResizeFilter + 1); its per-frame step
// sub_25CE20 resamples an RGBA8888 frame (impl formats 3, 4, 0x18 on this path).
namespace {

// Kernels of the function table 0x4A1288 (index = mode - 1) and their supports (0x34FE20).
float k_box(float x) { return (x > -0.5f && x <= 0.5f) ? 1.0f : 0.0f; }  // sub_24A430
float k_triangle(float x) {                                              // sub_24A460
  const float t = x < 0.0f ? -x : x;
  return t < 1.0f ? 1.0f - t : 0.0f;
}
// sub_24A4A0 (Bell). Its first test is `0 > t`, so the 0.75 - t^2 branch is never taken:
// inside the support (t < 1 for this mode) the result is 0.5 * (t - 1.5)^2.
float k_bell(float x) {
  const float t = x < 0.0f ? -x : x;
  if (0.0f > t) return 0.75f - t * t;
  if (t < 1.5f) {
    const float u = t + -1.5f;
    return u * (u * 0.5f);
  }
  return 0.0f;
}
float k_hermite(float x) {  // sub_24A500
  const float t = x < 0.0f ? -x : x;
  return t < 1.0f ? t * (t * (t + t + -3.0f)) + 1.0f : 0.0f;
}
float k_bspline(float x) {  // sub_24A550
  const float t = x < 0.0f ? -x : x;
  if (t < 1.0f) {
    const float t2 = t * t;
    return (t * (t2 * 0.5f) - t2) + 0.6666667f;
  }
  if (t < 2.0f) {
    const float u = 2.0f - t;
    return u * (u * u) * 0.16666667f;
  }
  return 0.0f;
}
float k_mitchell(float x) {  // sub_24A5C0 (B = C = 1/3)
  const float x2 = x * x;
  const float t = x < 0.0f ? -x : x;
  if (t < 1.0f) return (x2 * t * 7.0f + x2 * -12.0f + 5.3333335f) / 6.0f;
  if (t < 2.0f) return (x2 * 12.0f + x2 * t * -2.3333333f + t * -20.0f + 10.666667f) / 6.0f;
  return 0.0f;
}
float k_lanczos3(float x) {  // sub_24A660 (sinf)
  const float t = x < 0.0f ? -x : x;
  if (!(t < 3.0f)) return 0.0f;
  const float a = t * 3.1415927f;
  float s1 = 1.0f, s2 = 1.0f;
  if (a != 0.0f) s1 = std::sin(a) / a;
  const float b = (t / 3.0f) * 3.1415927f;
  if (b != 0.0f) s2 = std::sin(b) / b;
  return s1 * s2;
}
using Kernel = float (*)(float);
constexpr Kernel kKernels[8] = {k_box, k_triangle, k_box, k_bell, k_hermite, k_bspline, k_mitchell, k_lanczos3};
constexpr float kSupport[8] = {0.5f, 1.0f, 0.5f, 1.0f, 1.0f, 2.0f, 2.0f, 3.0f};

struct Contrib {
  int32_t offset;  // byte offset 4 * j
  float weight;
};

// sub_249D80: per destination index, the source contributors (index clamped to
// [0, src - 1], stored as a byte offset) with weights divided by their sum.
std::vector<std::vector<Contrib>> weights(int32_t src, int32_t dst, int32_t mode) {
  const Kernel k = kKernels[mode - 1];
  const float support = kSupport[mode - 1];
  const float scale = float(dst) / float(src);
  std::vector<std::vector<Contrib>> out(size_t(dst > 0 ? dst : 0));
  const bool down = 1.0f > scale;
  const float width = down ? support / scale : support;
  const float fscale = 1.0f / scale;
  for (int32_t i = 0; i < dst; ++i) {
    const float center = float(double(i) + 0.5) / scale + -0.5f;
    const float left = std::ceil(center - width), right = std::floor(width + center);
    float sum = 0.0f;
    auto& c = out[size_t(i)];
    for (int32_t j = int32_t(left); right >= float(j); ++j) {
      float w = down ? k((center - float(j)) / fscale) / fscale : k(center - float(j));
      const int32_t n = j < src ? j : src - 1;
      c.push_back(Contrib{j < 0 ? 0 : 4 * n, w});
      sum += w;
    }
    for (auto& e : c) e.weight = e.weight / sum;
  }
  return out;
}

// The byte store of sub_24A720 / sub_24A860: above 255 -> 255, else max(0, v) truncated.
uint8_t store(float v) {
  if (v > 255.0f) return 255;
  const double d = double(v) > 0.0 ? double(v) : 0.0;
  return uint8_t(int32_t(d));
}

void accumulate(const uint8_t* base, const std::vector<Contrib>& c, uint8_t* out) {
  float r = 0, g = 0, b = 0, a = 0;
  for (const auto& e : c) {
    r += e.weight * float(base[e.offset]);
    g += e.weight * float(base[e.offset + 1]);
    b += e.weight * float(base[e.offset + 2]);
    a += e.weight * float(base[e.offset + 3]);
  }
  out[0] = store(r);
  out[1] = store(g);
  out[2] = store(b);
  out[3] = store(a);
}

// sub_25CE20 on an RGBA8888 frame: nearest neighbour (mode 1) or the separable filter.
std::vector<uint8_t> resample(const std::vector<uint8_t>& src, int32_t sw, int32_t sh, int32_t dw, int32_t dh,
                              int32_t mode) {
  std::vector<uint8_t> dst(size_t(dw) * dh * 4);
  if (mode == 1) {
    // Index maps (int)((i + 0.5) * (src / dst)); a row equal to the previous one is copied.
    const float fx = float(sw) / float(dw), fy = float(sh) / float(dh);
    std::vector<int32_t> xmap(static_cast<size_t>(dw));
    for (int32_t x = 0; x < dw; ++x) xmap[size_t(x)] = int32_t(fx * (float(x) + 0.5f));
    for (int32_t y = 0; y < dh; ++y) {
      const int32_t sy = int32_t(fy * (float(y) + 0.5f));
      for (int32_t x = 0; x < dw; ++x)
        std::memcpy(&dst[(size_t(y) * dw + x) * 4], &src[(size_t(sy) * sw + xmap[size_t(x)]) * 4], 4);
    }
    return dst;
  }
  // Horizontal pass (sub_24A720) into a temporary of dw columns x sh rows held column by
  // column, column x at (dw - 1 - x) * sh; the vertical pass (sub_24A860) walks it back
  // from the right edge of the destination.
  const auto wx = weights(sw, dw, mode);
  std::vector<uint8_t> tmp(size_t(dw) * sh * 4);
  for (int32_t y = 0; y < sh; ++y) {
    const uint8_t* row = &src[size_t(y) * sw * 4];
    for (int32_t x = 0; x < dw; ++x) accumulate(row, wx[size_t(x)], &tmp[(size_t(dw - 1 - x) * sh + y) * 4]);
  }
  const auto wy = weights(sh, dh, mode);
  for (int32_t k = 0; k < dw; ++k) {
    const uint8_t* col = &tmp[size_t(k) * sh * 4];
    const int32_t x = dw - 1 - k;
    for (int32_t y = 0; y < dh; ++y) accumulate(col, wy[size_t(y)], &dst[(size_t(y) * dw + x) * 4]);
  }
  return dst;
}

}  // namespace

// ResizeNative 0x2dff70: unknown handle -> 0x80B30005; no impl -> 0x80B30004; a size over
// 4096 -> "size.width=%d, size.height=%d" 0x80B30003 (also stored as the image status).
// The result is a new decoded RGBA8888 image (sub_295D00) under its own handle.
int32_t resize(uint32_t handle, const ImageSize& size, uint32_t filter, uint32_t* resized) {
  auto obj = lookup(handle);
  if (!obj) return err::kObjectDisposed;
  std::lock_guard<std::mutex> lock(obj->mutex);
  if (!obj->impl) return err::kInvalidOperation;
  if (uint32_t(size.width) > 0x1000 || uint32_t(size.height) >= 0x1001) {
    set_exception_info("size.width=" + std::to_string(size.width) + ", size.height=" + std::to_string(size.height));
    obj->status = err::kOutOfRange;
    return err::kOutOfRange;
  }
  Impl& impl = *obj->impl;
  if (impl.format != kFmtRgba8888) {
    // Other impl formats are first replaced by an RGBA8888 conversion (sub_200EB0 with the
    // converter off_4A0E30), the same step decode_object does not reproduce.
    host::note_unimplemented("libScePsm Image::ResizeNative of impl format " + std::to_string(impl.format) +
                             " (RGBA8888 conversion off_4A0E30)");
    set_exception_info("Failed to convert to ImageFormat.Rgba8888 from image mode=" + std::to_string(impl.format));
    return err::kInvalidOperation;
  }
  const int32_t mode = int32_t(filter) + 1;
  // The filter's prepare (sub_25CC20) needs a non-empty target and sub_249D80 has kernels
  // for modes 1..8 only; otherwise sub_200EB0 yields no image ("Resize failed").
  if (size.width <= 0 || size.height <= 0 || mode < 1 || mode > 8) {
    set_exception_info("Resize failed");
    return err::kInvalidOperation;
  }
  decode_frames(impl);  // the filter framework reads the source frames through the codec
  if (!impl.frame0 || impl.frame0->size() < size_t(uint32_t(impl.width)) * uint32_t(impl.height) * 4) {
    set_exception_info("Resize decode failed ret=80b30023");
    return err::kImageSystem;
  }
  *resized = register_object(
      raw_object(kFmtRgba8888, size, resample(*impl.frame0, impl.width, impl.height, size.width, size.height, mode)));
  return 0;
}

// Header-only open (sub_295880 with option = null) of a candidate texture file.
bool probe_file_image_size(const uint8_t* data, size_t size, int32_t* width, int32_t* height) {
  if (!data) return false;
  Impl impl;
  impl.source.assign(data, data + size);
  if (open_file_image(impl) < 0) return false;
  *width = impl.width;
  *height = impl.height;
  return true;
}

// sub_296990: with an impl whose decode (+112) and header (+128) sizes are positive, whose
// frame table has entries (+24 x +28, or the second table +56 x +60) none of which is
// populated yet (+32 == 0 and +64 == 0) and whose codec is alive, the codec decode runs
// (+8, mode 1: for DDS the frame link). Returns entry 0 of the frame table ({pointer, size};
// null when the table is empty or the decode failed). Only DDS impls reach this from the
// texture/pixel-data paths (they are the only images with a block-compressed format).
bool frame0(uint32_t handle, std::shared_ptr<const std::vector<uint8_t>>* out) {
  auto obj = lookup(handle);
  if (!obj) return false;
  std::lock_guard<std::mutex> lock(obj->mutex);
  out->reset();
  if (!obj->impl) return true;  // the native dereferences the impl unchecked
  Impl& impl = *obj->impl;
  const bool sizes_ok = impl.width > 0 && impl.height > 0 && impl.header_width > 0 && impl.header_height > 0;
  if (sizes_ok && impl.frame_slots != 0 && !impl.frame0 && impl.has_decoder) {
    if (impl.file_type == kFileDds) {
      link_dds(impl);
    } else {
      host::note_unimplemented("libScePsm Image frame link (sub_296990) of file type " + std::to_string(impl.file_type));
    }
  }
  if (impl.frame_slots != 0) *out = impl.frame0;
  return true;
}

bool snapshot(uint32_t handle, ImageState* out) {
  auto obj = lookup(handle);
  if (!obj) return false;
  std::lock_guard<std::mutex> lock(obj->mutex);
  out->has_impl = obj->impl != nullptr;
  out->internal_format = obj->impl ? obj->impl->format : 0;
  out->width = obj->impl ? obj->impl->width : 0;
  out->height = obj->impl ? obj->impl->height : 0;
  out->shared_video_memory_type = obj->shared_vm_type;
  out->alpha_channel_type = obj->impl ? obj->impl->alpha_type : 0;
  out->pixels = obj->pixels;
  return true;
}

// The pixel slot is immutable here (shared with snapshots and, for raw/decoded images,
// with the impl's frame 0); the native writes the single buffer both point at, so the
// host draws into a copy and repoints every slot that held the old buffer.
int32_t draw_into_pixels(uint32_t handle, int32_t (*draw)(const PixelTarget& target, void* ctx), void* ctx) {
  auto obj = lookup(handle);
  if (!obj) return err::kObjectDisposed;
  std::lock_guard<std::mutex> lock(obj->mutex);
  PixelTarget t;
  t.has_impl = obj->impl != nullptr;
  if (obj->impl) {
    t.internal_format = obj->impl->format;
    t.width = obj->impl->width;
    t.height = obj->impl->height;
  }
  std::shared_ptr<std::vector<uint8_t>> copy;
  if (obj->pixels) {
    copy = std::make_shared<std::vector<uint8_t>>(*obj->pixels);
    t.pixels = copy->data();
    t.size = copy->size();
  }
  const int32_t r = draw(t, ctx);
  if (copy) {
    const std::shared_ptr<const std::vector<uint8_t>> old = obj->pixels;
    obj->pixels = copy;
    if (obj->impl && obj->impl->frame0 == old) obj->impl->frame0 = copy;
  }
  return r;
}

}  // namespace ssn::psm::imaging
