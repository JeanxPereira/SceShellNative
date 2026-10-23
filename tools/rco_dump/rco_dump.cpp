// Standalone check of the libScePsm CxmlResources / Imaging reimplementation
// (psmhost/libScePsm/resources, psmhost/libScePsm/imaging) against real firmware .rco files.
//
// usage: rco_dump <vsh_asset dir> <out dir> [texture id ...]
//   Opens Sce.PlayStation.PUI_UI3.rco the way CxmlLoader.RegisterResourceFileFromSystemExVshAsset
//   does (systemCommonPath), sets the locale, lists the texture ids, fetches each requested
//   texture through GetTextureResource, decodes it through the Image object model and writes
//   the decoded pixels to <out dir>/<id>.png. Also exercises the resize path, the Alpha8
//   conversion and GetString on Sce.PlayStation.PUI.rco.
//
// usage: rco_dump --svg <vsh_asset dir> <out dir> [--scale k] [n | texture id ...]
//   Dumps every SVG texture raw and rasterises the first n (or the listed ids) through the
//   libScePsm SVG codec (imaging/image_svg.cpp), at header size and with DecodeSize k x.
//
// usage: rco_dump --rco <dir> <rco file name> <out dir> [n | texture id ...]
//   Opens any .rco (e.g. app/NPXS40087/psm/Application/resource/Sce.Vsh.ShellUI.Ime.rco),
//   lists every texture with its sniffed file type and Image open result and, for the first
//   n (default all) or the listed ids, writes it out. DDS textures (imaging/image_dds.cpp)
//   are written raw (<id>.dds), as Image.GetPixelData returns them (<id>.pixeldata.dds: the
//   libScePsm DDS header + the linked level 0) and, for viewing only, block-decompressed to
//   <id>.png by a plain BC1..BC5 decoder in this tool (libScePsm keeps them compressed and
//   the GPU samples them; its own CPU decompressor sub_23E080 is not reproduced). Other
//   textures are decoded through Image.Decode and written as PNG (JPEG files also raw,
//   <id>.jpg; the JPEG codec needs SSN_HAVE_PSM_JPEG and libjpeg-turbo, see
//   third_party/jpeg.cmake — work/jpeg_build builds such a variant).
//
// Build (VS x64 environment, from the repository root; no Mono needed):
//   clang-cl /std:c++20 /O2 /EHsc /utf-8 /DNOMINMAX /DWIN32_LEAN_AND_MEAN /D_CRT_SECURE_NO_WARNINGS
//     /Ilauncher\src /Ipsmhost /Ithird_party tools\rco_dump\rco_dump.cpp
//     psmhost\libScePsm\imaging\psm_common.cpp psmhost\libScePsm\imaging\image_core.cpp
//     psmhost\libScePsm\imaging\image_store.cpp psmhost\libScePsm\resources\cxml_document.cpp
//     psmhost\libScePsm\resources\cxml_resource.cpp psmhost\libScePsm\imaging\image_svg.cpp
//     psmhost\libScePsm\imaging\image_dds.cpp psmhost\libScePsm\imaging\image_jpeg.cpp third_party\lodepng\lodepng.cpp /Fe<dir>\rco_dump.exe
#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <string>
#include <vector>

#include "libScePsm/imaging/image_core.h"
#include "libScePsm/imaging/image_dds.h"
#include "libScePsm/imaging/image_store.h"
#include "libScePsm/imaging/psm_common.h"
#include "libScePsm/resources/cxml_document.h"
#include "libScePsm/resources/cxml_resource.h"
#include "lodepng/lodepng.h"

namespace ssn::host {
void note_unimplemented(const std::string& id) { std::printf("  [unimplemented] %s\n", id.c_str()); }
}  // namespace ssn::host

using namespace ssn::psm;

static std::string g_asset_dir;

static std::string resolve(const std::string& console_path) {
  const std::string prefix = "/system_ex/vsh_asset/";
  if (console_path.compare(0, prefix.size(), prefix) == 0) return g_asset_dir + "/" + console_path.substr(prefix.size());
  return console_path;
}

static void save(const std::string& path, const imaging::DecodedImage& d) {
  if (!d.pixels) {
    std::printf("  (no pixels)\n");
    return;
  }
  unsigned e;
  if (d.bytes_per_pixel == 1) {  // Alpha8: write as grey for viewing
    e = lodepng_encode_file(path.c_str(), d.pixels->data(), d.width, d.height, LCT_GREY, 8);
  } else {
    e = lodepng_encode32_file(path.c_str(), d.pixels->data(), d.width, d.height);
  }
  std::printf("  wrote %s (%s)\n", path.c_str(), e ? lodepng_error_text(e) : "ok");
}

