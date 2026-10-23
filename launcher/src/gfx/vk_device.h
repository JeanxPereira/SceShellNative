// Vulkan backend: device, swapchain, per-frame command recording and resources.
//
// The PSM graphics API the firmware drives is an immediate-mode, GLES2-style API
// (state + DrawArrays into the current frame buffer, SwapBuffers to present). The
// backend records everything for a frame into one command buffer, rendering into
// offscreen targets with dynamic rendering, and presents the screen target by
// blitting it to the swapchain image.
//
// Frame synchronisation follows libScePsm's SwapBuffers (0x1e6d40): it first waits for the
// previous frame (sub_2BC0 -> sceCompositorWaitPostEvent, the event the compositor posts once
// it has consumed frame N-1's canvas), then submits frame N with its post-event / composite
// commands (sub_78F0 -> sub_2D70) and returns without waiting for frame N. So while the CPU
// runs frame N+1, frame N may still be executing on the GPU, and GPU-written memory (e.g. the
// background layer's end-of-pipe labels) reads as the GPU left it. The host keeps two frame
// slots (command buffer, fence, half of the upload ring, captures): present() paces (the
// vblank stand-in), waits for frame N-1's fence, submits frame N and returns. On the GPU the
// frames still run one after the other (each frame's command buffer starts with a full
// memory barrier).
#pragma once
#include <windows.h>
#include <vulkan/vulkan.h>

#include <atomic>
#include <cstdint>
#include <functional>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

namespace ssn::gfx {

struct Image {
  VkImage image = VK_NULL_HANDLE;
  VkDeviceMemory memory = VK_NULL_HANDLE;
  VkImageView view = VK_NULL_HANDLE;
  VkFormat format = VK_FORMAT_UNDEFINED;
  VkImageAspectFlags aspect = VK_IMAGE_ASPECT_COLOR_BIT;
  uint32_t width = 0, height = 0, levels = 1;
  VkImageLayout layout = VK_IMAGE_LAYOUT_UNDEFINED;
  bool valid() const { return image != VK_NULL_HANDLE; }
};

struct Buffer {
  VkBuffer buffer = VK_NULL_HANDLE;
  VkDeviceMemory memory = VK_NULL_HANDLE;
  void* mapped = nullptr;
  VkDeviceSize size = 0;
};

// A slice of the per-frame upload ring (host-visible, valid until the frame retires, i.e.
// until the present() after the one that submits it).
struct Slice {
  VkBuffer buffer = VK_NULL_HANDLE;
  VkDeviceSize offset = 0;
  void* data = nullptr;
};

// Host-side frame capture (--capture-dir / --capture-every / --capture-frames): which
// presented frames (1-based, as counted by present()) are written as PNG into `dir`.
struct CaptureSchedule {
  std::string dir;
  uint64_t every = 0;            // every Nth frame (0 = off)
  std::vector<uint64_t> frames;  // explicit frame numbers
  bool wants(uint64_t frame) const;
};

class Device {
 public:
  // With hwnd == nullptr the device runs headless: no surface or swapchain; present()
  // renders into the offscreen screen image only and paces frames with a ~60 Hz timer.
  bool init(HINSTANCE instance, HWND hwnd);
  bool headless() const { return headless_; }
  void shutdown();

  VkDevice device() const { return device_; }
  VkPhysicalDevice physical() const { return physical_; }

  Image create_image(uint32_t w, uint32_t h, uint32_t levels, VkFormat format, VkImageUsageFlags usage);
  void destroy_image(Image& img);
  // Copies tightly packed texels into a sub-rectangle of one mip level (recorded into
  // the current frame, before any rendering that follows in submission order).
  void upload_image(Image& img, uint32_t level, int x, int y, uint32_t w, uint32_t h, const void* data, size_t bytes);

  // Per-frame transient memory for vertex, index and uniform data.
  Slice alloc(VkDeviceSize size, VkDeviceSize align = 16);

  // The frame's command buffer (begins the frame on first use).
  VkCommandBuffer cmd();
  void transition(VkCommandBuffer cb, Image& img, VkImageLayout to);

  // Rendering scope over a colour (and optional depth/stencil) target; begun lazily
  // and ended before transfers or presentation.
  void bind_target(Image* color, Image* depth);
  void ensure_rendering();
  void end_rendering();
  bool rendering() const { return rendering_; }
  Image* target_color() const { return target_color_; }
  Image* target_depth() const { return target_depth_; }

