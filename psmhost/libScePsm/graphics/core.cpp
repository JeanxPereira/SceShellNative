#include "libScePsm/graphics/core.h"

#include <algorithm>
#include <cstring>

#include <windows.h>

#include "core/log.h"
#include "libScePsm/graphics/objects.h"

namespace ssn::psm::gfx {

Core& Core::get() {
  static Core c;
  return c;
}

int32_t Core::create() {
  // The context's screen is the display buffer (GetScreenInfo reports the device's
  // buffer size, libScePsm 0x1e75e0), whatever size was requested at creation.
  screen_w_ = video_out.width;
  screen_h_ = video_out.height;
  auto& dev = ssn::gfx::device();
  screen_ = dev.create_image(screen_w_, screen_h_, 1, VK_FORMAT_R8G8B8A8_UNORM,
                             VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT |
                                 VK_IMAGE_USAGE_SAMPLED_BIT);
  if (!screen_.valid()) return -1;
  // The global constant buffer (ctx+368) is created by Create (0x1e5d50) as 8 bytes
  // initialised to {1.0f, 1.0f} (u_GlobalAlpha, u_GlobalIntensity).
  const float init[2] = {1.0f, 1.0f};
  global_constants_.assign(reinterpret_cast<const uint8_t*>(init), reinterpret_cast<const uint8_t*>(init) + 8);
  created_ = true;
  log::info("psm.gfx", "graphics context created, screen {}x{}", screen_w_, screen_h_);
  return 0;
}

void Core::destroy() {
  created_ = false;
}

void Core::apply(uint32_t update, const GraphicsState* state, const int32_t* handles, int32_t count) {
  if (state) state_ = *state;
  if (handles) std::memcpy(handles_.data(), handles, sizeof(int32_t) * std::min<int32_t>(count, 24));
  // Texture units are bound by 0x1f32a0 only when the update carries a texture bit
  // (update >= 0x40000000): units 0 .. min(8, DirtyTextureSlotCount)-1 whose handle names a
  // live texture are re-bound (0xac70 / 0xabe0); a zero or unknown handle leaves the unit
  // as it was. There is no other caller of those bind functions in libScePsm, so a unit
  // keeps its last texture until it is rebound.
  if (update >= 0x40000000u && handles) {
    const int32_t n = std::min<int32_t>(8, state_.dirty_texture_slot_count);
    for (int32_t i = 0; i < n; ++i) {
      const int32_t h = handles_[kHandleTexture0 + i];
      if (h != 0 && Objects::get().pixel_buffer(h)) bound_textures_[i] = h;
    }
  }
}

bool Core::bind_frame_buffer() {
  auto& dev = ssn::gfx::device();
  const int32_t fb = handles_[kHandleFrameBuffer];
  if (fb == 0) {
    dev.bind_target(&screen_, nullptr);
    return true;
  }
  FrameBuffer* f = Objects::get().frame_buffer(fb);
  if (!f || !f->color) return false;
  dev.bind_target(&f->color->image, f->depth ? &f->depth->image : nullptr);
  return true;
}

std::string Core::capture_path_for_next_frame() {
  const uint64_t n = ++frame_;
  char dir[512], frames[1024];
  if (!GetEnvironmentVariableA("SSN_CAPTURE_DIR", dir, sizeof dir)) return {};
  if (!GetEnvironmentVariableA("SSN_CAPTURE_FRAMES", frames, sizeof frames)) return {};
  std::string list = std::string(",") + frames + ",";
  if (list.find("," + std::to_string(n) + ",") == std::string::npos) return {};
  return std::string(dir) + "/frame_" + std::to_string(n) + ".bmp";
}

}  // namespace ssn::psm::gfx
