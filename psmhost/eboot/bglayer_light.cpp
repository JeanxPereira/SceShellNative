// The BackgroundLayer "Light" layer (ShellUI eboot, FW 4.03): ctor 0x9DE50, vtable 0xCC8FB0
// {+16 Initialize 0x9DFB0, +64 Update 0x9E760, +72 Render 0x9E7E0, +80 IsVisible 0x75C20},
// SetColor 0x9E340, colour animation 0x9E3F0.
//
// One full-screen pass of light_p (launcher/shaders/bglayer/bglayer_light_p.frag) with the
// eboot's basic_vv: the floor lit by two spot lights through an IES profile (texFloor), the
// light shaft (texVolume) and a point light, gamma-corrected and dithered. When the particle
// layer renders it hands its target to this pass (texP, particleAlpha; sub_9EB90), which
// composites the particles with a slight chromatic offset.
#include "eboot/bglayer_light.h"

#include <windows.h>

#include <cstring>
#include <string>

#include "core/log.h"
#include "eboot/bglayer.h"
#include "eboot/bglayer_gpu.h"
#include "host/boundary.h"

namespace ssn::eboot::bglayer {
namespace {

constexpr uint64_t kIesFloorVa = 0xBC6180;    // GNF, 16640 bytes: 128x128 8_UNORM, SW_4KB_S
constexpr uint64_t kIesVolumeVa = 0xBC2080;   // GNF, 16640 bytes
constexpr uint64_t kColourTableVa = 0xBC1EC0; // 4 x 112 bytes (ColorCb layout + pad)
constexpr size_t kIesSize = 16640;

PassLayout g_layout;
VkPipeline g_pipeline[2] = {VK_NULL_HANDLE, VK_NULL_HANDLE};  // [blend disabled, blend enabled]
VkFormat g_pipeline_format[2] = {VK_FORMAT_UNDEFINED, VK_FORMAT_UNDEFINED};
PFN_vkCmdPushDescriptorSetKHR g_push = nullptr;

// s_memrealtime substitute (see the shader header): host performance counter scaled to 100 MHz.
uint32_t realtime_counter() {
  LARGE_INTEGER c, f;
  QueryPerformanceCounter(&c);
  QueryPerformanceFrequency(&f);
  return static_cast<uint32_t>(static_cast<unsigned long long>(c.QuadPart) * 100000000ull / static_cast<unsigned long long>(f.QuadPart));
}

}  // namespace

bool LightLayer::initialize() {
  const auto table = eboot_bytes(kColourTableVa, sizeof table_);
  const auto floor = eboot_bytes(kIesFloorVa, kIesSize);
  const auto volume = eboot_bytes(kIesVolumeVa, kIesSize);
  if (table.size() != sizeof table_ || floor.size() != kIesSize || volume.size() != kIesSize) {
    log::error("bglayer", "Light: eboot data (colour table / IES textures) not readable");
    return false;
  }
  std::memcpy(table_, table.data(), sizeof table_);
  std::string err;
  // createIesTex (sub_86E120 on the embedded GNF images): +120 texFloor, +128 texVolume.
  if (!load_gnf(floor.data(), floor.size(), tex_floor_, err) || !load_gnf(volume.data(), volume.size(), tex_volume_, err)) {
    log::error("bglayer", "Light: IES texture: {}", err);
    return false;
  }
  // 0x9DFB0 tail: +24 = 0 (no auto-hide), state 4, colour set 0 copied from ymmword 0xBC1EC0,
  // +224 (animation duration) = -1, +213 (ColorCb dirty) = 1.
  state = 4;
  colour_index_ = 0;
  std::memcpy(colours_, table_[0], sizeof colours_);
  anim_duration_ = -1.0f;
  colours_dirty_ = true;
  ready_ = true;
  host::note_unimplemented("HOST VALUE bglayer light_p s_memrealtime dither seed = host performance counter at 100 MHz, one value per draw");
  return true;
}

void LightLayer::select_colours(uint32_t visibility_dimmed, bool high_contrast, bool animate) {
  const int32_t index = static_cast<int32_t>(high_contrast ? visibility_dimmed + 2 : visibility_dimmed);  // 0x9E340
  if (colour_index_ == index) return;
  colour_index_ = index;
  // Animated when requested and intensity (+0xC8), its scale (+0xCC) and opacity are all >= 0.01.
  if (animate && !(0.01f > intensity_source) && !(0.01f > intensity_scale) && !(0.01f > opacity)) {
    anim_elapsed_ = 0.0f;    // +0xDC
    anim_duration_ = 300.0f; // +0xE0 (qword 0x4396000000000000)
    return;
  }
  std::memcpy(colours_, table_[index], sizeof colours_);
  anim_duration_ = -1.0f;
  colours_dirty_ = true;
}

void LightLayer::animate_colours(float frame_ms) {  // 0x9E3F0
  const float dur = anim_duration_;
  if (!(0.0f < dur)) return;
  const float prev = anim_elapsed_;
  const float now = prev + frame_ms;
  anim_elapsed_ = now;
  const float* target = table_[colour_index_];
  bool snap = !(now < dur + -1.0f);
  float k = 0.0f;
  if (!snap) {
    float q = 1.0f - prev / dur;
    q = q * q;
    q = q * q;                        // (1 - prev/dur)^4
    const float e_prev = 1.0f - q;
    if (e_prev > 0.9999f) {
      snap = true;
    } else {
      float n = 1.0f - now / dur;
      n = n * n;
      n = n * n;
      k = (1.0f - (n + e_prev)) / q;  // (e_now - e_prev) / (1 - e_prev)
    }
  }
  if (snap) {
    std::memcpy(colours_, target, sizeof colours_);
    anim_duration_ = -1.0f;
  } else {
    for (int i = 0; i < 27; ++i) colours_[i] = colours_[i] + k * (target[i] - colours_[i]);
  }
  colours_dirty_ = true;
}

void LightLayer::update(float frame_ms) {  // 0x9E760
  const float t = frame_ms * 0.001f + time_;
  // Past 3600 s the clock restarts, but only while the layer is invisible (opacity < 0.0001).
  time_ = (3600.0f < t) ? (opacity < 0.0001f ? 0.0f : t) : t;
  animate_colours(frame_ms);
}

void LightLayer::set_particle_texture(ssn::gfx::Image* tex, float alpha) {  // 0x9EB90
  particle_tex_ = nullptr;  // the previous texture is released (sub_872270)
  if (state == 4 && tex) {
    particle_alpha = alpha;  // +0xD0
    particle_tex_ = tex;     // +168
  }
}

void LightLayer::render(ssn::gfx::Image& target, bool blend) {  // 0x9E7E0
  auto& dev = ssn::gfx::device();
  std::lock_guard<std::recursive_mutex> record(dev.record_lock());
  VkDevice d = dev.device();
  if (!g_layout.layout) {
    g_layout = make_pass_layout(2, 3, 4);
    g_push = reinterpret_cast<PFN_vkCmdPushDescriptorSetKHR>(vkGetDeviceProcAddr(d, "vkCmdPushDescriptorSetKHR"));
  }
  const int pb = blend ? 1 : 0;
  if (!g_pipeline[pb] || g_pipeline_format[pb] != target.format) {
    // light+104 = XfGraphics default vertex shader (basic_vv), light+112 = light_p; primitive type 6.
    g_pipeline[pb] = make_fullscreen_pipeline(g_layout, shader_module("bglayer_basic_vv"), shader_module("bglayer_light_p"),
                                              VK_PRIMITIVE_TOPOLOGY_TRIANGLE_STRIP, target.format, blend);
    g_pipeline_format[pb] = target.format;
  }
  if (!g_pipeline[pb]) return;

  // VolatileCB (+144, 16 bytes) rewritten every draw.
  float x = intensity_source;
  float e = (0.5f > x) ? (x + x) * (x + x) : 2.0f - (2.0f - (x + x)) * (2.0f - (x + x));
  // particleAlpha = +0xD0 while a particle texture (+168) is set, else 0 (0x9E959).
  const float volatile_cb[4] = {time_, opacity, e * 0.5f * intensity_scale, particle_tex_ ? particle_alpha : 0.0f};
  // ColorCb (+152, 112 bytes) is rewritten when dirty (+213); the host uploads its copy every draw.
  colours_dirty_ = false;

  ssn::gfx::Image* prev_color = dev.target_color();
  ssn::gfx::Image* prev_depth = dev.target_depth();
  if (particle_tex_) {  // the particle target was just rendered: make it readable
    dev.end_rendering();
    dev.transition(dev.cmd(), *particle_tex_, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
  }
  dev.bind_target(&target, nullptr);
  dev.ensure_rendering();
  VkCommandBuffer cb = dev.cmd();

  ssn::gfx::Slice s0 = dev.alloc(256, 256);
  std::memcpy(s0.data, volatile_cb, sizeof volatile_cb);
  ssn::gfx::Slice s1 = dev.alloc(256, 256);
  std::memset(s1.data, 0, 112);
  std::memcpy(s1.data, colours_, sizeof colours_);

  VkDescriptorBufferInfo bi[2] = {{s0.buffer, s0.offset, 16}, {s1.buffer, s1.offset, 112}};
  // Samplers: the S# immediates of light_p (0xE0..0x100 and 0x734/0x73C).
  VkDescriptorImageInfo ii[3] = {
      {sampler_for_ssharp(0x1B6, 0x00FFF000, 0x05500000, 0), tex_floor_.view, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL},
      {sampler_for_ssharp(0x30, 0x00FFF000, 0x05500000, 0), tex_volume_.view, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL},
      // t2 = +168 (particle target) or, without one, +120 (texFloor) — 0x9EA44.
      {sampler_for_ssharp(0x92, 0, 0x02500000, 0), particle_tex_ ? particle_tex_->view : tex_floor_.view,
       VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL},
  };
  VkWriteDescriptorSet w[5]{};
  for (uint32_t i = 0; i < 5; ++i) {
    w[i].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
    w[i].dstBinding = i;
    w[i].descriptorCount = 1;
    if (i < 2) {
      w[i].descriptorType = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER;
      w[i].pBufferInfo = &bi[i];
    } else {
      w[i].descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
      w[i].pImageInfo = &ii[i - 2];
    }
  }
  vkCmdBindPipeline(cb, VK_PIPELINE_BIND_POINT_GRAPHICS, g_pipeline[pb]);
  g_push(cb, VK_PIPELINE_BIND_POINT_GRAPHICS, g_layout.layout, 0, 5, w);
  const uint32_t rt = realtime_counter();
  vkCmdPushConstants(cb, g_layout.layout, VK_SHADER_STAGE_FRAGMENT_BIT, 0, 4, &rt);
  // Viewport = the BackgroundLayer viewport (sub_DF0E0 with the display rectangle); PS5 clip
  // space +Y is the top of the target, hence the flipped Vulkan viewport.
  VkViewport vp{0.0f, float(target.height), float(target.width), -float(target.height), 0.0f, 1.0f};
  VkRect2D sc{{0, 0}, {target.width, target.height}};
  vkCmdSetViewport(cb, 0, 1, &vp);
  vkCmdSetScissor(cb, 0, 1, &sc);
  vkCmdDraw(cb, 3, 1, 0, 0);  // sub_86B740(ctx, 3)

  dev.bind_target(prev_color, prev_depth);
  particle_tex_ = nullptr;  // 0x9EACE: +168 released after the draw
}

}  // namespace ssn::eboot::bglayer
