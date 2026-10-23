// The 80-byte audio description libScePsm's PSS audio layer keeps for every audio data
// object (data object +600, returned by sub_17AF20; copied by sub_17ACA0 / sub_17AEF0) and
// hands to the decoders (decoder +328, vtable+72) and voices. Only the words this host
// reads are named; the rest are kept so a copy stays a copy.
#pragma once
#include <cstdint>
#include <cstring>

namespace ssn::psm::audio {

struct PssAudioInfo {
  uint32_t w[20] = {};

  // [0] decoder / data type: 0 PCM, 1 ADPCM, 2 MP3, 3 ATRAC9, 4 HEVAG (the managed
  //     SoundDataFormat numbering, GetDataFormatNative 0x2d6b20); 6 for an unparsed container.
  uint32_t type() const { return w[0]; }
  // [1] storage kind: 2 = memory image (sub_179A50 / sub_17AD20 store +604 = 2).
  uint32_t storage() const { return w[1]; }
  uint32_t rate() const { return w[2]; }      // [2] frames per second
  uint32_t channels() const { return w[3]; }  // [3]
  uint32_t bits() const { return w[4]; }      // [4] bits per decoded sample
  uint32_t frames() const { return w[5]; }    // [5] total frames (0 = unknown)
  uint32_t bitrate() const { return w[7]; }   // [7] (GetBitRateNative)
  uint32_t loop_flag() const { return w[8]; } // [8] (+32, sub_160FC0)
  float block_bytes() const { float f; std::memcpy(&f, &w[9], 4); return f; }  // [9] (+36)
  uint32_t block_frames() const { return w[10]; }                              // [10] (+40)
  uint64_t length() const { return uint64_t(w[12]) | (uint64_t(w[13]) << 32); }  // +48
  void set_length(uint64_t v) { w[12] = uint32_t(v); w[13] = uint32_t(v >> 32); }
  uint64_t loop_start() const { return uint64_t(w[16]) | (uint64_t(w[17]) << 32); }  // +64
  uint64_t loop_end() const { return uint64_t(w[18]) | (uint64_t(w[19]) << 32); }    // +72
};
static_assert(sizeof(PssAudioInfo) == 80);

}  // namespace ssn::psm::audio