  // Blits `screen` to the swapchain and submits the frame after the previous one has
  // retired (see the header comment); it does not wait for this frame. When `capture_path`
  // is set the screen is also written there as a 32-bit BMP, and frames selected by the
  // capture schedule are written as PNG, both once the frame has retired (next present(),
  // flush_captures() or shutdown()). Afterwards the per-frame hook runs with the number of
  // the frame just presented (1-based).
  void present(Image& screen, const std::string& capture_path = {});

  void set_capture_schedule(CaptureSchedule s) { capture_ = std::move(s); }
  std::function<void(uint64_t frame)> on_frame_presented;
  uint64_t frames_presented() const { return presented_; }
  // GPU time of the last retired frame's command buffer in nanoseconds (timestamps at its
  // start and end; a frame split by a mid-frame resource retirement counts its last part);
  // 0 when the queue has no timestamps.
  uint32_t gpu_frame_ns() const { return gpu_frame_ns_.load(); }
  // Retires the frame still in flight (writing its captures) and waits for PNG captures
  // still being encoded on worker threads.
  void flush_captures();

  uint32_t memory_type(uint32_t bits, VkMemoryPropertyFlags props) const;

  // Records `fill` into a one-off command buffer, submits it and waits for it to finish
  // (under the record lock, so it never interleaves with the frame's queue submissions).
  // For work libScePsm runs to completion on the calling thread (the GPU texture compressor).
  void run_now(const std::function<void(VkCommandBuffer)>& fill);

  // Serialises command recording. The frame's single command buffer is shared by every
  // caller: the render thread's draws and the texture uploads libScePsm performs on the
  // threads that create textures (asset loaders, text renderer). The console needs no such
  // lock for uploads (they are CPU writes to GPU-visible memory); the host records them.
  std::recursive_mutex& record_lock() { return record_lock_; }

 private:
  std::recursive_mutex record_lock_;
  bool create_swapchain();
  void begin_frame();
  void pace_frame();
  void retire(uint32_t slot);  // waits for the slot's frame and finishes its captures

  VkInstance instance_ = VK_NULL_HANDLE;
  VkSurfaceKHR surface_ = VK_NULL_HANDLE;
  VkPhysicalDevice physical_ = VK_NULL_HANDLE;
  VkDevice device_ = VK_NULL_HANDLE;
  VkQueue queue_ = VK_NULL_HANDLE;
  uint32_t queue_family_ = 0;
  VkSwapchainKHR swapchain_ = VK_NULL_HANDLE;
  VkFormat swap_format_ = VK_FORMAT_UNDEFINED;
  VkExtent2D swap_extent_{};
  std::vector<VkImage> swap_images_;
  std::vector<VkImageLayout> swap_layouts_;
  VkCommandPool pool_ = VK_NULL_HANDLE;
  // The two frame slots (see the header comment); slot_ is the one being recorded.
  struct FrameSlot {
    VkCommandBuffer cb = VK_NULL_HANDLE;
    VkFence fence = VK_NULL_HANDLE;
    VkSemaphore acquired = VK_NULL_HANDLE;
    bool submitted = false;
    std::vector<Buffer> overflow;  // ring spills, released when the slot retires
    // Capture of the submitted frame, written when it retires.
    uint64_t frame = 0;
    Slice readback{};
    uint32_t width = 0, height = 0;
    VkFormat format = VK_FORMAT_UNDEFINED;
    std::string bmp_path;
    bool png = false;
  };
  FrameSlot slots_[2];
  uint32_t slot_ = 0;
  VkCommandBuffer cb_ = VK_NULL_HANDLE;  // slots_[slot_].cb
  std::vector<VkSemaphore> finished_;    // per swapchain image
  bool in_frame_ = false;
  Buffer ring_;                          // halves: one per frame slot
  VkDeviceSize ring_head_ = 0;           // offset in the current slot's half
  bool rendering_ = false;
  Image* target_color_ = nullptr;
  Image* target_depth_ = nullptr;
  VkPhysicalDeviceMemoryProperties mem_props_{};
  HWND hwnd_ = nullptr;
  bool headless_ = false;
  HANDLE pace_timer_ = nullptr;     // headless frame pacing
  int64_t next_frame_qpc_ = 0;
  uint64_t presented_ = 0;
  VkQueryPool timestamps_ = VK_NULL_HANDLE;
  float timestamp_period_ = 1.0f;
  std::atomic<uint32_t> gpu_frame_ns_{0};
  CaptureSchedule capture_;
  std::mutex captures_mu_;
  std::vector<std::thread> captures_;
};

Device& device();

}  // namespace ssn::gfx