// GetTextureResource -> Image(UnmanagedBuffer) -> Decode -> snapshot, as CxmlLoader does.
static int texture(uint32_t rh, const std::string& id, const resources::TextureResourceOption& opt, const std::string& out_dir,
                   bool alpha8_convert) {
  resources::TextureResourceNative tex{};
  int32_t r = resources::get_texture_resource(rh, 3, id, &opt, &tex);
  std::printf("GetTextureResource(%s, density=%.1f, %dx%d) = 0x%08x flags=0x%x ubuffer=%u bytes ninepatch=[%d,%d,%d,%d] margin=[%d,%d,%d,%d]\n",
              id.c_str(), opt.pixel_density, opt.width, opt.height, uint32_t(r), tex.attributes, tex.ubuffer.size,
              tex.nine_patch[0], tex.nine_patch[1], tex.nine_patch[2], tex.nine_patch[3], tex.margin[0], tex.margin[1],
              tex.margin[2], tex.margin[3]);
  if (r != 0) {
    std::printf("  exception info: %s\n", get_exception_info().c_str());
    return 1;
  }
  imaging::PsmImageOption io{0, 0};
  uint32_t ih = 0;
  r = imaging::create_from_file_image(reinterpret_cast<const uint8_t*>(static_cast<uintptr_t>(tex.ubuffer.address)),
                                      tex.ubuffer.size, &io, &ih);
  resources::dispose_resource_buffer_ptr(rh, &tex.ubuffer);
  std::printf("  Image(UnmanagedBuffer) = 0x%08x handle=0x%08x\n", uint32_t(r), ih);
  if (r != 0) {
    std::printf("  exception info: %s\n", get_exception_info().c_str());
    return 1;
  }
  imaging::ImageSize sz{};
  uint32_t ff = 0, fmt = 0, alpha = 0;
  int32_t frames = 0;
  imaging::get_size(ih, &sz);
  imaging::get_file_format(ih, &ff);
  imaging::get_format(ih, &fmt);
  imaging::get_alpha_channel_type(ih, &alpha);
  imaging::get_frame_count(ih, &frames);
  imaging::set_shared_video_memory_type(ih, 1);
  r = imaging::decode(ih);
  std::printf("  size=%dx%d fileFormat=%u format=%u alphaType=%u frames=%d decode=0x%08x\n", sz.width, sz.height, ff, fmt, alpha,
              frames, uint32_t(r));
  uint32_t show = ih;
  if (alpha8_convert) {
    uint32_t ch = 0;
    r = imaging::convert(ih, 1, 0, &ch, 0);
    std::printf("  Convert(Alpha8) = 0x%08x handle=0x%08x\n", uint32_t(r), ch);
    if (r == 0) show = ch;
  }
  auto d = imaging::find_image(int32_t(show));
  if (d) {
    std::printf("  find_image: %dx%d imageFormat=%u texturePixelFormat=%u bpp=%u sharedVM=%u bytes=%zu\n", d->width, d->height,
                d->image_format, d->texture_pixel_format, d->bytes_per_pixel, d->shared_video_memory_type,
                d->pixels ? d->pixels->size() : 0);
    if (d->pixels && !d->pixels->empty()) {
      const auto& p = *d->pixels;
      if (d->bytes_per_pixel == 4)
        std::printf("  first pixel RGBA = %u %u %u %u\n", p[0], p[1], p[2], p[3]);
      else
        std::printf("  first pixel A = %u\n", p[0]);
    }
    std::string name = id;
    if (opt.width > 0) name += "_resize" + std::to_string(opt.width);
    if (opt.pixel_density > 1.5f) name += "_4k";
    if (alpha8_convert) name += "_alpha8";
    save(out_dir + "/" + name + ".png", *d);
  }
  if (show != ih) imaging::release(show);
  imaging::release(ih);
  std::printf("  after release find_image -> %s\n", imaging::find_image(int32_t(ih)) ? "alive" : "gone");
  return 0;
}

