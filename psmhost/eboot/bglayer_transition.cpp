// The BackgroundLayer "Transition" layer — see bglayer_transition.h. Every function cites the eboot
// address it reproduces; the control flow follows the disassembly (labels in comments).
#include "eboot/bglayer_transition.h"

#include <windows.h>
#include <xmmintrin.h>

#include <cmath>
#include <cstring>
#include <limits>
#include <map>
#include <mutex>
#include <string>

#include "core/log.h"
#include "eboot/bglayer_gpu.h"
#include "host/boundary.h"
#include "libScePsm/graphics/objects.h"

namespace ssn::eboot::bglayer {
namespace {

constexpr float kFltMax = std::numeric_limits<float>::max();
// Ripple parameters per degree (writable data 0xD61220, 4 x 40 bytes = ripple_p c0) and slide
// parameters (0xD612C0, 4 x 24 bytes = slide_in_p c0). The transition writes into them (+0x14 of
// the slide entry) as the firmware does; the initial values are read from the eboot.
float g_ripple[4][10];
float g_slide[4][6];
std::once_flag g_tables_once;
const float kSlideDirection[2] = {-1.0f, 1.0f};  // 0xB8E2D8: [type == 7]
const float kFallbackDelay[2] = {500.0f, 680.0f};  // 0xB8E1B8: [image-viewer bit clear]

void load_tables() {
  std::call_once(g_tables_once, [] {
    const auto r = eboot_bytes(0xD61220, sizeof g_ripple);
    const auto s = eboot_bytes(0xD612C0, sizeof g_slide);
    if (r.size() == sizeof g_ripple) std::memcpy(g_ripple, r.data(), sizeof g_ripple);
    if (s.size() == sizeof g_slide) std::memcpy(g_slide, s.data(), sizeof g_slide);
    if (r.size() != sizeof g_ripple || s.size() != sizeof g_slide)
      log::error("bglayer", "Transition: ripple / slide parameter tables not readable");
  });
}

float duration_166(int32_t degree) { return static_cast<float>(static_cast<uint32_t>(degree)) * 166.666672f + 300.0f; }  // dword_AFFF50, dword_AFFD38
float duration_366(int32_t degree) { return static_cast<float>(static_cast<uint32_t>(degree)) * 366.666687f + 300.0f; }  // dword_AFFD10

std::vector<int32_t> g_unmap;  // BackgroundLayer +592 (begin +600, end +608)
std::mutex g_unmap_lock;

// --- the gradation spline (0xA7390 with the solver 0xA7D10) ---------------------------------
// 0xA7D10: Gauss-Jordan elimination of the n x (n + 1) augmented system, as the firmware writes it:
// the pivot search compares |a[k][i]| (row k, column i) with |a[best][k]|, rows are swapped as
// whole vectors, a pivot below dword_B001B8 leaves the result zero, the pivot row is divided from
// column k + 1 on and the other rows are reduced from column k + 1 on.
std::vector<float> solve(std::vector<std::vector<float>> m, const std::vector<float>& rhs) {
  const int n = static_cast<int>(m.size());
  std::vector<float> res(n, 0.0f);
  float eps;
  {
    const auto e = eboot_bytes(0xB001B8, 4);
    std::memcpy(&eps, e.data(), 4);
  }
  for (int i = 0; i < n; ++i) {
    m[i].resize(n + 1, 0.0f);
    m[i][n] = rhs[i];
  }
  for (int k = 0; k < n; ++k) {
    int best = k;
    for (int i = k; i < n; ++i)
      if (std::fabs(m[k][i]) > std::fabs(m[best][k])) best = i;
    if (best != k) std::swap(m[k], m[best]);
    const float p = m[k][k];
    if (!(std::fabs(p) >= eps)) return std::vector<float>(n, 0.0f);  // jb: pivot too small (or NaN)
    for (int j = k + 1; j <= n; ++j) m[k][j] = m[k][j] / p;
    for (int i = 0; i < n; ++i) {
      if (i == k) continue;
      for (int j = k + 1; j <= n; ++j) m[i][j] = m[i][j] - m[i][k] * m[k][j];
    }
  }
  for (int i = 0; i < n; ++i) res[i] = m[i][n];
  return res;
}

// 3 * dy_i / h_i for a pair of segments with one rcpps + Newton step, as 0xA77C7..0xA780D write it.
// HOST VALUE: vrcpps is the host CPU's approximation (the PS5's Zen 2 table may differ in the low
// bits; the Newton step leaves at most about one ulp of difference).
void three_slopes(float h0, float h1, float dy0, float dy1, float& q0, float& q1) {
  const __m128 h = _mm_setr_ps(h0, h1, 0.0f, 0.0f);
  const __m128 r = _mm_rcp_ps(h);
  const __m128 a = _mm_mul_ps(_mm_setr_ps(dy0, dy1, 0.0f, 0.0f), _mm_set1_ps(3.0f));  // dword_B0010C
  const __m128 q = _mm_mul_ps(a, r);
  const __m128 e = _mm_sub_ps(a, _mm_mul_ps(h, q));
  const __m128 res = _mm_add_ps(q, _mm_mul_ps(r, e));
  float o[4];
  _mm_storeu_ps(o, res);
  q0 = o[0];
  q1 = o[1];
}

void make_gradation(Gradation& g) {  // 0xA7390
  float* f = g.f;
  const float m = f[8];                                      // +0x20 midpoint
  const float x[5] = {-0.01f, 0.0f, m, 1.0f, 1.00999999f};  // 0xBC23D70A, 0, +0x20, 1.0, 0x3F8147AE
  (void)x;
  const float y[5] = {f[5], f[5], f[6], f[7], f[7]};        // +0x14, +0x14, +0x18, +0x1C, +0x1C
  const float h[4] = {0.01f, m, 1.0f - m, 0.00999999046f};  // 0x3C23D70A, m, 1 - m, 0x3C23D700
  std::vector<std::vector<float>> a = {
      {1.0f, 0.0f, 0.0f, 0.0f, 0.0f},
      {h[0], (h[0] + h[1]) + (h[0] + h[1]), h[1], 0.0f, 0.0f},
      {0.0f, h[1], (h[1] + h[2]) + (h[1] + h[2]), h[2], 0.0f},
      {0.0f, 0.0f, h[2], (h[2] + h[3]) + (h[2] + h[3]), h[3]},
      {0.0f, 0.0f, 0.0f, 0.0f, 1.0f},
  };
  std::vector<float> rhs(5, 0.0f);
  for (int i = 1; i <= 3; ++i) {  // 0xA77B1 / 0xA7811 / 0xA7884
    float al = 0.0f;
    if (h[i] != 0.0f && h[i - 1] != 0.0f) {
      float q0, q1;
      three_slopes(h[i - 1], h[i], y[i] - y[i - 1], y[i + 1] - y[i], q0, q1);
      al = q1 - q0;
    }
    rhs[i] = al;
  }
  const std::vector<float> c = solve(a, rhs);
  // b_i = dy_i / h_i + h_i * (2 c_i + c_{i+1}) * (-1/3), d_i = (c_{i+1} - c_i) / (3 h_i), register
  // flow of 0xA7943..0xA7B5F (x0..x5 are the xmm registers the stores read).
  float b[4] = {}, d[4] = {};
  float x0, x1, x2, x3, x5 = 0.0f;
  if (h[0] != 0.0f) {
    b[0] = (y[1] - y[0]) / h[0] + h[0] * (c[0] + c[0] + c[1]) * -0.333333343f;
    x0 = (c[1] - c[0]) / (h[0] * 3.0f);
  } else {
    b[0] = 0.0f;
    x0 = 0.0f;
  }
  d[0] = x0;
  if (h[1] != 0.0f) {
    x0 = (y[2] - y[1]) / h[1] + h[1] * (c[1] + c[1] + c[2]) * -0.333333343f;
    b[1] = x0;
    x1 = (c[2] - c[1]) / (h[1] * 3.0f);
  } else {
    b[1] = 0.0f;
    x1 = 0.0f;
  }
  d[1] = x1;
  if (h[2] != 0.0f) {
    x2 = (y[3] - y[2]) / h[2] + h[2] * (c[2] + c[2] + c[3]) * -0.333333343f;
    b[2] = x2;
    x3 = (c[3] - c[2]) / (h[2] * 3.0f);
  } else {
    b[2] = 0.0f;
    x2 = 0.0f;  // 0xA7A39 xorps xmm2 before the compare
    x3 = 0.0f;
  }
  d[2] = x3;
  if (h[3] != 0.0f) {
    b[3] = (y[4] - y[3]) / h[3] + h[3] * (c[3] + c[3] + c[4]) * -0.333333343f;
    x5 = (c[4] - c[3]) / (h[3] * 3.0f);
  } else {
    b[3] = 0.0f;
  }
  d[3] = x5;
  f[9] = y[1];   // +0x24
  f[10] = x0;    // +0x28
  f[11] = c[1];  // +0x2C
  f[12] = x1;    // +0x30
  f[13] = y[2];  // +0x34
  f[14] = x2;    // +0x38
  f[15] = c[2];  // +0x3C
  f[16] = x3;    // +0x40
}

// Gradation inputs of 0xA82A0 per basemat type (+0xAC): {centre x, y, +8, +0xC, +0x10, +0x14,
// +0x18, +0x1C, +0x20}.
void gradation_input(int32_t type, Gradation& g) {
  std::memset(g.f, 0, sizeof g.f);
  auto rd = [](uint64_t va, float* out, int n) {
    const auto b = eboot_bytes(va, 4 * n);
    if (b.size() == size_t(4 * n)) std::memcpy(out, b.data(), 4 * n);
  };
  switch (type) {
    case 1:  // xmmword_B8A530, xmmword_B8B7F0, 0x3F4000003F400000, 0.5
      rd(0xB8A530, g.f, 2);
      rd(0xB8B7F0, g.f + 2, 4);
      g.f[6] = 0.75f; g.f[7] = 0.75f; g.f[8] = 0.5f;
      break;
    case 2:  // dword_AFFC24 (low qword, upper 0), xmmword_B8B200, 0x3F4000003DCCCCCD, 0x3EE21965
      rd(0xAFFC24, g.f, 1);
      rd(0xB8B200, g.f + 2, 4);
      { const uint32_t a = 0x3DCCCCCDu, b = 0x3F400000u, c = 0x3EE21965u;
        std::memcpy(&g.f[6], &a, 4); std::memcpy(&g.f[7], &b, 4); std::memcpy(&g.f[8], &c, 4); }
      break;
    case 3:  // qword_B8AF60, xmmword_B8C870, 0x3F4000003EE66666, 0.5
      rd(0xB8AF60, g.f, 2);
      rd(0xB8C870, g.f + 2, 4);
      { const uint32_t a = 0x3EE66666u; std::memcpy(&g.f[6], &a, 4); }
      g.f[7] = 0.75f; g.f[8] = 0.5f;
      break;
    case 4:  // xmmword_B8A530, xmmword_B8A8A0, 0x3F59999A3F59999A, 0.5
      rd(0xB8A530, g.f, 2);
      rd(0xB8A8A0, g.f + 2, 4);
      { const uint32_t a = 0x3F59999Au; std::memcpy(&g.f[6], &a, 4); std::memcpy(&g.f[7], &a, 4); }
      g.f[8] = 0.5f;
      break;
    default:  // xmmword_B8A530 low qword, the rest zero
      rd(0xB8A530, g.f, 2);
      break;
  }
  make_gradation(g);
}

// 0xA84A0: which basemat type the visibility / high-contrast changes select.
void basemat_select(Basemat& b) {
  const int32_t hc = b.args.hc, vis = b.args.vis, type = b.args.type;
  if (hc == 2 && vis == 0) return;
  uint8_t al = b.dirty;
  if (al && b.saved_type == 0) b.saved_type = b.type;
  auto finish = [&](bool restart) { if (restart) b.time = 0.0f; };  // 0xA8631
  auto a8651 = [&](bool restart) { b.saved_type = 0; finish(restart); };
  if (hc == 2) {
    if (vis == 3) {
      if ((type | 2) != 2) {  // 0xA863C
        b.dirty = 1;
        b.type = b.saved_type;
        a8651(true);
        return;
      }
      const bool ne = b.type != 0;
      b.dirty = ne;
      b.type = 0;
      a8651(ne);
      return;
    }
    if (vis == 2) {
      if (b.type != 1) b.saved_type = b.type;
      b.type = 1;
      finish(al != 0);
      return;
    }
    if (vis == 1) {
      b.saved_type = b.type != 0 ? b.type : b.cur_type;
      b.dirty = 1;
      b.type = 1;
      finish(true);
      return;
    }
    finish(al != 0);  // 0xA862D
    return;
  }
  if (vis == 0 && b.type == 0) return;
  int32_t eax;
  if (hc == 3) {  // 0xA85F8
    const bool ne = b.type != b.saved_type;
    b.dirty = ne;
    b.type = b.saved_type;
    a8651(ne);
    return;
  }
  if (hc == 1) {  // 0xA85BE
    eax = b.type;
    if (eax != 4) b.saved_type = eax;
    if (vis == 3 && (type | 2) == 2) {
      const bool ne = eax != 0;
      b.dirty = ne;
      b.type = 0;
      finish(ne);
      return;
    }
    b.dirty = eax != 4;  // 0xA8616
    const bool ne = eax != 4;
    b.type = 4;
    finish(ne);
    return;
  }
  if (hc == 0) {
    eax = b.type;
    b.dirty = eax != 4;
    b.saved_type = eax;
    const bool ne = eax != 4;  // 0xA8620
    b.type = 4;
    finish(ne);
    return;
  }
  finish(al != 0);
}

// --- GPU ---------------------------------------------------------------------------------------
struct Pipelines {
  PassLayout layout;
  std::map<std::pair<std::string, VkFormat>, VkPipeline> map;
  PFN_vkCmdPushDescriptorSetKHR push = nullptr;
};
Pipelines g_gpu;

VkPipeline pipeline(const std::string& ps, VkFormat fmt) {
  if (!g_gpu.layout.layout) {
    g_gpu.layout = make_pass_layout(3, 2, 4);  // 0 user data, 1 c0, 2 c1, 3 / 4 t0 / t1; realtime push
    g_gpu.push = reinterpret_cast<PFN_vkCmdPushDescriptorSetKHR>(vkGetDeviceProcAddr(ssn::gfx::device().device(), "vkCmdPushDescriptorSetKHR"));
  }
  auto& p = g_gpu.map[{ps, fmt}];
  if (!p)  // basic_vv + the pixel shader, blending disabled (sub_DF760 + sub_DE620), primitive 6
    p = make_fullscreen_pipeline(g_gpu.layout, shader_module("bglayer_basic_vv"), shader_module("bglayer_" + ps),
                                 VK_PRIMITIVE_TOPOLOGY_TRIANGLE_STRIP, fmt, false);
  return p;
}

uint32_t realtime_counter() {  // as light_p (HOST VALUE, README)
  LARGE_INTEGER c, f;
  QueryPerformanceCounter(&c);
  QueryPerformanceFrequency(&f);
  return static_cast<uint32_t>(static_cast<unsigned long long>(c.QuadPart) * 100000000ull / static_cast<unsigned long long>(f.QuadPart));
}

// One full-screen draw (sub_DE8B0(gfx, 1, 1): 3 vertices) of `ps` into `target` with the textures
// bound to t0 / t1 and the constant buffers c0 / c1. The user-data block carries the T# WIDTH /
// HEIGHT fields of the bound textures (dwords 1 / 2 of each T#; the shaders read them); the other
// T# bits and the S# / V# dwords read 0 because the transcription takes those from the bindings.
void draw(const std::string& ps, ssn::gfx::Image& target, ssn::gfx::Image* t0, ssn::gfx::Image* t1, uint32_t tsharp_stride,
          const void* c0, size_t c0_size, const void* c1, size_t c1_size) {
  auto& dev = ssn::gfx::device();
  VkPipeline p = pipeline(ps, target.format);
  if (!p) return;
  ssn::gfx::Image* tex[2] = {t0, t1 ? t1 : t0};
  dev.end_rendering();
  for (auto* t : tex)
    if (t && t->layout != VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL) dev.transition(dev.cmd(), *t, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
  dev.bind_target(&target, nullptr);
  dev.ensure_rendering();
  VkCommandBuffer cb = dev.cmd();
  ssn::gfx::Slice ud = dev.alloc(256, 256), s0 = dev.alloc(256, 256), s1 = dev.alloc(256, 256);
  std::memset(ud.data, 0, 256);
  std::memset(s0.data, 0, 256);
  std::memset(s1.data, 0, 256);
  uint32_t* u = static_cast<uint32_t*>(ud.data);
  for (int i = 0; i < 2; ++i) {
    if (!tex[i]) continue;
    const uint32_t w = tex[i]->width - 1, h = tex[i]->height - 1;
    u[i * tsharp_stride + 1] = (w & 3u) << 30;
    u[i * tsharp_stride + 2] = ((w >> 2) & 0xFFFu) | ((h & 0x3FFFu) << 14);
  }
  if (c0) std::memcpy(s0.data, c0, c0_size);
  if (c1) std::memcpy(s1.data, c1, c1_size);
  VkDescriptorBufferInfo bi[3] = {{ud.buffer, ud.offset, 256}, {s0.buffer, s0.offset, 256}, {s1.buffer, s1.offset, 256}};
  const VkSampler smp = sampler_for_ssharp(0x92, 0x00F00000, 0x05500000, 0);
  VkDescriptorImageInfo ii[2] = {{smp, tex[0]->view, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL},
                                 {smp, tex[1]->view, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL}};
  VkWriteDescriptorSet wr[5]{};
  for (uint32_t i = 0; i < 5; ++i) {
    wr[i].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
    wr[i].dstBinding = i;
    wr[i].descriptorCount = 1;
    if (i < 3) {
      wr[i].descriptorType = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER;
      wr[i].pBufferInfo = &bi[i];
    } else {
      wr[i].descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
      wr[i].pImageInfo = &ii[i - 3];
    }
  }
  vkCmdBindPipeline(cb, VK_PIPELINE_BIND_POINT_GRAPHICS, p);
  g_gpu.push(cb, VK_PIPELINE_BIND_POINT_GRAPHICS, g_gpu.layout.layout, 0, 5, wr);
  const uint32_t rt = realtime_counter();
  vkCmdPushConstants(cb, g_gpu.layout.layout, VK_SHADER_STAGE_FRAGMENT_BIT, 0, 4, &rt);
  VkViewport vp{0.0f, float(target.height), float(target.width), -float(target.height), 0.0f, 1.0f};
  VkRect2D sc{{0, 0}, {target.width, target.height}};
  vkCmdSetViewport(cb, 0, 1, &vp);
  vkCmdSetScissor(cb, 0, 1, &sc);
  vkCmdDraw(cb, 3, 1, 0, 0);
}

XfTex make_texture(uint32_t texel, uint32_t w, uint32_t h, bool target) {  // 0xE0590 (format 18) / 0xE1850
  auto t = std::make_shared<XfTexture>();
  auto& dev = ssn::gfx::device();
  t->width = w;
  t->height = h;
  // XF format 18 -> qword_C9E4C0[18] = data format 0x38 (8_8_8_8 UNORM), dst_sel XYZW.
  t->own = dev.create_image(w, h, 1, VK_FORMAT_R8G8B8A8_UNORM,
                            VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT | (target ? VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT : 0));
  if (!target) dev.upload_image(t->own, 0, 0, 0, 1, 1, &texel, 4);  // sub_E0BA0
  return t;
}

}  // namespace

XfTexture::~XfTexture() {
  if (own.valid()) ssn::gfx::device().destroy_image(own);
}
ssn::gfx::Image* XfTexture::image() {
  if (psm) return psm->image.valid() ? &psm->image : nullptr;
  return own.valid() ? &own : nullptr;
}

void push_unmap_request(int32_t id) {  // 0x8F060 -> 0x739C0
  if (id < 0) return;
  std::lock_guard<std::mutex> lk(g_unmap_lock);
  g_unmap.push_back(id);
}
uint32_t unmap_request_count() {
  std::lock_guard<std::mutex> lk(g_unmap_lock);
  return static_cast<uint32_t>(g_unmap.size());
}
int32_t pop_unmap_request() {
  std::lock_guard<std::mutex> lk(g_unmap_lock);
  if (g_unmap.empty()) return -1;
  const int32_t id = g_unmap.back();
  g_unmap.pop_back();
  return id;
}
void clear_unmap_requests() {
  std::lock_guard<std::mutex> lk(g_unmap_lock);
  g_unmap.clear();
}

// ---------------------------------------------------------------------------------------------
bool TransitionLayer::initialize() {  // 0xA3CB0
  load_tables();
  // +376: 1x1 format 18 with 0xFF000000 (opaque black); +384: 1x1 with 0 (transparent).
  black_ = make_texture(0xFF000000u, 1, 1, false);
  transparent_ = make_texture(0u, 1, 1, false);
  if (!black_->image() || !transparent_->image()) return false;
  cur_.reset();
  next_.reset();
  overlay_.reset();
  // basemat +208: dirty, colour 0xB8BEF0, time 0 / duration 1000, both gradations from the
  // default input (centre 0xB8A530, the rest zero).
  basemat_ = Basemat{};
  gradation_input(0, basemat_.from);
  gradation_input(0, basemat_.to);
  state = 4;  // +24 = 0, state 4
  return true;
}

bool TransitionLayer::visible() const {  // 0xA7310
  return shader_ != 0 && state == 4 && opacity > 0.0001f;
}

void TransitionLayer::frame(float frame_ms) {  // 0xA46B0
  f6C_ = frame_ms;
  f78_ = static_cast<double>(frame_ms) + f78_;
  if (0.0001f > opacity) f78_ = 0.0;
  snapshot_.reset();  // +0x188
}

XfTex* TransitionLayer::slot(int32_t id) {
  switch (id) {
    case 0: return &slots_[0];
    case 1: return &slots_[1];
    case 2: return &slots_[4];
    case 4: return &slots_[5];
    case 5: return &slots_[2];
    case 6: return &slots_[3];
    default: return nullptr;
  }
}

void TransitionLayer::release_slot(int32_t id) {  // 0xA6760
  XfTex* s = slot(id);
  if (!s || !*s) return;
  if (cur_ && cur_ == *s && cur_ != black_) cur_ = black_;
  if (next_ && next_ == *s && next_ != black_) next_ = black_;
  push_unmap_request(id);  // sub_8F060
  s->reset();
}

int32_t TransitionLayer::map_texture(int32_t id, const XfTex& tex) {  // 0xA6F90
  if (bB8_) return 0;
  XfTex* s = slot(id);
  if (!s) return -2;
  if (!*s) {
    if (!tex) return 0;
    *s = tex;  // E0A60(tex, 0, 0, 0): clamp 2 on all axes (the S# draw() uses)
    return 0;
  }
  if (cur_ && cur_ == *s && cur_ != black_) cur_ = black_;
  if (next_ && next_ == *s && next_ != black_) next_ = black_;
  *s = tex;
  return 0;
}

int32_t TransitionLayer::set_basemat(const BGBasematParam& p) {  // 0xA6F70 -> 0xA8900
  Basemat& b = basemat_;
  b.color[0] = p.color[0];
  b.color[1] = p.color[1];
  b.color[2] = p.color[2];
  b.color[3] = 1.0f;
  b.type = static_cast<uint32_t>(p.type - 1) < 3 ? p.type : 0;
  b.duration = p.duration;
  b.time = 0.0f;
  b.dirty = 1;
  return 0;
}

void TransitionLayer::basemat_update(const BasematArgs& a) {  // 0xA82A0
  Basemat& b = basemat_;
  b.time = a.dt + b.time;
  b.args = a;
  basemat_select(b);  // 0xA84A0
  if (b.dirty) {
    Gradation g;
    gradation_input(b.type, g);  // switch on +0xAC - 1, then 0xA7390
    b.to = g;
    b.dirty = 0;
  }
  auto adopt = [&] {  // 0xA841B
    b.cur_type = b.type;
    b.from = b.to;
    b.time = b.duration + 1.0f;
  };
  if (a.flag) {
    adopt();
  } else if (b.cur_type != b.type && a.shader == 1 && b.time > b.duration) {
    adopt();
  }
}

int32_t TransitionLayer::set_transition(const BGTransitionParam& p) {  // 0xA6980
  if (basemat_.time < basemat_.duration) {  // finish a running basemat animation
    basemat_.time = basemat_.duration;
    const BasematArgs saved = basemat_.args;
    basemat_update(saved);
  }
  const int32_t flags = p.flag;
  if (flags & 2) {  // cancel
    const int32_t t = type_;
    if (static_cast<uint32_t>(t - 6) >= 4) {
      if (t != 2 && t != 0) return 0;
      if ((sub_state_ | 2) == 2) sub_state_ = 3;
      else bB8_ = 1;
      return 0;
    }
    if (static_cast<uint32_t>(sub_state_ - 1) < 2) {
      bB8_ = 1;
      return 0;
    }
    if (sub_state_ != 0) return 0;
    f70_ = 0.0f;
    next_.reset();
    sub_state_ = 3;
    return 0;
  }
  if (flags & 4) {  // fallback
    if (sub_state_ != 0) return 0;
    if (static_cast<uint32_t>(type_ - 6) > 3) return 0;
    const uint32_t f10 = static_cast<uint32_t>(flags) & 0x10;
    fC0_ = f10 ? fC0_ : 2;
    sub_state_ = static_cast<int32_t>((f10 >> 3) ^ 2);
    f70_ = 0.0f;
    next_.reset();
    if (static_cast<uint32_t>(type_ - 7) < 2) {
      f80_ = duration_166(degree_);
      shader_ = 4;
      g_slide[degree_][5] = kSlideDirection[type_ == 7 ? 1 : 0];
    } else if (type_ == 9) {
      f80_ = duration_166(degree_);
      shader_ = 5;
    } else if (type_ == 6) {
      f80_ = duration_166(degree_);
      shader_ = 2;
    }
    return 0;
  }
  b199_ = (flags >> 7) & 1;
  f8C_ = p.capture_scale;
  img_id_ = flags & 1;
  fC4_ = fC0_;
  id_bits_ = static_cast<uint32_t>(flags) & 0xFF0000u;
  bB8_ = 0;
  fA0_ = kFallbackDelay[static_cast<int8_t>(flags & 0xFF) >= 0 ? 1 : 0];
  bA4_ = bA5_ = 0;
  bA6_ = (flags >> 3) & 1;
  bA7_ = (flags >> 5) & 1;
  b198_ = (flags >> 6) & 1;
  const int32_t t = p.type;
  if (t < 0) {
    type_ = -1;
    degree_ = 2;
    return 0;
  }
  type_ = t & 0xFF;
  if (t == 1) {
    f74_ = 0.0f;
    sub_state_ = 1;
    return 0;
  }
  f70_ = 0.0f;
  f80_ = 800.0f;
  const uint32_t deg = (static_cast<uint32_t>(t) >> 16) & 0xFF;
  f84_ = p.center_x;
  f88_ = p.center_y;
  degree_ = static_cast<int32_t>(deg < 3 ? deg : 3);
  switch (t & 0xFF) {
    case 0: case 2:
      next_ = black_;
      sub_state_ = 0;
      fC0_ = 1;
      f80_ = 1400.0f;
      break;
    case 1:
      f74_ = 0.0f;
      sub_state_ = 1;
      break;
    case 5:
      if (fC0_ == 2) {
        shader_ = 0;
        sub_state_ = 1;
        fC0_ = 2;
        f80_ = 1.0f;
        break;
      }
      shader_ = 2;
      next_.reset();
      sub_state_ = 1;
      fC0_ = 2;
      f80_ = duration_166(degree_);  // (type | 2) != 2
      break;
    case 10:
      type_ = 6;
      [[fallthrough]];
    case 6: case 7: case 8: case 9:
      if (degree_ == 0) type_ = 9;
      shader_ = 1;
      next_ = black_;
      sub_state_ = 0;
      fC0_ = 3;
      f80_ = ((type_ | 2) == 2) ? duration_366(degree_) : duration_166(degree_);
      break;
    default:
      break;
  }
  return 0;
}

void TransitionLayer::update(BackgroundLayerState& st) {  // 0xA53F0
  const float x0 = f6C_;
  float x1, x2;
  if (bB8_) {
    const float d = x0 + x0;
    f70_ = d + f70_;
    x1 = f70_;
    x2 = d + f74_;
  } else {
    f70_ = x0 + f70_;
    x1 = f70_;
    x2 = x0 + f74_;
  }
  f74_ = x2;
  st.opacity = 1.0f;
  {
    const float x3 = f9C_;
    if (1.0f > x3 && sub_state_ != 0) {
      const float x4 = x1 / f80_;
      // cmovb: x4 < x3 (or unordered) keeps +0x9C; vminss sel, 1.0 (1.0 unless sel < 1.0)
      const float sel = (x4 < x3 || std::isnan(x4) || std::isnan(x3)) ? f9C_ : x4;
      f9C_ = sel < 1.0f ? sel : 1.0f;
    }
  }
  // high-contrast (state +1) and visibility (+3) changes
  int32_t r8;
  {
    const uint8_t al = st.high_contrast, dl = bD8_;
    if (al && !dl) r8 = 0;
    else if (al && dl) r8 = 1;
    else if (!al && dl) r8 = 3;
    else r8 = 2;
    bD8_ = al;
  }
  int32_t r13;
  {
    const uint8_t al = st.bg_scene_visibility, dl = bD9_;
    if (al && !dl) r13 = 1;
    else if (al && dl) r13 = 2;
    else if (!al && dl) r13 = 3;
    else r13 = 0;
    bD9_ = al != 0;
  }
  const uint32_t esi = static_cast<uint32_t>(st.show_overlay_image_immediately_flag) | b198_;
  b198_ = esi != 0;

  uint32_t r15 = 0x20;
  bool r12 = false;
  auto end_unmaps = [&] {  // 0xA5932..0xA598A: unmap the slots of the image not shown
    const int32_t id = img_id_;
    release_slot(2);
    release_slot(4);
    if (id != 0) {
      release_slot(0);
      release_slot(5);
      if (id == 1) return;
    }
    release_slot(1);
    release_slot(6);
  };
  auto shader_for_type = [&]() {  // 0xA648E / 0xA5D10 / 0xA649A pattern
    if (type_ == 9) shader_ = 5;
    else if (type_ == 6) shader_ = 2;
    else {
      shader_ = 4;
      g_slide[degree_][5] = kSlideDirection[type_ == 7 ? 1 : 0];
    }
  };

  switch (static_cast<uint32_t>(type_)) {
    case 6: case 7: case 8: case 9: {
      if (sub_state_ == 2) {  // 0xA5892
        if (x1 < f80_ || std::isnan(x1) || std::isnan(f80_)) break;
        type_ = -1;
        cur_ = next_;
        next_ = black_;
        end_unmaps();
        shader_ = cur_ ? 1 : 0;
        r15 = 1;
        break;
      }
      if (sub_state_ == 1) {  // 0xA59A7
        if (x1 < f80_ || std::isnan(x1) || std::isnan(f80_)) break;
        cur_ = next_;
        next_ = black_;
        shader_ = cur_ ? 1 : 0;
        sub_state_ = 0;
        end_unmaps();
        r15 = bB8_ == 0 ? 0x20 : 1;
        r12 = true;
        break;
      }
      if (sub_state_ != 0) {  // 0xA5AD9
        next_.reset();
        release_slot(img_id_);
        release_slot(img_id_ + 5);
        shader_ = 1;
        bB8_ = 1;
        type_ = -1;
        r15 = 0x11;
        break;
      }
      XfTex img = slots_[img_id_ & 1];
      if (img) {
        if (bA7_ && x1 < 2000.0f) {  // retry: wait until 2000 ms
          r15 = 0x20;
          break;
        }
        // 0xA5E40
        sub_state_ = 2;
        f80_ = duration_166(degree_);
        f70_ = x0;
        next_ = img;
        if (type_ == 9) { shader_ = 5; r15 = 0; break; }
        if (type_ == 6) { shader_ = bA5_ ? 5 : bA4_ * 3 + 2; r15 = 0; break; }
        if (bA5_ || bA4_) { shader_ = 5; r15 = 0; break; }  // 0xA6221 -> 0xA6237
        shader_ = 4;  // 0xA64E5
        g_slide[degree_][5] = kSlideDirection[type_ == 7 ? 1 : 0];
        r15 = 0;
        break;
      }
      // 0xA5C63: no image yet
      const uint8_t cl = bA4_;
      if (b199_) {
        r15 = 0x20;
        if (cl) break;
        if (x1 < fA0_) break;
        sub_state_ = 1;
        f80_ = 500.0f;
        f70_ = x0;
        next_ = black_;
        shader_for_type();
        fA0_ = 3000.0f;
        bA4_ = 1;
        break;
      }
      // 0xA61D8
      const uint8_t sil = bA5_;
      bool to_6329 = false;
      if (!cl) {
        if (!sil) to_6329 = true;
        else { r15 = 0x20; break; }
      } else {
        if (sil) { r15 = 0x20; break; }
        if (x1 < fA0_ || bA7_) to_6329 = true;
        else {
          fA0_ = kFltMax;
          f70_ = x0;
          bA5_ = 1;
          r15 = 0x40;  // RequestFallbackImage
          break;
        }
      }
      if (to_6329) {  // 0xA6329
        r15 = 0x20;
        if (bA7_) break;
        XfTex blur = slots_[2 + (img_id_ & 1)];
        if (blur) {
          sub_state_ = 1;
          f80_ = duration_166(degree_);
          f70_ = x0;
          next_ = blur;
          shader_for_type();
          bA5_ = 1;  // 0xA660E
          fA0_ = 3000.0f;
          r15 = 0;
          break;
        }
        // 0xA6522
        if (cl) break;
        if (x1 < fA0_) break;
        sub_state_ = 1;
        f80_ = duration_166(degree_);
        f70_ = x0;
        next_ = black_;
        shader_for_type();
        fA0_ = 3000.0f;  // 0xA6668
        bA4_ = 1;
        r15 = 0;
      }
      break;
    }
    case 0: {  // 0xA55B8 (LaunchingGame)
      r15 = 0x30;
      int32_t s = sub_state_;
      if (s == 0) {  // 0xA55E1
        XfTex img = slots_[img_id_ & 1];
        if (!img) { r15 = 0x20; break; }
        shader_ = degree_ == 0 ? 5 : 2;
        sub_state_ = 1;
        f80_ = duration_366(degree_);
        f70_ = x0;
        next_ = img;
        overlay_ = transparent_;
        r15 = 0x28;  // Cancelable | RequestNextOverlayImage
        break;
      }
      if (s == 1) {  // 0xA5F2B
        if (esi != 0 && slots_[5] && overlay_ != slots_[5]) {
          overlay_ = slots_[5];
          x1 = f70_;
        }
        if (x1 < f80_ || std::isnan(x1) || std::isnan(f80_)) { r15 = 0x20; break; }
        shader_ = 3;
        sub_state_ = bB8_ ? 3 : 2;
        f80_ = 12000.0f;
        f70_ = f6C_;
        cur_ = next_;
        next_ = black_;
        overlay_ = black_;
        f90_ = 1.0f;
        f94_ = b198_ ? 1.0f : 0.0f;
        f98_ = -1.0f;
        release_slot(img_id_ ^ 1);
        r12 = true;
        s = 2;  // falls into case 2
      }
      if (s == 2) {  // 0xA60F6
        if (r13 == 2) f70_ = f70_ - f6C_;
        float x = f98_;
        if (0.0f > x && slots_[5]) {
          next_ = slots_[5];
          f90_ = 1.0f;
          f98_ = f70_;
          x = f98_;
        }
        if (!(x < 0.0f) && 1.0f > f94_) {
          float u = (f70_ - x) * 0.000454545458f;  // dword_B0005C
          u = std::fmin(u, 1.0f);
          const bool hi = !(u < 0.5f);
          float e = u + u;
          e = hi ? 2.0f - (2.0f - e) * (2.0f - e) : e * e;
          f94_ = e * 0.5f;
        }
        const float t = f70_, d = f80_;
        if (!(t < d)) {  // 0xA642E
          shader_ = 3;
          fC0_ = 1;
          type_ = -1;
          r15 = 0x21;
          break;
        }
        float u = t / d;
        const bool hi = !(u < 0.5f);
        float e = u + u;
        e = hi ? 2.0f - (2.0f - e) * (2.0f - e) : e * e;
        f90_ = 1.0f / (e * 0.05f + 1.0f);
        r15 = 0x20;
        break;
      }
      if (s == 3) break;  // r15 = 0x30
      // default 0xA5EE9
      log::warn("bglayer", "BGLayer/Trans: Unknown state");
      fC0_ = 1;
      type_ = -1;
      shader_ = 0;
      r15 = 1;
      break;
    }
    case 1: {  // 0xA56D4 (Hide)
      if (!(x2 < 300.0f)) {  // 0xA5B58
        fC0_ = 0;
        type_ = -1;
        shader_ = 0;
        cur_ = black_;
        next_ = black_;
        f9C_ = 0.0f;
        release_slot(2); release_slot(4); release_slot(0); release_slot(5); release_slot(1); release_slot(6);
        r15 = 1;
        break;
      }
      float o = std::fmax(x2 * 0.00333333341f, 0.0f);
      o = std::fmin(o, 1.0f);
      f9C_ = 1.0f - o;
      st.opacity = f9C_;
      r15 = 0x20;
      break;
    }
    case 2: {  // 0xA5703 (LaunchingGameBC)
      if (sub_state_ == 1) {  // 0xA5D1F
        if (x1 < f80_ || std::isnan(x1) || std::isnan(f80_)) { r15 = 0x20; break; }
        type_ = -1;
        cur_ = next_;
        next_ = black_;
        end_unmaps();
        shader_ = 1;
        r15 = 1;
        break;
      }
      if (sub_state_ != 0) { r15 = 0x30; break; }
      r15 = 0x20;
      XfTex img = slots_[img_id_ & 1];
      if (!img) break;
      sub_state_ = 1;
      f80_ = duration_366(degree_);
      f70_ = x0;
      next_ = img;
      shader_ = degree_ == 0 ? 5 : 2;
      break;
    }
    case 5: {  // 0xA57EF (SystemDefault)
      if (x1 < f80_ || std::isnan(x1) || std::isnan(f80_)) { r15 = 0x20; break; }
      type_ = -1;
      shader_ = 0;
      cur_.reset();
      next_ = black_;
      release_slot(2); release_slot(4); release_slot(0); release_slot(5); release_slot(1); release_slot(6);
      r15 = 1;
      break;
    }
    default: {  // 0xA57C7
      const float o = fC0_ != 0 ? 1.0f : 0.0f;
      f9C_ = o;
      st.opacity = o;
      r15 = 0x20;
      break;
    }
  }
  // 0xA6258 epilogue
  st.transition_type = type_;
  BasematArgs a;
  a.dt = f6C_;
  a.flag = static_cast<uint8_t>((r15 | (r12 ? 1u : 0u)) & 1u);
  a.hc = r8;
  a.vis = r13;
  a.type = type_;
  a.shader = shader_;
  a.sub_state = sub_state_;
  basemat_update(a);  // 0xA82A0
  const uint32_t fl = (basemat_.time >= basemat_.duration) ? r15 : r15 + 0x80;  // BasematAnimationInProgress
  st.transition_flags = (static_cast<uint32_t>(sub_state_) << 24) | (fl | id_bits_);
  if (type_ == -1) id_bits_ = 0;
}

// ---------------------------------------------------------------------------------------------
bool TransitionLayer::prerender(ssn::gfx::Image*& target, bool& alone) {  // 0xA4720
  alone = false;
  if (0.0001f > opacity || shader_ == 0) return false;
  if (cur_ && next_) {
    alone = true;
    return false;
  }
  if (!capture_ || capture_->width != target->width || capture_->height != target->height)
    capture_ = make_texture(0, target->width, target->height, true);  // sub_E1850(w, h, 18)
  snapshot_ = capture_;  // +0x188 = +0x190
  target = capture_->image();
  return true;
}

ssn::gfx::Image* TransitionLayer::capture(ssn::gfx::Image& target) {  // 0xA48D0
  if (snapshot_) return snapshot_->image();
  if (cur_ && next_) return black_->image();
  // Not reached from Render 0x70DD0 (the hook 0xA4720 always leaves +0x188 set when one of the two
  // images is missing); the firmware would copy the target with basic_p here.
  if (!capture_ || capture_->width != target.width || capture_->height != target.height)
    capture_ = make_texture(0, target.width, target.height, true);
  const float one = 1.0f;
  draw("basic_p", *capture_->image(), &target, nullptr, 8, &one, 4, nullptr, 0);
  return capture_->image();
}

void TransitionLayer::render(ssn::gfx::Image& target) {  // 0xA4D70
  if (shader_ == 0) return;
  auto& dev = ssn::gfx::device();
  std::lock_guard<std::recursive_mutex> record(dev.record_lock());
  ssn::gfx::Image* prev_color = dev.target_color();
  ssn::gfx::Image* prev_depth = dev.target_depth();
  const float op = opacity;  // var_AC = +0x58
  ssn::gfx::Image* old = capture(target);
  float t = f70_ / f80_;
  t = t < 1.0f ? std::fmax(t, 0.0f) : 1.0f;  // vmaxss / vcmpltss / vblendvps
  auto ease_out = [](float v) { const float s = 1.0f - v; return 1.0f - s * s; };
  float c0[10] = {};
  size_t c0_size = 8;
  std::string ps;
  uint32_t ntex = 2;
  uint8_t flags2[2] = {0, 0};
  bool dl = false;
  auto tex_of = [&](const XfTex& s) -> ssn::gfx::Image* {
    if (s) {
      ssn::gfx::Image* i = s->image();
      if (i) return i;
    }
    return old;
  };
  const float bprog = 1.0f - [&] { const float v = 1.0f - std::fmin(basemat_.time / basemat_.duration, 1.0f); return v * v; }();
  switch (shader_) {
    case 2: {  // ripple (or ripple_with_logo when +0x198)
      if (b198_) {  // +0x108: ripple_with_logo_p (#47) with the overlay texture t2
        host::note_unimplemented("bglayer: Transition ripple_with_logo_p (shader #47, LaunchingGame with an overlay logo) not transcribed: that draw is skipped");
        dev.bind_target(prev_color, prev_depth);
        return;
      }
      float* e = g_ripple[degree_];
      std::memcpy(&e[0], &f84_, 4);
      std::memcpy(&e[1], &f88_, 4);
      e[2] = op;
      const float s = (1.0f - t) * (1.0f - t);
      const float s2 = s * s;
      e[3] = s * (s2 * -0.649999976f + -0.200000003f) + (s2 * s2 * -0.150000006f + 1.0f);  // 0xA4F03..
      e[4] = static_cast<float>(std::pow(static_cast<double>(e[3]), 2.2));             // qword_B8E1B0
      std::memcpy(c0, e, 40);
      c0_size = 40;
      ps = "ripple_p";
      ntex = b198_ ? 3 : 2;
      break;
    }
    case 3:  // game splash
      c0[0] = op;
      c0[1] = f94_;
      c0[2] = f90_;
      c0[3] = bprog;
      c0_size = 16;
      ps = "game_splash_p";
      dl = true;
      break;
    case 4: {  // slide in
      float* e = g_slide[degree_];
      e[0] = op;
      e[1] = ease_out(t);
      std::memcpy(c0, e, 24);
      c0_size = 24;
      ps = "slide_in_p";
      break;
    }
    case 5:  // cross fade
      c0[0] = op;
      c0[1] = ease_out(t);
      ps = "cross_fade_p";
      break;
    default:  // single
      c0[0] = op;
      c0[1] = bprog;
      ps = "single_p";
      dl = true;
      break;
  }
  if (dl) {  // 0xA527A
    flags2[0] = cur_ && cur_ != black_;
    flags2[1] = flags2[0];
  } else {  // 0xA5180
    flags2[0] = cur_ && cur_ != black_;
    flags2[1] = next_ && next_ != black_;
  }
  // 0xA8670: the basemat constant buffer c1 (160 bytes, two GradationParameter).
  float c1[40] = {};
  {
    const Basemat& b = basemat_;
    const Gradation& to = dl ? (b.args.sub_state == 0 ? b.from : b.to) : b.to;
    const Gradation& from = b.from;
    const float* color = b.color;  // byte_EC1476 == 0 (the 0xB8C390 debug colour is not selected)
    auto put = [&](float* o, const Gradation& g, bool enable) {
      std::memcpy(o + 0, color, 16);
      std::memcpy(o + 4, &g.f[9], 16);
      std::memcpy(o + 8, &g.f[13], 16);
      o[12] = g.f[0] * 0.000520833360f;  // dword_AFFA34
      o[13] = g.f[1] * 0.000925925910f;  // dword_AFF8E0
      o[14] = enable ? 1.0f : 0.0f;
      o[15] = g.f[3] * 0.000520833360f;
      o[16] = g.f[4] * 0.000925925910f;
      o[17] = static_cast<float>(static_cast<double>(g.f[2]) * 0.017453292519943295);  // qword_B8E5A8
      o[18] = g.f[7];
      o[19] = g.f[8];
    };
    put(c1, from, flags2[0] != 0);
    put(c1 + 20, to, flags2[dl ? 0 : 1] != 0);
  }
  ssn::gfx::Image* t0 = tex_of(cur_);
  ssn::gfx::Image* t1 = tex_of(next_);
  if (ntex == 3) (void)tex_of(overlay_);
  if (!t0 || !t1) {
    dev.bind_target(prev_color, prev_depth);
    return;
  }
  draw(ps, target, t0, ps == "single_p" ? nullptr : t1, 8, c0, c0_size, c1, sizeof c1);
  dev.bind_target(prev_color, prev_depth);
}

}  // namespace ssn::eboot::bglayer
