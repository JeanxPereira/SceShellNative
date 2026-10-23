// Host compositor canvas table (see canvas.h).
#include "libSceComposite/canvas.h"

#include <memory>
#include <mutex>
#include <unordered_map>

namespace ssn::composite {
namespace {

struct Canvas {
  int32_t w = 0, h = 0;
  uint64_t generation = 0;
  std::vector<uint8_t> rgba;
};

std::mutex g_lock;
std::unordered_map<uint32_t, std::shared_ptr<Canvas>> g_canvases;
uint32_t g_next = 0x100;  // HOST VALUE: handle numbering (the compositor's own scheme is not traced)

}  // namespace

uint32_t canvas_create() {
  std::lock_guard<std::mutex> l(g_lock);
  uint32_t h;
  do {
    h = g_next++;
  } while (h == 0 || h == 0xFFFFFFFFu || g_canvases.count(h));
  g_canvases.emplace(h, std::make_shared<Canvas>());
  return h;
}

void canvas_destroy(uint32_t handle) {
  std::lock_guard<std::mutex> l(g_lock);
  g_canvases.erase(handle);
}

bool canvas_exists(uint32_t handle) {
  std::lock_guard<std::mutex> l(g_lock);
  return g_canvases.count(handle) != 0;
}

void canvas_publish(uint32_t handle, int32_t w, int32_t h, std::vector<uint8_t>&& rgba) {
  std::lock_guard<std::mutex> l(g_lock);
  auto it = g_canvases.find(handle);
  if (it == g_canvases.end()) return;
  Canvas& c = *it->second;
  c.w = w;
  c.h = h;
  c.rgba = std::move(rgba);
  ++c.generation;
}

bool canvas_take(uint32_t handle, uint64_t& generation, int32_t& w, int32_t& h, std::vector<uint8_t>& rgba) {
  std::lock_guard<std::mutex> l(g_lock);
  auto it = g_canvases.find(handle);
  if (it == g_canvases.end()) return false;
  const Canvas& c = *it->second;
  if (c.generation == generation || c.rgba.empty()) return false;
  generation = c.generation;
  w = c.w;
  h = c.h;
  rgba = c.rgba;
  return true;
}

}  // namespace ssn::composite
