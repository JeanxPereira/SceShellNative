// "ShellUIBackgroundLayer" P/Invokes (Sce.Vsh.ShellUI.BGLayer.BGLayerNative) — ShellUI eboot
// (NPXS40087, FW 4.03) {name, fn} table at 0xCC88F0. See bglayer.h for the object model.
//
// Reproduced:
//   InitializeNative 0x8DB30 / FinalizeNative 0x8DBF0 / UpdateNative 0x8DCC0 /
//   UpdateDrawSomethingNative 0x8E780, with the parts of BackgroundLayer Update (0x72540),
//   the theme selection (0x72E60) and Render (0x70DD0) that drive the Light layer and the
//   LightParticle layer (bglayer_particle.cpp: effect timelines, particle_c simulation,
//   particle_vv/p draws, composited by light_p), the brightness and cross-fade of the extra
//   layer (+8 / +0x10 / +0x18 / +0x44, +0x1D0) and the Transition layer (bglayer_transition.cpp:
//   background images, their transitions and the basemat; MapTextureNative 0x8E540,
//   GetUnmapTextureRequestNative 0x8E5B0, SetBGTransitionNative 0x8E5D0, SetBGBasematNative 0x8E600).
// The boot animations' large particles (modes 3 / 4) are part of the LightParticle layer.
// Not reproduced (each logs through host::note_unimplemented when it would act):
//   the wave layer (themes 0x00..0x3F; unreachable in FW 4.03, see README), exclusive clear,
//   VR / compositor / scan-in paths.
// Every other entry of the table stays a logging stub (Prep).
#include "eboot/bglayer.h"
#include "eboot/bglayer_bgm.h"

#include <cmath>
#include <cstring>
#include <memory>
#include <mutex>

#include "core/log.h"
#include "eboot/bglayer_light.h"
#include "eboot/bglayer_particle.h"
#include "eboot/bglayer_transition.h"
#include "gfx/vk_device.h"
#include "libScePsm/graphics/objects.h"
#include "host/boundary.h"
#include "libScePsm/graphics/core.h"

