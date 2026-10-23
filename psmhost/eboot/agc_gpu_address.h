// Texture addressing of the background layer's GNF images, reproduced from the firmware's own
// statically linked sce::AgcGpuAddress / Agc texture code.
//
// Source binaries (decrypted, analysed with IDA; addresses are image-relative):
//   libScePsm.sprx  (work/ida/libScePsm_dyn.elf)            -- primary citation, "psm 0x..."
//   ShellUI eboot   (system_ex/app/NPXS40087/eboot.bin)      -- the same library, "eboot 0x..."
// Both link the same code; every routine and table cited below was checked to be identical in
// the two images (the eboot copy sits at psm address + 0x7EF7F0 for the swizzle-equation code
// and + 0x89E3D0 for the swizzle-routine table).
//
// Scope: what the GNFs shipped with the ShellUI need -- one 1D/2D surface, one mip level, swizzle
// modes 0 (linear) and 5 (4 KiB "S" pattern), element sizes 1..16 bytes. Anything else is
// rejected with an error instead of being guessed.
#pragma once
#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

namespace ssn::eboot::agc {

// Element layout of a T# data format. psm sub_1558F0 (eboot sub_881A80): qword_3D6CD0[format]
// indexes 10-byte records at 0x3D7290; spec builder psm sub_157EB0 (eboot sub_8864C0) spreads
// bytes 0,3,1,2 of the record into the spec (pshufb mask 0x34ECD0).
struct ElementLayout {
  uint32_t log2_bytes;  // record byte 0
  uint32_t mul_x;       // record byte 3: width multiplier
  uint32_t div_x;       // record byte 1: texels per element horizontally
  uint32_t div_y;       // record byte 2: texels per element vertically
};
bool element_layout(uint32_t format, ElementLayout& out);

// The texture description Agc::translate (psm sub_1584A0, eboot sub_886B20) extracts from a T#,
// after the spec builder (psm sub_157EB0) -- the input of computeAssetSummary.
struct TextureSpec {
  uint32_t format = 0;     // T# dword1 [28:20]
  uint32_t type = 0;       // T# dword3 [31:28]
  uint32_t sw_mode = 0;    // T# dword3 [24:20]
  uint32_t dim = 0;        // dword_34F800[type - 8]: 0 1D, 1 2D, 2 3D
  ElementLayout element{};
  uint32_t width = 0;      // T# dwords1:2 [43:30] + 1
  uint32_t height = 0;     // T# dwords1:2 [59:46] + 1
  uint32_t depth = 0;      // T# dword4 [12:0] + 1, or 1 for types 11, 12, 13, 15
  uint32_t log2_fragments = 0;
  uint32_t levels = 0;     // T# dword5 [7:4] + 1, or 1 for types 14/15
  uint32_t slices = 0;     // T# dword4 [12:0] + 1, or 1 for type 10
};
bool translate(const uint32_t tsharp[8], TextureSpec& out, std::string& err);

// The part of the AgcGpuAddress surface summary (psm sub_A0CC0, eboot sub_88E880) the detiler reads.
struct SurfaceSummary {
  uint32_t sw_mode = 0;          // +0
  uint32_t log2_elem_bytes = 0;  // +8
  uint32_t log2_fragments = 0;   // +52 (only mode 24 keeps it; 0 otherwise)
  uint32_t log2_block_bytes = 0; // +60
  uint32_t log2_block_w = 0;     // +84 (elements)
  uint32_t log2_block_h = 0;     // +88
  uint32_t log2_block_d = 0;     // +92
  uint64_t slice_size = 0;       // +112
  uint64_t total_size = 0;       // +120
  // level 0 record (+128):
  uint32_t width_elems = 0;      // +128
  uint32_t height_elems = 0;     // +132
  uint32_t depth = 0;            // +136
  uint32_t pitch = 0;            // +140 (elements, aligned to the block width)
  uint32_t aligned_height = 0;   // +144
  uint32_t aligned_depth = 0;    // +148
  uint64_t level_offset = 0;     // +152
  uint64_t level_size = 0;       // +160
};
bool compute_surface_summary(const TextureSpec& spec, SurfaceSummary& out, std::string& err);

// detileSurface (psm sub_A2320 -> sub_A2360, eboot sub_890400) over the whole level-0, slice-0
// surface: writes width_elems x height_elems elements, rows packed tightly, into `linear`.
bool detile_surface(const SurfaceSummary& s, const uint8_t* tiled, size_t tiled_size, uint8_t* linear,
                    size_t linear_size, std::string& err);

// A GNF image read the way the eboot's CreateFromGnf (sub_876D30) reads it -- "GNF " magic,
// header byte 10 < 32, version (byte 8) 4, one texture (byte 9); the 32-byte T# at +16; data at
// 8 + contents size (dword +4) + T# dword0, T# dword7 bytes long -- then translated, summarised
// and detiled with the functions above. `linear` receives the level-0 elements, rows packed.
bool decode_gnf(const uint8_t* data, size_t size, TextureSpec& spec, std::vector<uint8_t>& linear, std::string& err);

}  // namespace ssn::eboot::agc
