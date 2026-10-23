// libScePsm JPEG codec (FW 4.03) over libjpeg-turbo 2.0.2. See image_jpeg.h.
#include "libScePsm/imaging/image_jpeg.h"

#if defined(SSN_HAVE_PSM_JPEG)

#include <csetjmp>
#include <cstdio>
#include <cstring>

extern "C" {
#include "jpeglib.h"
#include "jerror.h"
}

namespace ssn::psm::imaging::jpeg {
namespace {

// The setjmp sites (SetAttributeInfo 0x24eb30, DecompressImage 0x250830, FinishDecompress
// 0x24eaa0) map the libjpeg message code the error_exit hook (0x251ae0) long-jumps with.
static_assert(JERR_EMPTY_IMAGE == 32 && JERR_OUT_OF_MEMORY == 54 && JERR_SOF_UNSUPPORTED == 60 && JERR_UNKNOWN_MARKER == 68 &&
                  JERR_INPUT_EMPTY == 42,
              "libjpeg-turbo 2.0.2 message codes (JPEG_LIB_VERSION 62)");
uint32_t map_jump(int code) {
  switch (code) {
    case JERR_EMPTY_IMAGE:
    case JERR_SOF_UNSUPPORTED:
    case JERR_UNKNOWN_MARKER: return 0x80B35500u;
    case JERR_OUT_OF_MEMORY: return 0x80B35507u;
    default: return 0x80B35501u;
  }
}

struct ErrorMgr {
  jpeg_error_mgr pub;
  std::jmp_buf jump;
};

// 0x251ae0: longjmp(codec jmp_buf, err->msg_code). The other jpeg_std_error hooks are kept
// (warnings go through the stock output_message).
[[noreturn]] void error_exit(j_common_ptr cinfo) {
  ErrorMgr* e = reinterpret_cast<ErrorMgr*>(cinfo->err);
  std::longjmp(e->jump, cinfo->err->msg_code);
}

// The codec's own source manager over the encoded buffer {pointer, size, position} (codec
// +16): init 0x251b00 sets the start-of-file flag; fill 0x251b10 ("_mread") copies at most
// 4096 bytes into its 4096-byte buffer, and at the end of the data raises JERR_INPUT_EMPTY
// on an empty file or inserts a fake EOI (no JWRN_JPEG_EOF warning); skip 0x251be0 is the
// stock skip over that fill; resync is jpeg_resync_to_restart, term does nothing.
struct Source {
  jpeg_source_mgr pub;
  const uint8_t* ptr;
  size_t size;
  size_t pos;
  bool start_of_file;
  JOCTET buffer[4096];
};

void init_source(j_decompress_ptr cinfo) { reinterpret_cast<Source*>(cinfo->src)->start_of_file = true; }

boolean fill_input_buffer(j_decompress_ptr cinfo) {
  Source* s = reinterpret_cast<Source*>(cinfo->src);
  size_t n = s->size - s->pos;
  if (n >= 0x1001) n = 4096;
  if (n != 0) {
    std::memcpy(s->buffer, s->ptr, n);
    s->ptr += n;
    s->pos += n;
  } else {
    if (s->start_of_file) ERREXIT(cinfo, JERR_INPUT_EMPTY);
    s->buffer[0] = 0xFF;
    s->buffer[1] = JPEG_EOI;
    n = 2;
  }
  s->pub.next_input_byte = s->buffer;
  s->pub.bytes_in_buffer = n;
  s->start_of_file = false;
  return TRUE;
}

void skip_input_data(j_decompress_ptr cinfo, long num_bytes) {
  if (num_bytes <= 0) return;
  jpeg_source_mgr* src = cinfo->src;
  while (num_bytes > long(src->bytes_in_buffer)) {
    num_bytes -= long(src->bytes_in_buffer);
    fill_input_buffer(cinfo);
  }
  src->next_input_byte += size_t(num_bytes);
  src->bytes_in_buffer -= size_t(num_bytes);
}

void term_source(j_decompress_ptr) {}

void attach_source(j_decompress_ptr cinfo, Source* s, const uint8_t* data, size_t size) {
  s->ptr = data;
  s->size = size;
  s->pos = 0;
  s->start_of_file = false;
  s->pub.next_input_byte = nullptr;
  s->pub.bytes_in_buffer = 0;
  s->pub.init_source = init_source;
  s->pub.fill_input_buffer = fill_input_buffer;
  s->pub.skip_input_data = skip_input_data;
  s->pub.resync_to_restart = jpeg_resync_to_restart;
  s->pub.term_source = term_source;
  cinfo->src = &s->pub;
}

// The EXIF part of SetAttributeInfo (0x24eb30; a TinyEXIF-style parser, entries read by
// 0x252f90) as far as the orientation goes: the first saved APP1 marker must be
// "Exif\0\0" + a TIFF header ("II"/"MM", 42) with more than 13 bytes; IFD0 at the TIFF
// offset needs offset + 6 < len, offset + 8 <= len and offset + 12 * count + 12 <= len.
// Every IFD0 entry inside the data whose tag is 0x0112 and type SHORT with at least one
// value sets the orientation (the last one wins); SHORT values are inline for counts <= 2,
// else at TIFF offset (bounds-checked). The rest of the parse (sub-IFDs, GPS) does not
// touch the orientation.
uint32_t exif_orientation(const uint8_t* d, uint32_t len) {
  if (len < 6 || std::memcmp(d, "Exif\0\0", 6) != 0 || len <= 13) return 0;
  bool le;
  if (d[6] == 'M' && d[7] == 'M') {
    le = false;
    if (((d[8] << 8) | d[9]) != 42) return 0;
  } else if (d[6] == 'I' && d[7] == 'I') {
    le = true;
    if ((d[8] | (d[9] << 8)) != 42) return 0;
  } else {
    return 0;
  }
  auto u16 = [&](uint32_t o) -> uint32_t { return le ? (d[o] | (d[o + 1] << 8)) : ((d[o] << 8) | d[o + 1]); };
  auto u32 = [&](uint32_t o) -> uint32_t {
    return le ? (uint32_t(d[o]) | (uint32_t(d[o + 1]) << 8) | (uint32_t(d[o + 2]) << 16) | (uint32_t(d[o + 3]) << 24))
              : ((uint32_t(d[o]) << 24) | (uint32_t(d[o + 1]) << 16) | (uint32_t(d[o + 2]) << 8) | uint32_t(d[o + 3]));
  };
  const uint32_t ifd = u32(10);
  if (!(uint64_t(ifd) + 6 < len) || uint64_t(ifd) + 8 > len) return 0;
  const uint32_t count = u16(ifd + 6);
  if (uint64_t(ifd) + 12ull * count + 12 > len) return 0;
  uint32_t orientation = 0;
  for (uint32_t i = 0, e = ifd + 8; i < count; ++i, e += 12) {
    if (uint64_t(e) + 12 > len) continue;  // entry outside the data: tag stays 0xFF
    if (u16(e) != 0x0112 || u16(e + 2) != 3) continue;
    const uint32_t n = u32(e + 4);
    if (n == 0) continue;
    if (n >= 3) {
      const uint64_t off = u32(e + 8);
      if (off + 2ull * n + 6 > len) continue;
      orientation = u16(uint32_t(off + 6));
    } else {
      orientation = u16(e + 8);
    }
  }
  return orientation;
}

}  // namespace

bool available() { return true; }

bool read_header(const uint8_t* data, size_t size, bool apply_orientation, Header* out, uint32_t* codec_error) {
  *codec_error = 0;
  jpeg_decompress_struct cinfo;
  ErrorMgr err;
  Source src;
  cinfo.err = jpeg_std_error(&err.pub);
  err.pub.error_exit = error_exit;
  if (const int code = setjmp(err.jump)) {
    *codec_error = map_jump(code);
    jpeg_destroy_decompress(&cinfo);
    return false;
  }
  jpeg_create_decompress(&cinfo);
  attach_source(&cinfo, &src, data, size);
  if (apply_orientation) jpeg_save_markers(&cinfo, JPEG_APP0 + 1, 0xFFFF);
  bool ok = jpeg_read_header(&cinfo, TRUE) == JPEG_HEADER_OK;  // "jpeg_read_header() failed": no codec error
  if (ok) {
    out->width = int32_t(cinfo.image_width);
    out->height = int32_t(cinfo.image_height);
    out->orientation = 0;
    if (apply_orientation && cinfo.marker_list)
      out->orientation = exif_orientation(cinfo.marker_list->data, cinfo.marker_list->data_length);
  }
  jpeg_destroy_decompress(&cinfo);
  return ok;
}

bool decode(const uint8_t* data, size_t size, int32_t width, int32_t height, std::vector<uint8_t>* rgba, uint32_t* codec_error) {
  *codec_error = 0;
  if (width <= 0 || height <= 0) return false;
  rgba->assign(size_t(width) * size_t(height) * 4, 0);
  uint8_t* const frame = rgba->data();
  const size_t pitch = size_t(width) * 4;  // attribute row alignment 1 (+76)
  jpeg_decompress_struct cinfo;
  ErrorMgr err;
  Source src;
  cinfo.err = jpeg_std_error(&err.pub);
  err.pub.error_exit = error_exit;
  if (const int code = setjmp(err.jump)) {
    *codec_error = map_jump(code);
    jpeg_destroy_decompress(&cinfo);
    return false;
  }
  jpeg_create_decompress(&cinfo);
  attach_source(&cinfo, &src, data, size);
  if (jpeg_read_header(&cinfo, TRUE) != JPEG_HEADER_OK) {  // "SetAttributeInfo() error"
    jpeg_destroy_decompress(&cinfo);
    return false;
  }
  cinfo.scale_num = 1;
  cinfo.scale_denom = 1;  // codec +976 (1 unless the codec's subrect hook changed it)
  jpeg_calc_output_dimensions(&cinfo);
  jpeg_start_decompress(&cinfo);
  const JDIMENSION ow = cinfo.output_width;
  const int comps = cinfo.output_components;
  if (comps != 1 && comps != 3 && comps != 4) {  // "format unsupported in DecompressImage"
    *codec_error = 0x80B35500u;
    jpeg_destroy_decompress(&cinfo);
    return false;
  }
  if (ow > JDIMENSION(width) || cinfo.output_height > JDIMENSION(height)) {  // cannot happen at 1/1
    jpeg_destroy_decompress(&cinfo);
    return false;
  }
  JSAMPARRAY row = (*cinfo.mem->alloc_sarray)(reinterpret_cast<j_common_ptr>(&cinfo), JPOOL_IMAGE, ow * JDIMENSION(comps), 1);
  uint8_t* dst = frame;
  while (cinfo.output_scanline < cinfo.output_height) {
    jpeg_read_scanlines(&cinfo, row, 1);
    const JSAMPLE* s = row[0];
    uint8_t* p = dst;
    for (JDIMENSION x = 0; x < ow; ++x, p += 4) {
      if (comps == 1) {
        p[0] = p[1] = p[2] = s[x];
      } else if (comps == 3) {
        p[0] = s[3 * x], p[1] = s[3 * x + 1], p[2] = s[3 * x + 2];
      } else {
        const uint32_t k = s[4 * x + 3];
        p[0] = uint8_t((32897u * s[4 * x] * k) >> 23);
        p[1] = uint8_t((32897u * s[4 * x + 1] * k) >> 23);
        p[2] = uint8_t((32897u * s[4 * x + 2] * k) >> 23);
      }
      p[3] = 0xFF;
    }
    dst += pitch;
  }
  jpeg_finish_decompress(&cinfo);
  jpeg_destroy_decompress(&cinfo);  // FinishDecompress 0x24eaa0
  return true;
}

}  // namespace ssn::psm::imaging::jpeg

#else  // !SSN_HAVE_PSM_JPEG

namespace ssn::psm::imaging::jpeg {
bool available() { return false; }
bool read_header(const uint8_t*, size_t, bool, Header*, uint32_t* codec_error) {
  *codec_error = 0x80B35500u;
  return false;
}
bool decode(const uint8_t*, size_t, int32_t, int32_t, std::vector<uint8_t>*, uint32_t* codec_error) {
  *codec_error = 0x80B35500u;
  return false;
}
}  // namespace ssn::psm::imaging::jpeg

#endif
