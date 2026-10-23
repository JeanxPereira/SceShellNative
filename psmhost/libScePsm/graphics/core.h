// libScePsm graphics core: the single graphics context the firmware creates
// (libScePsm keeps it in one global, 0x53f4c0) and the objects it renders with.
#pragma once
#include <array>
#include <atomic>
#include <chrono>
#include <cstdint>
#include <memory>
#include <string>
#include <unordered_map>
#include <vector>

#include "gfx/vk_device.h"
#include "libScePsm/graphics/types.h"

namespace ssn::psm::gfx {

struct VideoOut {
  // sceVideoOutGetResolutionStatus of the main port as the host presents it.
  int32_t width = 1920, height = 1080;
  int32_t refresh_code = 3;  // SCE_VIDEO_OUT_REFRESH_RATE_59_94HZ
};

class Core {
 public:
  static Core& get();

  bool created() const { return created_; }
  int32_t create();
  void destroy();

  int32_t screen_width() const { return screen_w_; }
  int32_t screen_height() const { return screen_h_; }
  ssn::gfx::Image& screen() { return screen_; }

  // Latest state and handle array pushed by Update / DrawArrays.
  void apply(uint32_t update, const GraphicsState* state, const int32_t* handles, int32_t count);
  const GraphicsState& state() const { return state_; }
  const std::array<int32_t, 24>& handles() const { return handles_; }
  // Texture handle bound to each unit (sticky, see apply()).
  const std::array<int32_t, 8>& bound_textures() const { return bound_textures_; }

  // Binds the frame buffer selected by handles[1] (0 = the screen) as render target.
  bool bind_frame_buffer();

  VideoOut video_out;

  // globalConstantBuffer contents (PsmGraphicsContext::UpdateGlobalConstantBuffer).
  std::vector<uint8_t>& global_constants() { return global_constants_; }

  // Frame capture: SSN_CAPTURE_DIR + SSN_CAPTURE_FRAMES ("1,30,120") select which
  // presented frames are written as BMP; returns the path for the frame being
  // presented (empty when none) and advances the frame counter.
  std::string capture_path_for_next_frame();
  uint64_t frame_index() const { return frame_; }

  // The post-swap-buffer object list (std::list at 0x527330, size qword_527340, under the
  // lock "ScePsmGraphicsPostSwapBuffer" qword_527318; created by 0x1e3d90). Exports
  // iKBWRnPccBs / _5dDsI2vg6dc (0x1e4080 / 0x1e4120) append an object, _1Z9IoxZ508g
  // (0x1e41d0) removes it (deferred while the hooks run). BeginRender / FlushRender /
  // SwapBuffers / WaitVBlank call the objects' hooks (vtable +32 / +40 / +24 and +16); the
  // ShellUI eboot registers its BGLayer root there (InitializeNative 0x8DB30). The host keeps
  // the count, which selects the branch of PsmGraphicsContext::WaitVBlank; the registrants'
  // hooks are not called by the host.
  void add_post_swap_object() { ++post_swap_objects_; }
  void remove_post_swap_object() {
    if (post_swap_objects_ > 0) --post_swap_objects_;
  }
  bool has_post_swap_objects() const { return post_swap_objects_ > 0; }

 private:
  std::atomic<int32_t> post_swap_objects_{0};
  bool created_ = false;
  int32_t screen_w_ = 0, screen_h_ = 0;
  ssn::gfx::Image screen_;
  GraphicsState state_{};
  std::array<int32_t, 24> handles_{};
  std::array<int32_t, 8> bound_textures_{};
  uint64_t frame_ = 0;
  std::vector<uint8_t> global_constants_;
};

// The next vblank of the video out port (graphics_device.cpp). HOST VALUE: a clock at the
// port's refresh rate, counted from its first use.
std::chrono::steady_clock::time_point next_vblank();

}  // namespace ssn::psm::gfx