// --svg mode: every texture of Sce.PlayStation.PUI_UI3.rco whose "src" file sniffs as SVG
// (libScePsm sub_26F3E0) is written raw to <out dir>/raw/<id>.svg; the selected ones (the
// first <n> SVG ids, or the ids listed) are also decoded through the Image object model at
// their header size and with Image.DecodeSize (SetDecodeSize 0x2df980) at k x, as
// UIImage.FilterIfNeeded does for SVG files, and written as PNG: <id>_premul.png holds the
// premultiplied pixels exactly as libScePsm hands them to textures, <id>.png an
// unpremultiplied copy for viewing.
static bool is_svg(const uint8_t* d, size_t n) {
  if (n >= 3 && d[0] == 0xEF && d[1] == 0xBB && d[2] == 0xBF) d += 3, n -= 3;
  return (n >= 5 && std::memcmp(d, "<?xml", 5) == 0) || (n >= 4 && std::memcmp(d, "<svg", 4) == 0);
}

static void save_rgba(const std::string& path, const std::vector<uint8_t>& px, int w, int h, bool unpremultiply) {
  std::vector<uint8_t> v = px;
  if (unpremultiply) {
    for (size_t i = 0; i + 3 < v.size(); i += 4) {
      const unsigned a = v[i + 3];
      for (int c = 0; c < 3; ++c) v[i + c] = a ? uint8_t(std::min(255u, (v[i + c] * 255u + a / 2) / a)) : 0;
    }
  }
  const unsigned e = lodepng_encode32_file(path.c_str(), v.data(), unsigned(w), unsigned(h));
  std::printf("  wrote %s (%s)\n", path.c_str(), e ? lodepng_error_text(e) : "ok");
}

static void svg_decode(const std::vector<uint8_t>& bytes, const std::string& id, const std::string& out_dir, int scale) {
  imaging::PsmImageOption io{0, 0};
  for (int pass = 0; pass < 2; ++pass) {
    if (pass == 1 && scale <= 1) break;
    uint32_t h = 0;
    int32_t r = imaging::create_from_file_image(bytes.data(), bytes.size(), &io, &h);
    if (r != 0) {
      std::printf("  %s: Image = 0x%08x (%s)\n", id.c_str(), uint32_t(r), get_exception_info().c_str());
      return;
    }
    if (pass == 1) {
      imaging::ImageSize s{};
      imaging::get_size(h, &s);
      const imaging::ImageSize ds{s.width * scale, s.height * scale};
      r = imaging::set_decode_size(h, ds);
      std::printf("  SetDecodeSize(%dx%d) = 0x%08x\n", ds.width, ds.height, uint32_t(r));
    }
    imaging::ImageSize sz{};
    uint32_t ff = 0, fmt = 0, alpha = 0;
    imaging::get_size(h, &sz);
    imaging::get_file_format(h, &ff);
    imaging::get_format(h, &fmt);
    imaging::get_alpha_channel_type(h, &alpha);
    r = imaging::decode(h);
    std::printf("  %s: size=%dx%d fileFormat=%u format=%u alphaType=%u decode=0x%08x\n", id.c_str(), sz.width, sz.height, ff, fmt,
                alpha, uint32_t(r));
    auto d = imaging::find_image(int32_t(h));
    if (r == 0 && d && d->pixels) {
      const std::string base = out_dir + "/" + id + (pass ? "_x" + std::to_string(scale) : "");
      save_rgba(base + "_premul.png", *d->pixels, d->width, d->height, false);
      save_rgba(base + ".png", *d->pixels, d->width, d->height, true);
    }
    imaging::release(h);
  }
}

