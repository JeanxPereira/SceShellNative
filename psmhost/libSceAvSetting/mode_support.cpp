// libSceAvSetting (FW 4.03) capability checks over a SceAvControlMonitorInfo, transcribed
// from the module (addresses are libSceAvSetting.sprx offsets). The video check is the
// tree under sub_58D0: resolution stepping (sub_5CC0, one capability handler and one
// "next resolution" step per refresh rate), colorimetry stepping (sub_61D0 / sub_6EB0),
// encoding and colour-depth stepping (sub_6F70) and the per-encoding depth test
// (sub_7460); sub_5B30 compares the request with the mode the steps resolved.
//
// SceAvControlMonitorInfo offsets read here (the managed struct of Sce.Vsh.SysBridge):
//   +27 flag (bit 0: native info)        +28/+36/+44 60 Hz / 50 Hz / other-rate capability bits
//   +60/+68/+76 3D 60/50/other bits      +100 extended (4:2:0) bits   +108 "original" bits
//   +116 rgb, +117 yuv444, +118 yuv422, +119 xvYCC, +120 other colour spaces (bits = depths:
//        1 24 bpp, 8 30 bpp, 2 36 bpp; +116 bit 4 RGB quantisation selectable; +120 0x10/0x20 BT.2020)
//   +148 support_contents                +152 audioBlockNum, +156 speakerPlacement, +160 audio[]
//   +848/+850/+852 deep colour 4K <30p / 4:2:0 / RGB-4:4:4   +856 HDR eotf bits
//   +869 FreeSync capability, +872 VRR supported
#include "libSceAvSetting/mode_support.h"

#include <cstring>

