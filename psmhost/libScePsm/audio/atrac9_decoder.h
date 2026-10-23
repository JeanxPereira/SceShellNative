// The ATRAC9 decoder of libScePsm.sprx (FW 4.03): decoder type 3 of the PSS audio layer
// (factory sub_190350 case 3: 1192-byte objects, constructor sub_191C50, vtable off_49C3E8
// over the decoder base, see pss_decoder.h; one in the pool). Every RIFF/WAVE resource whose
// WAVE_FORMAT_EXTENSIBLE sub-format is the ATRAC9 GUID (xmmword_34E930) is described as type
// 3 by the RIFF parser (sub_178940): 16-bit PCM out, the fact chunk's sample count and
// encoder delay, the smpl chunk's loop. The decoding itself is libSceAudiodec's
// (audiodec_atrac9.h).
//
// vtable off_49C3E8:
//   +16 sub_191D10 144 * channels * bits of a description
//   +24 sub_176EE0 input bytes for an output size (base)
//   +32 sub_1923B0 frame -> superframe-aligned frame / byte, encoder delay in output bytes
//   +40 sub_192180 -> +64                        +48 sub_192190 clear, skip to the superframe
//   +56 sub_176F30 -> +112 sub_177340 (base)     +64 sub_191D30 clear the context
//   +72 sub_191D80 create the audiodec instance  +80 sub_1920C0 delete it, free the buffers
//   +88 sub_192170 type (3)                      +96 sub_1924E0 seekable (1)
//   +120 sub_192200 decode frames into the PCM buffer, dropping the skip bytes first
//   +128 sub_176FE0 (base)
// Fields: +8 -> +40 carry buffer of +16 = 1089 bytes; +1132 = (info +32 == 0), +1136 info
// [6], +1140 rate/channels, +1148 bits, +1152 the audiodec control block, +1160/+1168 a
// 2048-byte buffer, +1176/+1184/+1188 the PCM buffer (32 * bits * channels bytes).
#pragma once
#include <cstdint>
#include <vector>

#include "libScePsm/audio/audiodec_atrac9.h"
#include "libScePsm/audio/pss_audio_info.h"
#include "libScePsm/audio/pss_decoder.h"

namespace ssn::psm::audio {

class Atrac9Decoder final : public PssDecoder {
 public:
  Atrac9Decoder() : PssDecoder(1089) {}  // sub_191C50

  static constexpr uint32_t kType = 3;  // sub_192170
  uint32_t type() const override { return kType; }
  // sub_191D10: (144 * desc[2] * desc[1]) & 0x1FFFFFFF (channels * bits of the description).
  uint32_t chunk_size(const uint32_t* desc) const override {
    return desc ? (144u * desc[2] * desc[1]) & 0x1FFFFFFFu : 0;
  }
  uint32_t seek(const PssAudioInfo& info, uint64_t total, uint64_t* frame, uint64_t* byte, uint32_t* skip,
                uint32_t* skip_bytes) override;                                    // sub_1923B0
  uint32_t skip(const uint8_t* src, uint32_t src_len, uint32_t* skipped, uint64_t pos) override;  // sub_192190
  uint32_t reset() override;                                                       // sub_191D30
  uint32_t set_info(const PssAudioInfo& info) override;                            // sub_191D80
  void release() override;                                                         // sub_1920C0
  bool seekable() const override { return true; }                                  // sub_1924E0

 private:
  uint32_t decode_blocks(const uint8_t* src, uint32_t src_len, uint32_t* consumed, uint8_t* out, uint32_t out_cap,
                         uint32_t* produced, uint32_t* skip_bytes) override;      // sub_192200

  bool ctrl_ = false;                 // +1152 (the control block exists)
  AudiodecAtrac9 audiodec_;           // its instance (+1152 -> +0)
  uint32_t superframe_ = 0;           // ctrl +92: (int)block bytes of the description
  uint32_t pcm_bytes_ = 0;            // +1184
  std::vector<uint8_t> pcm_;          // +1176 (+1188 = capacity)
};

}  // namespace ssn::psm::audio