static int svg_mode(int argc, char** argv) {
  if (argc < 4) {
    std::fprintf(stderr, "usage: rco_dump --svg <vsh_asset dir> <out dir> [--scale k] [n | id ...]\n");
    return 2;
  }
  g_asset_dir = argv[2];
  const std::string out_dir = argv[3];
  int scale = 1, first_n = 0;
  std::vector<std::string> wanted;
  for (int i = 4; i < argc; ++i) {
    if (std::strcmp(argv[i], "--scale") == 0 && i + 1 < argc) {
      scale = std::atoi(argv[++i]);
    } else if (argv[i][0] >= '0' && argv[i][0] <= '9') {
      first_n = std::atoi(argv[i]);
    } else {
      wanted.push_back(argv[i]);
    }
  }
  uint32_t rh = 0;
  int32_t r = resources::create(&rh, "Sce.PlayStation.PUI_UI3.rco", false, true, resolve);
  if (r != 0) return 1;
  resources::set_locale(rh, "en");
  std::string ids;
  resources::get_ids(rh, 3, &ids);
  std::vector<std::string> all;
  for (size_t p = 0; p <= ids.size();) {
    size_t q = ids.find(',', p);
    if (q == std::string::npos) q = ids.size();
    if (q > p) all.push_back(ids.substr(p, q - p));
    p = q + 1;
  }
  const std::string raw_dir = out_dir + "/raw";
  std::filesystem::create_directories(raw_dir);
  int nsvg = 0, decoded = 0;
  for (const auto& id : all) {
    resources::TextureResourceNative tex{};
    if (resources::get_texture_resource(rh, 3, id, nullptr, &tex) != 0) continue;
    const auto* p = reinterpret_cast<const uint8_t*>(static_cast<uintptr_t>(tex.ubuffer.address));
    std::vector<uint8_t> bytes(p, p + tex.ubuffer.size);
    resources::dispose_resource_buffer_ptr(rh, &tex.ubuffer);
    if (!is_svg(bytes.data(), bytes.size())) continue;
    ++nsvg;
    if (FILE* f = std::fopen((raw_dir + "/" + id + ".svg").c_str(), "wb")) {
      std::fwrite(bytes.data(), 1, bytes.size(), f);
      std::fclose(f);
    }
    const bool pick = wanted.empty() ? decoded < first_n : std::find(wanted.begin(), wanted.end(), id) != wanted.end();
    if (pick) {
      ++decoded;
      svg_decode(bytes, id, out_dir, scale);
    }
  }
  std::printf("%zu textures, %d SVG (raw files in %s), %d decoded\n", all.size(), nsvg, raw_dir.c_str(), decoded);
  resources::destroy(rh);
  return 0;
}

// ---- --rco mode ----------------------------------------------------------------------
// Viewer-only BC decoder (standard S3TC/RGTC rules, 1/3-2/3 interpolation with integer
// rounding). Not libScePsm code: it only turns the block data the codec hands over into
// something a person can look at.
static void rgb565(uint16_t c, int out[3]) {
  out[0] = ((c >> 11) & 31) * 255 / 31;
  out[1] = ((c >> 5) & 63) * 255 / 63;
  out[2] = (c & 31) * 255 / 31;
}
static void bc1_colors(const uint8_t* b, uint8_t pal[4][4], bool force4) {
  const uint16_t c0 = uint16_t(b[0] | (b[1] << 8)), c1 = uint16_t(b[2] | (b[3] << 8));
  int a[3], z[3];
  rgb565(c0, a), rgb565(c1, z);
  const bool four = c0 > c1 || force4;
  for (int k = 0; k < 3; ++k) {
    pal[0][k] = uint8_t(a[k]), pal[1][k] = uint8_t(z[k]);
    if (four) {
      pal[2][k] = uint8_t((2 * a[k] + z[k]) / 3), pal[3][k] = uint8_t((a[k] + 2 * z[k]) / 3);
    } else {
      pal[2][k] = uint8_t((a[k] + z[k]) / 2), pal[3][k] = 0;
    }
  }
  pal[0][3] = pal[1][3] = pal[2][3] = 255;
  pal[3][3] = four ? 255 : 0;
}
static void bc4_values(const uint8_t* b, uint8_t v[8]) {
  v[0] = b[0], v[1] = b[1];
  if (v[0] > v[1]) {
    for (int i = 1; i < 7; ++i) v[i + 1] = uint8_t(((7 - i) * v[0] + i * v[1]) / 7);
  } else {
    for (int i = 1; i < 5; ++i) v[i + 1] = uint8_t(((5 - i) * v[0] + i * v[1]) / 5);
    v[6] = 0, v[7] = 255;
  }
}
static uint64_t bits48(const uint8_t* b) {
  uint64_t x = 0;
  for (int i = 0; i < 6; ++i) x |= uint64_t(b[2 + i]) << (8 * i);
  return x;
}
// Internal format 16 (DXT1), 18 (DXT3), 20 (DXT5), 33 (BC4), 34 (BC5) -> RGBA8.
static bool bc_decompress(int32_t fmt, const uint8_t* src, size_t n, int w, int h, std::vector<uint8_t>* out) {
  if (fmt != 16 && fmt != 18 && fmt != 20 && fmt != 33 && fmt != 34) return false;
  const int bs = (fmt == 16 || fmt == 33) ? 8 : 16;
  const int bw = (w + 3) / 4, bh = (h + 3) / 4;
  if (size_t(bw) * bh * bs > n) return false;
  out->assign(size_t(w) * h * 4, 0);
  for (int by = 0; by < bh; ++by)
    for (int bx = 0; bx < bw; ++bx) {
      const uint8_t* b = src + (size_t(by) * bw + bx) * bs;
      uint8_t px[16][4];
      if (fmt == 33 || fmt == 34) {
        uint8_t r[8], g[8] = {};
        bc4_values(b, r);
        const uint64_t ri = bits48(b);
        uint64_t gi = 0;
        if (fmt == 34) bc4_values(b + 8, g), gi = bits48(b + 8);
        for (int i = 0; i < 16; ++i) {
          const uint8_t rv = r[(ri >> (3 * i)) & 7];
          if (fmt == 33) {
            px[i][0] = px[i][1] = px[i][2] = rv, px[i][3] = 255;
          } else {
            px[i][0] = rv, px[i][1] = g[(gi >> (3 * i)) & 7], px[i][2] = 0, px[i][3] = 255;
          }
        }
      } else {
        const uint8_t* cb = fmt == 16 ? b : b + 8;
        uint8_t pal[4][4];
        bc1_colors(cb, pal, fmt != 16);
        const uint32_t idx = uint32_t(cb[4]) | (uint32_t(cb[5]) << 8) | (uint32_t(cb[6]) << 16) | (uint32_t(cb[7]) << 24);
        uint8_t av[8] = {};
        uint64_t ai = 0;
        if (fmt == 20) bc4_values(b, av), ai = bits48(b);
        for (int i = 0; i < 16; ++i) {
          std::memcpy(px[i], pal[(idx >> (2 * i)) & 3], 4);
          if (fmt == 18) px[i][3] = uint8_t(((b[i / 2] >> (4 * (i & 1))) & 15) * 17);
          if (fmt == 20) px[i][3] = av[(ai >> (3 * i)) & 7];
        }
      }
      for (int y = 0; y < 4; ++y)
        for (int x = 0; x < 4; ++x) {
          const int X = bx * 4 + x, Y = by * 4 + y;
          if (X < w && Y < h) std::memcpy(out->data() + (size_t(Y) * w + X) * 4, px[y * 4 + x], 4);
        }
    }
  return true;
}

