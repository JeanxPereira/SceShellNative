// Sce.PlayStation.Core.Imaging.Font text engine (libScePsm.sprx, FW 4.03). See
// font_engine.h for the object layout this mirrors; every routine names the native
// function it reproduces. Built only with the vendored FreeType/HarfBuzz
// (third_party/fonts.cmake defines SSN_HAVE_PSM_TEXT).
#if defined(SSN_HAVE_PSM_TEXT)

#include "libScePsm/text/font_engine.h"

#include <bit>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <list>
#include <map>
#include <mutex>
#include <unordered_map>
#include <vector>

#include <ft2build.h>
#include FT_FREETYPE_H
#include FT_OUTLINE_H

#include <hb-ft.h>
#include <hb.h>
#define HB_DONT_DEFINE_STDINT
#include <ucdn.h>

#include "core/log.h"
#include "host/boundary.h"
#include "host/context.h"
#include "libScePsm/error.h"
#include "libScePsm/imaging/psm_common.h"

namespace ssn::psm::text {
namespace {

constexpr float kOneOver64 = 0.015625f;  // dword_350154

// ---------------------------------------------------------------------------------------
// Alias table qword_53FCE0 (9 rows x 184 bytes), filled by sub_29B020 at module init.
//   +0/+24/+48    file names Light / Medium / Bold (24-byte strings)
//   +72/+96/+120  the italic-style slots for Light / Medium / Bold
//   +144/+148/+152 per-weight size scale (Light / Medium / Bold)
//   +156          font2 flag: file under /common_ex/font2 instead of /common/font
//   +160..+176    synthetic-emphasis divisors: Light, Medium, Bold, UI3Light, UI3Bold
struct AliasRow {
  const char* file[6];
  float scale[3];
  bool font2;
  int32_t divisor[5];
};
float f32(uint32_t bits) { return std::bit_cast<float>(bits); }

const AliasRow& alias_row(uint32_t i) {
  static const AliasRow kRows[9] = {
      {{"SST-Light.otf", "SST-Medium.otf", "SST-Bold.otf", "SST-LightItalic.otf", "SST-MediumItalic.otf",
        "SST-BoldItalic.otf"},
       {f32(0x3F800000), f32(0x3F800000), f32(0x3F800000)}, false, {0, 0, 36, 0, -80}},
      {{"SIE-RDC-Pr5N-M-JPN.otf", "SIE-RDC-Pr5N-B-JPN.otf", "SIE-RDC-Pr5N-B-JPN.otf", "SIE-RDC-Pr5N-M-JPN.otf",
        "SIE-RDC-Pr5N-B-JPN.otf", "SIE-RDC-Pr5N-B-JPN.otf"},
       {f32(0x3F68F5C3), f32(0x3F72FA2F), f32(0x3F72FA2F)}, false, {0, -72, 40, 0, -72}},
      {{"SSTArabic-Light.otf", "SSTArabic-Medium.otf", "SSTArabic-Bold.otf", "SSTArabic-Light.otf",
        "SSTArabic-Medium.otf", "SSTArabic-Bold.otf"},
       {f32(0x3F6147AE), f32(0x3F6147AE), f32(0x3F6147AE)}, false, {0, 0, 32, 0, -96}},
      {{"DFHEI5-SONY.TTF", "DFHEI5-SONY.TTF", "DFHEI5-SONY.TTF", "DFHEI5-SONY.TTF", "DFHEI5-SONY.TTF",
        "DFHEI5-SONY.TTF"},
       {f32(0x3F6E0000), f32(0x3F6E0000), f32(0x3F6E0000)}, false, {-48, 48, 20, -32, 32}},
      {{"SCEPS4Yoongd-Light.otf", "SCEPS4Yoongd-Medium.otf", "SCEPS4Yoongd-Bold.otf", "SCEPS4Yoongd-Light.otf",
        "SCEPS4Yoongd-Medium.otf", "SCEPS4Yoongd-Bold.otf"},
       {f32(0x3F6E0000), f32(0x3F6E0000), f32(0x3F6E0000)}, false, {0, 0, 24, -88, 0}},
      {{"SSTThai-Light.otf", "SSTThai-Medium.otf", "SSTThai-Bold.otf", "SSTThai-Light.otf", "SSTThai-Medium.otf",
        "SSTThai-Bold.otf"},
       {f32(0x3F6147AE), f32(0x3F6147AE), f32(0x3F6147AE)}, false, {0, 0, 40, 0, -80}},
      {{"SSTVietnamese-Light.otf", "SSTVietnamese-Medium.otf", "SSTVietnamese-Bold.otf", "SSTVietnamese-Light.otf",
        "SSTVietnamese-Medium.otf", "SSTVietnamese-Bold.otf"},
       {f32(0x3F800000), f32(0x3F800000), f32(0x3F800000)}, false, {0, 0, 40, 0, -80}},
      {{"PS4Icon.ttf", "PS4Icon.ttf", "PS4Icon.ttf", "PS4Icon.ttf", "PS4Icon.ttf", "PS4Icon.ttf"},
       {f32(0x3F800000), f32(0x3F800000), f32(0x3F800000)}, true, {0, 0, 0, 0, 0}},
      {{"SSTAribStdB24-Regular.ttf", "SSTAribStdB24-Regular.ttf", "SSTAribStdB24-Regular.ttf",
        "SSTAribStdB24-Regular.ttf", "SSTAribStdB24-Regular.ttf", "SSTAribStdB24-Regular.ttf"},
       {f32(0x3F6E0000), f32(0x3F6E0000), f32(0x3F6E0000)}, false, {0, 0, 0, 0, 0}},
  };
  return kRows[i];
}

// Face search order of sub_29CA90: ymmword_43CAD0 (default) / ymmword_43CAA0 (zh-Hant /
// zh-Hans system language, byte_540358), each followed by face 8 (Arib).
constexpr int32_t kFaceOrder[9] = {0, 7, 2, 1, 4, 3, 5, 6, 8};
constexpr int32_t kFaceOrderChinese[9] = {0, 7, 2, 3, 1, 4, 5, 6, 8};
// FontWeight.Bold emphasis divisor per alias row 0..4 (dword_43CAF4); 40 for the others.
constexpr int32_t kBoldDivisor[5] = {36, 40, 32, 20, 24};
// Registered-metrics scales (unk_3505D8 / unk_350730), index 0 with EmojiMetricsStyleUI3.
constexpr float kRegisteredSize[2] = {1.13333332538604736f, 1.09090912342071533f};
constexpr float kRegisteredBearingY[2] = {0.82352942228317261f, 0.83333331346511841f};

// FontWeight / FontStyle values (Core.dll).
constexpr uint32_t kWeightBold = 1, kWeightLight = 2, kWeightMedium = 3, kWeightUI3Light = 4, kWeightUI3Bold = 5,
                   kWeightLegacyBold = 1000;
constexpr uint32_t kStyleItalic = 2;
// FontAttribute bits.
constexpr uint32_t kAttrScaleAdjust = 1, kAttrEmojiUI3 = 2, kAttrFixed26Dot6 = 0x100;

// ---------------------------------------------------------------------------------------
// Global state.
std::recursive_mutex g_lock;     // qword_53FCB8 (taken by every font routine below)
bool g_chinese_locale = false;   // byte_540358

// Font file records (72 bytes, list qword_5406A8 / count dword_5406B0), shared by every
// face that opens the same path.
struct FontFile {
  int32_t refs = 0;           // +0
  int32_t status = 0;         // +4
  std::string path;           // +8
  std::vector<uint8_t> data;  // +32 data object (mmapped file)
  bool loaded = false;
  hb_blob_t* blob = nullptr;  // +48 -> {blob, hb_face}
  hb_face_t* hb_face = nullptr;
  bool has_pair = false;
};
std::list<FontFile*> g_files;

// sub_2C7250: "embed:" names are refused (status 0x80B30013); otherwise the file is opened,
// its size must be 1..0x3000000 bytes, and it is mapped whole ("ScePsmFontPagein").
void load_font_file(FontFile& f, const std::string& console_path) {
  if (console_path.rfind("embed:", 0) == 0) {
    f.status = err::kOutOfMemory;
    return;
  }
  std::ifstream in(f.path, std::ios::binary | std::ios::ate);
  if (!in) {
    f.status = err::kFileNotFound;  // host stand-in for the sceKernelOpen error code
    log::warn("psm.font", "[PSM] sceKernelOpen '{}' ret=0x{:x}", console_path, uint32_t(f.status));
    return;
  }
  const std::streamoff size = in.tellg();
  if (size <= 0) {
    f.status = int32_t(size);
    return;
  }
  if (uint64_t(size) >= 0x3000001ull) {
    f.status = -1;
    return;
  }
  f.data.resize(size_t(size));
  in.seekg(0);
  in.read(reinterpret_cast<char*>(f.data.data()), size);
  f.loaded = bool(in);
}

void destroy_font_file(FontFile* f) {
  if (f->has_pair) {
    hb_face_destroy(f->hb_face);
    hb_blob_destroy(f->blob);
  }
  delete f;
}

// ---------------------------------------------------------------------------------------
// Face (vtable off_4A2540).
struct Face {
  hb_face_t* hb_face = nullptr;  // +8
  hb_font_t* hb_font = nullptr;  // +16
  uint32_t alias = 0;            // +24
  uint32_t style = 0;            // +28
  uint32_t weight = 0;           // +32
  int32_t ascent = 0;            // +36
  int32_t descent = 0;           // +40
  int32_t size_fixed = 0;        // +44
  FontFile* file = nullptr;      // +48

