// LightParticle effect timelines (ShellUI eboot, FW 4.03): the 192-byte object the particle layer
// keeps at +2008 (created in Initialize 0x93AB0, vectors allocated by 0x98430).
//
// An effect is a binary blob embedded in the eboot (name table 0xD60E10, data 0xD60E50, sizes
// 0xBB0D80, 7 entries: coldboot, spread_expanded, spread_expanded_fadeout, bottom_camCal,
// bottom_fadeout, initboot_to_spread_no_movie, initboot_to_bottom_no_movie). It holds no code:
// only timelines of field writes into the emitter parameter blocks the particle shaders read
// (the 248-byte compute block, the 320-byte vertex/pixel block and their "large particle"
// counterparts). Parsed by sub_99280 -> 0x992D0 (name match) -> 0x99AC0 -> 0x99DC0.
#pragma once
#include <array>
#include <cstdint>
#include <string>
#include <vector>

namespace ssn::eboot::bglayer {

// One "set" entry (80 bytes in the eboot): at `time`, write each op into every target block.
struct EffectSetOp {
  uint32_t type;     // +0: 1/4 u64, 2/5 u32, 3/6 u16, 7 f64, 8 f32, 10 masked u32, 11 V# copy
  uint64_t value;    // +8
  uint64_t offset;   // +16: byte offset in the target block
};
struct EffectSet {
  float time = 0.0f;               // +0
  std::vector<int32_t> targets;    // +16 (vector at +24..+32): emitter slots 0..7
  std::vector<EffectSetOp> ops;    // +48 (vector at +56..+64), 24 bytes each
};
// One "interpolation" entry: between start and end, every op is written as from + (to - from) * k.
struct EffectInterpOp {
  uint32_t type;
  uint64_t from, to;  // +8, +16
  uint64_t offset;    // +24
};
struct EffectInterp {
  float start = 0.0f, end = 0.0f;  // +0, +4
  std::vector<int32_t> targets;
  std::vector<EffectInterpOp> ops;  // 32 bytes each
};

class ParticleEffect {
 public:
  // The vectors of the object, by qword index (a1[5] .. a1[20]).
  enum Vec : int {
    kSetCs = 5, kSetVs = 6, kInterpCs = 7, kInterpVs = 8,
    kSetLargeCs = 10, kSetLargeVs = 11, kInterpLargeCs = 12, kInterpLargeVs = 13,
    kInitCs = 16, kInitVs = 17, kInitLargeCs = 18, kInitLargeVs = 19,
  };

  // sub_99280(obj, name): finds the blob called `name` among the 7 and parses it (0x99DC0).
  // Returns false (and leaves the object as it was) when no blob matches or the blob is malformed.
  bool load(const std::string& name);

  // sub_996B0: every set/interp entry of the CS/VS and large CS/VS timelines whose time is at
  // least 5.9999 s moves by (x - 6.0).
  void shift_times(float x);
  // sub_99900(a, b, c): with a > 0 the initial VS count writes (u32 at +32) of vector 17 become 0;
  // every VS interpolation entry is rescaled to t * (writes the count ? b : c) + a.
  void delay_vs(float a, float b, float c);

  // sub_97A70 (= 0x97C30 / 0x97DF0): every entry of `vec` with 0 <= t - time <= window writes its
  // ops into the target blocks. `vsharps` resolves type-11 ops (table entry, slot) -> 16 bytes.
  using VsharpTable = std::array<std::array<const uint8_t*, 4>, 2>;
  void apply_sets(int vec, float t, float window, uint8_t* const* blocks, const VsharpTable& vsharps) const;
  // sub_97FB0 (= 0x98130 / 0x982B0): every entry of `vec` with t >= start and t - dt <= end writes
  // its ops at k = clamp((t - start) / (end - start), 0, 1) (op writer 0x9DC30).
  void apply_interps(int vec, float t, float dt, uint8_t* const* blocks) const;

  // +0xA8 .. +0xB8: five floats read after the target lists.
  float tail[5]{};

 private:
  std::vector<EffectSet> set_[21];
  std::vector<EffectInterp> interp_[21];
};

}  // namespace ssn::eboot::bglayer
