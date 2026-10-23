// The HEVAG decoder of libScePsm.sprx (FW 4.03): decoder type 4 of the PSS audio layer
// (factory sub_190350 case 4: 432-byte objects, constructor sub_190B00, vtable off_49C350
// over the decoder base sub_176E80 / off_49B458, see pss_decoder.h). Every "VAGp" resource is
// described as type 4 by the VAG parser (sub_179210), so all ShellUI sound effects go
// through it.
//
// vtable off_49C350 as the voices use it:
//   +16 sub_190C10 input chunk size (1024)      +24 sub_176EE0 input bytes for an output size
//   +32 sub_191A50 frame -> byte position         +48 sub_176F20 -> +40 sub_190D50 (skip)
//   +56 sub_176F30 -> +112 sub_177340 (decode)    +64 sub_190D10 reset
//   +72 sub_190C20 set the audio description      +80 sub_190CE0 release
//   +88 sub_190D40 type (4)                       +96 sub_191C40 seekable (1)
//   +120 sub_190D60 decode whole block groups     +128 sub_176FE0 apply a requested state
//
// Samples are decoded to 32-bit float (the VAG parser describes the data as 32-bit) and
// interleaved per frame. The block decoder (sub_190F70) reads the shift scale table at
// libScePsm 0x3D8C80 and the 4-tap predictor table at 0x3D8CC0 from the dump.
#pragma once
#include <cstdint>
#include <vector>

#include "libScePsm/audio/pss_audio_info.h"
#include "libScePsm/audio/pss_decoder.h"

namespace ssn::psm::audio {

class HevagDecoder final : public PssDecoder {
 public:
  HevagDecoder();  // sub_190B00

  static constexpr uint32_t kType = 4;  // sub_190D40
  static void load_tables();             // reads the two tables from the dump's libScePsm.sprx

  uint32_t type() const override { return kType; }
  uint32_t chunk_size(const uint32_t*) const override { return 1024; }  // sub_190C10
  uint32_t seek(const PssAudioInfo& info, uint64_t total, uint64_t* frame, uint64_t* byte, uint32_t* skip,
                uint32_t* skip_bytes) override;  // sub_191A50
  // sub_176F20 -> sub_190D50: *skipped = 0.
  uint32_t skip(const uint8_t*, uint32_t, uint32_t* skipped, uint64_t) override {
    *skipped = 0;
    return 0;
  }
  uint32_t reset() override;                            // sub_190D10 (vtable+64)
  uint32_t set_info(const PssAudioInfo& info) override; // sub_190C20 (vtable+72)
  void release() override { scratch_.clear(); }         // sub_190CE0 (vtable+80)
  bool seekable() const override { return true; }       // sub_191C40

 private:
  // sub_190D60 (vtable+120)
  uint32_t decode_blocks(const uint8_t* src, uint32_t src_len, uint32_t* consumed, uint8_t* out, uint32_t out_cap,
                         uint32_t* produced, uint32_t* skip_bytes) override;
  void decode_block(float* history, const uint8_t* block, float* out);  // sub_190F70

  // type 4
  float history_[2][4] = {};        // +40, 16 bytes per channel
  PssAudioInfo info_{};             // +328
  int32_t reset_flag_ = 0;          // +408
  std::vector<float> scratch_;      // +416, +424 = size (3584)
};

}  // namespace ssn::psm::audio