  // hb_ft_font_get_face, as the native tests font->destroy == _hb_ft_font_destroy inline.
  FT_Face ft() const { return hb_font ? hb_ft_font_get_face(hb_font) : nullptr; }

  ~Face() {  // sub_29D3A0
    if (hb_font) hb_font_destroy(hb_font);
    hb_font = nullptr;
    hb_face = nullptr;
    if (FontFile* f = file) {
      if (f->refs-- == 1) {
        g_files.remove(f);
        destroy_font_file(f);
      }
    }
    file = nullptr;
  }

  // sub_29D4A0 (vtable +16): pick the alias row's file for weight/style, map the sandbox
  // path onto the console filesystem and open it through the shared record list.
  bool open() {
    const AliasRow& row = alias_row(alias);
    int slot;
    if (weight == kWeightBold || weight == kWeightMedium)
      slot = style == kStyleItalic ? 4 : 1;
    else if (weight == kWeightUI3Bold)
      slot = style == kStyleItalic ? 5 : 2;
    else
      slot = style == kStyleItalic ? 3 : 0;
    const std::string name = row.file[slot];
    // Native path: "/" + sceKernelGetFsSandboxRandomWord() + ("/common/font/" or, for the
    // font2 rows, "/common_ex/font2/") + name. SceShellCore mounts those sandbox folders
    // with nullfs from /preinst/common/font and /system_ex/common_ex/font2
    // (mount_into_sandbox strings in SceShellCore.elf), which is what the host resolves.
    const std::string console = std::string(row.font2 ? "/system_ex/common_ex/font2/" : "/preinst/common/font/") + name;
    Firmware* fw = host::ctx().firmware;
    if (!fw) {
      log::error("psm.font", "ERROR: sandboxEnabled=false");
      return false;
    }
    const std::string path = fw->host_path(console);
    FontFile* rec = nullptr;
    for (FontFile* f : g_files)
      if (f->path == path) {
        rec = f;
        break;
      }
    if (!rec) {
      rec = new FontFile;
      rec->path = path;
      load_font_file(*rec, console);
      if (rec->loaded) g_files.push_back(rec);
    }
    file = rec;
    const int32_t before = rec->refs++;
    if (!rec->loaded) {
      rec->refs = before;
      if (before == 0) {
        g_files.remove(rec);
        destroy_font_file(rec);
      }
      file = nullptr;
      return false;
    }
    return true;
  }

  // sub_29D990 (vtable +24): FT_Get_Char_Index on the hb-ft face.
  uint32_t char_index(uint32_t cp) const { return FT_Get_Char_Index(ft(), cp); }

  // sub_29D9E0 (vtable +32): FT_Load_Glyph(FT_LOAD_NO_HINTING) plus libScePsm's synthetic
  // emphasis and oblique for outline glyphs.
  FT_Error load_glyph(uint32_t gid) {
    FT_Face face = ft();
    const FT_Error e = FT_Load_Glyph(face, gid, FT_LOAD_NO_HINTING);  // sub_1943F0(face, gid, 2)
    if (e != 0) return e;
    FT_GlyphSlot slot = face->glyph;
    if (slot->format != FT_GLYPH_FORMAT_OUTLINE || alias == 7) return 0;
    const AliasRow& row = alias_row(alias);
    auto strength_base = [&] { return FT_MulFix(face->units_per_EM, face->size->metrics.y_scale); };  // sub_194DB0
    if ((weight & ~1u) == kWeightUI3Light) {  // UI3Light / UI3Bold
      const int32_t d = weight == kWeightUI3Light ? row.divisor[3] : row.divisor[4];
      if (d != 0) embolden(slot, strength_base() / d);
      // LABEL_43: oblique only when the italic file is the regular one.
      if (style == kStyleItalic) {
        const bool same = (weight == kWeightUI3Bold && std::strcmp(row.file[2], row.file[5]) == 0) ||
                          std::strcmp(row.file[0], row.file[3]) == 0;
        if (same) oblique(slot);
      }
      return 0;
    }
    if (weight == kWeightLegacyBold)
      embolden(slot, strength_base() / 24);
    else if (weight == kWeightBold)
      embolden(slot, strength_base() / (alias <= 4 ? kBoldDivisor[alias] : 40));
    // LABEL_22
    switch (alias) {
      case 1:
      case 3:
      case 8:
        if (weight == 0 || weight == kWeightLight) {
          if (alias != 1) embolden(slot, strength_base() / -48);
        } else if (weight == kWeightMedium) {
          embolden(slot, alias == 1 ? strength_base() / -72 : strength_base() / 48);
        }
        [[fallthrough]];
      case 2:
      case 4:
        if (style == kStyleItalic) oblique(slot);  // LABEL_67 -> LABEL_68
        break;
      default:
        break;
    }
    return 0;
  }

  // The emphasis block repeated in sub_29D9E0 (sub_19AE40 = FT_Outline_Embolden).
  static void embolden(FT_GlyphSlot slot, FT_Pos s) {
    FT_Pos s2 = s;
    if (slot->format == FT_GLYPH_FORMAT_OUTLINE) {
      FT_Outline_Embolden(&slot->outline, s);
      s2 = s * 2;
    }
    if (slot->advance.x) slot->advance.x += s2;
    if (slot->advance.y) slot->advance.y += s2;
    slot->metrics.width += s2;
    slot->metrics.height += s2;
    slot->metrics.horiBearingY += s2;
    slot->metrics.horiAdvance += s2;
    slot->metrics.vertBearingX += -(s2 / 2);
    slot->metrics.vertBearingY += s2;
    slot->metrics.vertAdvance += s2;
  }

  // LABEL_68: shear {xx 1.0, xy 0x3A00, yx 0, yy 1.0} (ymmword_34F500, sub_196D30 =
  // FT_Outline_Transform), width += ceil(height * 0.333f) (dword_350170).
  static void oblique(FT_GlyphSlot slot) {
    if (slot->format == FT_GLYPH_FORMAT_OUTLINE) {
      FT_Matrix m{0x10000, 0x3A00, 0, 0x10000};
      FT_Outline_Transform(&slot->outline, &m);
    }
    slot->metrics.width += int32_t(std::ceil(float(slot->metrics.height) * 0.333f));
  }

