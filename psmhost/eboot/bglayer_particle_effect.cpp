// LightParticle effect timelines — see bglayer_particle_effect.h.
#include "eboot/bglayer_particle_effect.h"

#include <cmath>
#include <cstring>

#include "core/log.h"
#include "eboot/bglayer.h"

namespace ssn::eboot::bglayer {
namespace {

// 0xD60E50: the blob pointers. The table holds relocated addresses (R_X86_64_RELATIVE slots in
// the file), so the values are taken from the relocated image: 0xBB0DC0 coldboot, 0xBB2D70
// spread_expanded, 0xBB4B70 spread_expanded_fadeout, 0xBB72E0 bottom_camCal, 0xBB9B40
// bottom_fadeout, 0xBBC250 initboot_to_spread_no_movie, 0xBBEBB0 initboot_to_bottom_no_movie.
constexpr uint64_t kBlobVa[7] = {0xBB0DC0, 0xBB2D70, 0xBB4B70, 0xBB72E0, 0xBB9B40, 0xBBC250, 0xBBEBB0};
constexpr uint64_t kBlobSizeVa = 0xBB0D80;  // u64[7], plain data

// The reader functor off_CC8F78 {+0 sub_99D70 read(n): bounded memcpy, +8 sub_999F0 string}.
struct Reader {
  const uint8_t* d;
  size_t size, pos = 0;
  bool read(void* out, size_t n) {  // 0x99D70
    if (pos + n > size) return false;
    std::memcpy(out, d + pos, n);
    pos += n;
    return true;
  }
  bool u32(uint32_t& v) { return read(&v, 4); }
  bool name(std::string& s) {  // 0x999F0: u32 length (clamped to 1024) + bytes, C string
    uint32_t n = 0;
    if (!u32(n)) return false;
    if (n > 1024) n = 1024;
    std::vector<char> b(n + 1, 0);
    if (!read(b.data(), n)) return false;
    s.assign(b.data());
    return true;
  }
};

constexpr int kSetVecs[10] = {5, 6, 10, 11, 14, 16, 17, 18, 19, 20};
constexpr int kInterpVecs[5] = {7, 8, 12, 13, 15};

// 0x9DC30: one interpolation op at k (float), computed in double.
void write_interp(const EffectInterpOp& op, uint8_t* block, float kf) {
  const double k = kf;
  uint8_t* p = block + op.offset;
  switch (op.type) {
    case 1: {  // u64: unsigned conversions (vunpcklps/vsubpd idiom), result converted back unsigned
      const double a = static_cast<double>(op.from), b = static_cast<double>(op.to);
      const double r = (b - a) * k + a;
      const uint64_t v = r >= 9223372036854775808.0 ? (static_cast<uint64_t>(static_cast<int64_t>(r - 9223372036854775808.0)) ^ 0x8000000000000000ull)
                                                  : static_cast<uint64_t>(static_cast<int64_t>(r));
      std::memcpy(p, &v, 8);
      break;
    }
    case 2: {  // u32 values, truncated through a 64-bit conversion
      const double a = static_cast<double>(static_cast<uint32_t>(op.from)), b = static_cast<double>(static_cast<uint32_t>(op.to));
      const uint32_t v = static_cast<uint32_t>(static_cast<int64_t>((b - a) * k + a));
      std::memcpy(p, &v, 4);
      break;
    }
    case 3: case 6: {  // 16-bit (3 zero-extended, 6 sign-extended)
      const int32_t a = op.type == 3 ? static_cast<uint16_t>(op.from) : static_cast<int16_t>(op.from);
      const int32_t b = op.type == 3 ? static_cast<uint16_t>(op.to) : static_cast<int16_t>(op.to);
      const uint16_t v = static_cast<uint16_t>(static_cast<int32_t>((static_cast<double>(b) - a) * k + a));
      std::memcpy(p, &v, 2);
      break;
    }
    case 4: {
      const double a = static_cast<double>(static_cast<int64_t>(op.from)), b = static_cast<double>(static_cast<int64_t>(op.to));
      const int64_t v = static_cast<int64_t>((b - a) * k + a);
      std::memcpy(p, &v, 8);
      break;
    }
    case 5: {
      const double a = static_cast<int32_t>(op.from), b = static_cast<int32_t>(op.to);
      const int32_t v = static_cast<int32_t>((b - a) * k + a);
      std::memcpy(p, &v, 4);
      break;
    }
    case 7: {
      double a, b;
      std::memcpy(&a, &op.from, 8);
      std::memcpy(&b, &op.to, 8);
      const double v = a + (b - a) * k;
      std::memcpy(p, &v, 8);
      break;
    }
    case 8: {
      float a, b;
      const uint32_t fa = static_cast<uint32_t>(op.from), fb = static_cast<uint32_t>(op.to);
      std::memcpy(&a, &fa, 4);
      std::memcpy(&b, &fb, 4);
      const float v = static_cast<float>((static_cast<double>(b) - a) * k + a);
      std::memcpy(p, &v, 4);
      break;
    }
    default: break;
  }
}

}  // namespace

bool ParticleEffect::load(const std::string& wanted) {
  const auto sizes = eboot_bytes(kBlobSizeVa, 7 * 8);
  if (sizes.size() != 56) return false;
  for (int i = 0; i < 7; ++i) {  // 0x992D0
    uint64_t size;
    std::memcpy(&size, sizes.data() + 8 * i, 8);
    const auto blob = eboot_bytes(kBlobVa[i], static_cast<size_t>(size));
    if (blob.size() != size) return false;
    Reader r{blob.data(), blob.size()};
    std::string name;
    if (!r.name(name)) return false;
    if (name != wanted) continue;
    // 0x99AC0: the name again, then a format version (<= 1), then the body (0x99DC0).
    r.pos = 0;
    uint32_t version = 0;
    if (!r.name(name) || !r.u32(version) || version > 1) return false;
    uint32_t header[8];  // 0x9C030: eight u32 the parser only reads past
    for (auto& h : header)
      if (!r.u32(h)) return false;
    std::vector<EffectSet> set[21];
    std::vector<EffectInterp> interp[21];
    struct V9 { uint8_t raw[16]; std::vector<int32_t> targets; uint32_t a0, a8, a4; };
    std::vector<V9> v9;
    struct Meta { uint8_t raw[36]; uint32_t l1, l2; };
    std::vector<Meta> meta;
    uint32_t n;
    for (int v : kSetVecs) {
      if (!r.u32(n)) return false;
      set[v].resize(n);  // sub_9C2A0
    }
    for (int v : kInterpVecs) {
      if (!r.u32(n)) return false;
      interp[v].resize(n);  // sub_9C690
    }
    if (!r.u32(n)) return false;
    v9.resize(n);  // vector 9, 48-byte entries (sub_9CA80)
    if (!r.u32(n)) return false;
    meta.resize(n);  // local 152-byte records (sub_9CDD0): names, discarded after parsing
    std::vector<uint32_t> set_nt[21], set_no[21], int_nt[21], int_no[21];
    for (int v : kSetVecs) {  // 12-byte headers: {time, target count, op count}
      for (auto& e : set[v]) {
        uint8_t h[12];
        if (!r.read(h, 12)) return false;
        std::memcpy(&e.time, h, 4);
        uint32_t nt, no;
        std::memcpy(&nt, h + 4, 4);
        std::memcpy(&no, h + 8, 4);
        e.targets.resize(nt);
        e.ops.resize(no);
      }
    }
    for (int v : kInterpVecs) {  // 16-byte headers: {start, end, target count, op count}
      for (auto& e : interp[v]) {
        uint8_t h[16];
        if (!r.read(h, 16)) return false;
        std::memcpy(&e.start, h, 4);
        std::memcpy(&e.end, h + 4, 4);
        uint32_t nt, no;
        std::memcpy(&nt, h + 8, 4);
        std::memcpy(&no, h + 12, 4);
        e.targets.resize(nt);
        e.ops.resize(no);
      }
    }
    for (auto& e : v9) {
      if (!r.read(e.raw, 16)) return false;
      uint32_t nt;
      std::memcpy(&nt, e.raw + 12, 4);
      e.targets.resize(nt);
    }
    for (auto& e : meta) {
      if (!r.read(e.raw, 36)) return false;
      std::memcpy(&e.l1, e.raw + 28, 4);
      std::memcpy(&e.l2, e.raw + 32, 4);
    }
    for (int v : kSetVecs)  // target lists (sub_9D350)
      for (auto& e : set[v])
        if (!r.read(e.targets.data(), 4 * e.targets.size())) return false;
    for (int v : kInterpVecs)
      for (auto& e : interp[v])
        if (!r.read(e.targets.data(), 4 * e.targets.size())) return false;
    for (auto& e : v9)
      if (!r.read(e.targets.data(), 4 * e.targets.size())) return false;
    float t5[5];
    for (auto& f : t5)  // +0xA8 .. +0xB8
      if (!r.read(&f, 4)) return false;
    for (int v : kSetVecs)  // set ops (sub_9D520), 24 bytes: {u32 type, u32 pad, u64 value, u64 offset}
      for (auto& e : set[v])
        for (auto& op : e.ops) {
          uint8_t b[24];
          if (!r.read(b, 24)) return false;
          std::memcpy(&op.type, b, 4);
          std::memcpy(&op.value, b + 8, 8);
          std::memcpy(&op.offset, b + 16, 8);
        }
    for (int v : kInterpVecs)  // interpolation ops (sub_9D750), 32 bytes
      for (auto& e : interp[v])
        for (auto& op : e.ops) {
          uint8_t b[32];
          if (!r.read(b, 32)) return false;
          std::memcpy(&op.type, b, 4);
          std::memcpy(&op.from, b + 8, 8);
          std::memcpy(&op.to, b + 16, 8);
          std::memcpy(&op.offset, b + 24, 8);
        }
    for (auto& e : v9)  // three u32 into +0, +8, +4
      if (!r.u32(e.a0) || !r.u32(e.a8) || !r.u32(e.a4)) return false;
    for (auto& e : meta) {  // 0x9C150: two strings per record
      std::vector<uint8_t> s(e.l1 + e.l2);
      if (!r.read(s.data(), s.size())) return false;
    }
    for (int v = 0; v < 21; ++v) {
      set_[v] = std::move(set[v]);
      interp_[v] = std::move(interp[v]);
    }
    std::memcpy(tail, t5, sizeof tail);
    if (r.pos != r.size) log::warn("bglayer", "particle effect '{}': {} trailing bytes", wanted, r.size - r.pos);
    return true;
  }
  return false;
}

void ParticleEffect::shift_times(float x) {  // 0x996B0
  const float d = x + -6.0f;                 // dword_AFFDE8
  const float limit = 5.99989986f;           // dword_B00058 (0x40BFFF2E)
  for (int v : {5, 6, 10, 11})
    for (auto& e : set_[v])
      if (!(limit > e.time)) e.time = d + e.time;
  for (int v : {7, 8, 12, 13})
    for (auto& e : interp_[v])
      if (!(limit > e.start)) {
        e.start = d + e.start;
        e.end = d + e.end;
      }
}

void ParticleEffect::delay_vs(float a, float b, float c) {  // 0x99900
  if (a > 0.0f) {
    for (auto& e : set_[kInitVs])
      for (auto& op : e.ops)
        if (op.offset == 32 && op.type == 2) op.value &= 0xFFFFFFFF00000000ull;  // dword +8 = 0
  }
  for (auto& e : interp_[kInterpVs]) {
    bool count = false;
    for (auto& op : e.ops)
      if (op.type == 2 && op.offset == 32) count = true;
    const float s = count ? b : c;
    e.start = s * e.start + a;
    e.end = s * e.end + a;
  }
}

void ParticleEffect::apply_sets(int vec, float t, float window, uint8_t* const* blocks, const VsharpTable& vsharps) const {
  for (const auto& e : set_[vec]) {  // 0x97A70
    const float d = t - e.time;
    if (d < 0.0f || std::isnan(d) || d > window) continue;  // jb / ja
    for (int32_t target : e.targets) {
      uint8_t* blk = blocks[target];
      if (!blk) continue;
      for (const auto& op : e.ops) {
        uint8_t* p = blk + op.offset;
        const uint32_t lo = static_cast<uint32_t>(op.value), hi = static_cast<uint32_t>(op.value >> 32);
        switch (op.type) {
          case 1: case 4: std::memcpy(p, &op.value, 8); break;
          case 2: case 5: std::memcpy(p, &lo, 4); break;
          case 3: case 6: { const uint16_t w = static_cast<uint16_t>(lo); std::memcpy(p, &w, 2); break; }
          case 7: std::memcpy(p, &op.value, 8); break;
          case 8: std::memcpy(p, &lo, 4); break;
          case 10: {  // value | (old & ~mask)
            uint32_t old;
            std::memcpy(&old, p, 4);
            const uint32_t v = lo | (old & ~hi);
            std::memcpy(p, &v, 4);
            break;
          }
          case 11: {  // 16 bytes from the V# table: entry lo, slot hi
            const uint8_t* src = (lo < 2 && hi < 4) ? vsharps[lo][hi] : nullptr;
            if (src) std::memcpy(p, src, 16);
            else log::warn("bglayer", "particle effect: V# copy ({}, {}) has no source", lo, hi);
            break;
          }
          default: break;
        }
      }
    }
  }
}

void ParticleEffect::apply_interps(int vec, float t, float dt, uint8_t* const* blocks) const {
  const float tb = t - dt;
  for (const auto& e : interp_[vec]) {  // 0x97FB0
    if (t < e.start || std::isnan(t) || std::isnan(e.start)) continue;  // vucomiss xmm0, start; jb
    if (tb > e.end) continue;                                            // vucomiss t - dt, end; ja
    float k = (t - e.start) / (e.end - e.start);
    k = std::fmin(k, 1.0f);  // vminss k, 1.0 (NaN k -> 1.0)
    k = std::fmax(k, 0.0f);  // vmaxss k, 0.0
    if (e.targets.empty()) continue;
    for (int32_t target : e.targets) {
      uint8_t* blk = blocks[target];
      if (!blk) continue;
      for (const auto& op : e.ops) write_interp(op, blk, k);
    }
  }
}

}  // namespace ssn::eboot::bglayer
