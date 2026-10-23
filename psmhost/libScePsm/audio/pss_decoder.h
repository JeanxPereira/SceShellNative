// The decoder base of libScePsm's PSS audio layer (FW 4.03): sub_176E80 and the vtable slots
// every decoder type shares (off_49B458). The concrete decoders are the HEVAG decoder (type 4,
// hevag_decoder.h) and the ATRAC9 decoder (type 3, atrac9_decoder.h); the voices drive them
// through these slots (the offsets are the vtable offsets the voice code calls):
//
//   +16  chunk size for a description            +24 sub_176EE0 input bytes for an output size
//   +32  frame -> byte position (seek)            +48 skip after a decode error
//   +56  sub_176F30 -> +112 sub_177340 decode through the carry buffer (+8 buffer, +16 size)
//   +64  reset                                    +72 set the audio description
//   +80  release                                  +88 type
//   +96  seekable                                 +120 decode (type specific)
//   +128 sub_176FE0 apply a requested state       (sub_176FB0 requests a state)
#pragma once
#include <atomic>
#include <cstdint>
#include <vector>

#include "libScePsm/audio/pss_audio_info.h"

namespace ssn::psm::audio {

class PssDecoder {
 public:
  virtual ~PssDecoder() = default;

  virtual uint32_t type() const = 0;                       // +88
  virtual uint32_t chunk_size(const uint32_t* desc) const = 0;  // +16 (desc = rate, ch, bits)
  uint32_t input_size(uint32_t out_bytes) const;           // +24 sub_176EE0
  // +32: the decoder position for source frame *frame: *frame is moved to the frame decoding
  // can restart at, *byte is its data offset, *skip frames to drop before it, *skip_bytes
  // output bytes the decoder itself drops after the restart (voice +288 / +292).
  virtual uint32_t seek(const PssAudioInfo& info, uint64_t total, uint64_t* frame, uint64_t* byte, uint32_t* skip,
                        uint32_t* skip_bytes) = 0;
  // +48: after a decode error: input bytes to step over (*skipped), at data offset `pos`.
  virtual uint32_t skip(const uint8_t* src, uint32_t src_len, uint32_t* skipped, uint64_t pos) = 0;
  // +56 (sub_176F30 -> sub_177340). `skip_bytes` (voice +292) passes through to +120.
  uint32_t decode(const uint8_t* src, uint32_t src_len, uint32_t* consumed, uint8_t* out, uint32_t out_cap,
                  uint32_t* produced, uint32_t* skip_bytes);
  virtual uint32_t reset() = 0;                            // +64
  virtual uint32_t set_info(const PssAudioInfo& info) = 0; // +72
  virtual void release() = 0;                              // +80
  virtual bool seekable() const = 0;                       // +96
  void request_state(int32_t s);                           // sub_176FB0

 protected:
  explicit PssDecoder(uint32_t carry_cap) : carry_(carry_cap, 0), carry_cap_(carry_cap) {}
  // +120
  virtual uint32_t decode_blocks(const uint8_t* src, uint32_t src_len, uint32_t* consumed, uint8_t* out,
                                 uint32_t out_cap, uint32_t* produced, uint32_t* skip_bytes) = 0;
  uint32_t apply_state();  // sub_176FE0 (vtable+128)

  std::vector<uint8_t> carry_;      // pointed to by +8
  uint32_t carry_cap_;              // +16 (256 for HEVAG, 1089 for ATRAC9)
  uint32_t carried_ = 0;            // +20
  float ratio_ = 1.0f;              // +24 (xmmword_34E630)
  std::atomic<int32_t> requested_{0};  // +28
  int32_t state_ = 0;               // +32
  int32_t reset_pending_ = 0;       // +36
};

}  // namespace ssn::psm::audio