  // sub_29CBB0: FT_Render_Glyph(NORMAL) and the per-row bitmap_top nudge.
  FT_GlyphSlot render(uint32_t codepoint) {
    FT_GlyphSlot slot = ft()->glyph;
    const FT_Error e = FT_Render_Glyph(slot, FT_RENDER_MODE_NORMAL);  // sub_196FC0(slot, 0)
    if (e != 0) {
      log::warn("psm.font", "FT_Render_Glyph codepoint=0x{:x} ret={}", codepoint, e);
      return nullptr;
    }
    switch (alias) {
      case 1:
        if (codepoint - 0xFF00u >= 0x5Bu) ++slot->bitmap_top;
        break;
      case 2:
        slot->bitmap_top -= int32_t(float(((size_fixed + 32) >> 6) * -276) / 2112.0f);  // dword_350188
        break;
      case 3:
      case 4:
      case 8:
        slot->bitmap_top -= -1;
        break;
      default:
        break;
    }
    return slot;
  }
};

// The {codepoint, glyph index, face} triple the stack routines fill (16 bytes natively).
struct GlyphRef {
  uint32_t codepoint = 0;
  uint32_t gid = 0;
  Face* face = nullptr;
};

// ---------------------------------------------------------------------------------------
// Font stack (vtable off_4A24F0).
struct FontStack {
  std::vector<Face*> faces;  // +8 / +16
  bool arib = false;         // +20
  int32_t size_fixed = 0;    // +24
  uint32_t style = 0;        // +28
  uint32_t weight = 0;       // +32
  uint32_t attribute = 0;    // +36
  int32_t ascent = 0;        // +40
  int32_t descent = 0;       // +44
  int32_t center = 0;        // +48

  ~FontStack() {  // sub_29C130
    for (Face* f : faces) delete f;
  }

  // sub_29CA90 (vtable +24): first face in the locale's search order that maps the code
  // point; otherwise face 0 with glyph 0 (returns false).
  bool get_glyph(uint32_t cp, GlyphRef* r) {
    const int32_t* order = g_chinese_locale ? kFaceOrderChinese : kFaceOrder;
    r->face = nullptr;
    for (size_t i = 0; i < faces.size(); ++i) {
      Face* f = faces[size_t(order[i])];
      r->codepoint = cp;
      r->gid = f->char_index(cp);
      if (r->gid != 0) {
        r->face = f;
        return true;
      }
    }
    r->face = faces[0];
    r->gid = 0;
    return false;
  }

  // sub_29D390 (vtable +32): the glyph's face loads it.
  FT_Error load_glyph(const GlyphRef& r) { return r.face->load_glyph(r.gid); }

  // sub_29C210 (vtable +16).
  bool init() {
    const size_t count = arib ? 9 : 8;
    for (size_t i = 0; i < count; ++i) {
      Face* f = new Face;
      f->alias = uint32_t(i);
      f->style = style;
      f->weight = weight;
      faces.push_back(f);
    }
    for (size_t i = 0; i < count; ++i) {
      Face* f = faces[i];
      if (!f->open()) {
        log::warn("psm.font", "Could not load face {}", i);
        return false;
      }
      FontFile* rec = f->file;
      if (!rec->has_pair) {
        const int32_t len = int32_t(rec->data.size());
        hb_blob_t* blob = len > 0 ? hb_blob_create(reinterpret_cast<const char*>(rec->data.data()), uint32_t(len),
                                                   HB_MEMORY_MODE_READONLY, rec->data.data(), nullptr)
                                  : hb_blob_get_empty();
        hb_face_t* face = hb_face_create(blob, 0);  // sub_2B75D0
        if (!face) {
          set_exception_info("couldn't create face");
          hb_blob_destroy(blob);
          log::warn("psm.font", "Failed new face {}", i);
          return false;
        }
        rec->blob = blob;
        rec->hb_face = face;
        rec->has_pair = true;
      }
      f->hb_face = rec->hb_face;
    }
    ascent = 0;
    descent = 0;
    center = 0;
    for (size_t i = 0; i < faces.size(); ++i) {
      Face* f = faces[i];
      const AliasRow& row = alias_row(f->alias);
      float scale;
      if ((attribute & kAttrScaleAdjust) && f->alias == 5)
        scale = f32(0x3F4CCCCD);  // dword_3501DC (0.8)
      else if (f->weight == kWeightBold || f->weight == kWeightMedium)
        scale = row.scale[1];
      else if (f->weight == kWeightUI3Bold)
        scale = row.scale[2];
      else
        scale = row.scale[0];
      const float trial = scale * float(size_fixed);
      // Inline hb_font_create (parent = empty font, klass = empty funcs, scale = upem) on
      // the face (the empty face when there is none), then hb_font_set_scale and
      // hb_ft_font_set_funcs (sub_2C7870: FT_New_Memory_Face on the face blob, Unicode /
      // MS-symbol charmap, FT_Set_Char_Size(|x_scale|, |y_scale|, 0, 0)).
      hb_font_t* font = hb_font_create(f->hb_face ? f->hb_face : hb_face_get_empty());
      const int32_t s = int32_t(trial);
      hb_font_set_scale(font, s, s);
      hb_ft_font_set_funcs(font);
      f->hb_font = font;
      FT_Face ft = f->ft();
      if (!ft) {
        log::warn("psm.font", "hb_ft_font_get_face failed, m_face=[{}], sizeFixed=[{}], scale=[{:f}], trial_size=[{}]",
                  f->alias, size_fixed, double(scale), s);
        set_exception_info("couldn't set font size");
        return false;
      }
      face_extents(*f, ft);
      f->size_fixed = size_fixed;
      if (i != 5 && (i & 0x7FFFFFFB) != 2) {  // Thai, Arabic and Vietnamese rows excluded
        if (ascent < f->ascent) ascent = f->ascent;
        if (descent < f->descent) descent = f->descent;
      }
    }
    // Center: half of (bearingY('T') + bearingY('L') - height('L')), read from the slot
    // of the face that maps 'T' after 'L' was loaded.
    GlyphRef t, l;
    get_glyph('T', &t);
    load_glyph(t);
    FT_GlyphSlot slot = t.face->ft()->glyph;
    const float t_by = float(slot->metrics.horiBearingY) * kOneOver64;
    get_glyph('L', &l);
    load_glyph(l);
    center = int32_t((float(slot->metrics.horiBearingY - slot->metrics.height) * kOneOver64 + t_by) * 0.5f);
    return true;
  }

