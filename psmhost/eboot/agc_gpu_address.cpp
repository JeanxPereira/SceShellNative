#include "eboot/agc_gpu_address.h"

#include <cstring>

namespace ssn::eboot::agc {

namespace {

// --- data format -> element layout -------------------------------------------------------------
// psm sub_1558F0: `if (format <= 0xB6) { idx = (uint32_t)qword_3D6CD0[format]; if (idx <= 0x3C)
// record = 0x3D7290 + 10 * idx; }`. Listed as {log2_bytes, mul_x, div_x, div_y} = record bytes
// {0, 3, 1, 2} (the order psm sub_157EB0's pshufb 0x34ECD0 stores them in); formats whose index
// is out of range or whose record is empty are {0,0,0,0}. Extracted from libScePsm; the eboot's
// sub_881A80 (tables 0xC9E7B0 / 0xC9ED70) holds the same values.
constexpr ElementLayout kFormatElements[183] = {
    {0,0,0,0}, {0,1,1,1}, {0,1,1,1}, {0,1,1,1}, {0,1,1,1}, {0,1,1,1}, {0,1,1,1}, {1,1,1,1},  // 0..
    {1,1,1,1}, {1,1,1,1}, {1,1,1,1}, {1,1,1,1}, {1,1,1,1}, {1,1,1,1}, {1,1,1,1}, {1,1,1,1},  // 8..
    {1,1,1,1}, {1,1,1,1}, {1,1,1,1}, {1,1,1,1}, {2,1,1,1}, {2,1,1,1}, {2,1,1,1}, {2,1,1,1},  // 16..
    {2,1,1,1}, {2,1,1,1}, {2,1,1,1}, {2,1,1,1}, {2,1,1,1}, {2,1,1,1}, {2,1,1,1}, {2,1,1,1},  // 24..
    {2,1,1,1}, {2,1,1,1}, {2,1,1,1}, {2,1,1,1}, {2,1,1,1}, {2,1,1,1}, {2,1,1,1}, {2,1,1,1},  // 32..
    {2,1,1,1}, {2,1,1,1}, {2,1,1,1}, {2,1,1,1}, {2,1,1,1}, {2,1,1,1}, {2,1,1,1}, {2,1,1,1},  // 40..
    {2,1,1,1}, {2,1,1,1}, {2,1,1,1}, {2,1,1,1}, {2,1,1,1}, {2,1,1,1}, {2,1,1,1}, {2,1,1,1},  // 48..
    {2,1,1,1}, {2,1,1,1}, {2,1,1,1}, {2,1,1,1}, {2,1,1,1}, {2,1,1,1}, {3,1,1,1}, {3,1,1,1},  // 56..
    {3,1,1,1}, {3,1,1,1}, {3,1,1,1}, {3,1,1,1}, {3,1,1,1}, {3,1,1,1}, {3,1,1,1}, {3,1,1,1},  // 64..
    {2,3,1,1}, {2,3,1,1}, {2,3,1,1}, {4,1,1,1}, {4,1,1,1}, {4,1,1,1}, {0,0,0,0}, {0,0,0,0},  // 72..
    {0,0,0,0}, {0,0,0,0}, {0,0,0,0}, {0,0,0,0}, {0,0,0,0}, {0,0,0,0}, {0,0,0,0}, {0,0,0,0},  // 80..
    {0,0,0,0}, {0,0,0,0}, {0,0,0,0}, {0,0,0,0}, {0,0,0,0}, {0,0,0,0}, {0,0,0,0}, {0,0,0,0},  // 88..
    {0,0,0,0}, {0,0,0,0}, {0,0,0,0}, {0,0,0,0}, {0,0,0,0}, {0,0,0,0}, {0,0,0,0}, {0,0,0,0},  // 96..
    {0,0,0,0}, {0,0,0,0}, {0,0,0,0}, {0,0,0,0}, {0,0,0,0}, {0,0,0,0}, {0,0,0,0}, {0,0,0,0},  // 104..
    {0,0,0,0}, {0,0,0,0}, {0,0,0,0}, {0,0,0,0}, {0,0,0,0}, {0,0,0,0}, {0,0,0,0}, {0,0,0,0},  // 112..
    {0,0,0,0}, {0,0,0,0}, {0,0,0,0}, {0,0,0,0}, {0,0,0,0}, {0,0,0,0}, {0,0,0,0}, {0,0,0,0},  // 120..
    {0,1,1,1}, {1,1,1,1}, {2,1,1,1}, {2,1,1,1}, {2,1,1,1}, {1,1,1,1}, {1,1,1,1}, {1,1,1,1},  // 128..
    {1,1,1,1}, {0,1,1,1}, {0,1,8,1}, {0,1,8,1}, {2,1,1,1}, {0,0,0,0}, {0,0,0,0}, {0,0,0,0},  // 136..
    {0,0,0,0}, {0,0,0,0}, {0,0,0,0}, {2,1,2,1}, {2,1,2,1}, {2,1,2,1}, {2,1,2,1}, {2,1,2,1},  // 144..
    {2,1,2,1}, {2,1,2,1}, {2,1,2,1}, {0,0,0,0}, {0,1,1,1}, {0,1,1,1}, {0,1,1,1}, {0,1,1,1},  // 152..
    {0,1,1,1}, {0,1,1,1}, {1,1,1,1}, {1,1,1,1}, {2,1,1,1}, {2,1,1,1}, {2,1,1,1}, {3,1,1,1},  // 160..
    {3,1,4,4}, {3,1,4,4}, {3,1,4,4}, {4,1,4,4}, {4,1,4,4}, {4,1,4,4}, {4,1,4,4}, {3,1,4,4},  // 168..
    {3,1,4,4}, {4,1,4,4}, {4,1,4,4}, {4,1,4,4}, {4,1,4,4}, {4,1,4,4}, {4,1,4,4},  // 176..
};

// T# resource type -> surface dimension: dword_34F800[type - 8] = {0, 1, 2, 1, 0, 1, 1, 1}
// (psm sub_157EB0; eboot dword_B8D3A0). 0 = 1D, 1 = 2D, 2 = 3D.
constexpr uint32_t kTypeDim[8] = {0, 1, 2, 1, 0, 1, 1, 1};

// --- swizzle modes -----------------------------------------------------------------------------
// word_3D66C0[sw_mode] (eboot word_CA0D90): per-mode flags. computeAssetSummary (psm sub_A0CC0)
// takes log2(block bytes) = ((2 * flags) & 0xC) + 8 and picks the block-shape table with bits 1/2;
// the detiler (psm sub_A2360) uses bit 0 to select its 256-byte-shift variant. Mode 0 = 0x0000
// (256-byte linear block), mode 5 = 0x0012 (4 KiB blocks, bit 0 clear).
constexpr uint32_t kModeLinear = 0;
constexpr uint32_t kMode4KbS = 5;
constexpr uint16_t kMode5Flags = 0x0012;

// Block shape in elements, log2 {width, height, depth}, indexed by log2(element bytes).
// Mode 0: unk_3D6410 (psm) / unk_CA0AE0 (eboot) -- one 256-byte row, 256 / element bytes wide.
constexpr uint32_t kLinearBlock[5][3] = {{8, 0, 0}, {7, 0, 0}, {6, 0, 0}, {5, 0, 0}, {4, 0, 0}};
// Modes with flags bit 1 (4 KiB), non-3D surfaces: unk_3D6580 (psm) / unk_CA0C50 (eboot).
// (3D surfaces of "thin"-flagged modes use unk_3D6540 instead; not reproduced, see below.)
constexpr uint32_t k4KbBlock2D[5][3] = {{6, 6, 0}, {6, 5, 0}, {5, 5, 0}, {5, 4, 0}, {4, 4, 0}};

// Element-offset equations of the 4 KiB "S" mode. The detiler takes them from the routine table
// unk_471540 (psm; eboot unk_D0F910), 536-byte records indexed
// [surface is 3D: 85760][sw_mode: 2680][log2 element bytes: 536]. Each record holds
//   +0  byte:  log2(elements per 16-byte run) -- elements copied together, x-contiguous;
//   +8  fn:    y part of the in-block offset (scalar, edi = y, esi = slice);
//   +16 fn:    x part (AVX2, ymm1 = 8 x coordinates, ymm0 = y part in, in-block offset out);
//   +24/+280:  run-copy routines of the tile / detile directions.
// The x routine XORs its terms into the y part; the result is a byte offset inside the 4 KiB
// block. Below: the records for sw_mode 5, non-3D (unk_471540 + 5 * 2680 + 536 * e).
// e = 0 (record 0x474998): run 1 << 4 elements.
uint32_t s4k_y_e0(uint32_t y) { return ((y & 0x20) << 5) + ((y & 0x1f) << 4); }        // psm 0xA61B0, eboot 0x8959A0
uint32_t s4k_x_e0(uint32_t x) {                                                         // psm 0xA61D0, eboot 0x8959C0
  return (x & 0xf) ^ ((x << 5) & 0x200) ^ ((x << 6) & 0x800);
}
// e = 1 (record 0x474BB0): run 1 << 3 elements.
uint32_t s4k_y_e1(uint32_t y) { return (((y << 4) & 0x70) + ((y & 8) << 5)) | ((y & 0x10) << 6); }  // psm 0xA6210, eboot 0x895A00
uint32_t s4k_x_e1(uint32_t x) {                                                                      // psm 0xA6230, eboot 0x895A20
  return ((x + x) & 0xe) ^ ((x << 4) & 0x80) ^ ((x << 5) & 0x200) ^ ((x << 6) & 0x800);
}
// e = 2 (record 0x474DC8): run 1 << 2 elements; the y routine is the same code as e = 1.
uint32_t s4k_y_e2(uint32_t y) { return (((y << 4) & 0x70) + ((y & 8) << 5)) | ((y & 0x10) << 6); }  // psm 0xA6290, eboot 0x895A80
uint32_t s4k_x_e2(uint32_t x) {                                                                      // psm 0xA62B0, eboot 0x895AA0
  return ((x << 2) & 0xc) ^ ((x << 5) & 0x80) ^ ((x << 6) & 0x200) ^ ((x << 7) & 0x800);
}
// e = 3 (record 0x474FE0): run 1 << 1 elements.
uint32_t s4k_y_e3(uint32_t y) { return (((y << 4) & 0x30) + ((y & 4) << 6)) | ((y & 8) << 7); }    // psm 0xA6310, eboot 0x895B00
uint32_t s4k_x_e3(uint32_t x) {                                                                      // psm 0xA6330, eboot 0x895B20
  return ((x << 3) & 0x8) ^ ((x << 5) & 0xc0) ^ ((x << 6) & 0x200) ^ ((x << 7) & 0x800);
}
// e = 4 (record 0x4751F8): run 1 element; the y routine is the same code as e = 3.
uint32_t s4k_y_e4(uint32_t y) { return (((y << 4) & 0x30) + ((y & 4) << 6)) | ((y & 8) << 7); }    // psm 0xA6390, eboot 0x895B80
uint32_t s4k_x_e4(uint32_t x) {                                                                      // psm 0xA63B0, eboot 0x895BA0
  return ((x << 6) & 0xc0) ^ ((x << 7) & 0x200) ^ ((x << 8) & 0x800);
}

struct SwizzleRecord {
  uint32_t log2_run_elems;
  uint32_t (*y_part)(uint32_t);
  uint32_t (*x_part)(uint32_t);
};
constexpr SwizzleRecord kMode5Records[5] = {
    {4, s4k_y_e0, s4k_x_e0}, {3, s4k_y_e1, s4k_x_e1}, {2, s4k_y_e2, s4k_x_e2},
    {1, s4k_y_e3, s4k_x_e3}, {0, s4k_y_e4, s4k_x_e4},
};

uint32_t align_up(uint32_t v, uint32_t a) { return (v + a - 1) & ~(a - 1); }

}  // namespace

bool element_layout(uint32_t format, ElementLayout& out) {
  // psm sub_1558F0: format > 0xB6 -> 0x8A6C001B; empty record -> rejected by sub_A0A30.
  if (format >= sizeof kFormatElements / sizeof kFormatElements[0]) return false;
  out = kFormatElements[format];
  return out.div_x != 0;
}

bool translate(const uint32_t d[8], TextureSpec& s, std::string& err) {
  // Agc::translate, psm sub_1584A0 (eboot sub_886B20): field extraction from the T# dwords.
  const uint64_t d12 = uint64_t(d[1]) | (uint64_t(d[2]) << 32);
  s.format = (d[1] >> 20) & 0x1ff;
  s.width = uint32_t((d12 >> 30) & 0x3fff) + 1;
  s.height = uint32_t((d12 >> 46) & 0x3fff) + 1;
  s.type = d[3] >> 28;
  s.sw_mode = (d[3] >> 20) & 0x1f;
  const uint32_t d4_count = (d[4] & 0x1fff) + 1;
  // depth: 1 for types 11, 12, 13, 15 (bit test of 0x17 on type - 11), else dword4 [12:0] + 1.
  const uint32_t t11 = s.type - 11;
  s.depth = (t11 < 5 && ((0x17u >> t11) & 1)) ? 1 : d4_count;
  // slices: 1 for type 10 (3D), else dword4 [12:0] + 1.
  s.slices = s.type == 10 ? 1 : d4_count;
  // levels: dword5 [7:4] + 1; MSAA types (dword3 [31:29] all set) have one level and take
  // log2(fragments) from dword3 [19:16].
  uint32_t fragments = 0;
  s.levels = ((d[5] & 0xff) >> 4) + 1;
  if ((~d[3] & 0xE0000000u) == 0) {
    s.levels = 1;
    fragments = (d[3] >> 16) & 0xf;
  }
  // Spec builder, psm sub_157EB0 (eboot sub_8864C0).
  if (s.type < 8) {
    err = "T# type " + std::to_string(s.type) + " is not an image";  // 0x8A6C000E
    return false;
  }
  s.dim = kTypeDim[s.type - 8];
  if (!element_layout(s.format, s.element)) {
    err = "T# format " + std::to_string(s.format) + " has no element layout";  // 0x8A6C001B/1C
    return false;
  }
  if (s.type == 11 && s.slices % 6 != 0) {
    err = "cube T# with " + std::to_string(s.slices) + " slices";  // 0x8A6C001E
    return false;
  }
  s.log2_fragments = fragments - 1 < 3 ? fragments : 0;
  return true;
}

bool compute_surface_summary(const TextureSpec& t, SurfaceSummary& s, std::string& err) {
  // Validation subset of psm sub_A0A30 that applies to the modes reproduced here.
  const ElementLayout& el = t.element;
  if (el.log2_bytes > 4 || (el.mul_x != 1 && !(t.sw_mode == kModeLinear && el.log2_bytes == 2 && el.mul_x == 3)) ||
      t.width - 1 > 0x3fff || t.height - 1 > 0x3fff || (t.height > 1 && t.dim == 0) || t.depth - 1 > 0x1fff ||
      (t.depth > 1 && t.dim != 2) || t.levels == 0 || t.slices - 1 > 0x1fff) {
    err = "surface description rejected by the AgcGpuAddress validator (sub_A0A30)";
    return false;
  }
  // Scope of this reproduction (the firmware supports more; nothing else is guessed):
  if (t.sw_mode != kModeLinear && t.sw_mode != kMode4KbS) {
    err = "swizzle mode " + std::to_string(t.sw_mode) + " not reproduced";
    return false;
  }
  if (t.levels != 1) {
    err = std::to_string(t.levels) + " mip levels: the multi-level layout (mip tail) is not reproduced";
    return false;
  }
  if (t.dim == 2) {
    err = "3D surface layout not reproduced";
    return false;
  }
  if (t.log2_fragments != 0) {
    err = "multi-fragment surface not reproduced";
    return false;
  }

  // psm sub_A0CC0 (eboot sub_88E880).
  s = SurfaceSummary{};
  s.sw_mode = t.sw_mode;
  s.log2_elem_bytes = el.log2_bytes;
  s.log2_fragments = 0;
  const uint32_t* block;
  if (t.sw_mode == kModeLinear) {
    block = kLinearBlock[el.log2_bytes];  // unk_3D6410 + 12 * e
    s.log2_block_bytes = 8;               // flags 0: ((2 * 0) & 0xC) + 8
  } else {
    block = k4KbBlock2D[el.log2_bytes];   // unk_3D6580 + 12 * e
    s.log2_block_bytes = ((2u * kMode5Flags) & 0xC) + 8;  // 12
  }
  s.log2_block_w = block[0];
  s.log2_block_h = block[1];
  s.log2_block_d = block[2];
  // Element extent of level 0: ceil(width * mul / div_x) x ceil(height / div_y).
  const uint32_t w = (t.width * el.mul_x + el.div_x - 1) / el.div_x;
  const uint32_t h = (t.height + el.div_y - 1) / el.div_y;
  const uint32_t log2_bytes = el.log2_bytes;  // + log2 fragments (0)
  if (t.sw_mode == kModeLinear) {
    // Linear branch (mode 0), level loop with m = 0: the width rounded down to a multiple of the
    // multiplier (min 1), pitch aligned to the 256-byte block, height not aligned, size =
    // pitch * height elements; level offsets are assigned from the last level up, so a single
    // level sits at offset 0.
    uint32_t wm = (el.div_x - 1 + el.mul_x * t.width) / el.div_x;
    wm -= wm % el.mul_x;
    s.width_elems = wm ? wm : 1;
    s.height_elems = h;
    s.depth = t.depth;
    s.pitch = align_up(w ? w : 1, 1u << s.log2_block_w);
    s.aligned_height = h ? h : 1;
    s.aligned_depth = t.depth;
    s.level_size = (uint64_t(s.aligned_height) * s.pitch) << log2_bytes;
  } else {
    // Tiled branch with fewer than two levels: pitch / height / depth aligned to the block.
    s.width_elems = w;
    s.height_elems = h;
    s.depth = t.depth;
    s.pitch = align_up(w, 1u << s.log2_block_w);
    s.aligned_height = align_up(h, 1u << s.log2_block_h);
    s.aligned_depth = align_up(t.depth, 1u << s.log2_block_d);
    s.level_size = (uint64_t(s.pitch) * (1u << s.log2_block_d) * s.aligned_height) << log2_bytes;
  }
  s.level_offset = 0;
  s.slice_size = s.level_size;
  s.total_size = s.slice_size * t.slices;  // non-3D: slice count
  return true;
}

bool detile_surface(const SurfaceSummary& s, const uint8_t* tiled, size_t tiled_size, uint8_t* linear, size_t linear_size,
                    std::string& err) {
  // psm sub_A2360 entry checks (0x8A6A0013: source smaller than the surface; destination size).
  const uint32_t eb = 1u << s.log2_elem_bytes;
  if (s.total_size > tiled_size) {
    err = "tiled data smaller than the surface (" + std::to_string(tiled_size) + " < " + std::to_string(s.total_size) + ")";
    return false;
  }
  if (uint64_t(s.width_elems) * s.height_elems * eb > linear_size) {
    err = "destination too small";
    return false;
  }
  // Region = the whole level: x [0, width), y [0, height), slice 0; destination row pitch =
  // width (the a6 region and a9/a10 pitches sub_A2320 passes).
  const uint64_t base = s.level_offset + s.slice_size * 0;
  const size_t dst_row = size_t(s.width_elems) * eb;

  if (s.sw_mode == kModeLinear) {
    // sub_A2360, sw_mode 0 branch: one memcpy per row; source row pitch =
    // (pitch >> log2_block_w) << log2_block_bytes bytes.
    const uint64_t row_bytes = uint64_t(s.pitch >> s.log2_block_w) << s.log2_block_bytes;
    for (uint32_t y = 0; y < s.height_elems; ++y)
      std::memcpy(linear + y * dst_row, tiled + base + row_bytes * y, dst_row);
    return true;
  }

  // sub_A2360, table-driven branch (sw_mode 5 here; flags bit 0 clear, so block numbers are
  // shifted by log2_block_bytes and the mip-tail x/y offsets -- zero for a single level -- are
  // added to the coordinates fed to the equations).
  const SwizzleRecord& r = kMode5Records[s.log2_elem_bytes];
  const uint32_t pitch_blocks = s.pitch >> s.log2_block_w;
  const uint32_t run = 1u << r.log2_run_elems;  // elements per copied run (16 bytes)
  const uint32_t tail_x = 0, tail_y = 0;
  for (uint32_t y = 0; y < s.height_elems; ++y) {
    const uint32_t y_part = r.y_part(y + tail_y);
    const uint32_t block_row = pitch_blocks * (y >> s.log2_block_h);
    uint8_t* dst = linear + y * dst_row;
    for (uint32_t x = 0; x < s.width_elems; x += run) {
      const uint32_t n = s.width_elems - x < run ? s.width_elems - x : run;  // partial last run
      const uint32_t block = (x >> s.log2_block_w) + block_row;
      const uint32_t offset = (block << s.log2_block_bytes) + (y_part ^ r.x_part(x + tail_x));
      std::memcpy(dst + size_t(x) * eb, tiled + base + offset, size_t(n) * eb);
    }
  }
  return true;
}

bool decode_gnf(const uint8_t* data, size_t size, TextureSpec& spec, std::vector<uint8_t>& linear, std::string& err) {
  // CreateFromGnf, eboot sub_876D30 (its error messages in quotes).
  if (size < 0x30 || std::memcmp(data, "GNF ", 4) != 0) {
    err = "Invalid magic";
    return false;
  }
  if (data[10] >= 0x20) {
    err = "Invalid alignment " + std::to_string(data[10]);
    return false;
  }
  if (data[8] != 4) {
    err = "Unsupported version=" + std::to_string(data[8]);
    return false;
  }
  if (data[9] != 1) {
    err = "Support only single texture " + std::to_string(data[9]);
    return false;
  }
  uint32_t contents, d[8];
  std::memcpy(&contents, data + 4, 4);
  std::memcpy(d, data + 16, 32);
  const uint64_t data_off = 8 + uint64_t(contents) + d[0], data_size = d[7];
  if (data_off + data_size > size) {
    err = "GNF image size is not sufficient (actual=" + std::to_string(size) +
          ", expected=" + std::to_string(data_off + data_size) + ")";
    return false;
  }
  SurfaceSummary sum;
  if (!translate(d, spec, err) || !compute_surface_summary(spec, sum, err)) return false;
  linear.assign(size_t(sum.width_elems) * sum.height_elems << sum.log2_elem_bytes, 0);
  return detile_surface(sum, data + data_off, size_t(data_size), linear.data(), linear.size(), err);
}

}  // namespace ssn::eboot::agc
