// Data of the dump's libSceNgs2.native.sprx — see ngs2_image.h.
#include "libSceNgs2/ngs2_image.h"

#include <cstring>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <mutex>

#include "core/log.h"
#include "host/context.h"

namespace ssn::ngs2 {
namespace {

struct ModuleImage {
  bool loaded = false;
  std::vector<uint8_t> file;
  struct Seg {
    uint64_t vaddr, filesz, offset;
  };
  std::vector<Seg> segs;
};

ModuleImage& image() {
  static ModuleImage img;
  static std::once_flag once;
  std::call_once(once, [] {
    Firmware* fw = host::ctx().firmware;
    if (!fw) return;
    const std::string path = fw->host_path("/system/common/lib/libSceNgs2.native.sprx");
    std::ifstream f(std::filesystem::path(path), std::ios::binary);
    if (!f) {
      log::error("ngs2", "cannot read {}", path);
      return;
    }
    img.file.assign(std::istreambuf_iterator<char>(f), std::istreambuf_iterator<char>());
    const auto& d = img.file;
    if (d.size() < 64 || std::memcmp(d.data(), "\x7f" "ELF", 4) != 0 || d[4] != 2) {
      log::error("ngs2", "{} is not a decrypted ELF64 module", path);
      return;
    }
    uint64_t phoff;
    uint16_t phentsize, phnum;
    std::memcpy(&phoff, &d[0x20], 8);
    std::memcpy(&phentsize, &d[0x36], 2);
    std::memcpy(&phnum, &d[0x38], 2);
    for (uint32_t i = 0; i < phnum; ++i) {
      const size_t o = static_cast<size_t>(phoff) + size_t(i) * phentsize;
      if (o + 56 > d.size()) break;
      uint32_t type;
      std::memcpy(&type, &d[o], 4);
      if (type != 1) continue;  // PT_LOAD
      ModuleImage::Seg s{};
      std::memcpy(&s.offset, &d[o + 8], 8);
      std::memcpy(&s.vaddr, &d[o + 16], 8);
      std::memcpy(&s.filesz, &d[o + 32], 8);
      img.segs.push_back(s);
    }
    img.loaded = !img.segs.empty();
  });
  return img;
}

template <class T>
std::vector<T> table(uint64_t va, size_t n) {
  const auto b = module_bytes(va, n * sizeof(T));
  std::vector<T> v;
  if (b.size() != n * sizeof(T)) return v;
  v.resize(n);
  std::memcpy(v.data(), b.data(), b.size());
  return v;
}

}  // namespace

std::vector<uint8_t> module_bytes(uint64_t va, size_t size) {
  const ModuleImage& img = image();
  if (!img.loaded) return {};
  for (const auto& s : img.segs) {
    if (va >= s.vaddr && va + size <= s.vaddr + s.filesz) {
      const uint64_t off = s.offset + (va - s.vaddr);
      if (off + size > img.file.size()) return {};
      return {img.file.begin() + static_cast<ptrdiff_t>(off), img.file.begin() + static_cast<ptrdiff_t>(off + size)};
    }
  }
  return {};
}

const std::vector<uint16_t>& frame_samples_table() {
  static const std::vector<uint16_t> t = table<uint16_t>(0x4A520, 16);
  return t;
}
const std::vector<float>& resampler_fir() {
  static const std::vector<float> t = table<float>(0x6C930, 512 * 4);
  return t;
}
const std::vector<uint16_t>& matrix_index_table() {
  static const std::vector<uint16_t> t = table<uint16_t>(0x577D0, 4 * 81);
  return t;
}
const std::vector<float>& matrix_floats() {
  // The largest index of the four 81-entry blocks plus its 8 x 8 matrix bounds the float table.
  static const std::vector<float> t = [] {
    const auto& idx = matrix_index_table();
    size_t n = 0;
    for (uint16_t i : idx) n = std::max<size_t>(n, size_t(i) + 64);
    return idx.empty() ? std::vector<float>{} : table<float>(0x54890, n);
  }();
  return t;
}

}  // namespace ssn::ngs2