  // The per-face ascent/descent block of sub_29C210 (0x29c56a..0x29c86c): integer ceil of
  // the FreeType ascender/descender at y_ppem, with the emphasis stroke folded in for the
  // UI3 weights and the Light/Medium rows of aliases 1/3/4/8, and a one-pixel widening for
  // those aliases.
  void face_extents(Face& f, FT_Face ft) {
    const AliasRow& row = alias_row(f.alias);
    const int32_t upem = ft->units_per_EM;
    const int32_t asc = ft->ascender;
    const int32_t desc = ft->descender;
    const int32_t ppem = ft->size->metrics.y_ppem;
    const bool widened = f.alias <= 8 && ((0x11Au >> f.alias) & 1u);
    auto arabic_adjust = [&] { return int32_t(float(((f.size_fixed + 32) >> 6) * -276) / 2112.0f); };
    if ((f.weight & ~1u) == kWeightUI3Light) {
      const int32_t d = f.weight == kWeightUI3Light ? row.divisor[3] : row.divisor[4];
      const int32_t stroke = d != 0 ? (2 * upem) / d : 0;
      const int32_t height = asc - desc;
      const int32_t adj = widened ? -1 : (f.alias == 2 ? arabic_adjust() : 0);
      f.descent = (((-desc) * stroke) / height + (upem + ppem * (-desc) - 1)) / upem + adj;
      f.ascent = ((stroke * asc) / height + (upem + ppem * asc - 1)) / upem - adj;
      return;
    }
    int32_t a, d;
    if (widened && (f.weight == kWeightMedium || f.weight == kWeightLight)) {
      const int32_t s = f.weight == kWeightMedium ? int32_t((uint32_t(upem) * 0xAAABu) >> 20) : upem / -24;
      const int32_t height = asc - desc;
      a = ((asc * s) / height + upem - 1 + ppem * asc) / upem;
      d = (upem + ppem * (-desc) - 1 + ((-desc) * s) / height) / upem;
    } else {
      a = (upem + ppem * asc - 1) / upem;
      d = (upem + ~(ppem * desc)) / upem;
    }
    const int32_t adj = widened ? -1 : (f.alias == 2 ? arabic_adjust() : 0);
    f.ascent = a - adj;
    f.descent = d + adj;
  }
};

// ---------------------------------------------------------------------------------------
// Font entries (80 bytes, list qword_53FCA8 / count dword_53FCB0).
struct CacheNode {  // 72-byte glyph-metrics cache node
  FT_Face face = nullptr;  // +16
  uint32_t gid = 0;        // +24
  float m[4] = {};         // +36 width, height, bearingX, bearingY
  float advance = 0;       // +52
  float extra[4] = {};     // +56
};
struct Entry {
  int32_t refs = 0;            // +0
  int32_t status = 0;          // +4
  std::string name;            // +8
  int32_t size_fixed = 0;      // +32
  uint32_t style = 0;          // +36
  uint32_t weight = 0;         // +40
  uint32_t attribute = 0;      // +44
  FontStack* stack = nullptr;  // +48
  std::list<CacheNode> cache;  // +56 / +64
  float colon_nudge = 0;       // +72
  float bar_nudge = 0;         // +76

