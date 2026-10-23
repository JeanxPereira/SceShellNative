// The BackgroundLayer "Transition" layer (ShellUI eboot, FW 4.03): ctor 0xA38D0 (416 bytes),
// vtable 0xCC9148 {+16 Initialize 0xA3CB0, +64 Update 0xA46B0, +72 Render 0xA4D70, +80 IsVisible
// 0xA7310, +104 IsBusy 0xA7340}, the per-frame state machine 0xA53F0 (BackgroundLayer Update),
// SetBGTransition 0xA6980, MapTexture 0xA6F90, the slot release 0xA6760, the render-target hook
// 0xA4720 and the old-image capture 0xA48D0. It owns the "basemat" object (+208, 204 bytes:
// SetBasemat 0xA8900, Update 0xA82A0 / 0xA84A0, the gradation spline 0xA7390 / 0xA7D10, and the
// GradationParameter constant buffer 0xA8670).
//
// Background images reach it as libScePsm textures (MapTextureNative -> XF texture wrapper
// 0xE0860); it draws them full screen with one of the eboot's transition pixel shaders
// (#44 single_p, #5 cross_fade_p, #46 ripple_p, #48 slide_in_p, #49 game_splash_p; #47
// ripple_with_logo_p is not transcribed) over basic_vv, blending disabled.
#pragma once
#include <cstdint>
#include <memory>
#include <vector>

#include "eboot/bglayer.h"
#include "gfx/vk_device.h"

namespace ssn::psm::gfx {
struct PixelBuffer;
}

namespace ssn::eboot::bglayer {

// An XF texture (vtable 0xCCA090): either the wrapper of a libScePsm texture (0xE0860) or a texture
// the layer creates (0xE0590 / 0xE1850). All use the sampler of +44: {mag 1, min 1, clamp 2, 2, 2,
// z 1, mip 1, max LOD 15} = S# {0x92, 0x00F00000, 0x05500000, 0}.
struct XfTexture {
  std::shared_ptr<psm::gfx::PixelBuffer> psm;  // +96 (E0860)
  ssn::gfx::Image own;
  uint32_t width = 0, height = 0;               // +112 / +116
  ~XfTexture();
  ssn::gfx::Image* image();
};
using XfTex = std::shared_ptr<XfTexture>;

// The 68-byte gradation description (basemat +0 "from" / +0x44 "to").
struct Gradation {
  float f[17]{};
};

// The 28-byte argument block of the basemat update 0xA82A0 (also kept at basemat +0x88).
struct BasematArgs {
  float dt = 0.0f;       // +0  transition +0x6C
  uint8_t flag = 0;      // +4
  int32_t hc = 0;        // +8  high-contrast change (0 set, 1 stays set, 2 stays clear, 3 cleared)
  int32_t vis = 0;       // +12 visibility change (1 set, 2 stays set, 3 cleared, 0 stays clear)
  int32_t type = 0;      // +16 transition type
  int32_t shader = 0;    // +20 transition +0xC8
  int32_t sub_state = 0; // +24 transition +0xBC
};

struct Basemat {
  Gradation from, to;       // +0x00, +0x44
  BasematArgs args;         // +0x88
  uint8_t dirty = 1;        // +0xA4
  int32_t cur_type = 0;     // +0xA8
  int32_t type = 0;         // +0xAC
  int32_t saved_type = 0;   // +0xB0
  float color[4] = {0.00784f, 0.01568f, 0.03137f, 1.0f};  // +0xB4 (xmmword 0xB8BEF0)
  float time = 0.0f;        // +0xC4
  float duration = 1000.0f; // +0xC8
};

// SetBGTransitionNative's BGTransitionParam (managed struct, 20 bytes).
struct BGTransitionParam {
  int32_t type;   // TransitionType | Degree << 16
  int32_t flag;
  float center_x, center_y, capture_scale;
};
// SetBGBasematNative's BGBasematParam (20 bytes).
struct BGBasematParam {
  int32_t type;
  float color[3];
  float duration;
};

class TransitionLayer {
 public:
  bool initialize();  // 0xA3CB0
  // Layer base: +24 = 0 (no auto-hide), state 4 after Initialize; opacity +0x58 (BackgroundLayer +4).
  int32_t state = 0;
  float opacity = 0.0f;
  bool visible() const;                                  // 0xA7310
  bool active() const { return type_ != -1; }            // 0xA7300
  void frame(float frame_ms);                            // vtable +64, 0xA46B0
  void update(BackgroundLayerState& st);                 // 0xA53F0
  int32_t set_transition(const BGTransitionParam& p);    // 0xA6980
  int32_t set_basemat(const BGBasematParam& p);          // 0xA6F70 -> 0xA8900
  int32_t map_texture(int32_t id, const XfTex& tex);     // 0xA6F90
  float out_opacity() const { return f9C_; }             // +0x9C
  // Render hooks of BackgroundLayer Render (0x70DD0).
  // 0xA4720: true when the layers before this one must render into the capture texture (+400),
  // which is then returned in `target`; `alone` = the transition covers the screen by itself.
  bool prerender(ssn::gfx::Image*& target, bool& alone);
  void render(ssn::gfx::Image& target);                  // 0xA4D70

 private:
  void release_slot(int32_t id);                         // 0xA6760
  XfTex* slot(int32_t id);
  void basemat_update(const BasematArgs& a);             // 0xA82A0
  ssn::gfx::Image* capture(ssn::gfx::Image& target);    // 0xA48D0

  // fields by offset
  float f6C_ = 16.0f;            // +0x6C frame ms (ctor 0x41800000)
  float f70_ = 0.0f, f74_ = 0.0f;// +0x70, +0x74
  double f78_ = 0.0;             // +0x78
  float f80_ = 100.0f;           // +0x80 duration (xmmword 0xB89BF0)
  float f84_ = 0.0f, f88_ = 0.0f;// +0x84 / +0x88 centre
  float f8C_ = 1.0f;             // +0x8C capture scale
  float f90_ = 1.0f, f94_ = 0.0f, f98_ = -1.0f, f9C_ = 0.0f, fA0_ = 0.0f;
  uint8_t bA4_ = 0, bA5_ = 0, bA6_ = 0, bA7_ = 0;
  int32_t type_ = -1;            // +0xA8
  int32_t degree_ = 0;           // +0xAC
  int32_t img_id_ = 0;           // +0xB0
  uint32_t id_bits_ = 0;         // +0xB4
  uint8_t bB8_ = 0;              // +0xB8
  int32_t sub_state_ = 1, fC0_ = 2, fC4_ = 2, shader_ = 0;  // +0xBC..+0xC8 (xmmword 0xB8BEE0)
  Basemat basemat_;              // +0xD0
  uint8_t bD8_ = 0, bD9_ = 0;
  XfTex slots_[6];               // +0x130 id0, +0x138 id1, +0x140 id5, +0x148 id6, +0x150 id2, +0x158 id4
  XfTex cur_, next_, overlay_, black_, transparent_, snapshot_, capture_;  // +0x160..+0x190
  uint8_t b198_ = 0, b199_ = 0;
};

// The BackgroundLayer's unmap-request vector (+592; pushed by 0x739C0, popped by
// GetUnmapTextureRequestNative 0x8E5B0).
void push_unmap_request(int32_t id);
uint32_t unmap_request_count();
int32_t pop_unmap_request();  // the last request, or -1 when there is none
void clear_unmap_requests();

}  // namespace ssn::eboot::bglayer
