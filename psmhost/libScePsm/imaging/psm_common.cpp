#include "libScePsm/imaging/psm_common.h"

#include <atomic>
#include <mutex>

namespace ssn::psm {
namespace {

std::atomic<uint32_t> g_handle_counter{0};  // dword_5423F0 (starts zeroed in .bss)
std::mutex g_exception_mutex;               // qword_5423F8
std::string g_exception_info;               // qword_542400 / qword_542408

// The vpsrlvd/vpsllvd/vpblendd/vpand/vpor sequence of the generator, written out.
uint32_t permute(uint32_t x) {
  return ((x >> 8) & 0x00FF0000u) | ((x >> 16) & 0x000000FFu) | ((x << 16) & 0xFF000000u) | ((x << 8) & 0x0000FF00u);
}

}  // namespace

// Inlined in every libScePsm constructor icall (see psm_common.h for the formula).
uint32_t generate_handle(const std::function<bool(uint32_t)>& in_use) {
  for (;;) {
    uint32_t h;
    do {
      h = permute(g_handle_counter.fetch_add(1) ^ 0x512F967Bu);
    } while (h == 0);
    if (!in_use || !in_use(h)) return h;
  }
}

// sub_2F5A70 (formatted) and the direct qword_542400 stores: replace the message.
void set_exception_info(const std::string& message) {
  std::lock_guard<std::mutex> lock(g_exception_mutex);
  g_exception_info = message;
}

// The "Unknown type" sites (e.g. GetIdCount 0x2fac70) copy the old message and append.
void append_exception_info(const std::string& suffix) {
  std::lock_guard<std::mutex> lock(g_exception_mutex);
  g_exception_info += suffix;
}

std::string get_exception_info() {
  std::lock_guard<std::mutex> lock(g_exception_mutex);
  return g_exception_info;
}

}  // namespace ssn::psm