static std::string g_rco_dir;
static std::string resolve_any(const std::string& console_path) {
  const size_t slash = console_path.find_last_of('/');
  return g_rco_dir + "/" + (slash == std::string::npos ? console_path : console_path.substr(slash + 1));
}

static void write_file(const std::string& path, const uint8_t* d, size_t n) {
  if (FILE* f = std::fopen(path.c_str(), "wb")) {
    std::fwrite(d, 1, n, f);
    std::fclose(f);
  }
}

static int rco_mode(int argc, char** argv) {
  if (argc < 5) {
    std::fprintf(stderr, "usage: rco_dump --rco <dir> <rco file name> <out dir> [n | id ...]\n");
    return 2;
  }
  g_rco_dir = argv[2];
  const std::string rco = argv[3], out_dir = argv[4];
  int first_n = -1;
  std::vector<std::string> wanted;
  for (int i = 5; i < argc; ++i) {
    if (argv[i][0] >= '0' && argv[i][0] <= '9') first_n = std::atoi(argv[i]);
    else wanted.push_back(argv[i]);
  }
  std::filesystem::create_directories(out_dir);
  uint32_t rh = 0;
  int32_t r = resources::create(&rh, rco, false, false, resolve_any);
  std::printf("Create(\"%s\") = 0x%08x\n", rco.c_str(), uint32_t(r));
  if (r != 0) return 1;
  resources::set_locale(rh, "en");
  std::string ids;
  resources::get_ids(rh, 3, &ids);
  std::vector<std::string> all;
  for (size_t p = 0; p <= ids.size();) {
    size_t q = ids.find(',', p);
    if (q == std::string::npos) q = ids.size();
    if (q > p) all.push_back(ids.substr(p, q - p));
    p = q + 1;
  }
  int written = 0, failed = 0;
  for (const auto& id : all) {
    resources::TextureResourceNative tex{};
    if (resources::get_texture_resource(rh, 3, id, nullptr, &tex) != 0) {
      std::printf("%s: GetTextureResource failed (%s)\n", id.c_str(), get_exception_info().c_str());
      continue;
    }
    const auto* p = reinterpret_cast<const uint8_t*>(static_cast<uintptr_t>(tex.ubuffer.address));
    std::vector<uint8_t> bytes(p, p + tex.ubuffer.size);
    resources::dispose_resource_buffer_ptr(rh, &tex.ubuffer);
    imaging::PsmImageOption io{0, 0};
    uint32_t ih = 0;
    r = imaging::create_from_file_image(bytes.data(), bytes.size(), &io, &ih);
    uint32_t ff = 0, fmt = 255;
    imaging::ImageSize sz{};
    if (r == 0) imaging::get_file_format(ih, &ff), imaging::get_format(ih, &fmt), imaging::get_size(ih, &sz);
    const char* sniffed = bytes.size() >= 4 && std::memcmp(bytes.data(), "DDS ", 4) == 0 ? "DDS"
                          : bytes.size() >= 8 && bytes[0] == 0x89 && bytes[1] == 'P'  ? "PNG"
                          : bytes.size() >= 2 && bytes[0] == 0xFF && bytes[1] == 0xD8 ? "JPEG"
                          : is_svg(bytes.data(), bytes.size())                         ? "SVG"
                                                                                       : "other";
    const bool pick = !wanted.empty() ? std::find(wanted.begin(), wanted.end(), id) != wanted.end()
                                      : (first_n < 0 || written < first_n);
    std::printf("%-44s %-5s %8zu bytes  Image=0x%08x fileFormat=%u imageFormat=%u %dx%d%s%s\n", id.c_str(), sniffed, bytes.size(),
                uint32_t(r), ff, fmt, sz.width, sz.height, r ? " exc=" : "", r ? get_exception_info().c_str() : "");
    if (r != 0) {
      ++failed;
      continue;
    }
    if (pick) {
      ++written;
      const std::string base = out_dir + "/" + id;
      if (std::strcmp(sniffed, "DDS") == 0) {
        write_file(base + ".dds", bytes.data(), bytes.size());
        std::shared_ptr<const std::vector<uint8_t>> level0;
        imaging::frame0(ih, &level0);
        const imaging::dds::Spec spec = imaging::dds::parse(bytes.data(), bytes.size());
        std::printf("  dds: internal format %d, %ux%u, %u mips, %u faces, level0 %zu bytes (linked %zu)\n", spec.format, spec.width,
                    spec.height, spec.mips, spec.faces, size_t(spec.level_size[0]), level0 ? level0->size() : 0);
        if (level0) {
          uint8_t hdr[148];
          const uint32_t hl = imaging::dds::write_header(fmt, sz.width, sz.height, hdr);
          std::vector<uint8_t> pd(hdr, hdr + hl);
          pd.insert(pd.end(), level0->begin(), level0->end());
          if (fmt >= 2 && fmt <= 8) write_file(base + ".pixeldata.dds", pd.data(), pd.size());
          std::vector<uint8_t> rgba;
          if (spec.format == 4) {
            rgba.assign(level0->begin(), level0->end());
          } else if (!bc_decompress(spec.format, level0->data(), level0->size(), sz.width, sz.height, &rgba)) {
            std::printf("  (no viewer decoder for internal format %d)\n", spec.format);
          }
          if (!rgba.empty()) save_rgba(base + ".png", rgba, sz.width, sz.height, false);
        }
      } else {
        if (std::strcmp(sniffed, "JPEG") == 0) write_file(base + ".jpg", bytes.data(), bytes.size());
        const int32_t dr = imaging::decode(ih);
        auto d = imaging::find_image(int32_t(ih));
        if (dr == 0 && d && d->pixels && d->bytes_per_pixel == 4)
          save_rgba(base + ".png", *d->pixels, d->width, d->height, d->alpha_channel_type == 1);
        else
          std::printf("  decode = 0x%08x (%s)\n", uint32_t(dr), get_exception_info().c_str());
      }
    }
    imaging::release(ih);
  }
  std::printf("%zu textures, %d written, %d failed to open\n", all.size(), written, failed);
  resources::destroy(rh);
  return 0;
}

