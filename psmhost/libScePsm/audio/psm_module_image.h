// Read-only access to data of the firmware's own libScePsm.sprx (FW 4.03) at module
// virtual addresses — the same addresses the analysed database (work/ida*/libScePsm_dyn.elf)
// uses. Tables the audio code needs (the HEVAG decoder's scale and predictor tables) are
// read from the dump at run time instead of being copied into this repository.
#pragma once
#include <cstddef>
#include <cstdint>
#include <vector>

namespace ssn::psm::audio {

// Bytes [va, va + size) of /system_ex/common_ex/lib/libScePsm.sprx (a decrypted ELF64 whose
// PT_LOAD segments map file offsets to these addresses); empty when the module is missing or
// the range is not file-backed.
std::vector<uint8_t> psm_module_bytes(uint64_t va, size_t size);

}  // namespace ssn::psm::audio
