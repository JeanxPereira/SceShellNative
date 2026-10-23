// HEVAG decoder of libScePsm.sprx (FW 4.03) — see hevag_decoder.h.
#include "libScePsm/audio/hevag_decoder.h"

#include <cstring>
#include <mutex>

#include "core/log.h"
#include "host/boundary.h"
#include "libScePsm/audio/psm_module_image.h"

// The firmware computes every sample with separate vmulss/vaddss; no contraction to FMA.
#if defined(__clang__)
#pragma clang fp contract(off)
#endif

namespace ssn::psm::audio {
namespace {

constexpr uint32_t kErrFail = 0x80B3F001u;  // 2159276033

// unk_3D8C80: 16 float scales indexed by the low nibble of byte 0; unk_3D8CC0: 16 bytes
// (4 floats) per predictor, indexed by (byte0 >> 4) | (byte1 & 0xF0) (0..255).
struct Tables {
  float scale[16] = {};
  float coef[256][4] = {};
  bool ok = false;
};

const Tables& tables() {
  static Tables t;
  static std::once_flag once;
  std::call_once(once, [] {
    const auto s = psm_module_bytes(0x3D8C80, sizeof(t.scale));
    const auto c = psm_module_bytes(0x3D8CC0, sizeof(t.coef));
    if (s.size() == sizeof(t.scale) && c.size() == sizeof(t.coef)) {
      std::memcpy(t.scale, s.data(), sizeof(t.scale));
      std::memcpy(t.coef, c.data(), sizeof(t.coef));
      t.ok = true;
    } else {
      log::error("psm.audio", "HEVAG tables (libScePsm 0x3D8C80 / 0x3D8CC0) not readable; decoding silence");
    }
  });
  return t;
}

}  // namespace

void HevagDecoder::load_tables() { (void)tables(); }

// sub_190B00: base sub_176E80 (+8 -> +72, +16 = 256, +24 = 1.0f, +28..+39 = 0), +408 = 0,
// +416/+424 = 0, +328 (description) and +40 (history) zeroed.
HevagDecoder::HevagDecoder() : PssDecoder(256) {}

// sub_190C20: the description is copied to +328 and +20 (carry) cleared. Only HEVAG (type 4)
// with 1 or 2 channels and a non-zero (int)block-bytes is accepted; the first call allocates
// the 3584-float scratch and returns, later calls clear it and reset (vtable+64).
uint32_t HevagDecoder::set_info(const PssAudioInfo& info) {
  info_ = info;
  carried_ = 0;
  if (info_.type() != 4 || info_.channels() - 1 > 1) return kErrFail;
  if (static_cast<int64_t>(info_.block_bytes()) == 0) return kErrFail;
  if (scratch_.empty()) {
    scratch_.assign(3584, 0.0f);  // +424 = 3584, 14336 bytes
    return 0;
  }
  std::fill(scratch_.begin(), scratch_.end(), 0.0f);
  reset();
  return 0;
}

// sub_190D10: +408 = 1, history cleared.
uint32_t HevagDecoder::reset() {
  reset_flag_ = 1;
  std::memset(history_, 0, sizeof(history_));
  return 0;
}

// sub_191A50 for the position this host reproduces: frame 0 is byte 0 with nothing to skip.
// Other positions (the float block arithmetic of sub_191A50) are not reproduced: no ShellUI
// sound seeks a VAG resource (BgmPlayer.SetPosition only reaches ATRAC9 resources).
uint32_t HevagDecoder::seek(const PssAudioInfo&, uint64_t, uint64_t* frame, uint64_t* byte, uint32_t* skip,
                            uint32_t* skip_bytes) {
  if (*frame != 0)
    host::note_unimplemented("libScePsm PSS HEVAG seek to a non-zero frame (sub_191A50) not reproduced; restarting at 0");
  *frame = 0;
  *byte = 0;
  *skip = 0;
  *skip_bytes = 0;
  return 0;
}

// sub_190D60: decodes whole groups of `channels` 16-byte blocks (one per channel, channel-
// interleaved), at most min(src_len / (16 ch), out_cap / (112 ch), 3584 / (28 ch)) groups.
// Each channel is decoded into the scratch (28 floats per block) and then interleaved into
// `out` (4 bytes per sample, stride 4 ch). A block whose flag nibble (byte 1 & 0xF) is 7
// ends the channel before it is decoded; a block with flag 1 is decoded but the count of
// output samples stops before it. *consumed = every group; *produced = the byte count kept.
uint32_t HevagDecoder::decode_blocks(const uint8_t* src, uint32_t src_len, uint32_t* consumed, uint8_t* out,
                                     uint32_t out_cap, uint32_t* produced, uint32_t*) {
  *produced = 0;
  *consumed = 0;
  const uint32_t ch = info_.channels();
  const uint32_t group = 16 * ch;
  const uint32_t per_group_out = 28 * ch;
  int32_t groups = static_cast<int32_t>(static_cast<uint64_t>(out_cap) / (4ull * per_group_out));
  if (static_cast<int32_t>(src_len / group) < groups) groups = static_cast<int32_t>(src_len / group);
  if (groups == 0) return 0;
  if (info_.type() != 4) return kErrFail;
  int32_t limit = static_cast<int32_t>(static_cast<uint32_t>(scratch_.size()) / per_group_out);
  if (groups < limit) limit = groups;
  int32_t samples = 28 * limit;                                    // v12
  int32_t bytes = static_cast<int32_t>(4 * 28 * limit * ch);       // v13
  for (uint32_t c = 0; c < ch; ++c) {
    if (limit != 0) {
      float* dst = scratch_.data();
      const uint8_t* blk = src + 16 * c;
      int32_t done_samples = 0, done_bytes = 0;
      int32_t n = 0;
      for (;;) {
        const uint8_t flag = blk[1] & 0xF;
        if (flag == 7) {
          samples = done_samples;
          bytes = done_bytes;
          break;
        }
        decode_block(history_[c], blk, dst);
        if (flag == 1) {
          samples = done_samples;
          bytes = done_bytes;
        }
        blk += group;
        done_bytes += static_cast<int32_t>(112 * ch);
        dst += 28;
        ++n;
        done_samples += 28;
        if (n == limit) break;
      }
    }
    if (samples != 0) {
      float* o = reinterpret_cast<float*>(out) + c;
      for (int32_t i = 0; i < samples; ++i) {
        *o = scratch_[size_t(i)];
        o += ch;
      }
    }
  }
  *consumed = group * static_cast<uint32_t>(limit);
  *produced = static_cast<uint32_t>(bytes);
  return 0;
}

// sub_190F70: one 16-byte block -> 28 floats. byte 0 = shift (low nibble) | predictor low
// bits (high nibble), byte 1 = predictor high bits (high nibble) | flags; bytes 2..15 hold
// the samples, low nibble first, each nibble taken as a signed byte with the nibble in the
// high half ((int8)(b << 4), (int8)(b & 0xF0)). For predictor indices up to 28 two taps are
// used, above 28 four:
//   s = (((scale * n + c0 * p1) + c1 * p2) [+ c2 * p3] [+ c3 * p4])
// (p1 = previous sample). The history keeps the last four samples, p1 first.
void HevagDecoder::decode_block(float* h, const uint8_t* b, float* out) {
  const Tables& t = tables();
  if (!t.ok) {
    std::memset(out, 0, 28 * sizeof(float));
    return;
  }
  const float scale = t.scale[b[0] & 0xF];
  const uint32_t pred = (b[0] >> 4) | (b[1] & 0xF0);
  const float c0 = t.coef[pred][0], c1 = t.coef[pred][1];
  float p1 = h[0], p2 = h[1], p3 = h[2], p4 = h[3];
  if (pred > 0x1C) {
    const float c2 = t.coef[pred][2], c3 = t.coef[pred][3];
    for (int i = 0; i < 28; ++i) {
      const uint8_t v = b[2 + i / 2];
      const int8_t nib = (i & 1) ? static_cast<int8_t>(v & 0xF0) : static_cast<int8_t>(static_cast<uint8_t>(v << 4));
      float s = scale * static_cast<float>(nib);
      s = s + c0 * p1;
      s = c1 * p2 + s;
      s = c2 * p3 + s;
      s = c3 * p4 + s;
      out[i] = s;
      p4 = p3;
      p3 = p2;
      p2 = p1;
      p1 = s;
    }
  } else {
    for (int i = 0; i < 28; ++i) {
      const uint8_t v = b[2 + i / 2];
      const int8_t nib = (i & 1) ? static_cast<int8_t>(v & 0xF0) : static_cast<int8_t>(static_cast<uint8_t>(v << 4));
      float s = scale * static_cast<float>(nib);
      s = c0 * p1 + s;
      s = c1 * p2 + s;
      out[i] = s;
      p4 = p3;
      p3 = p2;
      p2 = p1;
      p1 = s;
    }
  }
  h[0] = out[27];
  h[1] = out[26];
  h[2] = out[25];
  h[3] = out[24];
}

}  // namespace ssn::psm::audio
