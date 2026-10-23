// The BackgroundLayer "LightParticle" layer (ShellUI eboot, FW 4.03): ctor 0x938D0 (2024 bytes),
// vtable 0xCC8DB8 {+16 Initialize 0x93AB0, +24 Finalize 0x93DA0, +32 OnLoad 0x94000,
// +40 OnUnload 0x94010, +48 Prepare 0x94020, +56 Release 0x95A10, +64 Update (nullsub 0x96630),
// +72 Render 0x971B0, +80 IsVisible 0x75C20, +104 IsBusy 0x97470}.
//
// The particles live on the GPU: a pool of 6000 records of 68 bytes simulated by the compute
// shader particle_c (one dispatch per emitter, 64 lanes per group) and drawn by particle_vv /
// particle_p as 6 vertices per particle into a 10_11_11_FLOAT target, which the Light layer
// composites (light_p texP x particleAlpha). The CPU side only drives emitter parameter blocks
// (248 bytes per compute emitter, 320 bytes per draw emitter, 8 emitters per generation, two
// generations) from the effect timelines (bglayer_particle_effect.h). In the boot-animation
// modes 3 / 4 two more emitters per generation ("large particles", 248 / 236-byte blocks) are
// simulated by the same compute shader and drawn additively by large_particle_vv / _p with the
// Particle0/1.gnf textures.
#pragma once
#include <array>
#include <cstdint>
#include <memory>

#include "eboot/bglayer_particle_effect.h"
#include "gfx/vk_device.h"

namespace ssn::eboot::bglayer {

class LightLayer;

class ParticleLayer {
 public:
  explicit ParticleLayer(LightLayer& light);
  ~ParticleLayer();
  // Initialize (0x93AB0): labels, effect object, shaders #68 particle_c, #70 particle_vv,
  // #69 particle_p, blend controls. Returns false when a firmware resource is missing.
  bool initialize();

  // --- layer base (0xDD030 with type 2: "LightParticle", auto-hide +24 = 1) -----------------
  int32_t state = 0;      // +92
  float opacity = 0.0f;   // +88
  int32_t pending = 0;    // +100
  bool show();                                   // 0xDD640
  bool hide();                                   // 0xDD6B0
  void set_opacity(float o);                     // 0xDD560
  void step(int32_t busy_level, float frame_ms); // 0xDD210
  int32_t busy_level();                          // 0xDD4F0 (type 2)
  bool visible() const { return state == 4 && opacity > 0.0001f; }  // 0x75C20

  // SetMode (0x97560): effect / large-particle selection of a theme (1..6).
  void set_mode(int32_t mode);
  // 0x97510: the simulation step of the next render (+0x704).
  void set_time_step(bool paused, float frame_ms);
  // Render (0x971B0) with the BackgroundLayer target and viewport {x, y, w, h}.
  void render(ssn::gfx::Image& target, const int32_t viewport[4]);

  int32_t mode() const { return mode_; }

 private:
  struct Gpu;
  bool on_load();   // +32 0x94000
  bool on_unload(); // +40 0x94010
  bool prepare();   // +48 0x94020
  bool release();   // +56 0x95A10
  bool busy() const;// +104 0x97470
  bool label_pending() const;        // the two end-of-pipe labels (+1976 / +1984), 0x97470
  void write_label();                // 0x971B0 tail: label = 1, end-of-pipe write of 0
  void render_frame(const int32_t viewport[4]);  // 0x971B0 with +1804 == 0
  void switch_generation();          // the generation flip shared by 0x94020 / 0x97560 / 0x96FF0
  void load_effect();                // 0x95920
  void update_timeline();            // 0x96FF0
  void render_internal(ssn::gfx::Image& rt);  // 0x96860
  uint32_t xorshift();               // xorshift128+ step of 0x97740 (state qword_D60E88/90)

  LightLayer& light_;                // +2000 (sub_93FF0)
  ParticleEffect effect_;            // +2008
  std::unique_ptr<Gpu> gpu_;

  int32_t mode_ = 0;                 // +1996
  int32_t effect_index_ = 255;       // +1904 (ctor)
  bool large_ = false;               // +1816
  bool dispatched_ = false;          // +1817
  uint8_t skip_render_ = 0;          // +1804
  uint32_t generation_ = 0;          // +144
  float gen_start_[2]{};             // +1752 (0x6D8)
  float gen_end_[2]{};               // +1760 (0x6E0)
  float time_ = 0.0f;                // +1792 (0x700)
  float dt_ = 0.0f;                  // +1796 (0x704)
  uint32_t frame_ = 0;               // +1800 (0x708)
  uint32_t label_index_ = 0;         // XF context +24 (0 at creation, flipped once per frame)
  float cs_time_offset_ = 0.0f;      // +0x7B0 (effect +0xB4)
  float vs_time_offset_ = 0.0f;      // +0x7B4 (effect +0xB8)
  float particle_alpha_scale_ = 0.3f;// +2016 (ctor qword 0x3F19999A3E99999A)
  // Emitter parameter blocks, [generation][emitter]: initializeBuffers (0x94820 / 0x94B70)
  // allocates them once (zeroed) and the functors 0x97740 / 0x97810 seed them.
  std::array<std::array<std::unique_ptr<std::array<uint8_t, 248>>, 8>, 2> cs_;  // m_resSrtCs +152
  std::array<std::array<std::unique_ptr<std::array<uint8_t, 320>>, 8>, 2> vs_;  // m_resSrtVsPs +792
  // Regular buffers (AllocateRegularBuffer): V#s +1820 particles (68 x 6000), +1836 / +1852
  // the two index permutations (4 x 6000).
  uint32_t vsharp_particles_[4]{}, vsharp_perm1_[4]{}, vsharp_perm2_[4]{};
  bool buffers_ = false;

  // --- large particles of the boot animations (modes 3 / 4, +1816) ----------------------------
  // initializeBuffers "m_largeParResSrtCs" (0x94ED0, +1432) / "m_largeParResSrtVsPs" (0x953B0,
  // +1592): per generation (80-byte stride) four GPU constant buffers and two CPU blocks; the CPU
  // blocks are [generation][emitter] here: CS 248 bytes at +1496 + 80g + 8e (functor 0x978E0),
  // VsPs 236 bytes at +1656 + 80g + 8e (functor 0x979B0).
  std::array<std::array<std::unique_ptr<std::array<uint8_t, 248>>, 2>, 2> lcs_;
  std::array<std::array<std::unique_ptr<std::array<uint8_t, 236>>, 2>, 2> lvs_;
  // The "LightParticle" resource job (+1952, vtable 0xCC8E40, run 0x976B0 -> 0x96240) that loads
  // Particle0/1.gnf; +1944 = done. Created by Prepare while large particles are wanted.
  struct Job;
  std::unique_ptr<Job> job_;
  bool make_large_textures();  // the job's results -> +1768 / +1776
  void dispatch(uint8_t* blk, uint32_t g, uint32_t cur, float tcur);  // sub_96640
};

}  // namespace ssn::eboot::bglayer
