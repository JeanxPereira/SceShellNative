// Read-only access to data of the dump's libSceNgs2.native.sprx at module virtual addresses (the
// addresses of the analysed database work/ida_r6ngs2/libSceNgs2.native.elf). The engine's tables
// (default channel matrices, the resampler FIR, the ATRAC9 frame-size table) are read from the
// dump at run time instead of being copied into this repository.
#pragma once
#include <cstddef>
#include <cstdint>
#include <vector>

namespace ssn::ngs2 {

// Bytes [va, va + size) of /system/common/lib/libSceNgs2.native.sprx; empty when the module is
// missing or the range is not file-backed.
std::vector<uint8_t> module_bytes(uint64_t va, size_t size);

// Typed views of the tables, loaded once (empty on failure; callers log and fail).
const std::vector<uint16_t>& frame_samples_table();  // word_4A520 (16 x u16)
const std::vector<float>& resampler_fir();           // 0x6C930: 512 phases x 4 taps
const std::vector<uint16_t>& matrix_index_table();   // 0x577D0 (off_6A9B8): 4 kinds x 81 x u16
const std::vector<float>& matrix_floats();           // 0x54890 (off_6A9C0)

}  // namespace ssn::ngs2
