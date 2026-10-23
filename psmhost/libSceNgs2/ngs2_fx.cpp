// libSceNgs2.native — the gain helpers shared by the patch mixer and the modules, the default
// channel matrices, and the two mastering-voice modules the BGLayer path runs: ChannelConverter
// (id 34) and Output (id 26). The other effect modules (PeakMeter, LfeFilter, Compressor, Gain on
// the mastering voice; Envelope, Distortion, UserFx, Filter on the sampler voice) only run once a
// command has set one of their parameters (voice +300 & +568, see ngs2_core.cpp), which the eboot
// never does.
//
// The firmware's vector paths (AVX, taken when the buffers are 32-byte aligned, which NGS2's grain
// buffers are) round differently from its scalar tails; both are reproduced with the firmware's
// operation order. FMA appears only where the firmware fuses (vfmadd213ps in sub_53E0).
#include <algorithm>
#include <cmath>
#include <cstring>

#include "core/log.h"
#include "host/boundary.h"
#include "libSceNgs2/ngs2_image.h"
#include "libSceNgs2/ngs2_internal.h"

namespace ssn::ngs2 {
namespace {

constexpr float kOne = 1.0f;    // dword_4A278
constexpr float kEight = 8.0f;  // dword_4A2DC
constexpr float kLane[8] = {0, 1, 2, 3, 4, 5, 6, 7};  // ymmword_4A680
inline bool aligned32(const void* p) { return (reinterpret_cast<uintptr_t>(p) & 0x1F) == 0; }

}  // namespace

// sub_50C0(src, dst, n, gPrev, gCur): dst = src * gain, the gain ramping linearly from gPrev to gCur.
void write_gain(const float* src, float* dst, uint32_t n, float g_prev, float g_cur) {
  if (g_prev == g_cur) {                         // 0x50CA (a NaN takes the ramp path)
    if (g_cur == 0.0f) { clear_samples(dst, n); return; }  // 0x50D8
    if (g_cur == kOne) {                         // 0x51D2: copy unless in place
      if (src != dst) std::memmove(dst, src, sizeof(float) * n);
      return;
    }
    for (uint32_t i = 0; i < n; ++i) dst[i] = g_cur * src[i];  // 0x5276 vmulps / vmulss
    return;
  }
  const float step = (g_cur - g_prev) / static_cast<float>(n);  // 0x513D
  uint32_t i = 0;
  float g = g_prev;
  if (aligned32(src) && aligned32(dst)) {        // 0x5151
    float lanes[8];
    for (int l = 0; l < 8; ++l) lanes[l] = g_prev + step * kLane[l];  // vmulps, vaddps
    const float step8 = step * kEight;
    for (uint32_t b = n >> 3; b; --b, i += 8)
      for (int l = 0; l < 8; ++l) {
        dst[i + l] = lanes[l] * src[i + l];
        lanes[l] = step8 + lanes[l];
      }
    g = lanes[0];                                // the tail continues from lane 0
  }
  for (; i < n; ++i) {                           // 0x51B0
    dst[i] = g * src[i];
    g = step + g;
  }
}

// sub_53E0(src, dst, n, gPrev, gCur): dst += src * gain (ramp gPrev -> gCur).
void accumulate_gain(const float* src, float* dst, uint32_t n, float g_prev, float g_cur) {
  const bool vec = aligned32(src) && aligned32(dst);
  if (g_prev == g_cur) {
    if (g_cur == kOne) {                         // 0x5404
      for (uint32_t i = 0; i < n; ++i) dst[i] = dst[i] + src[i];
      return;
    }
    if (g_cur == 0.0f) return;                   // 0x551F
    uint32_t i = 0;
    if (vec)
      for (uint32_t b = n >> 3; b; --b, i += 8)
        for (int l = 0; l < 8; ++l) dst[i + l] = std::fma(g_cur, src[i + l], dst[i + l]);  // vfmadd213ps
    for (; i < n; ++i) dst[i] = g_cur * src[i] + dst[i];  // 0x55B0 vmulss then vaddss
    return;
  }
  const float step = (g_cur - g_prev) / static_cast<float>(n);  // 0x548C
  uint32_t i = 0;
  float g = g_prev;
  if (vec) {
    float lanes[8];
    for (int l = 0; l < 8; ++l) lanes[l] = g_prev + step * kLane[l];
    const float step8 = step * kEight;
    for (uint32_t b = n >> 3; b; --b, i += 8)
      for (int l = 0; l < 8; ++l) {
        dst[i + l] = std::fma(lanes[l], src[i + l], dst[i + l]);  // 0x54D4
        lanes[l] = step8 + lanes[l];
      }
    g = lanes[0];
  }
  for (; i < n; ++i) {                           // 0x5500
    const float p = g * src[i];
    g = step + g;
    dst[i] = p + dst[i];
  }
}

// The default matrix of `kind` for numIn -> numOut channels (0x17E2C: kind block 0xA2 bytes,
// row 9 * min(numIn, 9), column numOut; floats at off_6A9C0).
const float* default_matrix(uint32_t kind, uint32_t num_in, uint32_t num_out) {
  const auto& idx = matrix_index_table();
  const auto& f = matrix_floats();
  if (idx.empty() || f.empty() || kind > 3 || num_out > 8) return nullptr;
  const size_t k = size_t(81) * kind + 9 * std::min<uint32_t>(num_in, 9) + num_out;
  if (k >= idx.size() || size_t(idx[k]) + size_t(num_in) * num_out > f.size()) return nullptr;
  return f.data() + idx[k];
}

// ChannelConverter defaults (descriptor 0x67080, written by sub_B220 from the parameter ranges):
// numInputChannels 0, numOutputChannels 0, the matrices = the default float table.
void cc_reset_params(CCParams& p) {
  p.num_in = 0;
  p.num_out = 0;
  const auto& f = matrix_floats();
  const size_t n = std::min(f.size(), std::size(p.m));
  std::memset(p.m, 0, sizeof p.m);
  if (n) std::memcpy(p.m, f.data(), n * sizeof(float));
}

// ChannelConverter process — sub_2C7D0. Matrices live in the parameters at float index
// idx[9 * in + out] (the default-table layout). `out_limit` = sub_15B30: the channel count of the
// render buffer the voice's Output module targets.
void channel_converter(ModuleCtx& c, const CCParams& p, CCState& st, uint32_t out_limit) {
  uint32_t in = p.num_in ? p.num_in : c.num_channels;
  if (in >= c.num_channels) in = c.num_channels;
  uint32_t out = p.num_out ? p.num_out : c.num_channels;
  if (out >= out_limit) out = out_limit;
  if (c.flags & 0x100) { st.num_in = 0; st.num_out = 0; }  // reset: *(qword*)state = 0
  const auto& idx = matrix_index_table();
  if (idx.empty()) return;
  const size_t k = 9 * size_t(in) + out;
  const float* cur = &p.m[idx[k]];
  const float* prev = (st.num_in == in && st.num_out == out) ? st.m : cur;
  // sub_15600: one temporary per input from the system pool (32-byte aligned on the console: pool
  // base +32 + 4*n*i); the inputs are copied first (sub_5CE0) since out may alias in.
  thread_local std::vector<std::vector<float>> al;
  thread_local std::vector<float*> tp;
  if (al.size() < in) al.resize(in);
  tp.assign(in, nullptr);
  for (uint32_t i = 0; i < in; ++i) {
    al[i].resize(c.num_samples + 8);
    float* a = al[i].data();
    while (!aligned32(a)) ++a;
    std::memcpy(a, c.in[i], sizeof(float) * c.num_samples);
    tp[i] = a;
  }
  uint64_t written = 0;
  for (uint32_t o = 0; o < out; ++o)
    for (uint32_t i = 0; i < in; ++i) {
      const float gp = prev[o * in + i], gc = cur[o * in + i];
      if (!(gp > 0.0f) && !(gc > 0.0f)) continue;  // 0x2C9A5
      if (written & (1ull << o)) accumulate_gain(tp[i], c.out[o], c.num_samples, gp, gc);
      else {
        write_gain(tp[i], c.out[o], c.num_samples, gp, gc);
        written |= 1ull << o;
      }
    }
  for (uint32_t ch = 0; ch < c.num_channels; ++ch)
    if (!(written & (1ull << ch))) clear_samples(c.out[ch], c.num_samples);  // sub_4F80
  std::memcpy(st.m, cur, sizeof(float) * out * in);
  st.num_in = in;
  st.num_out = out;
}

namespace {

constexpr float kS16Scale = 32767.0f;  // dword_4A400
constexpr float kMinusOne = -1.0f;     // dword_4A2D8
constexpr float kS16Min = -32767.0f;   // dword_4A378
const float kZero[2048] = {};          // unk_525C0: static zeros for the buffer channels a voice lacks

// sub_62E0: first writer of a render buffer this render.
void convert_write(uint32_t type, float* const* src, void* dst, uint32_t n, uint32_t ch) {
  if (type == 24) {  // float32 interleaved, channel k -> slot k, no scaling or clipping
    float* d = static_cast<float*>(dst);
    for (uint32_t i = 0; i < n; ++i)
      for (uint32_t k = 0; k < ch; ++k) d[i * ch + k] = src[k][i];
    return;
  }
  if (type == 18) {  // int16 interleaved
    int16_t* d = static_cast<int16_t*>(dst);
    for (uint32_t k = 0; k < ch; ++k)
      for (uint32_t i = 0; i < n; ++i) {
        const float s = src[k][i];
        float v = (kOne < s ? kOne : s) * kS16Scale;  // vminss 1.0, s ; vmulss
        if (s < kMinusOne) v = kS16Min;
        d[i * ch + k] = static_cast<int16_t>(static_cast<int32_t>(v));
      }
    return;
  }
  host::note_unimplemented("ngs2: render buffer waveform type other than 18 / 24 (sub_62E0)");
}

// sub_68D0: a buffer already written this render (type 24: d = s + d).
void convert_accumulate(uint32_t type, float* const* src, void* dst, uint32_t n, uint32_t ch) {
  if (type == 24) {
    float* d = static_cast<float*>(dst);
    for (uint32_t i = 0; i < n; ++i)
      for (uint32_t k = 0; k < ch; ++k) d[i * ch + k] = src[k][i] + d[i * ch + k];
    return;
  }
  host::note_unimplemented("ngs2: render buffer accumulation for waveform type 18 (sub_68D0)");
}

}  // namespace

// Output process — sub_307D0 -> sub_15980 -> sub_15990(ctx, outputId, in, numChannels).
void output_module(ModuleCtx& c, uint32_t output_id) {
  System* sys = c.voice->rack->system;
  if (output_id >= sys->num_buffers || !sys->buffers) return;
  const RenderBufferInfo& rb = sys->buffers[output_id];
  float* chans[38];
  const uint32_t n = c.num_samples;
  uint32_t ch = c.num_channels;
  for (uint32_t k = 0; k < ch && k < 38; ++k) chans[k] = c.in[k];
  if (rb.num_channels > ch) {
    for (uint32_t k = ch; k < rb.num_channels && k < 38; ++k) chans[k] = const_cast<float*>(kZero);
  }
  ch = rb.num_channels;
  const uint32_t bit = 1u << output_id;
  if (sys->written & bit) convert_accumulate(rb.waveform_type, chans, rb.buffer, n, ch);
  else {
    convert_write(rb.waveform_type, chans, rb.buffer, n, ch);
    sys->written |= bit;
  }
}

}  // namespace ssn::ngs2