  ~Entry() { delete stack; }
};
std::list<Entry*> g_entries;

// Registered metrics (std::map<int, CharMetrics>): qword_53FCD0 for EmojiMetricsStyleUI3,
// qword_53FCD8 otherwise.
std::map<int32_t, CharMetrics> g_registered_ui3, g_registered;
std::map<int32_t, CharMetrics>& registered_for(uint32_t attribute) {
  return (attribute & kAttrEmojiUI3) ? g_registered_ui3 : g_registered;
}

// Font objects (40 bytes, vtable off_4A24D0) in the font table xmmword_541300.
struct FontObject {
  int32_t refs = 0;       // +8
  Entry* entry = nullptr;  // +32
  ~FontObject();
};
std::mutex g_table_mutex;  // X6AKrGoJOU0_h_A
std::unordered_map<uint32_t, FontObject*> g_table;

// sub_2987F0: drop the entry reference; the last one unlinks and frees the entry.
FontObject::~FontObject() {
  std::lock_guard<std::recursive_mutex> lock(g_lock);
  if (entry && entry->refs-- == 1) {
    g_entries.remove(entry);
    delete entry;
  }
  entry = nullptr;
}

// The table insertion every constructor icall ends with (refcount 1).
uint32_t register_object(FontObject* obj) {
  std::lock_guard<std::mutex> lock(g_table_mutex);
  const uint32_t h = generate_handle([](uint32_t k) { return g_table.count(k) != 0; });
  g_table[h] = obj;
  obj->refs = 1;
  return h;
}

// Lookup + temporary reference, as every Font icall does (unknown -> 0x80B30005). The
// last reference destroys the object and unlinks it, as ReleaseNative (0x2e7ae0) does.
void drop_reference(uint32_t handle, FontObject* obj) {
  bool last = false;
  {
    std::lock_guard<std::mutex> lock(g_table_mutex);
    if (--obj->refs == 0) {
      last = true;
      auto it = g_table.find(handle);
      if (it != g_table.end() && it->second == obj) g_table.erase(it);
    }
  }
  if (last) delete obj;
}
struct FontRef {
  uint32_t handle = 0;
  FontObject* obj = nullptr;
  explicit FontRef(uint32_t h) : handle(h) {
    std::lock_guard<std::mutex> lock(g_table_mutex);
    auto it = g_table.find(h);
    if (it != g_table.end() && it->second) {
      obj = it->second;
      ++obj->refs;
    }
  }
  ~FontRef() {
    if (obj) drop_reference(handle, obj);
  }
  FontRef(const FontRef&) = delete;
  FontRef& operator=(const FontRef&) = delete;
  Entry* entry() const { return obj->entry; }
};

// ---------------------------------------------------------------------------------------
// sub_29A4A0: one character — metrics into `out`, and (with a target) the glyph drawn
// at (x, y) with the font ascent as baseline.
struct Target {
  uint8_t* dst = nullptr;
  int32_t width = 0, height = 0, pitch = 0;
  bool alpha8 = false;
};

void blend_glyph(const Target& t, FT_GlyphSlot slot, FontStack& stack, float x, float y, const int32_t* color) {
  const int32_t rows = int32_t(slot->bitmap.rows);
  const int32_t cols = int32_t(slot->bitmap.width);
  const int32_t bpitch = slot->bitmap.pitch;
  const uint8_t* buf = slot->bitmap.buffer;
  for (int32_t i = 0; i < rows; ++i) {
    const int32_t asc = stack.ascent;
    const int32_t row = int32_t(((float(asc) + y) - float(slot->bitmap_top)) + float(i));
    if (row < 0) continue;
    if (row >= t.height) break;
    for (int32_t j = 0; j < cols; ++j) {
      const int32_t col = int32_t((float(slot->bitmap_left) + x) + float(j));
      if (col < 0) continue;
      if (col >= t.width) break;
      const uint32_t cov = buf[j + i * bpitch];
      if (t.alpha8) {
        uint8_t& d = t.dst[size_t(t.pitch) * row + col];
        uint32_t v = uint32_t(color[3]) * cov;
        const uint32_t old = 255u * d;
        if (old >= v) v = old;
        d = uint8_t(v / 255u);
      } else {
        uint8_t* d = t.dst + 4 * (size_t(t.pitch / 4) * row + col);
        const uint32_t a = uint32_t(color[3]) * cov / 255u;
        const uint32_t px = uint32_t(color[0]) | (uint32_t(color[1]) << 8) | (uint32_t(color[2]) << 16) |
                            ((a + uint32_t(d[3]) * (255u - a) / 255u) << 24);
        std::memcpy(d, &px, 4);
      }
    }
  }
}

bool is_combining_mark(uint32_t c) {  // U+0300, 0301, 0303, 0309, 0323 (0x80000020B)
  const uint32_t i = c - 0x300u;
  return i <= 0x23u && ((0x80000020Bull >> i) & 1ull);
}

void glyph_metrics(Entry& e, const Target* target, float x, float y, uint32_t cp, const int32_t* color,
                   int32_t* max_right, CharMetrics* out, bool first, bool use_cache, bool disable_emoji) {
  FontStack& stack = *e.stack;
  GlyphRef g;
  stack.get_glyph(cp, &g);
  float m[4] = {}, adv = 0, extra[4] = {};
  bool drawable = false;
  bool resolved = false;
  auto& reg = registered_for(e.attribute);
  if (!disable_emoji && ((cp & 0xFFFFFC00u) == 0xEC00u || cp == 0x2120u || cp - 0xF860u < 0x9Fu || int32_t(cp) > 0xFFFF ||
                         cp - 0x2000u <= 0x12FFu)) {
    auto it = reg.find(int32_t(cp));
    if (it != reg.end()) {
      const float aspect = it->second.width / it->second.height;
      const int idx = (e.attribute & kAttrEmojiUI3) ? 0 : 1;
      const float size = float(e.size_fixed) * kOneOver64 * kRegisteredSize[idx];
      m[0] = size * aspect;
      m[1] = size;
      m[2] = 0;
      m[3] = size * kRegisteredBearingY[idx];
      adv = size * aspect;
      resolved = true;  // drawable stays false: registered characters are not drawn here
    }
  }
  if (!resolved) {
    FT_Face key = g.face->ft();
    if (max_right || use_cache) {
      for (const CacheNode& n : e.cache)
        if (n.face == key && n.gid == g.gid) {
          std::memcpy(m, n.m, sizeof m);
          adv = n.advance;
          std::memcpy(extra, n.extra, sizeof extra);
          drawable = resolved = true;
          break;
        }
    }
    if (!resolved) {
      if (stack.load_glyph(g) != 0) return;
      FT_GlyphSlot slot = g.face->ft()->glyph;
      m[0] = float(slot->metrics.width) * kOneOver64;
      m[1] = float(slot->metrics.height) * kOneOver64;
      m[2] = float(slot->metrics.horiBearingX) * kOneOver64;
      m[3] = float(slot->metrics.horiBearingY) * kOneOver64;
      adv = float(slot->metrics.horiAdvance) * kOneOver64;
      while (e.cache.size() >= 101) e.cache.pop_front();
      CacheNode n;
      n.face = key;
      n.gid = g.gid;
      std::memcpy(n.m, m, sizeof m);
      n.advance = adv;
      e.cache.push_back(n);
      if (slot->format != FT_GLYPH_FORMAT_OUTLINE) return;
      drawable = true;
    }
  }
  // LABEL_44
  const int32_t asc = stack.ascent;
  const float top = float(asc) + y;
  if (first && 0.0f > m[2]) {
    adv = adv - m[2];
    m[2] = 0;
  }
  *out = CharMetrics{x, top, m[0], m[1], m[2], m[3], adv, {}};
  std::memcpy(out->reserved, extra, sizeof extra);
  if (max_right) {
    int32_t r = int32_t(std::ceil(adv + x));
    const int32_t r2 = int32_t(std::ceil((m[2] + x) + m[0]));
    if (r < r2) r = r2;
    if (r < *max_right) r = *max_right;
    *max_right = r;
  }
  if ((cp & 0xFFFFFFF0u) != 0x2000u && cp - 0x205Fu >= 0x11u && drawable && target && target->dst) {
    FT_GlyphSlot slot = g.face->render(g.codepoint);
    if (!slot) return;
    if (is_combining_mark(cp)) slot->bitmap_left = 0;
    if (first && slot->bitmap_left < 0) slot->bitmap_left = 0;
    blend_glyph(*target, slot, stack, x, y, color);
  }
}

// ---------------------------------------------------------------------------------------
// HarfBuzz paths of sub_298A50 (right-to-left / Arabic, Thai).
struct Shaped {
  hb_buffer_t* buf = nullptr;
  hb_glyph_info_t* info = nullptr;
  hb_glyph_position_t* pos = nullptr;
  unsigned len = 0;
  ~Shaped() {
    if (buf) hb_buffer_destroy(buf);  // sub_29EC50
  }
};

// Inline hb_buffer_create + props + hb_buffer_add_utf16 (sub_29EF60) + hb_shape (plan
// create_cached / _hb_ot_shape / destroy) + hb_buffer_get_glyph_positions.
// props: 2 = RTL/Arab, 1 = LTR, 0 = left unset; guess = hb_buffer_guess_segment_properties.
void shape(Shaped& s, hb_font_t* font, const uint16_t* chars, unsigned offset, int len, int props, bool guess) {
  s.buf = hb_buffer_create();
  hb_buffer_add_utf16(s.buf, chars, -1, offset, len);
  if (props == 2) {
    hb_buffer_set_direction(s.buf, HB_DIRECTION_RTL);
    hb_buffer_set_script(s.buf, HB_SCRIPT_ARABIC);
  } else if (props == 1) {
    hb_buffer_set_direction(s.buf, HB_DIRECTION_LTR);
    if (guess) hb_buffer_guess_segment_properties(s.buf);
  }
  hb_shape(font, s.buf, nullptr, 0);
  s.info = hb_buffer_get_glyph_infos(s.buf, &s.len);
  s.pos = hb_buffer_get_glyph_positions(s.buf, nullptr);
}

int32_t utf16_strlen(const uint16_t* s) {
  int32_t n = 0;
  while (s[n] != 0) ++n;
  return n;
}

// sub_29CC70: shaped width (26.6 sum >> 6) and, optionally, per-cluster advances.
int32_t shaped_width(FontStack& stack, const uint16_t* chars, int32_t offset, int32_t len, CharMetrics* metrics,
                     int32_t metrics_length, int32_t dir) {
  auto put_advance = [&](uint32_t index, float v, bool sub) {
    if (int64_t(index) >= metrics_length) {  // the native writes past the managed array
      host::note_unimplemented("libScePsm Font shaped metrics: cluster index outside the CharMetrics array (0x29cc70)");
      return;
    }
    metrics[index].horizontal_advance = sub ? metrics[index].horizontal_advance - v : v;
  };
  Shaped s;
  Face* main = stack.faces[2];
  if (dir == 2) {
    shape(s, main->hb_font, chars, uint32_t(offset), len, 2, false);
  } else {
    main = stack.faces[5];
    shape(s, main->hb_font, chars, uint32_t(offset), len, 1, true);
  }
  if (s.len == 0) return 0;
  Shaped fb;
  bool have_fb = false;
  uint32_t prev = s.info[0].cluster;
  uint32_t map = uint32_t(len) + ~s.info[0].cluster;
  uint32_t run = 0;
  int32_t total = 0;
  for (unsigned g = 0; g < s.len; ++g) {
    const uint32_t prev_cluster = prev;
    if (g != 0) {
      const uint32_t c = s.info[g].cluster;
      map = prev == c ? map - 1 : ~c + prev + map;
    }
    if (!have_fb && s.info[g].codepoint == 0) {
      shape(fb, stack.faces[1]->hb_font, chars, uint32_t(offset), len, dir == 2 ? 2 : 1, false);
      have_fb = true;
    }
    const hb_glyph_position_t* p = nullptr;
    if (s.info[g].codepoint != 0) {
      p = &s.pos[g];
    } else {
      const uint32_t k = map + g;
      if (k >= fb.len) {  // the native reads past the fallback buffer
        host::note_unimplemented("libScePsm Font shaped width: fallback glyph index outside the buffer (0x29cc70)");
        p = &s.pos[g];
      } else {
        p = &fb.pos[k];
      }
    }
    const int32_t adv = p->x_advance;
    if (metrics) {
      if (dir == 2) {
        put_advance(s.info[g].cluster, float(adv), false);
      } else if (s.pos[g].x_advance != 0) {
        uint32_t c;
        if (g != 0) {
          c = s.info[g].cluster;
          run = prev_cluster == c ? run + 1 : 0;
        } else {
          c = s.info[0].cluster;
          run = 0;
        }
        if (c + run >= uint32_t(offset + len)) {
          put_advance(c, float(adv), false);
        } else {
          put_advance(c + run, float(adv), false);
          if (run + c == 0) {
            hb_glyph_extents_t ext{};
            if (hb_font_get_glyph_extents(main->hb_font, s.info[g].codepoint, &ext) == 1 && ext.x_bearing < 0)
              put_advance(run + s.info[g].cluster, float(ext.x_bearing), true);
          }
        }
      }
    }
    prev = s.info[g].cluster;
    total += adv;
  }
  return total >> 6;
}

// The drawing half of sub_298A50's shaped branch (0x298ef8..0x29a3c9).
void shaped_draw(FontStack& stack, const Target& t, const int32_t* color, const int32_t* position,
                 const uint16_t* chars, int32_t dir) {
  const int32_t strlen16 = utf16_strlen(chars);
  GlyphRef first;
  stack.get_glyph(dir == 2 ? 0x200Fu : uint32_t(chars[0]), &first);
  Face* face = first.face;
  Shaped s;
  shape(s, face->hb_font, chars, 0, strlen16, dir == 2 ? 2 : 1, true);
  if (s.len == 0) return;
  Shaped fb;
  bool have_fb = false;
  const int32_t y = position[1];
  int32_t pen = position[0] << 6;
  int32_t bias = 0;
  uint32_t prev = s.info[0].cluster;
  uint32_t map = uint32_t(strlen16) + ~prev;
  Face* fb_face = stack.faces[1];
  for (unsigned g = 0; g < s.len; ++g) {
    if (g == 0 && dir != 2 && s.info[0].codepoint != 0) {
      hb_glyph_extents_t ext{};
      const hb_bool_t ok = hb_font_get_glyph_extents(face->hb_font, s.info[0].codepoint, &ext);
      int32_t b = ext.x_bearing;
      if (b >= 0) b = bias;
      if (ok == 1) bias = b;
    }
    if (g != 0) {
      const uint32_t c = s.info[g].cluster;
      map = prev == c ? map - 1 : ~c + map + prev;
    }
    if (!have_fb && s.info[g].codepoint == 0) {
      shape(fb, fb_face->hb_font, chars, 0, strlen16, dir == 2 ? 2 : 0, false);
      have_fb = true;
    }
    GlyphRef r;
    FT_GlyphSlot slot = nullptr;
    const hb_glyph_position_t* p = &s.pos[g];
    if (s.info[g].codepoint != 0) {
      r.gid = s.info[g].codepoint;
      face->load_glyph(r.gid);
      slot = face->render(0);
    } else {
      const uint32_t k = g + map;
      const uint32_t fg = k < fb.len ? fb.info[k].codepoint : 0;
      if (k >= fb.len) host::note_unimplemented("libScePsm Image.DrawText shaped: fallback glyph index outside the buffer");
      if (fg == 0) {
        face->load_glyph(0);
        slot = face->render(0);
      } else {
        fb_face->load_glyph(fg);
        slot = fb_face->render(0);
        p = &fb.pos[k];
      }
    }
    if (!slot) {
      log::error("psm.font", "RenderGlyph failed");
      return;
    }
    const int32_t adv = p->x_advance;
    const int32_t x0 = ((s.pos[g].x_offset + pen - bias) >> 6) + (bias != 0 ? 1 : 0);
    const int32_t yoff = s.pos[g].y_offset >> 6;
    const int32_t rows = int32_t(slot->bitmap.rows), cols = int32_t(slot->bitmap.width);
    for (int32_t i = 0; i < rows; ++i) {
      const int32_t asc = stack.ascent;
      const int32_t top = slot->bitmap_top + yoff;
      const int32_t row = asc + i + y - top;
      if (row < 0) continue;
      if (row >= t.height) break;
      for (int32_t j = 0; j < cols; ++j) {
        const int32_t col = slot->bitmap_left + x0 + j;
        if (col < 0) continue;
        if (col >= t.width) break;
        const uint32_t cov = slot->bitmap.buffer[j + i * slot->bitmap.pitch];
        if (t.alpha8) {
          uint8_t& d = t.dst[size_t(t.pitch) * row + col];
          uint32_t v = uint32_t(color[3]) * cov;
          const uint32_t old = 255u * d;
          if (old >= v) v = old;
          d = uint8_t(v / 255u);
        } else {
          uint8_t* d = t.dst + 4 * (size_t(t.pitch / 4) * row + col);
          const uint32_t a = uint32_t(color[3]) * cov / 255u;
          const uint32_t px = uint32_t(color[0]) | (uint32_t(color[1]) << 8) | (uint32_t(color[2]) << 16) |
                              ((a + uint32_t(d[3]) * (255u - a) / 255u) << 24);
          std::memcpy(d, &px, 4);
        }
      }
    }
    pen += adv;
    prev = s.info[g].cluster;
  }
}

// ---------------------------------------------------------------------------------------
// sub_298A50: lay out (and draw) chars[offset, offset + len).
struct Layout {
  const Target* target = nullptr;      // a2..a7 (dst, size, width, height, pitch, A8)
  const int32_t* position = nullptr;   // a8 {x, y}
  const uint16_t* chars = nullptr;     // a9
  int32_t offset = 0, len = 0;         // a10, a11
  const int32_t* color = nullptr;      // a12 {r, g, b, a}
  uint32_t fixed = 0;                  // a13 FixedFontFlag
  bool disable_emoji = false;          // a14
  int32_t* max_right = nullptr;        // a15
  CharMetrics* metrics = nullptr;      // a16
  int32_t metrics_length = 0;          // host: managed array length
  int32_t direction = 0;               // a17 TextDirection
};

// ICU 59's Normalizer2 (NFC, qword_53FCC8, created once under byte_53FCC0) applied to a
// base vowel + Vietnamese tone mark; the composite is used when the result is one code
// unit. ICU is not vendored: the canonical composition of the same Unicode 9 data comes
// from HarfBuzz 1.3.4's ucdn (hb_unicode_compose).
bool compose_vietnamese(uint16_t base, uint16_t mark, uint32_t* out) {
  hb_codepoint_t ab = 0;
  if (!hb_unicode_compose(hb_unicode_funcs_get_default(), base, mark, &ab)) return false;
  if (ab > 0xFFFF) return false;
  *out = ab;
  return true;
}

bool vietnamese_base(uint16_t c) {  // jumptable 0x29a1db + bitmask 0x18003 over U+01A0..U+01B0
  switch (c) {
    case 'A': case 'E': case 'I': case 'O': case 'U': case 'Y':
    case 'a': case 'e': case 'i': case 'o': case 'u': case 'y':
    case 0xC2: case 0xCA: case 0xD4: case 0xE2: case 0xEA: case 0xF4: case 0x102: case 0x103:
      return true;
    default: {
      const uint32_t i = uint32_t(c) - 0x1A0u;
      return i <= 0x10u && ((0x18003u >> i) & 1u);
    }
  }
}

void layout(Entry& e, const Layout& a) {
  FontStack& stack = *e.stack;
  const bool have_dst = a.target && a.target->dst;
  // Pre-scan: RLM or an R/AL bidi-class Arabic-block character -> right-to-left shaping;
  // a Thai letter (U+0E01..0E7F except U+0E3F) -> Thai shaping.
  bool thai = false;
  int32_t dir = a.direction;
  bool rtl = false;
  for (int32_t i = a.offset; i < a.offset + a.len; ++i) {
    const uint16_t c = a.chars[i];
    if (c == 0x200F) {
      rtl = true;
      break;
    }
    if ((c & 0xFF00) == 0x600) {
      const int bc = ucdn_get_bidi_class(c);  // ucdn tables byte_43CE40/word_43DF40/word_440800/byte_446250
      if (unsigned(bc - UCDN_BIDI_CLASS_R) < 2u) {
        rtl = true;
        break;
      }
    } else if (c != 0xE3F && uint16_t(c - 0xE01) < 0x7Fu) {
      thai = true;
      break;
    }
  }
  if (rtl) dir = 2;
  const bool shaped = (dir == 2 || thai) && (have_dst || a.max_right);
  if (shaped) {
    if (!have_dst) {
      *a.max_right = shaped_width(stack, a.chars, a.offset, a.len, a.metrics, a.metrics_length, thai ? 1 : dir);
      return;
    }
    shaped_draw(stack, *a.target, a.color, a.position, a.chars, dir);
    return;
  }

  // Simple path (LABEL_32).
  float x = float(a.position[0]);
  const float y = float(a.position[1]);
  const Target* t = have_dst ? a.target : nullptr;
  CharMetrics tmp{};
  glyph_metrics(e, nullptr, 0, 0, 32, a.color, nullptr, &tmp, false, false, false);
  const float space_adv = tmp.horizontal_advance;
  float maxw = 0;
  float digit_adv[10] = {};
  float comma = 0, period = 0, slash = 0, colon = 0;
  if (a.fixed != 0) {
    if (a.fixed & 2) {
      for (int d = 0; d < 10; ++d) {
        tmp = CharMetrics{};
        glyph_metrics(e, nullptr, 0, 0, uint32_t('0' + d), a.color, nullptr, &tmp, false, false, false);
        digit_adv[d] = tmp.horizontal_advance;
        maxw = digit_adv[d] > maxw ? digit_adv[d] : maxw;
      }
    }
    if (a.fixed & 4) {
      float* slots[4] = {&comma, &period, &slash, &colon};
      const uint32_t cps[4] = {',', '.', '/', ':'};
      for (int k = 0; k < 4; ++k) {
        tmp = CharMetrics{};
        glyph_metrics(e, nullptr, 0, 0, cps[k], a.color, nullptr, &tmp, false, false, false);
        *slots[k] = tmp.horizontal_advance;
        maxw = *slots[k] > maxw ? *slots[k] : maxw;
      }
    }
  }
  const float half_colon = (maxw - colon) * 0.5f;
  const float half_slash = (maxw - slash) * 0.5f;
  const float half_period = (maxw - period) * 0.5f;
  const float half_comma = (maxw - comma) * 0.5f;
  const float half_space = (maxw - space_adv) * 0.5f;
  const float width_f = float(t ? t->width : 0);
  const int32_t end = a.offset + a.len;
  const int32_t last = end - 1;
  const bool measuring = !have_dst;
  for (int32_t i = a.offset; i < end; ++i) {
    CharMetrics out{};
    float dx = 0;
    const uint16_t c = a.chars[i];
    if ((a.fixed & 2) && uint16_t(c - '0') <= 9) dx = (maxw - digit_adv[c - '0']) * 0.5f;
    bool punct = false;
    if (a.fixed & 4) {
      switch (c) {
        case ',': dx = half_comma; punct = true; break;
        case '.': dx = half_period; punct = true; break;
        case '/': dx = half_slash; punct = true; break;
        case ':': dx = half_colon; punct = true; break;
        default: break;
      }
    }
    if (!punct) {
      const float v = c == ' ' ? half_space : dx;
      if (a.fixed & 8) dx = v;
    }
    if (uint16_t(c + 0x769) < 2 || uint16_t(c + 0x1F2) < 2 || (c & 0xFFF0) == 0xF8A0) {
      if (a.metrics) a.metrics[i - a.offset].x = x + dx;
      continue;
    }
    uint32_t cp = c;
    bool combining = false, composed = false;
    if (is_combining_mark(c)) {
      combining = true;
    } else if (i + 1 < end && is_combining_mark(a.chars[i + 1]) && vietnamese_base(c)) {
      uint32_t ab;
      if (compose_vietnamese(c, a.chars[i + 1], &ab)) {
        cp = ab;
        composed = true;
      }
    }
    int32_t j = i;
    if ((cp & 0xFC00) == 0xD800 && i + 1 < end && (a.chars[i + 1] & 0xFC00) == 0xDC00) {
      cp = (uint32_t(a.chars[i]) << 10) + a.chars[i + 1] - 0x35FDC00u;
      j = i + 1;
      if (a.metrics) a.metrics[i - a.offset].x = x + dx;
    }
    float dy = 0;
    if (e.attribute & kAttrEmojiUI3) {
      if (j < last && j > 0 && cp == ':') {
        if (uint16_t(a.chars[j - 1] - '0') <= 9 && uint16_t(a.chars[j + 1] - '0') <= 9) dy = e.colon_nudge;
      } else if (j < last && j > 0 && cp == '|' && a.chars[j - 1] == ' ' && a.chars[j + 1] == ' ') {
        dy = e.bar_nudge;
      }
    }
    glyph_metrics(e, t, x + dx, y - dy, cp, a.color, a.max_right, &out, j == a.offset, a.metrics != nullptr,
                  a.disable_emoji);
    const uint16_t cj = a.chars[j];
    if (((a.fixed & 2) && uint16_t(cj - '0') < 10) ||
        ((a.fixed & 4) && cj <= ':' && ((0x400D00000000000ull >> cj) & 1ull)) || ((a.fixed & 8) && cj == ' ')) {
      out.horizontal_advance = maxw;
      if (j == a.offset && 0.0f > out.horizontal_bearing_x) {
        out.horizontal_advance = maxw - out.horizontal_bearing_x;
        out.horizontal_bearing_x = 0;
      }
    }
    if (combining) out.horizontal_advance = space_adv;
    if (a.metrics) {
      a.metrics[j - a.offset] = out;
      x = x + out.horizontal_advance;
      if (composed) {
        ++j;
        a.metrics[j - a.offset].x = x;
        a.metrics[j - a.offset].horizontal_advance = 0;
      }
    } else {
      x = x + out.horizontal_advance;
      j += composed ? 1 : 0;
    }
    i = j;
    if (!(x < width_f) && !measuring) break;
  }
}

FontStack* stack_of(Entry* e) { return e ? e->stack : nullptr; }

}  // namespace

// =========================================================================================
// sub_298480: find or create the shared entry for (alias name, size, style, weight, attr).
int32_t font_new_from_alias(uint32_t alias, int32_t size, uint32_t style, uint32_t weight, uint32_t attribute,
                            uint32_t* handle) {
  const std::string name = alias == 1 ? "__SystemArib__" : "__System__";
  auto* obj = new FontObject;
  {
    std::lock_guard<std::recursive_mutex> lock(g_lock);
    const int32_t size_fixed = (attribute & kAttrFixed26Dot6) ? size : size << 6;
    const uint32_t attr = attribute | kAttrFixed26Dot6;
    Entry* found = nullptr;
    for (Entry* e : g_entries)
      if (e->name == name && e->size_fixed == size_fixed && e->style == style && e->weight == weight &&
          e->attribute == attr) {
        found = e;
        break;
      }
    if (!found) {
      found = new Entry;
      const bool valid = (attribute & kAttrFixed26Dot6) ? uint32_t(size - 33) < 0xFFE0u : uint32_t(size - 1) < 0x400u;
      if (!valid) {
        found->status = err::kOutOfRange;
      } else {
        found->size_fixed = size_fixed;
        found->attribute = attr;
        found->name = name;
        found->style = style;
        found->weight = weight;
        auto* stack = new FontStack;
        stack->arib = name == "__SystemArib__";
        stack->size_fixed = found->size_fixed;
        stack->style = found->style;
        stack->weight = found->weight;
        stack->attribute = found->attribute;
        if (stack->init()) {
          found->stack = stack;
        } else {
          found->status = kErrorFontSystem;
          delete stack;
        }
      }
      g_entries.push_back(found);
    }
    obj->entry = found;
    ++found->refs;
  }
  const int32_t status = obj->entry->status;
  if (status < 0) {
    delete obj;
    return status;
  }
  *handle = register_object(obj);
  return 0;
}

// sub_2982B0 searches the entry list by file name but, on a miss, only allocates an entry
// without a font stack (status and refcount left uninitialised) and prints "This
// constructor is not used!". Not reproducible; reported as a font-system error.
int32_t font_new_from_filename(const std::string& filename, int32_t size, uint32_t style, uint32_t* handle) {
  (void)filename, (void)size, (void)style, (void)handle;
  log::warn("psm.font", "This constructor is not used!");
  host::note_unimplemented("libScePsm Font::NewFromFilenameSizeStyle (0x2e7590 -> sub_2982B0: entry without font stack)");
  return kErrorFontSystem;
}

void font_add_ref(uint32_t handle) {
  std::lock_guard<std::mutex> lock(g_table_mutex);
  auto it = g_table.find(handle);
  if (it != g_table.end()) ++it->second->refs;
}

void font_release(uint32_t handle) {
  FontObject* obj = nullptr;
  {
    std::lock_guard<std::mutex> lock(g_table_mutex);
    auto it = g_table.find(handle);
    if (it == g_table.end()) return;
    obj = it->second;
  }
  drop_reference(handle, obj);
}

int32_t font_get_name(uint32_t handle, std::string* name) {
  FontRef f(handle);
  if (!f.obj) return kErrorObjectDisposed;
  *name = f.entry()->name;
  return 0;
}

int32_t font_get_size(uint32_t handle, int32_t* size) {
  FontRef f(handle);
  if (!f.obj) return kErrorObjectDisposed;
  *size = (f.entry()->size_fixed + 32) >> 6;
  return 0;
}

int32_t font_get_size_fixed(uint32_t handle, int32_t* size_fixed) {
  FontRef f(handle);
  if (!f.obj) return kErrorObjectDisposed;
  *size_fixed = f.entry()->size_fixed;
  return 0;
}

int32_t font_get_style(uint32_t handle, uint32_t* style) {
  FontRef f(handle);
  if (!f.obj) return kErrorObjectDisposed;
  *style = f.entry()->style;
  return 0;
}

int32_t font_get_weight(uint32_t handle, uint32_t* weight) {
  FontRef f(handle);
  if (!f.obj) return kErrorObjectDisposed;
  *weight = f.entry()->weight;
  return 0;
}

// GetMetrics: stack ascent / descent, (int)(((size + 32) >> 6) * 1.46f) (dword_3503FC), center.
int32_t font_get_metrics(uint32_t handle, FontMetrics* metrics) {
  FontRef f(handle);
  if (!f.obj) return kErrorObjectDisposed;
  FontStack* s = stack_of(f.entry());
  metrics->ascent = s->ascent;
  metrics->descent = s->descent;
  metrics->line_spacing = int32_t(float((f.entry()->size_fixed + 32) >> 6) * f32(0x3FBAE148));
  metrics->center = s->center;
  return 0;
}

int32_t font_get_text_width(uint32_t handle, const Utf16* text, int32_t offset, int32_t len, int32_t* width,
                            CharMetrics* metrics, int32_t metrics_length) {
  FontRef f(handle);
  if (!f.obj) return kErrorObjectDisposed;
  if (!text) return kErrorArgumentNull;
  if ((offset | len) < 0 || text->length < len || text->length - len < offset) return kErrorOutOfRange;
  if (metrics && metrics_length < len) {
    log::warn("psm.font", "[PSM] Buffer overflow detected!");
    return kErrorInvalidOperation;
  }
  // sub_298960: the same range test, then the layout routine in measuring mode with the
  // right edge out-parameter (direction 2 with a metrics array, 1 without).
  if (!(offset >= 0 && offset <= text->length && len >= 0 && len + offset <= text->length)) return kErrorOutOfRange;
  int32_t right = 0;
  const int32_t origin[2] = {0, 0};
  Layout a;
  a.position = origin;
  a.chars = text->chars;
  a.offset = offset;
  a.len = len;
  a.max_right = &right;
  a.metrics = metrics;
  a.metrics_length = metrics_length;
  a.direction = metrics ? 2 : 1;
  {
    std::lock_guard<std::recursive_mutex> lock(g_lock);
    layout(*f.entry(), a);
  }
  *width = right;
  return 0;
}

int32_t font_get_text_metrics(uint32_t handle, const Utf16* text, int32_t offset, int32_t len, int32_t fixed_font_flag,
                              int32_t disable_emoji, CharMetrics* metrics, int32_t metrics_length) {
  FontRef f(handle);
  if (!f.obj) return kErrorObjectDisposed;
  if (!text) return kErrorArgumentNull;
  if ((offset | len) < 0 || text->length < len || text->length - len < offset) return kErrorOutOfRange;
  if (metrics_length < len) {
    log::warn("psm.font", "[PSM] Buffer overflow detected!");
    return kErrorInvalidOperation;
  }
  if (offset < 0 || text->length < offset || len < 0 || len + offset > text->length) return kErrorOutOfRange;
  const int32_t origin[2] = {0, 0};
  Layout a;
  a.position = origin;
  a.chars = text->chars;
  a.offset = offset;
  a.len = len;
  a.fixed = uint32_t(fixed_font_flag);
  a.disable_emoji = disable_emoji == 1;
  a.metrics = metrics;
  a.metrics_length = metrics_length;
  a.direction = 1;
  std::lock_guard<std::recursive_mutex> lock(g_lock);
  layout(*f.entry(), a);
  return 0;
}

// MeasureTextMetricsNative: argument checks, then "[PSM] Font::MeasureTextMetrics is not
// implemented" and SCE_PSM_ERROR_FONT_SYSTEM (the firmware's own behaviour).
int32_t font_measure_text_metrics(uint32_t handle, int32_t metrics_length, const Utf16* text, int32_t offset,
                                  int32_t length) {
  FontRef f(handle);
  if (!f.obj) return kErrorObjectDisposed;
  if (!text) return kErrorArgumentNull;
  if ((offset | length) < 0 || text->length < length || text->length - length < offset) return kErrorOutOfRange;
  if (metrics_length >= length) {
    log::warn("psm.font", "[PSM] Font::MeasureTextMetrics is not implemented");
    return kErrorFontSystem;
  }
  log::warn("psm.font", "[PSM] Buffer overflow detected!");
  return kErrorInvalidOperation;
}

int32_t register_char_metrics(uint32_t alias, int32_t code, const CharMetrics& metrics, uint32_t attribute) {
  if (alias != 0) return kErrorFontSystem;
  std::lock_guard<std::recursive_mutex> lock(g_lock);
  registered_for(attribute)[code] = metrics;
  return 0;
}

int32_t unregister_char_metrics(uint32_t alias, int32_t code, uint32_t attribute) {
  if (alias != 0) return kErrorFontSystem;
  std::lock_guard<std::recursive_mutex> lock(g_lock);
  registered_for(attribute).erase(code);
  return 0;
}

void notify_system_configuration_change(const std::string& locale) {
  g_chinese_locale = locale.compare(0, 7, "zh-Hant") == 0 || locale.compare(0, 7, "zh-Hans") == 0;
}

int32_t draw_text(uint32_t font_handle, const DrawTarget& target, const int32_t color[4], const int32_t position[2],
                  const Utf16* text, uint32_t offset, uint32_t len, int32_t fixed_font_flag, int32_t direction,
                  int32_t disable_emoji) {
  FontRef f(font_handle);
  if (!f.obj) return kErrorObjectDisposed;
  if (!text) return kErrorArgumentNull;
  if (offset + len < offset || offset + len > uint32_t(text->length)) return kErrorOutOfRange;
  bool alpha8;
  if (target.internal_format == 14) {
    alpha8 = true;
  } else if (target.internal_format == 4) {
    alpha8 = false;
  } else {
    set_exception_info("Format has to be ImageFormat.Rgba8888 or ImageFormat.Alpha8: image mode=" +
                       std::to_string(target.internal_format));
    return kErrorInvalidOperation;
  }
  // Pitch handed to the layout: 4 * width for Rgba8888, width for Alpha8.
  const int32_t pitch = alpha8 ? target.width : 4 * target.width;
  if (target.pixels && target.size < size_t(pitch) * size_t(target.height)) {
    host::note_unimplemented("libScePsm Image.DrawText: pixel slot smaller than pitch * height");
    return kErrorImageSystem;
  }
  Target t;
  t.dst = target.pixels;
  t.width = target.width;
  t.height = target.height;
  t.pitch = pitch;
  t.alpha8 = alpha8;
  Layout a;
  a.target = &t;
  a.position = position;
  a.chars = text->chars;
  a.offset = int32_t(offset);
  a.len = int32_t(len);
  a.color = color;
  a.fixed = uint32_t(fixed_font_flag);
  a.disable_emoji = disable_emoji == 1;
  a.direction = direction;
  std::lock_guard<std::recursive_mutex> lock(g_lock);
  layout(*f.entry(), a);
  return 0;
}

}  // namespace ssn::psm::text

#endif  // SSN_HAVE_PSM_TEXT
