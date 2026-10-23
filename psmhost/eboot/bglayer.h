// ShellUI background layer ("ShellUIBackgroundLayer" P/Invokes of Sce.Vsh.ShellUI.BGLayer.BGLayerNative),
// reimplemented from the ShellUI eboot (NPXS40087 eboot.bin, FW 4.03).
//
// On the console the native side is the "XF" renderer statically linked into the eboot: a
// BackgroundLayer object (624 bytes, ctor 0x6F7A0, Initialize 0x6FF90, Update 0x72540, Render
// 0x70DD0) owning a list of layers (base class vtable 0xCC9498):
//   +392 wave        (sub_A2350, 69568 bytes)  — "WaveRenderer2", the PS4-style wave themes
//   +424 particle    (sub_938D0, 2024 bytes)   — "LightParticle": particle_c simulation + particle/large_particle draw
//   +416 light       (sub_9DE50, 352 bytes)    — "Light": full-screen light_p pass (floor, beams, point light)
//   +448 transition  (sub_A38D0, 416 bytes)    — "Transition": background images (ripple/slide/fade), basemat
// UpdateNative (0x8DCC0) runs Update and then Render, so the background is drawn into the
// display buffer while the managed UI updates, before the UI renders on top of it
// (BGLayerPlugin.OnLoaded sets Application.ClearFrameBufferAutomatically = false).
#pragma once
#include <cstdint>
#include <string>
#include <vector>

namespace ssn::eboot::bglayer {

// Sce.Vsh.ShellUI.BGLayer.BackgroundLayerState (StructLayout Sequential, Pack = 4). UpdateNative
// asserts sizeOfStruct == 164 ("updateBackgroundLayer" XF_ASSERT, line 463).
#pragma pack(push, 4)
struct MorpheusParameter {
  uint8_t draw_ui_widget;   // +0 (state +36)
  uint8_t pad_[3];
  int32_t mode_flags;       // +4 (state +40)
  float dimmer, vr3d_dimmer, bg_mask;
  float rendering_x, rendering_y, rendering_width, rendering_height;
  float frame_milliseconds, video_out_refresh_rate;
  int32_t vr_tracker2_status, gen2vr_lens_dist, vr_gaze_calibration_flags, stereo3dvr_flags, gen2vr_head_space_flags;
};
struct BackgroundLayerState {
  uint8_t draw_something;          // +0
  uint8_t high_contrast;           // +1
  uint8_t suppress_volatile_memory_block;  // +2
  uint8_t bg_scene_visibility;     // +3 (PUI BackgroundSceneVisibility: 0 Normal, 1 DimmedByModal, 2 HiddenByModal)
  float opacity;                   // +4
  float wave_opacity;              // +8
  uint32_t preset_colour_index;    // +12
  uint32_t theme_colour_index;     // +16
  int32_t current_area;            // +20
  float frame_milliseconds;        // +24
  float frame_real_milliseconds;   // +28
  int32_t rendering_mode;          // +32
  MorpheusParameter morpheus;      // +36
  uint32_t flags;                  // +100
  int32_t compositor_gpu_time;     // +104
  int32_t compositor_proc_gpu_time;// +108
  int32_t wave_gpu_time;           // +112
  int32_t wave_post_gpu_time;      // +116
  int32_t render_body_gpu_time;    // +120
  int32_t render_post_gpu_time;    // +124
  uint32_t unmap_texture_count;    // +128
  uint32_t transition_flags;       // +132
  int32_t display_width;           // +136
  int32_t display_height;          // +140
  uint32_t display_buffer_color_format;  // +144
  float safe_area;                 // +148
  int32_t transition_type;         // +152
  int32_t light_flag;              // +156
  int32_t show_overlay_image_immediately_flag;  // +160
};
#pragma pack(pop)
static_assert(sizeof(MorpheusParameter) == 64);
static_assert(sizeof(BackgroundLayerState) == 164);

// --- firmware data -------------------------------------------------------------------------
// Bytes at a virtual address of the ShellUI eboot (/system_ex/app/NPXS40087/eboot.bin, read
// from the firmware dump and mapped through its PT_LOAD program headers). Empty on failure.
// `va` is the address in the reference build every eboot address in psmhost cites (FW 4.03
// retail, build job J01736823, GNU build-id eb24014c...). For another build of the eboot the
// reader translates it through that build's table of the data the host reads (bglayer_gpu.cpp);
// an unknown build or an address missing from its table reads as empty (logged).
std::vector<uint8_t> eboot_bytes(uint64_t va, size_t size);

}  // namespace ssn::eboot::bglayer
