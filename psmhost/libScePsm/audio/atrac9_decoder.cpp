// The ATRAC9 decoder (PSS type 3) of libScePsm (FW 4.03) — see atrac9_decoder.h.
#include "libScePsm/audio/atrac9_decoder.h"

#include <chrono>
#include <cstring>
#include <thread>

namespace ssn::psm::audio {
namespace {
constexpr uint32_t kErrFail = 0x80B3F001u;      // -2135691263
constexpr uint32_t kErrNotReady = 0x80B3F012u;  // -2135691246
constexpr uint32_t kAuMax = 2048;               // 0x800 per sceAudiodecDecode call
}  // namespace

// sub_191D30: +20 (carry) = 0, then sceAudiodecClearContext on the instance.
uint32_t Atrac9Decoder::reset() {
  carried_ = 0;
  if (!ctrl_) return 0;
  if (audiodec_.clear_context() < 0) return kErrFail;
  return 0;
}

// sub_191D80: the first call builds the control block (instance handle -1, the parameter
// block {12, 1 (16-bit words), the 4 config bytes of info +56}, the stream info block
// {36, channels, info[6], rate, (int)block bytes, block frames}) and creates the instance —
// a failed creation leaves the handle at -1 and still succeeds (decoding then reports
// 0x80B3F012) —; later calls delete the instance (failure -> 0x80B3F001), grow the PCM
// buffer when needed and create it again (failure -> 0x80B3F001). +1184 = 32 * bits *
// channels (& 0x1FFFFFFF).
uint32_t Atrac9Decoder::set_info(const PssAudioInfo& info) {
  carried_ = 0;
  uint8_t cfg[4];
  std::memcpy(cfg, &info.w[14], 4);
  const uint32_t pcm = (32u * info.bits() * info.channels()) & 0x1FFFFFFFu;
  superframe_ = static_cast<uint32_t>(static_cast<int32_t>(info.block_bytes()));  // stream info +16 (ctrl +92)
  if (!ctrl_) {
    ctrl_ = true;
    audiodec_.create(cfg);  // a negative result leaves the instance absent (-1)
    pcm_bytes_ = pcm;
    pcm_.assign(pcm, 0);
    return 0;
  }
  audiodec_.destroy();
  pcm_bytes_ = pcm;
  if (pcm > pcm_.size()) pcm_.assign(pcm, 0);
  if (audiodec_.create(cfg) < 0) return kErrFail;
  return 0;
}

// sub_1920C0: the instance deleted, the control block and both buffers freed.
void Atrac9Decoder::release() {
  audiodec_.destroy();
  ctrl_ = false;
  pcm_bytes_ = 0;
  pcm_.clear();
  pcm_.shrink_to_fit();
}

// sub_192190: clear the context; the bytes to step over are the data offset's distance from
// the start of its superframe (stream info +16, the (int)block bytes written at creation),
// at most src_len. Without a control block nothing happens.
uint32_t Atrac9Decoder::skip(const uint8_t*, uint32_t src_len, uint32_t* skipped, uint64_t pos) {
  if (!ctrl_) return 0;
  if (audiodec_.clear_context() < 0) return kErrFail;
  const uint32_t sf = superframe_;
  uint32_t n = sf != 0 ? static_cast<uint32_t>(pos % sf) : 0;
  if (n > src_len) n = src_len;
  *skipped = n;
  return 0;
}

// sub_1923B0 (two copies of the data description, sub_17ACA0): bf = info[10] (frames per
// block), delay = info[15]. A frame within the first (bf - delay) frames restarts at frame
// 0 / byte 0; otherwise at its block: n = (u64)((double)frame / (double)bf) (0 when bf is 0),
// byte = (u64)(float block bytes) * n, frame = (u32)(bf * n). Nothing to skip as frames
// (+288 = 0); the decoder drops delay * channels * bits / 8 output bytes (+292).
uint32_t Atrac9Decoder::seek(const PssAudioInfo& info, uint64_t, uint64_t* frame, uint64_t* byte, uint32_t* skip,
                             uint32_t* skip_bytes) {
  const uint32_t bf = info.block_frames();
  const uint32_t delay = info.w[15];
  if (*frame <= static_cast<uint32_t>(bf - delay)) {
    *frame = 0;
    *byte = 0;
  } else {
    uint32_t n = 0;
    if (bf != 0) n = static_cast<uint32_t>(static_cast<int64_t>(static_cast<double>(*frame) / static_cast<double>(bf)));
    const float bb = info.block_bytes();
    const uint64_t bb_u = bb >= 9.22337204e18f ? (static_cast<uint64_t>(static_cast<int64_t>(bb - 9.22337204e18f)) ^ 0x8000000000000000ull)
                                               : static_cast<uint64_t>(static_cast<int64_t>(bb));
    *byte = bb_u * n;
    *frame = static_cast<uint32_t>(bf * n);
  }
  *skip = 0;
  *skip_bytes = delay * ((info.bits() * info.channels()) >> 3);
  return 0;
}

// sub_192200: one sceAudiodecDecode per access unit (at most 2048 bytes from src + used so
// far) into the PCM buffer (min(+1184, out_cap) bytes). An error result or a set error word
// ends with 0x80B3F001 (0x80B3F012 for 0x807F000F) and the bytes used before it. Output that
// the skip count (*skip_bytes, voice +292) covers is dropped and decoding goes on; the first
// frame with output beyond it ends the call. A call without output is retried after 100 us,
// at most ten times. The kept output is copied from the PCM buffer past the skip bytes.
uint32_t Atrac9Decoder::decode_blocks(const uint8_t* src, uint32_t src_len, uint32_t* consumed, uint8_t* out,
                                      uint32_t out_cap, uint32_t* produced, uint32_t* skip_bytes) {
  *produced = 0;
  *consumed = 0;
  uint32_t cap = pcm_bytes_;
  if (cap > out_cap) cap = out_cap;
  uint32_t used_total = 0;
  if (src_len != 0) {
    uint32_t retries = 0;
    for (;;) {
      if (!ctrl_ || !audiodec_.alive()) {
        *consumed = used_total;
        return kErrNotReady;
      }
      const uint32_t before = used_total;
      uint32_t feed = src_len - used_total;
      if (feed >= kAuMax) feed = kAuMax;
      uint32_t au_used = 0, pcm_made = 0, error = 0;
      const int32_t r = audiodec_.decode(src + used_total, feed, pcm_.data(), cap, &au_used, &pcm_made, &error);
      if (r != 0 || error != 0) {
        *produced = 0;
        *consumed = before;
        return kErrFail;
      }
      *consumed = au_used;
      *produced = pcm_made;
      used_total = au_used + before;
      if (pcm_made != 0) {
        if (*skip_bytes < pcm_made) break;
        *skip_bytes -= pcm_made;
        *produced = 0;
      } else {
        if (retries + 1 > 10) break;
        ++retries;
        std::this_thread::sleep_for(std::chrono::microseconds(100));
      }
      if (used_total >= src_len) break;
    }
  }
  *consumed = used_total;
  if (*produced <= *skip_bytes) {
    *skip_bytes -= *produced;
    *produced = 0;
  } else {
    *produced -= *skip_bytes;
    std::memcpy(out, pcm_.data() + *skip_bytes, *produced);
    *skip_bytes = 0;
  }
  return 0;
}

}  // namespace ssn::psm::audio
