// The BackgroundLayer "Light" layer (eboot: ctor 0x9DE50, vtable 0xCC8FB0, 352 bytes).
#pragma once
#include <cstdint>

#include "gfx/vk_device.h"

namespace ssn::eboot::bglayer {

class LightLayer {
 public:
  // Initialize (vtable +16, 0x9DFB0): light_p (shader #67), the two IES textures embedded in the
  // eboot (GNF images at 0xBC6180 "texFloor" and 0xBC2080 "texVolume", createIesTex), colour set 0
  // of the table at 0xBC1EC0, state 4. Returns false when a firmware resource is missing.
  bool initialize();
  bool ready() const { return ready_; }

  // Layer base (0xDD030 / 0xDD560 / 0x75C20).
  int32_t state = 0;     // +92: 4 = shown
  float opacity = 0.0f;  // +88 (+0x58), set through set_opacity (0xDD560; the light has no auto-hide, +24 = 0)
  void set_opacity(float o) { opacity = o; }
  bool visible() const { return state == 4 && opacity > 0.0001f; }  // 0x75C20

  // Fields written by the BackgroundLayer / particle layer.
  float intensity_source = 1.0f;  // +0xC8 (= BackgroundLayer +0x40 every Update)
  float intensity_scale = 0.0f;   // +0xCC (particle mode table 0xBC1DB8, or 1.0 when the theme has no particle layer)
  float particle_alpha = 1.0f;    // +0xD0 (set with the particle texture by sub_9EB90)

  // SetColor (0x9E340): colour set = visibility + (high contrast ? 2 : 0); animated over 300 ms
  // when `animate` and the light is lit, copied at once otherwise.
  void select_colours(uint32_t visibility_dimmed, bool high_contrast, bool animate);
  // Layer Update (vtable +64, 0x9E760) with the frame time in milliseconds.
  void update(float frame_ms);
  // sub_9EB90: the particle layer hands over its target (+168) and particleAlpha (+0xD0); only
  // while the light is shown (state 4). Render consumes and releases it (+168 = 0).
  void set_particle_texture(ssn::gfx::Image* tex, float alpha);
  // Render (vtable +72, 0x9E7E0) into the bound target. `blend` is the XF blend state current at
  // that point: BackgroundLayer Render enables {Add, SrcAlpha, OneMinusSrcAlpha}; the particle
  // layer's Render resets it to disabled (sub_DF760 + sub_DE620) after drawing its particles.
  void render(ssn::gfx::Image& target, bool blend);

 private:
  void animate_colours(float frame_ms);  // 0x9E3F0

  bool ready_ = false;
  float time_ = 10.0f;          // +0xC4 (xmmword 0xB8AB40 = {10, 1, 0, 1} -> +0xC4..+0xD0)
  int32_t colour_index_ = -1;   // +0xD8
  float anim_elapsed_ = 0.0f;   // +0xDC
  float anim_duration_ = -1.0f; // +0xE0
  float colours_[27]{};         // +0xF0..+0x15B: ColorCb (6 x float4, gamma, gintensity, noise)
  bool colours_dirty_ = true;   // +0xD5
  float table_[4][28]{};        // 0xBC1EC0: 4 sets x 112 bytes
  ssn::gfx::Image tex_floor_, tex_volume_;  // +120, +128
  ssn::gfx::Image* particle_tex_ = nullptr;  // +168
};

}  // namespace ssn::eboot::bglayer