// --file mode: one encoded image file through the Image object model (option
// IsApplyOrientation = 1), its attributes printed and the decoded pixels written as PNG.
static int file_mode(int argc, char** argv) {
  if (argc < 4) {
    std::fprintf(stderr, "usage: rco_dump --file <image file> <out.png>\n");
    return 2;
  }
  std::vector<uint8_t> bytes;
  if (FILE* f = std::fopen(argv[2], "rb")) {
    std::fseek(f, 0, SEEK_END);
    bytes.resize(size_t(std::ftell(f)));
    std::fseek(f, 0, SEEK_SET);
    if (!bytes.empty() && std::fread(bytes.data(), 1, bytes.size(), f) != bytes.size()) bytes.clear();
    std::fclose(f);
  }
  imaging::PsmImageOption io{0, 1};
  uint32_t ih = 0;
  int32_t r = imaging::create_from_file_image(bytes.data(), bytes.size(), &io, &ih);
  if (r != 0) {
    std::printf("%s: Image = 0x%08x (%s)\n", argv[2], uint32_t(r), get_exception_info().c_str());
    return 1;
  }
  imaging::ImageSize sz{};
  uint32_t ff = 0, fmt = 0;
  imaging::get_size(ih, &sz);
  imaging::get_file_format(ih, &ff);
  imaging::get_format(ih, &fmt);
  r = imaging::decode(ih);
  std::printf("%s: fileFormat=%u imageFormat=%u %dx%d decode=0x%08x%s%s\n", argv[2], ff, fmt, sz.width, sz.height, uint32_t(r),
              r ? " " : "", r ? get_exception_info().c_str() : "");
  auto d = imaging::find_image(int32_t(ih));
  if (r == 0 && d && d->pixels) save_rgba(argv[3], *d->pixels, d->width, d->height, false);
  imaging::release(ih);
  return r == 0 ? 0 : 1;
}