namespace ssn::avsetting {
namespace {

// The object at 0x1C420 is initialised by 0x5750 with xmmword_14170 = {50, 50, 10, 10}:
// the step limits for resolutions (sub_5CC0), colorimetries (sub_61D0), colour depths and
// encodings (sub_6F70).
constexpr int kResolutionSteps = 50;
constexpr int kColorimetrySteps = 50;
constexpr int kDepthSteps = 10;
constexpr int kEncodingSteps = 10;

enum { kEnc = 0, kRange = 1, kColorimetry = 2, kDepth = 3, kRefresh = 4, kResolution = 5, kContent = 6 };

uint8_t u8(const uint8_t* p, int off) { return p[off]; }
uint32_t u16(const uint8_t* p, int off) {
  uint16_t v;
  std::memcpy(&v, p + off, 2);
  return v;
}
uint32_t u32(const uint8_t* p, int off) {
  uint32_t v;
  std::memcpy(&v, p + off, 4);
  return v;
}
void copy_mode(int32_t* dst, const int32_t* src) { std::memcpy(dst, src, kVideoModeWords * 4); }

// ---- per-encoding colour-depth test ------------------------------------------------------

// sub_7460 (mode, flags): non-zero when the monitor takes `t`'s encoding at `t`'s depth.
uint32_t encoding_depth_ok(const uint8_t* info, const int32_t* t, uint32_t f) {
  const int32_t enc = t[kEnc];
  const int32_t depth = t[kDepth];
  const uint32_t depth_bit = depth == 36 ? 2u : depth == 30 ? 8u : depth == 24 ? 1u : 0u;
  if (static_cast<uint32_t>(enc - 2) < 2) {  // YUV 4:4:4 / 4:2:2
    uint32_t a = (f & 0x10) ? 10u : ((f & 0x400) == 0 ? 11u : 9u);
    if (f & 8) {
      if (enc == 2) {
        const uint32_t w = u16(info, 848);
        if ((w & 2) == 0) {
          if ((w & 1) == 0)
            a &= 1;
          else
            a &= ~6u;
        }
      }
    } else if (f & 4) {
      if (enc == 2) {
        const uint32_t w = u16(info, 852);
        if ((w & 2) == 0) {
          if (w & 1)
            a &= ~6u;
          else
            a &= 1;
        }
        if (f & 0x20) a &= 1;
      } else {
        const int32_t rate = t[kRefresh];
        const int32_t res = t[kResolution];
        int caps_off = -1;
        bool rate_flag = false;
        if (rate == 2)
          caps_off = 36;
        else if (rate == 3 || rate == 9 || rate == 0x8003)
          caps_off = 28;
        else if (rate == 13 || rate == 14 || rate == 0x800D)
          rate_flag = true;
        bool keep = false;
        if (caps_off >= 0) {
          if (res == 13)
            keep = (u32(info, caps_off) & 0x10000) != 0;
          else if (res == 12)
            keep = (u32(info, caps_off) & 0x8000) != 0;
        } else if (rate_flag) {
          keep = (f & 0x400) != 0;
        }
        if (!keep) a &= 1;
      }
    }
    const uint32_t lo = a & 1;
    if ((f & 0x40) == 0) a = lo;
    uint8_t yuv = u8(info, 117);
    if ((f & 0x420) == 0x20) a = lo;
    if ((f & 0x424) == 0x424 && enc != 2) {
      const uint8_t orig = u8(info, 117);
      yuv = orig | 8;
      if ((u8(info, 118) & 1) == 0) yuv = orig;
      if ((orig & 2) == 0) yuv = orig;
    }
    const uint32_t r = a & depth_bit & yuv;
    if (enc == 2) return r;
    return (u8(info, 118) & 1) ? r : 0;
  }
  if (enc == 4) {  // YUV 4:2:0
    if ((f & 2) == 0) return 0;
    uint32_t c;
    if (depth == 30)
      c = 1;
    else if (depth == 48)
      c = 4;
    else if (depth == 36)
      c = 2;
    else
      return (f & 0x10) == 0 ? 1u : 0u;
    return u16(info, 850) & c;
  }
  if (enc == 1) {  // RGB
    uint32_t d = ((f & 0x10) == 0 ? 1u : 0u) | 0xA;
    bool strip = false;  // loc_76D7
    if (f & 8) {
      const uint32_t w = u16(info, 848);
      if ((w & 2) == 0) {
        if (w & 1)
          d &= ~6u;
        else
          d &= 1;
      }
      strip = (f & 0x200) != 0;
    } else if (f & 4) {
      const uint32_t w = u16(info, 852);
      if ((w & 2) == 0) {
        if (w & 1)
          d &= ~6u;
        else
          d &= 1;
      }
      if (f & 0x20) d &= 1;
      strip = (f & 0x200) != 0;
    } else {
      strip = (f & 0x200) != 0;
    }
    if (strip) {
      d &= ~6u;
    } else {
      const uint32_t lo = d & 1;
      if ((f & 0x40) == 0) d = lo;
      if ((f & 0x820) == 0x20) d = lo;
    }
    return u8(info, 116) & d & depth_bit;
  }
  return 0;
}

// sub_6F70: encoding and depth search for one colorimetry; writes the found mode to `m`.
uint32_t encoding_depth_search(const uint8_t* info, int32_t* m, uint32_t f, bool keep_depth_first) {
  if (u8(info, 109) & 0x10) {
    m[kEnc] = 1;
    m[kDepth] = 30;
    return 1;
  }
  const uint8_t col = static_cast<uint8_t>(m[kColorimetry]);
  const int32_t enc = m[kEnc];
  int32_t depth = m[kDepth];
  int32_t e;
  if (col <= 20 && ((0x1D2406u >> col) & 1)) {  // RGB colorimetries
    if (f & 0x401) return 0;
    e = 1;
  } else if (f & 1) {
    e = 4;
  } else if (f & 0x400) {
    e = 3;
  } else if (enc == 1 || enc == 255) {
    e = 2;
  } else {
    e = ((enc ^ 4) | (f & 2)) != 0 ? enc : 2;
  }
  if (depth == 255) {
    depth = 24;
    if (f & 0x40) depth = (f & 0x400) ? 30 : 36;
  }
  int32_t t[kVideoModeWords];
  if (keep_depth_first) {  // r9 != 0: encodings inside, depths outside
    const int32_t e_next = (f & 4) ? 3 : 4;
    for (int outer = 0;; ++outer) {
      int32_t cur_e = e;
      for (int n = -1;;) {
        copy_mode(t, m);
        t[kEnc] = cur_e;
        t[kDepth] = depth;
        if (const uint32_t r = encoding_depth_ok(info, t, f)) {
          copy_mode(m, t);
          return r;
        }
        if (cur_e == 3) {
          cur_e = 4;
          if ((f & 2) == 0) break;
        } else if (cur_e == 2) {
          cur_e = e_next;
          if ((f & 6) == 0) break;
        } else {
          break;
        }
        if (++n >= kEncodingSteps) break;
      }
      if (depth == 24) {
        if ((f & 0x10) == 0) return 0;
        depth = 36;
      } else if (depth == 36) {
        depth = 30;
      } else if (depth == 30) {
        if (f & 0x10) return 0;
        depth = 24;
      } else {
        return 0;
      }
      if (outer >= kDepthSteps) return 0;
    }
  }
  // r9 == 0: depths inside, encodings outside.
  const int32_t first_depth = depth;
  const int32_t e_alt = (f & 4) ? 3 : 4;
  const bool f6 = (f & 6) != 0;
  const uint32_t force36 = ((f & 0x426) ^ 0x426) | static_cast<uint32_t>(depth ^ 30);
  for (int counter = 0;; ++counter) {
    if (((static_cast<uint32_t>(e) ^ 4) | force36) == 0) depth = 36;
    for (int n = -1;;) {
      copy_mode(t, m);
      t[kEnc] = e;
      t[kDepth] = depth;
      if (const uint32_t r = encoding_depth_ok(info, t, f)) {
        copy_mode(m, t);
        return r;
      }
      int32_t nd;
      if (depth == 24) {
        nd = 36;
        if ((f & 0x10) == 0) break;
      } else if (depth == 36) {
        nd = 30;
      } else if (depth == 30) {
        nd = 24;
        if (f & 0x10) break;
      } else {
        break;
      }
      ++n;
      depth = nd;
      if (n >= kEncodingSteps) break;
    }
    if (e == 1 || e == 4) return 0;
    if (e == 2) {
      e = e_alt;
      if (!f6) return 0;
    } else if (e == 3) {
      e = 4;
      if ((f & 2) == 0) return 0;
    }
    if (counter >= kDepthSteps) return 0;
    depth = first_depth;
  }
}

// ---- colorimetry ------------------------------------------------------------------------

// sub_6EB0: is colorimetry t[2] available; *deep = it needs deep colour.
uint32_t colorimetry_ok(const uint8_t* info, const int32_t* t, int32_t* deep) {
  const int32_t c = t[kColorimetry];
  switch (c) {
    case 1: case 2: case 4: case 5:
      *deep = 0;
      return 1;
    case 3: case 6: case 9: case 12: case 15:
      *deep = 0;
      return 0;
    case 7:
      *deep = 0;
      return u8(info, 119) & 1;
    case 8:
      *deep = 0;
      return u8(info, 119) & 2;
    case 10: case 13: case 16: {
      const uint32_t r = u8(info, 120) & 0x20;
      *deep = 1;
      if (c != 13) return r;
      if (r == 0) return 0;
      return (u32(info, 856) >> 2) & 1;
    }
    case 11: case 14: case 17: {
      const uint32_t r = u8(info, 120) & 0x10;
      *deep = 1;
      if (c == 11) return r;
      if (r == 0) return 0;
      return (u32(info, 856) >> 2) & 1;
    }
    case 18:
      *deep = 1;
      return u8(info, 856) & 4;
    case 19: case 20:
      *deep = 0;
      return u32(info, 108) & 0x1000;
    default:
      *deep = 0;
      return 0;
  }
}

// sub_61D0: colorimetry search for one resolution/refresh rate; writes the found mode to `w`.
uint32_t colorimetry_search(const uint8_t* info, int32_t* w, uint32_t f) {
  const uint64_t res1 = static_cast<uint64_t>(static_cast<uint32_t>(w[kResolution])) - 1;
  int32_t c = w[kColorimetry];
  int32_t step11 = 5, step14 = 11;  // var_74 / var_78
  bool def = true;
  if (c == 255) {
    const uint32_t orig = u32(info, 108);
    const int32_t enc = w[kEnc];
    bool rgb = enc == 1;
    if (!rgb) {
      if (enc != 255) {
        c = res1 < 8 ? 4 : 5;
      } else {
        c = 5;
        rgb = (f & 0x400) == 0;
      }
    }
    if (rgb) {
      c = 2;
      if (orig & 0x1000) c = (f & 0x1000) ? 20 : 19;
    }
  } else {
    switch (c) {
      case 3: case 6: {
        const int k = c != 3 ? 1 : 0;
        c = res1 > 7 ? 3 * k + 5 : 3 * k + 4;
        def = false;
        break;
      }
      case 4: case 5: case 7: case 8: case 11: case 14: case 17:
        def = false;
        break;
      case 9: c = 10; break;
      case 12: c = 13; break;
      case 15: c = 16; break;
      default: break;
    }
  }
  if (def) {
    step11 = 2;
    step14 = 10;
  }
  const int32_t step2 = res1 < 8 ? 4 : 5;  // var_7C
  bool v70 = false, v6c = false;
  int32_t t[kVideoModeWords];
  for (int n = 1;; ++n) {
    copy_mode(t, w);
    t[kColorimetry] = c;
    int32_t deep = 0;
    const uint32_t ok = colorimetry_ok(info, t, &deep);
    if (ok) {
      const bool sel = t[kEnc] == 255 && t[kDepth] != 255;
      if (const uint32_t r = encoding_depth_search(info, t, f | (deep != 0 ? 0x10u : 0u), sel)) {
        copy_mode(w, t);
        return r;
      }
    }
    int32_t nc = 2;
    switch (c) {
      case 1: case 18: case 19: case 20:
        break;
      case 2: {
        if ((f & 2) == 0) return 0;
        nc = step2;
        const bool old = v6c;
        v70 = true;
        v6c = false;
        if (old) return 0;
        break;
      }
      case 3: case 6: case 9: case 12: case 15:
        return 0;
      case 4: case 5: {
        const bool old = v70;
        v6c = true;
        v70 = false;
        if (old) return 0;
        break;
      }
      case 7: nc = 4; break;
      case 8: nc = 5; break;
      case 10: nc = 11; break;
      case 11:
        nc = step11;
        if (ok) return 0;
        break;
      case 13: nc = 14; break;
      case 14: case 17:
        nc = step14;
        if (ok) return 0;
        break;
      case 16: nc = 17; break;
      default:
        return 0;
    }
    if (n >= kColorimetrySteps) return 0;
    c = nc;
  }
}

// ---- per-refresh-rate resolution capability and "next resolution" steps ------------------

using Handler = uint32_t (*)(const uint8_t*, const int32_t*, uint32_t*);
using Next = bool (*)(int32_t*);

bool flag_negative(const uint32_t* fl) { return static_cast<int8_t>(*fl & 0xFF) < 0; }

// sub_6490: 23.98 / 24 Hz.
uint32_t caps_24(const uint8_t* info, const int32_t* w, uint32_t* fl) {
  const int32_t s = w[kResolution];
  uint32_t mask = 0, bits = 0;
  int off = 44;
  bool plain = true;
  if (s <= 160) {
    switch (s) {
      case 11: mask = 1; break;
      case 12: mask = 4; off = 100; bits = 12; break;
      case 13: mask = 8; off = 100; bits = 12; break;
      case 14: mask = 0x800; plain = false; break;
      default: mask = 0; break;
    }
  } else if (s == 161) {
    off = 76; mask = 1;
  } else if (s == 163) {
    off = 108; mask = 0x80;
  } else if (s == 193) {
    if ((u8(info, 76) & 1) == 0) return 0;
    off = 108; mask = 1;
  }
  const uint32_t r = mask & u32(info, off);
  if (r == 0) return 0;
  *fl |= plain ? bits : bits + 0x400;
  return r;
}
// sub_6550.
bool next_24(int32_t* m) {
  const int32_t s = m[kResolution];
  if (s > 160) {
    if (s == 161 || s == 193) { m[kResolution] = 11; return true; }
    if (s == 163) { m[kResolution] = 161; return true; }
    return false;
  }
  if (static_cast<uint32_t>(s - 12) < 2) { m[kResolution] = 11; return true; }
  return false;
}

// sub_6660: 50 Hz.
uint32_t caps_50(const uint8_t* info, const int32_t* w, uint32_t* fl) {
  const int32_t s = w[kResolution];
  uint32_t ext = 0, bits = 0, mask = 0;
  int off = 36;
  switch (s) {
    case 3: mask = 16; break;
    case 4: mask = 32; break;
    case 7: mask = 1; break;
    case 8: mask = 2; break;
    case 9: mask = 4; break;
    case 10: mask = 8; break;
    case 11: mask = 0x4000; break;
    case 12: mask = 0x8000; bits = 4; ext = 16; break;
    case 13: mask = 0x10000; bits = 4; ext = 64; break;
    case 160: off = 68; mask = 4; break;
    case 161: off = 68; mask = 0x4000; break;
    case 162: off = 108; mask = 128; break;
    case 192:
      if ((u32(info, 68) & 4) == 0) return 0;
      off = 108; mask = 1;
      break;
    case 193:
      if ((u32(info, 68) & 0x4000) == 0) return 0;
      off = 108; mask = 1;
      break;
    default: break;
  }
  uint32_t r = u32(info, off) & mask;
  const uint32_t e = u32(info, 100) & ext;
  if (e) {
    const uint32_t was0 = r == 0 ? 1u : 0u;
    if (r == 0) r = e;
    bits |= was0 | 2;
  } else if (r == 0) {
    return 0;
  }
  *fl |= bits;
  return r;
}
// sub_6770.
bool next_50(int32_t* m) {
  int32_t& s = m[kResolution];
  switch (s) {
    case 4: case 7: s = 3; return true;
    case 8: s = 7; return true;
    case 9: s = 8; return true;
    case 10: case 160: case 192: s = 9; return true;
    case 11: s = 10; return true;
    case 12: case 13: s = 11; return true;
    case 161: case 162: s = 160; return true;
    case 163: s = 161; return true;
    case 193: s = 192; return true;
    default: return false;
  }
}

// sub_67F0: 59.94 / 60 Hz.
uint32_t caps_60(const uint8_t* info, const int32_t* w, uint32_t* fl) {
  const int32_t s = w[kResolution];
  bool not227 = true;
  uint32_t bits = 0, ext = 0, r = 0;
  switch (s) {
    case 1: r = u32(info, 28) & 16; break;
    case 2: r = u32(info, 28) & 32; break;
    case 5: r = u32(info, 28) & 1; break;
    case 6: r = u32(info, 28) & 2; break;
    case 9: r = u32(info, 28) & 4; break;
    case 10: r = u32(info, 28) & 8; break;
    case 11: r = u32(info, 28) & 0x4000; break;
    case 12: r = u32(info, 28) & 0x8000; bits = 4; ext = 32; break;
    case 13: r = u32(info, 28) & 0x10000; bits = 4; ext = 128; break;
    case 160: r = u32(info, 60) & 4; break;
    case 161: r = u32(info, 60) & 0x4000; break;
    case 162: r = u32(info, 108) & 128; break;
    case 192: r = (u32(info, 60) & 4) ? (u32(info, 108) & 1) : 0; break;
    case 193: r = (u32(info, 60) & 0x4000) ? (u32(info, 108) & 1) : 0; break;
    case 225: r = flag_negative(fl) ? 1 : (u32(info, 108) & 8); break;
    case 227:
      not227 = false;
      r = flag_negative(fl) ? 1 : (u32(info, 108) & 4096);
      break;
    case 226: case 228: r = 1; break;
    default: r = 0; break;
  }
  const uint32_t e = u32(info, 100) & ext;
  if (e) {
    const uint32_t was0 = r == 0 ? 1u : 0u;
    if (r == 0) r = e;
    uint32_t v = bits | was0 | 2;
    if (!not227) v |= 0x200;
    *fl |= v;
    return r;
  }
  if (r == 0) return 0;
  *fl |= not227 ? bits : bits + 512;
  return r;
}
// sub_6960.
bool next_60(int32_t* m) {
  int32_t& s = m[kResolution];
  switch (s) {
    case 2: case 5: s = 1; return true;
    case 6: s = 5; return true;
    case 9: s = 6; return true;
    case 10: case 160: case 192: s = 9; return true;
    case 11: s = 10; return true;
    case 12: case 13: s = 11; return true;
    case 161: case 162: s = 160; return true;
    case 163: s = 161; return true;
    case 193: s = 192; return true;
    case 225: s = 226; return true;
    case 227: s = 228; return true;
    default: return false;
  }
}

// sub_6600: 30 / 29.97 Hz.
uint32_t caps_30(const uint8_t* info, const int32_t* w, uint32_t* fl) {
  const int32_t s = w[kResolution];
  if (s == 12) {
    const uint32_t r = u32(info, 100) & 1;
    if (r) *fl |= 12;
    return r;
  }
  const uint32_t mask = s == 14 ? 0x2000u : 0u;
  const uint32_t r = u32(info, 44) & mask;
  if (r) *fl |= s == 14 ? 1024u : 0u;
  return r;
}
// sub_6650 / sub_65F0.
bool next_none(int32_t*) { return false; }

// sub_65B0: 25 Hz.
uint32_t caps_25(const uint8_t* info, const int32_t* w, uint32_t* fl) {
  const int32_t s = w[kResolution];
  if (s == 12) {
    const uint32_t r = u32(info, 100) & 2;
    if (r) *fl |= 12;
    return r;
  }
  const uint32_t mask = s == 14 ? 4096u : 0u;
  return u32(info, 44) & mask;  // matched: flags |= 0
}

// sub_6AA0: 100 Hz.
uint32_t caps_100(const uint8_t* info, const int32_t* w, uint32_t* fl) {
  const int32_t s = w[kResolution];
  if (s == 11) {
    const uint32_t r = u32(info, 44) & 0x100;
    if (r) *fl |= 32;
    return r;
  }
  if (s == 12) {
    const uint32_t r = u32(info, 44) & 0x200;
    if (r) *fl |= 36;
    return r;
  }
  return 0;
}
// sub_6AE0 / sub_6D60 / sub_6E10.
bool next_12_to_11(int32_t* m) {
  if (m[kResolution] == 12) {
    m[kResolution] = 11;
    return true;
  }
  return false;
}

// sub_6B00: 119.88 Hz.
uint32_t caps_120(const uint8_t* info, const int32_t* w, uint32_t* fl) {
  const int32_t s = w[kResolution];
  uint32_t mask;
  switch (s) {
    case 5: mask = 32; break;
    case 6: mask = 64; break;
    case 9: mask = 16; break;
    case 10: mask = 8; break;
    case 11: mask = 0x80; break;
    case 7: case 8: return 0;
    case 12: break;
    case 224: case 225:
      if (flag_negative(fl)) { *fl |= 32; return 1; }
      if (const uint32_t r = u32(info, 108) & 8) { *fl |= 32; return r; }
      return 0;
    case 226: case 228:
      *fl |= 32;
      return 1;
    case 227:
      if (flag_negative(fl)) { *fl |= 0x220; return 1; }
      if (*fl & 0x100) return 0;
      if (const uint32_t r = u32(info, 108) & 0x1000) { *fl |= 0x220; return r; }
      return 0;
    default: return 0;
  }
  uint32_t r9, base;
  uint32_t hdmi21 = 0, ext420 = 0;  // r10 (non-zero: 4:4:4 at 120 Hz), eax
  bool r8;
  if (s == 12) {
    const uint32_t c444 = u32(info, 44) & 0x400;
    const uint32_t x = u32(info, 100);
    if (c444) {
      ext420 = (x >> 13) & 1;
      hdmi21 = 1;
      r9 = c444;
    } else {
      r9 = x & 0x2000;
      if (r9 == 0) return 0;
      hdmi21 = 0;
      ext420 = 0;
    }
    r8 = c444 != 0;
    base = 36;
  } else {
    r9 = mask & u32(info, 44);
    if (r9 == 0) return 0;
    base = 32;
    r8 = true;
  }
  const uint32_t ecx = ((u32(info, 36) | u32(info, 28)) & 0x18000) ? base + 0x800 : base;
  const uint32_t di = u16(info, 852) & 3;
  uint32_t v;
  if (hdmi21) {
    v = (ext420 ? 2 * (di != 0 ? 1u : 0u) + 0x400 : 0x400u) | ecx;
    if (!r8 && di) v |= 3;
  } else {
    v = ecx;
    if (!r8 && di) v |= 3;
  }
  *fl |= v;
  return r9;
}
// sub_6C90.
bool next_120(int32_t* m) {
  int32_t& s = m[kResolution];
  if (s == 12) { s = 11; return true; }
  if (s == 225) { s = 226; return true; }
  if (s == 227) { s = 228; return true; }
  return false;
}

// sub_6A00: 89.91 Hz (VR view resolutions).
uint32_t caps_90(const uint8_t* info, const int32_t* w, uint32_t* fl) {
  switch (w[kResolution]) {
    case 0xE1:
      if (flag_negative(fl)) { *fl |= 32; return 1; }
      if (const uint32_t r = u32(info, 108) & 8) { *fl |= 32; return r; }
      return 0;
    case 0xE2: case 0xE4:
      *fl |= 32;
      return 1;
    case 0xE3:
      if (*fl & 0x80) { *fl |= 544; return 1; }
      if (*fl & 0x100) return 0;
      if (const uint32_t r = u32(info, 108) & 4096) { *fl |= 544; return r; }
      return 0;
    default:
      return 0;
  }
}
// sub_6A70.
bool next_90(int32_t* m) {
  int32_t& s = m[kResolution];
  if (s == 225) { s = 226; return true; }
  if (s == 227) { s = 228; return true; }
  return false;
}

// sub_6CD0 (refresh 0x8003) / sub_6D80 (0x800D): the 60 Hz / 119.88 Hz checks for 1080p and
// 2160p, kept only with VRR (+872) or FreeSync (+869 bit 0 with context flag 0x2000).
uint32_t caps_vrr60(const uint8_t* info, const int32_t* w, uint32_t* fl) {
  uint32_t v = *fl;
  if (caps_60(info, w, &v) && static_cast<uint32_t>(w[kResolution] - 11) <= 1 &&
      (u8(info, 872) != 0 || ((*fl & 0x2000) && (u8(info, 869) & 1)))) {
    *fl = v;
    return 1;
  }
  return 0;
}
uint32_t caps_vrr120(const uint8_t* info, const int32_t* w, uint32_t* fl) {
  uint32_t v = *fl;
  if (caps_120(info, w, &v) && static_cast<uint32_t>(w[kResolution] - 11) <= 1 &&
      ((*fl & 0x2000) || u8(info, 872) != 0)) {
    *fl = v;
    return 1;
  }
  return 0;
}

// sub_5CC0: resolution search for the requested refresh rate; writes the mode to `m`.
uint32_t resolution_search(const uint8_t* info, const int32_t ctx[6], const int32_t* cur, int32_t* m) {
  int32_t s = m[kResolution];
  if (s <= -2) {
    if (s == static_cast<int32_t>(0x81FFFFFFu)) {  // ANY_S3D
      const int32_t fr = m[kRefresh];
      if (static_cast<uint32_t>(fr - 2) < 2)
        s = ctx[1] == 0 ? 0xA0 : 9;
      else if (fr == 4 || fr == 1)
        s = ctx[1] == 0 ? 0xA1 : 0xB;
      else
        return 0;
    } else if (s == static_cast<int32_t>(0xC1FFFFFFu)) {  // ANY_VR_VIEW
      s = 0xE1;
    } else if (s == static_cast<int32_t>(0xE1FFFFFFu)) {
      s = 0xE3;
    }
  } else if (s > 0x41FFFFFE) {
    if (s == 0x41FFFFFF) {  // ANY_SIMULVIEW
      const int32_t fr = m[kRefresh];
      if (static_cast<uint32_t>(fr - 2) < 2)
        s = ctx[1] == 0 ? 0xC0 : 9;
      else if (fr == 4 || fr == 1)
        s = ctx[1] == 0 ? 0xC1 : 0xB;
      else
        return 0;
    } else if (s == 0x61FFFFF0) {
      s = 12;
    }
  } else if (s == -1) {
    s = cur[kResolution];
  } else if (s == 0x21FFFFFF) {  // ANY_HFR
    s = 12;
  }
  m[kResolution] = s;

  Handler caps = caps_24;
  Next next = next_24;
  switch (static_cast<uint32_t>(m[kRefresh])) {
    case 1: case 4: break;
    case 2: caps = caps_50; next = next_50; break;
    case 3: case 9: caps = caps_60; next = next_60; break;
    case 5: case 6: caps = caps_30; next = next_none; break;
    case 7: caps = caps_25; next = next_none; break;
    case 12: caps = caps_100; next = next_12_to_11; break;
    case 13: caps = caps_120; next = next_120; break;
    case 35: caps = caps_90; next = next_90; break;
    case 0x8003: caps = caps_vrr60; next = next_12_to_11; break;
    case 0x800D: caps = caps_vrr120; next = next_12_to_11; break;
    default: return 0;
  }
  int32_t w[kVideoModeWords];
  for (int n = 0;;) {
    copy_mode(w, m);
    uint32_t fl = (ctx[2] != 0 ? 1u : 0u) << 7;
    if (ctx[3] != 0) fl |= 0x100;
    if (ctx[4] != 0) fl |= 0x1000;
    if (ctx[5] != 0) fl |= 0x2000;
    if (caps(info, w, &fl)) {
      if (ctx[0] != 0) fl |= 0x40;
      if (const uint32_t r = colorimetry_search(info, w, fl)) {
        copy_mode(m, w);
        return r;
      }
    }
    if (!next(m)) return 0;
    if (++n >= kResolutionSteps) return 0;
  }
}

// sub_59A0: resolves the request into the mode the monitor would get.
bool resolve(const uint8_t* info, const int32_t ctx[6], const int32_t* cur, const int32_t* mode, int32_t* out) {
  int32_t t[kVideoModeWords];
  copy_mode(t, mode);
  if (t[kRefresh] == -1) t[kRefresh] = cur[kRefresh];
  if (!resolution_search(info, ctx, cur, t)) return false;
  int32_t range = t[kRange];
  if (range == 255) range = 2;
  int32_t nr = static_cast<uint8_t>(t[kEnc]) == 1 ? range : 1;
  if (nr == 2) nr = 2 - ((u8(info, 116) & 4) == 0 ? 1 : 0);
  t[kRange] = nr;

  int32_t ct = t[kContent];
  bool zero = false;
  if (ct == 255) {
    const int32_t mc = mode[kColorimetry];
    const uint8_t lb = static_cast<uint8_t>(mc);
    if (lb <= 0x11 && ((0x249F8u >> lb) & 1))
      zero = true;
    else if (mc == 255 && static_cast<uint32_t>(mode[kEnc] - 2) < 3)
      zero = true;
    else
      ct = cur[kContent];
  }
  if (zero) {
    t[kContent] = 0;
  } else {
    t[kContent] = ct;
    if (ct != 0) {
      uint32_t mask;
      switch (static_cast<uint32_t>(ct)) {
        case 1: case 2: mask = static_cast<uint32_t>(ct); break;
        case 3: mask = 4; break;
        case 4: mask = 8; break;
        default: mask = 0; break;
      }
      if ((mask & u16(info, 148)) == 0) t[kContent] = 0;
    }
  }
  if (static_cast<uint32_t>(t[kEnc] - 3) <= 1) {
    if (t[kColorimetry] == 8)
      t[kColorimetry] = 5;
    else if (t[kColorimetry] == 7)
      t[kColorimetry] = 4;
  }
  copy_mode(out, t);
  return true;
}

// sub_5B30: the request against the resolved mode (255 / -1 fields and the ANY_*
// resolutions match a family).
int32_t matches(const int32_t* req, const int32_t* got) {
  const int32_t c = req[kColorimetry];
  const int32_t gc = got[kColorimetry];
  switch (c) {
    case 3: if ((gc & ~1) != 4) return 0; break;
    case 4: case 5: case 7: case 8: case 10: case 11: case 13: case 14:
      if (c != gc) return 0;
      break;
    case 6: if (static_cast<uint32_t>(gc - 7) >= 2) return 0; break;
    case 9: if ((gc & ~1) != 10) return 0; break;
    case 12: if (static_cast<uint32_t>(gc - 13) >= 2) return 0; break;
    case 15: if ((gc & ~1) != 16) return 0; break;
    default:
      if (c != 255 && c != gc) return 0;
      break;
  }
  const int32_t s = req[kResolution];
  const int32_t gs = got[kResolution];
  if (s == -1) {
  } else if (s == 0x21FFFFFF) {
    if (static_cast<uint32_t>(gs - 11) >= 2) return 0;
  } else if (s == 0x41FFFFFF) {
    if ((gs & ~1) != 0xC0) return 0;
  } else if (s == static_cast<int32_t>(0x81FFFFFFu)) {
    if ((gs & ~1) != 0xA0) return 0;
  } else if (s == static_cast<int32_t>(0xC1FFFFFFu)) {
    if (gs != 0xE1) return 0;
  } else if (s == static_cast<int32_t>(0xE1FFFFFFu)) {
    if (gs != 0xE3) return 0;
  } else if (s != gs) {
    return 0;
  }
  if (req[kEnc] != 255 && req[kEnc] != got[kEnc]) return 0;
  if (req[kRange] != 255 && req[kRange] != got[kRange]) return 0;
  if (req[kDepth] != 255 && req[kDepth] != got[kDepth]) return 0;
  if (req[kRefresh] != -1 && req[kRefresh] != got[kRefresh]) return 0;
  if (req[kContent] == 255) return 1;
  return req[kContent] == got[kContent] ? 1 : 0;
}

// ---- audio ------------------------------------------------------------------------------

// sub_7B40: LPCM speaker layout against the monitor's speaker allocation (+156).
uint32_t speakers_ok(const uint8_t* info, const uint8_t* mode) {
  if (u32(mode, 0) != 1) return 1;
  const uint32_t alloc = u32(info, 156);
  if (alloc == 0) return 1;
  static const uint16_t kLayout[51] = {
      0,    1,    3,    5,    7,    17,   19,   21,   23,   9,    11,   13,   15,   25,   27,   29,   31,
      73,   75,   77,   79,   33,   35,   37,   39,   49,   51,   53,   55,   41,   43,   45,   47,   3077,
      3079, 2565, 2567, 2305, 2307, 2177, 2179, 2581, 2583, 3093, 3095, 3589, 3591, 2309, 2311, 2181, 2183};
  const int32_t layout = static_cast<int32_t>(u32(mode, 16));
  if (layout == -1) return alloc;
  if (layout < 1 || layout > 50) return 0;
  return (kLayout[layout] & ~alloc) == 0 ? 1u : 0u;
}

// sub_7D40: coding type against one short audio descriptor.
bool coding_ok(const uint8_t* mode, const uint8_t* sad) {
  const uint32_t coding = u32(mode, 0);
  uint32_t v = 0;
  uint32_t ext = 4;
  switch (coding) {
    case 1: case 255: v = 1; break;
    case 2: case 3: case 4: case 5: case 6: case 7: case 8: case 9: case 10: case 11: case 12: case 13: case 14:
      v = coding;
      break;
    case 20: case 21: case 22: case 23: case 24: case 26: case 27: case 28: case 29:
      ext = coding - 16;
      return sad[0] == 15 && (ext & ~(u16(sad, 2) >> 3)) == 0;
    case 240: v = 11; break;
    case 241: return sad[0] == 11 && (u16(sad, 2) & 1) != 0;
    case 254: v = 255; break;
    default: v = 0; break;
  }
  return v == sad[0];
}

}  // namespace

int32_t video_mode_supported(const uint8_t* info, const int32_t ctx[6], const int32_t* cur, const int32_t* mode, int32_t* out) {
  if (!info || !cur || !mode) return kErrArg;
  for (int i = 7; i < 12; ++i)
    if (mode[i] != -1) return kErrParam;
  int32_t t[kVideoModeWords];
  if (!resolve(info, ctx, cur, mode, t)) copy_mode(t, cur);
  const int32_t r = matches(mode, t);
  if (out) copy_mode(out, t);
  return r;
}

int32_t audio_mode_supported(const uint8_t* info, const uint8_t* mode) {
  if (!info || !mode) return kErrArg;
  uint64_t options;
  std::memcpy(&options, mode + 32, 8);
  if (static_cast<int32_t>(u32(mode, 40)) != -1 || options != ~0ull) return kErrParam;
  const uint32_t speakers = speakers_ok(info, mode);
  if (!speakers) return 0;
  const uint32_t blocks = u16(info, 152);
  if (blocks == 0) return static_cast<int32_t>(speakers);  // no short audio descriptors: the layout result stands
  const int32_t channel = static_cast<int32_t>(u32(mode, 4));
  const int32_t word = static_cast<int32_t>(u32(mode, 8));
  const int32_t fs = static_cast<int32_t>(u32(mode, 12));
  for (uint32_t i = 0; i < blocks; ++i) {
    const uint8_t* sad = info + 160 + 8 * i;
    if (!coding_ok(mode, sad)) continue;
    bool check_ch = true;
    uint32_t need = 2;
    if (channel >= 3 && channel <= 8)
      need = static_cast<uint32_t>(channel);
    else if (channel != 2 && channel != 255)
      check_ch = false;
    if (check_ch && need > sad[1]) continue;
    if (u32(mode, 0) == 1) {
      uint32_t bits;
      switch (word) {
        case 1: case 15: bits = 1; break;
        case 2: bits = 2; break;
        case 3: bits = 4; break;
        case 255: bits = 0xFFFF; break;
        default: bits = 0; break;
      }
      if ((bits & u16(sad, 2)) == 0) continue;
    } else if (word != 1 && word != 15) {
      continue;
    }
    uint32_t fs_bit;
    switch (fs) {
      case -1: {
        const int32_t all = static_cast<int32_t>(u32(sad, 4));
        if (all == 0) continue;
        return all;
      }
      case 8: fs_bit = 1; break;
      case 9: fs_bit = 2; break;
      case 10: fs_bit = 4; break;
      case 12: fs_bit = 8; break;
      case 13: fs_bit = 16; break;
      case 14: fs_bit = 32; break;
      case 15: fs_bit = 64; break;
      default: fs_bit = 0; break;
    }
    if (const uint32_t r = u32(sad, 4) & fs_bit) return static_cast<int32_t>(r);
  }
  return 0;
}

int32_t hdr_supported(const uint8_t* info) {
  if ((u8(info, 856) & 4) == 0) return 0;
  return (u8(info, 120) & 0x30) != 0 ? 1 : 0;
}

}  // namespace ssn::avsetting
