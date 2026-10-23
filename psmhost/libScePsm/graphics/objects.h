// libScePsm graphics objects addressed by handle from managed code (PixelBuffer /
// Texture, FrameBuffer, VertexBuffer, ShaderProgram). libScePsm keeps one hash table
// per kind (0x527420 pixel buffers, 0x527438 shader programs, 0x527440 vertex buffers,
// 0x527448 frame buffers) and hands out non-zero handles derived from a per-table
// counter; the managed side treats handles as opaque. Objects are reference counted
// (AddRef / Delete).
#pragma once
#include <cstdint>
#include <memory>
#include <mutex>
#include <unordered_map>
#include <vector>

#include "gfx/vk_device.h"
#include "libScePsm/graphics/types.h"

namespace ssn::psm::gfx {

struct PixelBuffer {
  int32_t refs = 1;
  uint32_t type = 1;  // PixelBufferType
  int32_t width = 0, height = 0, levels = 1;
  uint32_t format = 0;  // PixelFormat
  uint32_t option = 0;  // PixelBufferOption
  std::string name;
  ssn::gfx::Image image;
  // Sampler state (TextureFilter / TextureWrap encodings, defaults from the native
  // constructor 0x1f52f0: mag = min = Linear, mip = Linear, wrap = ClampToEdge).
  uint8_t mag = 1, min = 1, mip = 1, wrap_s = 0, wrap_t = 0;
  float max_anisotropy = 1.0f;
  // Type 3 (canvas texture, 0x1ece20 case 3): id type +140 (1 AppId, 2 CanvasHandle), id
  // +144, resolved canvas handle +148 (0xFFFFFFFF until 0x1fbf60 resolves it). The host
  // keeps the generation of the compositor frame last copied into `image`.
  uint32_t canvas_id_type = 1, canvas_id = 0, canvas_handle = 0xFFFFFFFFu;
  uint64_t canvas_generation = 0;
};

struct FrameBuffer {
  int32_t refs = 1;
  std::shared_ptr<PixelBuffer> color, depth;
  int32_t color_level = 0, depth_level = 0;
};

// One CPU-side array per vertex format ("stream"); libScePsm allocates the streams
// separately (PsmVertexBuffer::Create 0x1eb3c0) and SetVertices de-interleaves the
// managed array into them.
struct VertexBuffer {
  int32_t refs = 1;
  int32_t vertex_count = 0, index_count = 0;
  std::vector<uint32_t> formats;            // VertexFormat per stream
  std::vector<uint32_t> stream_offsets;     // offset of each element in the interleaved vertex
  uint32_t vertex_stride = 0;               // interleaved stride (sum of rounded element sizes)
  std::vector<std::vector<uint8_t>> streams;
  std::vector<uint16_t> indices;
  int32_t valid_first = 0, valid_end = 0;   // range written by the last SetVertices
};

struct ShaderProgram;  // shader_program.h

class Objects {
 public:
  static Objects& get();

  template <class T>
  struct Table {
    std::unordered_map<int32_t, std::shared_ptr<T>> map;
    int32_t counter = 0;
  };

  int32_t add_pixel_buffer(std::shared_ptr<PixelBuffer> p) { return add(pixel_buffers_, std::move(p)); }
  int32_t add_frame_buffer(std::shared_ptr<FrameBuffer> p) { return add(frame_buffers_, std::move(p)); }
  int32_t add_vertex_buffer(std::shared_ptr<VertexBuffer> p) { return add(vertex_buffers_, std::move(p)); }
  int32_t add_shader_program(std::shared_ptr<ShaderProgram> p) { return add(shader_programs_, std::move(p)); }

  std::shared_ptr<PixelBuffer> pixel_buffer_ref(int32_t h) { return find(pixel_buffers_, h); }
  PixelBuffer* pixel_buffer(int32_t h) { return find(pixel_buffers_, h).get(); }
  FrameBuffer* frame_buffer(int32_t h) { return find(frame_buffers_, h).get(); }
  VertexBuffer* vertex_buffer(int32_t h) { return find(vertex_buffers_, h).get(); }
  ShaderProgram* shader_program(int32_t h) { return find(shader_programs_, h).get(); }

  Table<PixelBuffer>& pixel_buffers() { return pixel_buffers_; }
  Table<FrameBuffer>& frame_buffers() { return frame_buffers_; }
  Table<VertexBuffer>& vertex_buffers() { return vertex_buffers_; }
  Table<ShaderProgram>& shader_programs() { return shader_programs_; }

  std::recursive_mutex& mutex() { return mutex_; }

  template <class T>
  bool add_ref(Table<T>& t, int32_t h) {
    std::lock_guard lock(mutex_);
    auto it = t.map.find(h);
    if (it == t.map.end()) return false;
    ++it->second->refs;
    return true;
  }
  template <class T>
  void release(Table<T>& t, int32_t h) {
    std::lock_guard lock(mutex_);
    auto it = t.map.find(h);
    if (it == t.map.end()) return;
    if (--it->second->refs <= 0) t.map.erase(it);
  }

 private:
  template <class T>
  int32_t add(Table<T>& t, std::shared_ptr<T> p) {
    std::lock_guard lock(mutex_);
    int32_t h;
    do {
      h = ++t.counter;
    } while (h == 0 || t.map.count(h));
    t.map.emplace(h, std::move(p));
    return h;
  }
  template <class T>
  std::shared_ptr<T> find(Table<T>& t, int32_t h) {
    std::lock_guard lock(mutex_);
    auto it = t.map.find(h);
    return it == t.map.end() ? nullptr : it->second;
  }

  std::recursive_mutex mutex_;
  Table<PixelBuffer> pixel_buffers_;
  Table<FrameBuffer> frame_buffers_;
  Table<VertexBuffer> vertex_buffers_;
  Table<ShaderProgram> shader_programs_;
};

// Bits per pixel of each PixelFormat: first byte of the 8-byte per-format records at
// 0x3ef970 in libScePsm.
int32_t pixel_format_bits(uint32_t format);
// Vulkan format + component swizzle used to sample a PixelFormat the way the GPU
// format libScePsm picks for it would.
bool vk_format_for(uint32_t format, VkFormat& out, VkComponentMapping& swizzle);

// Element size in bytes of a VertexFormat as libScePsm sizes a stream element
// (PsmVertexBuffer::Create 0x1eb3c0 with the component-size table at 0x40fb40):
// componentBytes * components * (array count), 3- and 6-byte elements rounded to 4/8.
uint32_t vertex_format_size(uint32_t format);

// Type-3 (canvas) textures: copies the compositor canvas's newest frame into the texture's
// image before it is sampled (canvas_texture.cpp). Must run outside a render pass.
void sync_canvas_texture(PixelBuffer& pb);
bool vk_vertex_format(uint32_t format, VkFormat& out);

}  // namespace ssn::psm::gfx
