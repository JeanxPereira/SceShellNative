// Voice -> output converters of libScePsm (FW 4.03), transcribed from the disassembly — see
// pss_mix_converters.h. Operand order of every vmulss/vaddss/vsubss is kept (no FMA
// contraction) so the float results are the firmware's.
#include "libScePsm/audio/pss_mix_converters.h"

#include <cmath>
#include <cstdint>
#include <cstring>

#include "host/boundary.h"

#if defined(__clang__)
#pragma clang fp contract(off)
#endif

namespace ssn::psm::audio {
namespace {

// dword_35015C = 1.0, dword_3502B0 = 0.5, dword_350300 = 2.0, dword_3501C0 = -1.0.
constexpr float kOne = 1.0f, kHalf = 0.5f, kTwo = 2.0f, kMinusOne = -1.0f;

inline int64_t trunc_i(float f) { return static_cast<int64_t>(f); }  // vcvttss2si (64-bit)
inline float floor_f(float f) { return std::floor(f); }               // sub_18C440 (vroundss 9)

// Stereo pan of one frame (vL, vR already scaled by the volume) into dst[0..1]; the three
// cases of sub_16D970 / sub_16C9A0.
inline void pan_stereo(float* d, float vl, float vr, float pan) {
  if (kHalf > pan) {
    const float a = (kOne - (pan + pan)) * vr;
    d[0] = (vl + a) + d[0];
    d[1] = pan * (vr + vr) + d[1];
  } else {
    d[0] = (kTwo - (pan + pan)) * vl + d[0];
    d[1] = (vr + (kMinusOne + (pan + pan)) * vl) + d[1];
  }
}

}  // namespace

// [11] sub_16D970: 2-channel source, step 1. n = min(src, dst) frames; at the centre with a
// constant pan the frame is added as is (volume ramped per frame), else pan_stereo.
uint32_t pss_conv_stereo_unity(const float* src, float* dst, uint32_t off, uint32_t* src_frames, uint32_t* dst_frames,
                               float*, float vol, float dvol, float pan, float dpan, float) {
  uint32_t n = *dst_frames;
  if (*src_frames < n) n = *src_frames;
  float* d = dst + 2 * off;
  if (pan == kHalf && dpan == 0.0f) {
    for (uint32_t i = 0; i < n; ++i) {
      d[2 * i] = vol * src[2 * i] + d[2 * i];
      const float r = vol * src[2 * i + 1];
      vol = vol + dvol;
      d[2 * i + 1] = r + d[2 * i + 1];
    }
  } else {
    for (uint32_t i = 0; i < n; ++i) {
      const float vl = vol * src[2 * i];
      const float vr = vol * src[2 * i + 1];
      pan_stereo(d + 2 * i, vl, vr, pan);
      pan = pan + dpan;
      vol = vol + dvol;
    }
  }
  *src_frames = n;
  *dst_frames = n;
  return 0;
}

// [9] sub_16DE80: 1-channel source, step 1; centre: the sample goes to both sides, else
// L = (2 - 2p) v s, R = p (2 v s).
uint32_t pss_conv_mono_unity(const float* src, float* dst, uint32_t off, uint32_t* src_frames, uint32_t* dst_frames,
                             float*, float vol, float dvol, float pan, float dpan, float) {
  uint32_t n = *dst_frames;
  if (*src_frames < n) n = *src_frames;
  float* d = dst + 2 * off;
  if (pan == kHalf && dpan == 0.0f) {
    for (uint32_t i = 0; i < n; ++i) {
      const float x = vol * src[i];
      vol = vol + dvol;
      d[2 * i] = x + d[2 * i];
      d[2 * i + 1] = x + d[2 * i + 1];
    }
  } else {
    for (uint32_t i = 0; i < n; ++i) {
      const float x = vol * src[i];
      vol = vol + dvol;
      d[2 * i] = (kTwo - (pan + pan)) * x + d[2 * i];
      d[2 * i + 1] = pan * (x + x) + d[2 * i + 1];
      pan = pan + dpan;
    }
  }
  *src_frames = n;
  *dst_frames = n;
  return 0;
}

// [10] sub_16C9A0: 2-channel linear-interpolating resampler. state[0] = position relative to
// the first source frame (starts at -1.0, sub_15F670), state[1..2] = the frame before it.
// Phase 1 (position < 0) interpolates between the saved frame and source frame 0; phase 2
// between source frames i and i+1 while i+1 <= last frame. On exit the consumed source
// frame count is floor(position) (+1 when the step is <= 1, which also saves the frame at
// the position), and the state keeps the fractional position for the next call.
uint32_t pss_conv_stereo_resample(const float* src, float* dst, uint32_t off, uint32_t* src_frames,
                                  uint32_t* dst_frames, float* state, float vol, float dvol, float pan, float dpan,
                                  float step) {
  float pos = state[0];
  const uint32_t src_n = *src_frames;
  uint32_t left = *dst_frames;
  float* d = dst + 2 * off;
  if (left != 0 && 0.0f > pos) {
    float prev_r = state[2];
    float prev_l = state[1];
    for (;;) {
      const float p1 = pos + kOne;
      const int64_t f = trunc_i(floor_f(pos));
      const float a = pos * prev_l;
      const float b = pos * prev_r;
      const uint32_t i = static_cast<uint32_t>(2 * f + 2);
      const float l = vol * (p1 * src[i] - a);
      const float r = vol * (p1 * src[i + 1] - b);
      if (pan == kHalf) {
        d[0] = l + d[0];
        d[1] = r + d[1];
      } else {
        pan_stereo(d, l, r, pan);
      }
      pos = pos + step;
      if (!(pos < 0.0f)) {
        const uint32_t k = static_cast<uint32_t>(trunc_i(pos));
        prev_l = src[2 * k];
        prev_r = src[2 * k + 1];
      }
      pan = dpan + pan;
      vol = vol + dvol;
      d += 2;
      --left;
      if (left == 0 || !(0.0f > pos)) break;
    }
  }
  if (left != 0) {
    const float last = static_cast<float>(static_cast<int64_t>(static_cast<uint32_t>(src_n - 1)));
    float next = pos + step;
    if (!(last < next)) {
      uint32_t c = 0;
      const uint32_t total = left;
      for (;;) {
        const int64_t ip = trunc_i(pos);
        const uint32_t k = static_cast<uint32_t>(ip) * 2;
        const float frac = pos - static_cast<float>(static_cast<uint32_t>(ip));
        pos = next;
        const float w = kOne - frac;
        const float l = vol * (w * src[k] + frac * src[k + 2]);
        const float r = vol * (w * src[k + 1] + frac * src[k + 3]);
        if (pan == kHalf) {
          d[2 * c] = l + d[2 * c];
          d[2 * c + 1] = r + d[2 * c + 1];
        } else {
          pan_stereo(d + 2 * c, l, r, pan);
        }
        ++c;
        if (c == total) break;
        next = pos + step;
        pan = dpan + pan;
        vol = vol + dvol;
        if (!(last >= next)) break;
      }
      left -= c;
    }
  }
  const float fl = floor_f(pos);
  *src_frames = static_cast<uint32_t>(trunc_i(fl));
  *dst_frames -= left;
  if (step == kOne) {
    state[0] = kMinusOne;
  } else if (kOne > step) {
    state[0] = (pos - floor_f(pos)) + kMinusOne;
  } else {
    const uint32_t e = *src_frames;
    state[0] = pos - static_cast<float>(e);
    const uint32_t k = e == 0 ? 0 : 2 * e - 2;
    state[1] = src[k];
    state[2] = src[k | 1];
    if (*dst_frames == 0) *src_frames = src_n;
    return 0;
  }
  const uint32_t k = static_cast<uint32_t>(trunc_i(pos));
  state[1] = src[2 * k];
  state[2] = src[2 * k + 1];
  *src_frames += 1;
  return 0;
}

// [8] sub_16C6A0: the 1-channel resampler (same structure; state[1] = previous sample; the
// pan law is applied even at the centre).
uint32_t pss_conv_mono_resample(const float* src, float* dst, uint32_t off, uint32_t* src_frames, uint32_t* dst_frames,
                                float* state, float vol, float dvol, float pan, float dpan, float step) {
  float pos = state[0];
  const uint32_t src_n = *src_frames;
  uint32_t left = *dst_frames;
  float* d = dst + 2 * off;
  if (left != 0 && 0.0f > pos) {
    float prev = state[1];
    for (;;) {
      const float p1 = pos + kOne;
      const uint32_t i = static_cast<uint32_t>(trunc_i(floor_f(p1)));
      const float a = pos * prev;
      const float npos = pos + step;
      const float x = vol * (p1 * src[i] - a);
      d[0] = (kTwo - (pan + pan)) * x + d[0];
      d[1] = pan * (x + x) + d[1];
      pos = npos;
      if (!(pos < 0.0f)) prev = src[static_cast<uint32_t>(trunc_i(pos))];
      pan = pan + dpan;
      vol = vol + dvol;
      d += 2;
      --left;
      if (left == 0 || !(0.0f > pos)) break;
    }
  }
  if (left != 0) {
    const float last = static_cast<float>(static_cast<int64_t>(static_cast<uint32_t>(src_n - 1)));
    float next = pos + step;
    if (!(last < next)) {
      uint32_t c = 0;
      const uint32_t total = left;
      for (;;) {
        const uint32_t k = static_cast<uint32_t>(trunc_i(pos));
        const float frac = pos - static_cast<float>(k);
        pos = next;
        const float x = vol * ((kOne - frac) * src[k] + frac * src[k + 1]);
        d[2 * c] = (kTwo - (pan + pan)) * x + d[2 * c];
        d[2 * c + 1] = pan * (x + x) + d[2 * c + 1];
        ++c;
        if (c == total) break;
        next = pos + step;
        pan = pan + dpan;
        vol = vol + dvol;
        if (!(last >= next)) break;
      }
      left -= c;
    }
  }
  const float fl = floor_f(pos);
  *src_frames = static_cast<uint32_t>(trunc_i(fl));
  *dst_frames -= left;
  if (step == kOne) {
    state[0] = kMinusOne;
  } else if (kOne > step) {
    state[0] = (pos - floor_f(pos)) + kMinusOne;
  } else {
    const uint32_t e = *src_frames;
    state[0] = pos - static_cast<float>(e);
    state[1] = src[e == 0 ? 0 : e - 1];
    if (*dst_frames == 0) *src_frames = src_n;
    return 0;
  }
  state[1] = src[static_cast<uint32_t>(trunc_i(pos))];
  *src_frames += 1;
  return 0;
}

// sub_1665C0: 6-channel 16-bit source into an 8-channel float port (sub_15F360, port with 8
// channels, source 16-bit with 6 channels); channels 0..5 go to port channels 0..5, 6 and 7
// stay as they are; no pan. The volume and its step are scaled by 1/32768 (dword_3502D8).
// state[0] = position relative to the first source frame, state[1..3] = the 6 samples of the
// frame before it. Phase 1 (position < 0) interpolates between the saved frame and source
// frame 0; phase 2 between source frames i and i+1 while i+1 <= last frame.
uint32_t pss_conv_6ch16_8ch(const float* srcf, float* dst, uint32_t off, uint32_t* src_frames, uint32_t* dst_frames,
                            float* state, float vol, float dvol, float, float, float step) {
  const int16_t* src = reinterpret_cast<const int16_t*>(srcf);
  constexpr float kScale = 3.0517578125e-05f;  // dword_3502D8
  float* d = dst + 8 * off;
  float v = vol * kScale;
  const float dv = dvol * kScale;
  float pos = state[0];
  const uint32_t saved = *src_frames;
  int16_t prev[6];
  uint32_t left;
  auto load = [&](int16_t* to, uint32_t idx) { std::memcpy(to, src + idx, 12); };
  if (0.0f > pos) {
    std::memcpy(prev, state + 1, 12);
    uint32_t n = *dst_frames;
    left = n;
    if (n != 0 && 0.0f > pos) {
      for (;;) {
        const float w1 = pos + kOne;
        const int64_t f = trunc_i(floor_f(pos));
        const uint32_t idx = static_cast<uint32_t>(static_cast<int32_t>(f) * 6 + 6);
        const int16_t* cur = src + idx;
        for (int c = 0; c < 4; ++c) {
          const float x1 = w1 * static_cast<float>(cur[c]);
          const float x0 = pos * static_cast<float>(prev[c]);
          d[c] = v * (x1 - x0) + d[c];
        }
        for (int c = 4; c < 6; ++c) {
          const float x0 = pos * static_cast<float>(prev[c]);
          const float x1 = w1 * static_cast<float>(cur[c]);
          d[c] = v * (x1 - x0) + d[c];
        }
        pos = pos + step;
        if (!(pos < 0.0f)) load(prev, static_cast<uint32_t>(static_cast<int32_t>(trunc_i(pos)) * 6));
        v = v + dv;
        left = n - 1;
        if (n == 1) break;
        d += 8;
        n = left;
        if (!(0.0f > pos)) break;
      }
    }
  } else {
    load(prev, static_cast<uint32_t>(static_cast<int32_t>(trunc_i(pos)) * 6));
    left = *dst_frames;
  }
  if (left != 0) {
    const float last = static_cast<float>(static_cast<int64_t>(static_cast<uint32_t>(*src_frames - 1)));
    float next = step + pos;
    if (!(last < next)) {
      for (;;) {
        const uint32_t i = static_cast<uint32_t>(trunc_i(pos));
        const float frac = pos - static_cast<float>(static_cast<int64_t>(i));
        const float w0 = kOne - frac;
        const int16_t* a = src + static_cast<uint32_t>(i * 6);
        const int16_t* b = a + 6;
        for (int c = 0; c < 4; ++c) {
          const float x = w0 * static_cast<float>(a[c]) + frac * static_cast<float>(b[c]);
          d[c] = v * x + d[c];
        }
        {
          const float x = w0 * static_cast<float>(a[4]) + frac * static_cast<float>(b[4]);
          d[4] = v * x + d[4];
        }
        {
          const float x = w0 * static_cast<float>(a[5]) + frac * static_cast<float>(b[5]);
          d[5] = v * x + d[5];
        }
        pos = next;
        const uint32_t was = left;
        --left;
        if (was == 1) break;
        next = step + pos;
        d += 8;
        v = dv + v;
        if (!(last >= next)) break;
      }
    }
  }
  *src_frames = static_cast<uint32_t>(trunc_i(floor_f(pos)));
  *dst_frames -= left;
  if (step == kOne || kOne > step) {
    state[0] = step == kOne ? kMinusOne : (pos - floor_f(pos)) + kMinusOne;
    std::memcpy(state + 1, src + static_cast<uint32_t>(static_cast<int32_t>(trunc_i(pos)) * 6), 12);
    *src_frames += 1;
  } else {
    const uint32_t e = *src_frames;
    state[0] = pos - static_cast<float>(static_cast<int64_t>(e));
    std::memcpy(state + 1, src + (e == 0 ? 0 : 6 * e - 6), 12);
    if (*dst_frames == 0) *src_frames = saved;
  }
  return 0;
}

PssConverter pss_converter(int index) {
  switch (index) {
    case 8: return pss_conv_mono_resample;
    case 9: return pss_conv_mono_unity;
    case 10: return pss_conv_stereo_resample;
    case 11: return pss_conv_stereo_unity;
    default:
      host::note_unimplemented("libScePsm PSS audio converters 0..7 (8/16-bit sources, table 0x4FBED0) not reproduced");
      return nullptr;
  }
}

}  // namespace ssn::psm::audio