int main(int argc, char** argv) {
  if (argc >= 2 && std::strcmp(argv[1], "--file") == 0) return file_mode(argc, argv);
  if (argc >= 2 && std::strcmp(argv[1], "--svg") == 0) return svg_mode(argc, argv);
  if (argc >= 2 && std::strcmp(argv[1], "--rco") == 0) return rco_mode(argc, argv);
  if (argc < 3) {
    std::fprintf(stderr, "usage: rco_dump <vsh_asset dir> <out dir> [texture id ...]\n");
    return 2;
  }
  g_asset_dir = argv[1];
  const std::string out_dir = argv[2];

  uint32_t rh = 0;
  int32_t r = resources::create(&rh, "Sce.PlayStation.PUI_UI3.rco", false, true, resolve);
  std::printf("Create(\"Sce.PlayStation.PUI_UI3.rco\", systemCommonPath) = 0x%08x handle=0x%08x (host %s)\n", uint32_t(r), rh,
              resolve(resources::normalize_path("/system_ex/vsh_asset/Sce.PlayStation.PUI_UI3.rco")).c_str());
  if (r != 0) return 1;
  r = resources::set_locale(rh, "en");
  std::printf("SetLocale(\"en\") = 0x%08x\n", uint32_t(r));
  for (uint32_t t : {0u, 2u, 3u, 6u, 8u, 9u, 10u, 11u, 13u}) {
    uint32_t has = 0;
    resources::has_resource_type(rh, t, &has);
    std::printf("HasResourceType(%u) = %u\n", t, has);
  }
  uint32_t count = 0;
  r = resources::get_id_count(rh, 3, &count);
  std::printf("GetIdCount(Texture) = 0x%08x count=%u\n", uint32_t(r), count);
  std::string ids;
  r = resources::get_ids(rh, 3, &ids);
  std::printf("GetIds(Texture) = 0x%08x, %zu chars, first ids: %.200s...\n", uint32_t(r), ids.size(), ids.c_str());
  r = resources::get_id_count(rh, 2, &count);
  std::printf("GetIdCount(String) = 0x%08x (exception info \"%s\")\n", uint32_t(r), get_exception_info().c_str());

  std::vector<std::string> wanted;
  for (int i = 3; i < argc; ++i) wanted.push_back(argv[i]);
  if (wanted.empty()) wanted.push_back("image_basemat");
  for (const auto& id : wanted) texture(rh, id, resources::TextureResourceOption{0.f, 0, 0}, out_dir, false);
  // 4K variant, resize path (src -> src_lv1 -> src_lv2) and the Alpha8 conversion.
  texture(rh, "image_basemat", resources::TextureResourceOption{2.f, 0, 0}, out_dir, false);
  texture(rh, "iconid_challenges_bronze", resources::TextureResourceOption{0.f, 0, 0}, out_dir, false);
  texture(rh, "iconid_challenges_bronze", resources::TextureResourceOption{0.f, 200, 200}, out_dir, false);
  texture(rh, "image_pulldown_arrow", resources::TextureResourceOption{0.f, 0, 0}, out_dir, true);
  texture(rh, "no_such_texture", resources::TextureResourceOption{0.f, 0, 0}, out_dir, false);
  // Sound scripts (SystemSound.Initialize -> GetSoundScriptAll, GetSoundScriptCommand for k >= 3).
  uint32_t ns = 0;
  resources::get_id_count(rh, 9, &ns);
  std::vector<resources::SoundScript> scripts;
  uint32_t got = 0;
  r = resources::get_sound_script_all(rh, ns, &scripts, &got);
  std::printf("GetIdCount(SoundScript) = %u, GetSoundScriptAll = 0x%08x count=%u\n", ns, uint32_t(r), got);
  for (size_t i = 0; i < scripts.size() && i < 3; ++i) {
    const auto& s = scripts[i];
    std::printf("  %s commands=%u", s.id ? s.id : "(null)", s.command_count);
    for (uint32_t k = 0; k < s.command_count && k < 3; ++k) {
      const auto& c = s.commands[k];
      std::printf(" [%s %s tgt=%s value=%u:%d dir=%d]", c.id ? c.id : "-", c.command ? c.command : "-", c.target ? c.target : "-",
                  c.value_size, c.value[0], c.direction);
    }
    std::printf("\n");
  }
  for (int idx : {3, 4}) {
    const uint32_t caps[4] = {3, 2, 2, 2};
    resources::SoundScriptCommandResult cr;
    r = resources::get_sound_script_command(rh, 9, "psfx_psvr2_setup_section_finish", idx, caps, &cr);
    std::printf("GetSoundScriptCommand(psfx_psvr2_setup_section_finish, %d) = 0x%08x cmd=%s tgt=%s values=%zu dir=%d%s%s\n", idx,
                uint32_t(r), cr.command.c_str(), cr.target.c_str(), cr.value.size(), cr.direction, r ? " exc=" : "",
                r ? get_exception_info().c_str() : "");
  }
  resources::destroy(rh);

  // Strings: Sce.PlayStation.PUI.rco carries a stringtable with per-locale documents.
  uint32_t ph = 0;
  r = resources::create(&ph, "Sce.PlayStation.PUI.rco", false, true, resolve);
  std::printf("Create(\"Sce.PlayStation.PUI.rco\") = 0x%08x\n", uint32_t(r));
  if (r == 0) {
    std::string locales;
    resources::get_ids(ph, 2, &locales);
    std::printf("GetIds(String) (locale ids) = %s\n", locales.c_str());
    r = resources::set_locale(ph, "en");
    std::printf("SetLocale(\"en\") = 0x%08x\n", uint32_t(r));
    // First <string> id of the English document, read with the document reader directly.
    auto f = resources::FileSource::open(resolve("/system_ex/vsh_asset/Sce.PlayStation.PUI.rco"));
    resources::CxmlDocument doc;
    doc.load_from_file(*f);
    const int32_t st = doc.find_child_named(doc.root(), "stringtable");
    const int32_t en = doc.find_element(st, "locale", "en");
    const uint32_t k = doc.find_attribute(en, "src");
    const uint32_t slot = uint32_t(en) + 28 + 16 * k;
    std::vector<uint8_t> bytes(doc.u32(slot + 12));
    f->pread(bytes.data(), bytes.size(), uint64_t(doc.header.file_off) + doc.u32(slot + 8));
    resources::CxmlDocument ld;
    std::printf("locale document load = 0x%08x\n", uint32_t(ld.load_from_memory(bytes.data(), bytes.size(), false)));
    int shown = 0;
    for (int32_t c = ld.first_child(ld.root()); c >= 0 && shown < 5; c = ld.next_sibling(c), ++shown) {
      const uint32_t ik = ld.find_attribute(c, "id");
      const uint32_t is = uint32_t(c) + 28 + 16 * ik;
      const char* sid = reinterpret_cast<const char*>(ld.idtable.data()) + ld.u32(is + 8) + 4;
      std::u16string text;
      bool set = false;
      r = resources::get_string(ph, sid, &text, &set);
      std::string u8;
      for (char16_t ch : text) u8 += ch < 0x80 ? char(ch) : '?';
      std::printf("GetString(\"%s\") = 0x%08x \"%s\"\n", sid, uint32_t(r), u8.c_str());
    }
    resources::destroy(ph);
  }
  return 0;
}
