// The PSS decoder base of libScePsm (FW 4.03) — see pss_decoder.h.
#include "libScePsm/audio/pss_decoder.h"

#include <cstring>

namespace ssn::psm::audio {
namespace {
constexpr uint32_t kErrFail = 0x80B3F001u;  // 2159276033
constexpr uint32_t kErrArg = 0x80B3F003u;   // 2159276035
}  // namespace

// sub_176EE0: out_bytes / ratio when ratio != 0, else out_bytes.
uint32_t PssDecoder::input_size(uint32_t out_bytes) const {
  if (ratio_ != 0.0f) return static_cast<uint32_t>(static_cast<int64_t>(static_cast<float>(out_bytes) / ratio_));
  return out_bytes;
}

// sub_176FB0: exchange +28 with the state; 5 (stop) and 2 (seek) also set +36.
void PssDecoder::request_state(int32_t s) {
  requested_.exchange(s);
  if (s == 5 || s == 2) reset_pending_ = 1;
}

// sub_176FE0: takes the requested state (exchange with 0). 2 and 5 reset (vtable+64) and
// become the current state; 6 calls vtable+80 (release); 7 becomes current; a pending reset
// (+36) runs once more.
uint32_t PssDecoder::apply_state() {
  uint32_t r = 0;
  switch (requested_.exchange(0)) {
    case 2:
      reset();
      state_ = 2;
      reset_pending_ = 0;
      return r;
    case 5:
      reset();
      state_ = 5;
      reset_pending_ = 0;
      return r;
    case 6:
      release();
      state_ = 6;
      break;
    case 7:
      state_ = 7;
      break;
    default:
      break;
  }
  if (reset_pending_ != 0) {
    reset();
    reset_pending_ = 0;
  }
  return r;
}

// sub_177340 (through sub_176F30's argument check: src may be null only with src_len 0).
// The first part completes what was left in the carry buffer by the previous call; the rest
// is decoded in place; when nothing came out, what is left (at most the carry size) is
// carried. Outputs of a stopped decoder (state 5) are zero-filled.
uint32_t PssDecoder::decode(const uint8_t* src, uint32_t src_len, uint32_t* consumed, uint8_t* out, uint32_t out_cap,
                            uint32_t* produced, uint32_t* skip_bytes) {
  if ((src == nullptr && src_len != 0) || (out == nullptr && out_cap != 0)) return kErrArg;  // sub_176F30
  *produced = 0;
  *consumed = 0;
  const uint8_t* in = src;
  uint32_t in_len = src_len;
  if (carried_ != 0) {
    uint32_t take = carry_cap_ - carried_;
    if (take >= src_len) take = src_len;
    if (take != 0) std::memcpy(carry_.data() + carried_, src, take);
    apply_state();
    if (static_cast<uint32_t>(state_ - 5) < 2) return 0;
    uint32_t used = 0, made = 0;
    if (decode_blocks(carry_.data(), take + carried_, &used, out, out_cap, &made, skip_bytes) != 0) return kErrFail;
    if ((state_ == 5 || requested_.load() == 5) && made != 0) std::memset(out, 0, made);
    if (used == 0) {
      const uint32_t all = take + carried_;
      carried_ = all;
      if (take != 0) {
        *consumed = take;
      } else {
        *consumed = all;
        carried_ = 0;
      }
      return 0;
    }
    *produced = made;
    if (used < carried_) {
      const uint32_t left = carried_ + (take - used);
      carried_ = left;
      if (left != 0) std::memmove(carry_.data(), carry_.data() + used, left);
      return 0;
    }
    const uint32_t from_src = used - carried_;
    *consumed = from_src;
    carried_ = 0;
    if (made != 0) ratio_ = static_cast<float>(made) / static_cast<float>(used);
    in = src + from_src;
    in_len = src_len - from_src;
    out += made;
    out_cap -= made;
  }
  uint32_t rest = 0;
  if (in_len != 0) {
    apply_state();
    if (static_cast<uint32_t>(state_ - 5) < 2) return 0;
    uint32_t used = 0, made = 0;
    if (decode_blocks(in, in_len, &used, out, out_cap, &made, skip_bytes) != 0) return kErrFail;
    if ((state_ == 5 || requested_.load() == 5) && made != 0) std::memset(out, 0, made);
    *produced += made;
    *consumed += used;
    if (made != 0 && used != 0) ratio_ = static_cast<float>(made) / static_cast<float>(used);
    in += used;
    rest = in_len - used;
    if (*produced != 0) return 0;
  } else if (*produced != 0) {
    return 0;
  }
  if (rest > carry_cap_) rest = carry_cap_;
  carried_ = rest;
  *consumed += rest;
  if (rest != 0) std::memcpy(carry_.data(), in, rest);
  return 0;
}

}  // namespace ssn::psm::audio