namespace ssn::eboot::bglayer {
namespace {

// The preset table Initialize (0x6FF90) copies to +0xA0..+0x14F: ymmword 0xB8D2A0, 0xB8D3E0,
// 0xB8D400, 0xB8DC20, 0xB8D400 and xmmword 0xB8B670.
constexpr uint64_t kPresetTableVa[6] = {0xB8D2A0, 0xB8D3E0, 0xB8D400, 0xB8DC20, 0xB8D400, 0xB8B670};

struct BackgroundLayer {
  bool initialized = false;      // +0
  float opacity = 0.0f;          // +4
  float dim = 0.0f;              // +8: brightness of the extra layer, toward +0x10
  float extra_opacity = 0.0f;    // +0x0C: opacity handed to the extra layer (+0x1D0)
  float dim_target = 0.0f;       // +0x10
  float dim_target_prev = 0.0f;  // +0x14
  float xfade = 1.0f;            // +0x18 (ctor qword +24 = 0x439600003F800000)
  float xfade_ms = 300.0f;       // +0x1C
  float frame_ms = 16.7f;        // +0x28 (ctor 0x4185999A)
  float real_frame_ms = 16.7f;   // +0x2C
  int32_t skip_count = 0;        // +52
  float intensity_rate = 0.0f;   // +56 (+0x38)
  float global_fade = 0.0f;      // +60 (+0x3C)
  float intensity = 0.0f;        // +64 (+0x40)
  float preset_ramp = 0.0f;      // +0x44
  int32_t preset_prev = 0;       // +0x98
  int32_t preset = 0;            // +0x9C
  int32_t preset_table[44]{};    // +0xA0..+0x14F
  uint8_t no_ui_widget = 0;      // +32
  uint8_t no_draw = 0;           // +33
  uint8_t skip_render = 0;       // +34
  int32_t flags = 0;             // +472
  bool background_is_light = true;  // +456 == +416 (set in Initialize 0x6FF90)
  bool extra_is_particle = false;   // +0x1D0 == +424 (the only extra layer of the Light themes)
  LightLayer light;
  std::unique_ptr<ParticleLayer> particle;  // +424
  TransitionLayer trans;                    // +448 (0xA38D0)
};

std::mutex g_lock;
std::unique_ptr<BackgroundLayer> g_bl;  // qword_EC0080 -> +8
uint64_t g_update_count = 0;            // qword_EBFDF0

void note_once(const char* what) { host::note_unimplemented(std::string("bglayer: ") + what); }

struct ThemeResult {
  int32_t preset_value;   // *a5
  bool particle;          // *a6 == +424
};

// Theme selection (0x72E60). Light themes: preset colours {1,2,4,5,7} (bit mask 182) with theme
// group 4 (ThemeColourIndex 0x40..0x4F) show the Light layer as background and, for the cases
// with a particle mode, the LightParticle layer as the extra layer (*a6).
ThemeResult select_theme(BackgroundLayer& bl, uint32_t theme, uint32_t preset, bool high_contrast) {
  const uint32_t v8 = preset <= 0x1D ? preset : 11;
  bool fade_out = false;
  bool particle = false;
  int32_t table_index = static_cast<int32_t>(v8);  // v23 = +0xA0 + 4 * v8 unless a theme picks another
  bl.xfade_ms = 300.0f;  // +28 = 1134313472
  const bool masked = (182u >> v8) & 1;
  const bool wave_background = masked ? (theme >> 4) <= 3 : ((0x3FC00000u >> v8) & 1) != 0;
  if (masked && (theme >> 4) == 4) {
    if (!bl.background_is_light) bl.light.set_opacity(bl.opacity);  // +456 switches back to the light
    bl.background_is_light = true;
    int32_t mode = 0;
    switch (theme & 0xF) {
      case 0x0: case 0x1: bl.intensity_rate = 1.0f; break;
      case 0x2: bl.intensity_rate = 0.2f; bl.xfade_ms = 666.666626f; break;  // +28 = 0x4426AAAA
      case 0x4: bl.intensity_rate = 1.0f / 3.0f; mode = 2; break;
      case 0x5: bl.intensity_rate = 1.0f; mode = 3; break;
      case 0x6: bl.intensity_rate = 1.0f; mode = 4; break;
      case 0x7: bl.intensity_rate = 1.0f / 3.0f; mode = 6; break;
      case 0xE: bl.intensity_rate = -1.0f; break;
      case 0xF: bl.intensity_rate = -1.0f; fade_out = true; break;
      default: bl.intensity_rate = 1.0f; mode = 1; break;
    }
    if (mode != 0 && bl.particle) {  // LABEL_42: *a6 = +424, SetMode (0x97560)
      particle = true;
      bl.particle->set_mode(mode);
    }
    if (!particle) bl.light.intensity_scale = 1.0f;  // *a6 == 0 -> light+204 = 1.0
  } else if (wave_background) {
    // Wave themes: the background (+456) is the wave layer; the light gets opacity 0. The
    // groups 0..2 also pick their preset value from +0x118.. (v20 / v20 + 12 / v20 + 8).
    if (bl.background_is_light) bl.light.set_opacity(0.0f);
    bl.background_is_light = false;
    if (masked) {
      const uint32_t g = theme >> 4, k = theme & 0xF;
      if (g == 0 && k != 0 && k < 8) table_index = 30 + static_cast<int32_t>(k);
      if (g == 1) table_index = 30 + static_cast<int32_t>(k <= 3 ? k + 12 : 12);
      if (g == 2) table_index = 30 + static_cast<int32_t>(k <= 3 ? k + 8 : 8);
    }
    note_once("wave-layer themes (ThemeColourIndex groups 0..3, presets 22..29) not rendered");
  }
  // *a5 = table value (+26 with high contrast).
  const int32_t value = bl.preset_table[table_index] + (high_contrast ? 26 : 0);
  // Global fade (+60): down by 0.0003/ms on case 0xF, otherwise up by 0.02/ms to 1.
  if (fade_out) {
    if (bl.global_fade > 0.0f) {
      bl.global_fade = bl.global_fade + bl.frame_ms * -0.0003f;
      if (!(bl.global_fade > 0.0f)) bl.global_fade = 0.0f;
    }
  } else if (bl.global_fade < 1.0f) {
    bl.global_fade = bl.global_fade + bl.frame_ms * 0.02f;
    if (!(bl.global_fade < 1.0f)) bl.global_fade = 1.0f;
  }
  return {value, particle};
}

// +0x9C / +0x44 / +0x10 (0x72974..0x72AA1): the extra layer's brightness target follows the
// preset value: 1 when (value mod 26) >= 2, else 0, ramped at 0.0005/ms (0.0016/ms for values
// >= 10) from the previous target.
void preset_brightness(BackgroundLayer& bl, int32_t value, float prev_opacity) {
  float r;
  int32_t ecx = bl.preset;
  if (ecx == value) {
    r = bl.preset_ramp;
    bl.preset = value;
    if (!(1.0f >= r)) return;  // jnb 0x729D1, else 0x72AA8
  } else {
    bl.preset_prev = ecx;
    bl.dim_target_prev = bl.dim_target;
    bl.preset = value;
    r = 0.0f;
  }
  // 0x729D1
  if (prev_opacity < 0.0001f || std::isnan(prev_opacity) || 0.0001f > bl.opacity) {  // 0x729F1
    ecx = 1;
    if (bl.preset_prev != 1) {
      r = 1.0f;
      ecx = bl.preset_prev;
      bl.preset_ramp = 1.0f;
    }
  } else {
    ecx = bl.preset_prev;  // 0x72A15
  }
  // 0x72A1D
  const uint32_t ue = static_cast<uint32_t>(value), uc = static_cast<uint32_t>(ecx);
  const uint32_t e_new = ue <= 26 ? ue : ue - 26;
  const uint32_t e_old = uc <= 26 ? uc : uc - 26;
  const float target = e_new >= 2 ? 1.0f : 0.0f;
  const float r_old = (e_old >= 10 ? 0.0016f : 0.0005f) * bl.frame_ms;  // dword_AFFE70 / dword_B0003C
  const float r_new = (e_new >= 10 ? 0.0016f : 0.0005f) * bl.frame_ms;
  r = r + std::fmax(r_new, r_old);  // vmaxss xmm3, xmm3(new), xmm6(old)
  bl.preset_ramp = r;
  bl.dim_target = !(r < 1.0f) ? target : bl.dim_target_prev + (target - bl.dim_target_prev) * r;
}

// The extra layer (+0x1D0) cross-fade and its opacity (0x72AD0..0x72C35).
void extra_layer(BackgroundLayer& bl, bool new_particle) {
  ParticleLayer* const cand = new_particle && bl.particle ? bl.particle.get() : nullptr;
  ParticleLayer* cur = bl.extra_is_particle ? bl.particle.get() : nullptr;  // r15
  ParticleLayer* nw = cand;                                                  // var_40
  float x0 = bl.extra_opacity;
  float x1;
  if (0.99f > bl.intensity && cur == nullptr) {  // 0x72B6A
    bl.xfade = 1.0f;
    x1 = 1.0f;
    cur = nullptr;
  } else if (0.0001f > x0) {
    bl.xfade = 1.0f;
    x1 = 1.0f;
  } else {
    x1 = bl.xfade;
    if (nw == cur) {  // 0x72B87
      if (!(x1 >= 1.0f)) {
        x1 = x1 + bl.frame_ms / bl.xfade_ms;
        bl.xfade = x1;
        if (!(x1 < 1.0f)) {
          x1 = 1.0f;
          bl.xfade = 1.0f;
        }
      }
      cur = nw;
    } else if (x1 > 0.0f) {
      x1 = x1 - bl.frame_ms / bl.xfade_ms;
      bl.xfade = x1;
      if (x1 > 0.0f) {
        nw = cur;  // the old layer keeps fading out
      } else {
        bl.xfade = 0.0f;
        x1 = 0.0f;
      }
    }
  }
  bl.extra_opacity = x1 * x0;  // 0x72BD7
  bl.extra_is_particle = nw != nullptr;
  if (cur != nw) {
    if (cur) {
      cur->set_opacity(0.0f);  // sub_DD560(old, 0)
      cur->hide();             // sub_DD6B0
    }
    if (!nw) return;
    nw->show();  // sub_DD640
    cur = nw;
  }
  if (cur) cur->set_opacity(bl.extra_opacity);
}

void update(BackgroundLayer& bl, BackgroundLayerState& st) {  // 0x72540
  ++g_update_count;
  bl.flags = static_cast<int32_t>(st.flags);  // +472 = state +100
  bl.frame_ms = st.frame_milliseconds;
  bl.real_frame_ms = st.frame_real_milliseconds;
  const int32_t level = st.rendering_mode == 2 ? 2 : (st.rendering_mode == 1 ? 1 : 0);  // r15 (0x725A1)
  bl.trans.update(st);  // sub_A53F0(+448, state)
  // +8 (0x72600): toward +0x10 by 0.01 per update, or x0.98 once the import kiA9bZhbnAg(3)
  // reports >= 600000001 after 1801 updates. HOST VALUE: that import (module B2 / library Bj,
  // not identified) reads 0, so the decay branch is never taken.
  {
    const uint64_t idle = 0;
    if (idle < 0x23C34601ull || g_update_count < 0x709) bl.dim = std::fmin(bl.dim + 0.00999999978f, bl.dim_target);
    else bl.dim = bl.dim * 0.980000019f;
  }
  // Light intensity ramp (+0x40) at +0x38 per second, clamped to [0, 1] (0x72647).
  if (bl.intensity_rate > 0.0f) {
    bl.intensity = bl.intensity_rate * 0.001f * bl.frame_ms + bl.intensity;
    if (bl.intensity > 1.0f) {
      bl.intensity = 1.0f;
      bl.intensity_rate = 0.0f;
    }
  } else if (bl.intensity_rate < 0.0f) {
    bl.intensity = bl.intensity_rate * 0.001f * bl.frame_ms + bl.intensity;
    if (!(bl.intensity >= 0.0f)) {
      bl.intensity = 0.0f;
      bl.intensity_rate = 0.0f;
    }
  }
  // Compositor GPU times (rIH7UTXfho4 / EcTHemmz6n4) and the BG's own: the host has no
  // compositor; reported as a failed query (0).
  st.compositor_gpu_time = st.compositor_proc_gpu_time = 0;
  st.wave_gpu_time = st.wave_post_gpu_time = 0;
  st.render_body_gpu_time = st.render_post_gpu_time = 0;
  const ThemeResult th = select_theme(bl, st.theme_colour_index, st.preset_colour_index, st.high_contrast != 0);  // 0x72E60
  // Opacity (+4) = transition opacity (+0x9C, also written to state.Opacity) x global fade (+60);
  // sub_D5FB0 (VR) is false on the host. +0x0C = +4 x state.WaveOpacity x +8.
  const float prev_opacity = bl.opacity;  // var_44
  st.opacity = bl.trans.out_opacity();
  bl.opacity = bl.trans.out_opacity() * bl.global_fade;
  bl.extra_opacity = bl.opacity * st.wave_opacity * bl.dim;
  bl.light.intensity_source = bl.intensity;  // light+0xC8
  st.flags = 0;
  bl.no_ui_widget = st.morpheus.draw_ui_widget ^ 1;
  bl.no_draw = st.draw_something ^ 1;
  bl.skip_render = st.draw_something ? 0 : (0.0001f > bl.opacity ? 1 : 0);
  preset_brightness(bl, th.preset_value, prev_opacity);  // 0x72974
  extra_layer(bl, th.particle);                          // 0x72AD0
  if (bl.background_is_light) bl.light.set_opacity(bl.opacity);  // DD560(+456, +4)
  bl.trans.opacity = bl.opacity;                                  // DD560(+448, +4) at 0x72CA5 (no auto-hide)
  bl.light.select_colours(st.bg_scene_visibility == 1 ? 1u : 0u, st.high_contrast != 0, true);  // 0x9E340(+416, ...)
  // Layer updates (0x72CEA..0x72D73), list order wave / particle / light / transition. Only the
  // particle layer has a load type (2); the others report 0 from 0xDD4F0.
  if (bl.particle) {
    int32_t r15 = level;
    if (int32_t b = bl.particle->busy_level()) r15 = b;
    if (bl.particle->state != 0) {
      bl.particle->step(r15, bl.frame_ms);  // 0xDD210
      if (int32_t b = bl.particle->busy_level()) r15 = b;
    }
  }
  // The light stays in state 4 and runs its Update with the frame time.
  if (bl.light.state >= 2 && bl.light.state <= 5) bl.light.update(bl.frame_ms);
  // The transition layer stays in state 4 (no auto-hide) and runs its Update 0xA46B0.
  if (bl.trans.state >= 2 && bl.trans.state <= 5) bl.trans.frame(bl.frame_ms);
  // 0x72D75: the particle step of the next render, paused by LightFlag bit 1.
  if (bl.particle) bl.particle->set_time_step(((st.light_flag >> 1) & 1) != 0, bl.frame_ms);
  // 0x72DA1: a transition in progress (0xA7300: type != -1) or VR (sub_928A0, false on the host)
  // sets bit 5 of state +0x29 (Morpheus mode flags bit 13).
  if (bl.trans.active()) st.morpheus.mode_flags |= 0x2000;
  // LightFlag IsReady (bit 0) = particle layer shown (state 4) (0x72DCF).
  st.light_flag = st.light_flag & ~1;
  if (bl.particle && bl.particle->state == 4) st.light_flag |= 1;
  st.unmap_texture_count = unmap_request_count();  // (+0x260 - +0x258) / 4
}

void render(BackgroundLayer& bl) {  // 0x70DD0
  auto& core = ssn::psm::gfx::Core::get();
  if (!bl.initialized || !core.created()) return;
  if (bl.skip_render && bl.skip_count > 1) return;
  ssn::gfx::Image& screen = core.screen();  // XfGraphics default target = the display buffer
  if (!(0.0001f < bl.opacity) || static_cast<int8_t>(bl.flags & 0xff) < 0) {
    // sub_DEE00(gfx, ...): without exclusive clear, a hardware clear of the target to (0,0,0,0).
    auto& dev = ssn::gfx::device();
    std::lock_guard<std::recursive_mutex> record(dev.record_lock());
    ssn::gfx::Image* pc = dev.target_color();
    ssn::gfx::Image* pd = dev.target_depth();
    dev.bind_target(&screen, nullptr);
    dev.ensure_rendering();
    VkClearAttachment a{VK_IMAGE_ASPECT_COLOR_BIT, 0, {}};
    a.clearValue.color = {{0.0f, 0.0f, 0.0f, 0.0f}};
    VkClearRect r{{{0, 0}, {screen.width, screen.height}}, 0, 1};
    vkCmdClearAttachments(dev.cmd(), 1, &a, 1, &r);
    dev.bind_target(pc, pd);
    ++bl.skip_count;
    return;
  }
  // 0x71088: a visible transition may redirect the layers before it into its capture texture
  // (0xA4720, the old image of the transition), or cover the screen alone (both images present).
  ssn::gfx::Image* layer_target = &screen;
  bool alone = false;
  if (bl.trans.visible()) bl.trans.prerender(layer_target, alone);
  if (alone) {
    if (bl.trans.visible()) bl.trans.render(screen);  // 0x7111B
    bl.skip_count = 0;
    return;
  }
  // Layers in list order: wave (+392, not reproduced), particle (+424), light (+416),
  // transition (+448: invisible while +200 == 0; drawn into the original target, 0x713B3). The XF
  // blend state starts as {enable, Add, SrcAlpha, OneMinusSrcAlpha}; the particle layer leaves it
  // disabled.
  bool blend = true;
  const int32_t viewport[4] = {0, 0, static_cast<int32_t>(layer_target->width), static_cast<int32_t>(layer_target->height)};
  if (bl.particle && bl.particle->visible()) {
    bl.particle->render(*layer_target, viewport);
    blend = false;
  }
  if (bl.light.visible()) bl.light.render(*layer_target, blend);
  if (bl.trans.visible()) bl.trans.render(screen);
  bl.skip_count = 0;
}

}  // namespace

// InitializeNative — 0x8DB30: allocates the 48-byte root (vtable 0xCC8D00) and the 624-byte
// BackgroundLayer (0x6F7A0 + Initialize 0x6FF90), registers it (_5dDsI2vg6dc) and returns 0.
SSN_PINVOKE("ShellUIBackgroundLayer", "InitializeNative", int32_t, ssn_BGLayer_InitializeNative, ()) {
  std::lock_guard<std::mutex> lk(g_lock);
  auto bl = std::make_unique<BackgroundLayer>();
  // +0xA0..+0x14F: the preset table (0x6FF90).
  bool table_ok = true;
  for (int i = 0; i < 6; ++i) {
    const size_t n = i == 5 ? 4 : 8;
    const auto t = eboot_bytes(kPresetTableVa[i], 4 * n);
    if (t.size() != 4 * n) table_ok = false;
    else std::memcpy(bl->preset_table + 8 * i, t.data(), 4 * n);
  }
  if (!table_ok) log::error("bglayer", "BackgroundLayer preset table not readable");
  if (!bl->light.initialize()) log::error("bglayer", "Light layer not initialised: the background stays unrendered");
  // +424: LightParticle (0x938D0 + Initialize 0x93AB0); its light is +416 (sub_93FF0).
  bl->particle = std::make_unique<ParticleLayer>(bl->light);
  if (!bl->particle->initialize()) {
    log::error("bglayer", "LightParticle layer not initialised: particles stay unrendered");
    bl->particle.reset();
  }
  // +448: Transition (0xA38D0 + Initialize 0xA3CB0).
  if (!bl->trans.initialize()) log::error("bglayer", "Transition layer not initialised: background images stay unrendered");
  clear_unmap_requests();
  bl->global_fade = 0.0f;
  bl->intensity_rate = 1.0f;  // +56 = 1.0 at the end of 0x6FF90
  bl->initialized = bl->light.ready();
  // _5dDsI2vg6dc(root): the root joins libScePsm's post-swap-buffer list (0x1e4120).
  psm::gfx::Core::get().add_post_swap_object();
  g_bl = std::move(bl);
  bgm::create_manager();  // root +40 = the BGM manager (sub_8F1B0; bglayer_bgm.cpp)
  return 0;
}

// FinalizeNative — 0x8DBF0: deletes the root; its destructor (0x8F090) leaves the post-swap-buffer
// list (_1Z9IoxZ508g, libScePsm 0x1e41d0).
SSN_PINVOKE("ShellUIBackgroundLayer", "FinalizeNative", int32_t, ssn_BGLayer_FinalizeNative, ()) {
  std::lock_guard<std::mutex> lk(g_lock);
  if (g_bl) psm::gfx::Core::get().remove_post_swap_object();
  g_bl.reset();
  bgm::root_finalized();  // qword_EC0080 = 0: the BGM manager (root +40) is unreachable
  return 0;
}

// UpdateNative(ref state, sizeOfStruct) — 0x8DCC0: XF_ASSERT(size == 164); with a
// BackgroundLayer, VR state (sub_D5B60, not reproduced), Update (0x72540), Render (0x70DD0);
// with the BGM manager, its update (sub_8F3E0, bglayer_bgm.cpp). Returns 0.
SSN_PINVOKE("ShellUIBackgroundLayer", "UpdateNative", int32_t, ssn_BGLayer_UpdateNative, (BackgroundLayerState* st, int32_t size)) {
  if (size != static_cast<int32_t>(sizeof(BackgroundLayerState))) {
    log::error("bglayer", "[XF] updateBackgroundLayer:463 XF_ASSERT failed (size {})", size);
    return 0;
  }
  std::lock_guard<std::mutex> lk(g_lock);
  if (!g_bl || !st) return 0;
  update(*g_bl, *st);
  render(*g_bl);
  bgm::update(st->frame_milliseconds);  // sub_8F3E0(root[5], state +24)
  return 0;
}

// UpdateDrawSomethingNative(bool) — 0x8E780: no BackgroundLayer -> -1; true clears the
// "nothing drawn" / "skip render" bytes (+33, +34).
SSN_PINVOKE("ShellUIBackgroundLayer", "UpdateDrawSomethingNative", int32_t, ssn_BGLayer_UpdateDrawSomethingNative, (uint8_t draw)) {
  std::lock_guard<std::mutex> lk(g_lock);
  if (!g_bl) return -1;
  if (draw) g_bl->no_draw = g_bl->skip_render = 0;
  return 0;
}

// SetFocusChangeEffectNative(int, int, int, int, int, bool) — 0x8E3D0: with the root, the
// BackgroundLayer's 0x73A40: when the wave layer +432 exists, is the current layer (+464) and
// its opacity (+0x58) is above 0, sub_9FBC0 hands the arguments to the wave renderer (+128,
// sub_A16D0: focus position and ripple parameters) and clears its +0x1E4. Returns 0.
// The +432 layer is created only by the wave themes (sub_73560 from 0x72E60), which the host
// does not render (see select_theme); with a light theme +432 is null and nothing happens.
SSN_PINVOKE("ShellUIBackgroundLayer", "SetFocusChangeEffectNative", int32_t, ssn_BGLayer_SetFocusChangeEffectNative,
            (int32_t a, int32_t b, int32_t c, int32_t d, int32_t e, uint8_t f)) {
  (void)a, (void)b, (void)c, (void)d, (void)e, (void)f;
  std::lock_guard<std::mutex> lk(g_lock);
  if (g_bl && !g_bl->background_is_light)
    note_once("wave-layer focus-change effect (0x73A40 -> sub_A16D0) not rendered");
  return 0;
}

// MapTextureNative(int nativeTexId, uint psmTexHandle) — 0x8E540: no root -> -1; the VR manager
// (root +16, 0xD5F80) does not exist on the host, so 0x738B0: a non-zero handle is wrapped as an XF
// texture (0xE0860: the libScePsm texture of that handle; unknown handle -> -1); without the
// transition layer 0; else the transition's MapTexture (0xA6F90; -2 for an unknown id -> -2).
SSN_PINVOKE("ShellUIBackgroundLayer", "MapTextureNative", int32_t, ssn_BGLayer_MapTextureNative, (int32_t id, uint32_t handle)) {
  std::lock_guard<std::mutex> lk(g_lock);
  if (!g_bl) return -1;
  XfTex tex;
  if (handle != 0) {
    auto pb = psm::gfx::Objects::get().pixel_buffer_ref(static_cast<int32_t>(handle));
    if (!pb) return -1;
    tex = std::make_shared<XfTexture>();
    tex->psm = pb;
    tex->width = static_cast<uint32_t>(pb->width);   // +112 = texture +356
    tex->height = static_cast<uint32_t>(pb->height);  // +116 = texture +360
  }
  const int32_t r = g_bl->trans.map_texture(id, tex);
  return r == -2 ? -2 : r;
}

// GetUnmapTextureRequestNative() — 0x8E5B0: pops the last id of the BackgroundLayer's request
// vector (+600 / +608), -1 when empty or without a root.
SSN_PINVOKE("ShellUIBackgroundLayer", "GetUnmapTextureRequestNative", int32_t, ssn_BGLayer_GetUnmapTextureRequestNative, ()) {
  std::lock_guard<std::mutex> lk(g_lock);
  if (!g_bl) return -1;
  return pop_unmap_request();
}

// SetBGTransitionNative(BGTransitionParam) — 0x8E5D0 -> 0x709F0 -> 0xA6980.
SSN_PINVOKE("ShellUIBackgroundLayer", "SetBGTransitionNative", int32_t, ssn_BGLayer_SetBGTransitionNative, (BGTransitionParam p)) {
  std::lock_guard<std::mutex> lk(g_lock);
  if (!g_bl) return -1;
  return g_bl->trans.set_transition(p);
}

// SetBGBasematNative(BGBasematParam) — 0x8E600 -> 0x70A10 -> 0xA6F70 -> 0xA8900.
SSN_PINVOKE("ShellUIBackgroundLayer", "SetBGBasematNative", int32_t, ssn_BGLayer_SetBGBasematNative, (BGBasematParam p)) {
  std::lock_guard<std::mutex> lk(g_lock);
  if (!g_bl) return -1;
  return g_bl->trans.set_basemat(p);
}

}  // namespace ssn::eboot::bglayer
