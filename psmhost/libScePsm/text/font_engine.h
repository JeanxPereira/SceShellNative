// Sce.PlayStation.Core.Imaging.Font — the text engine of libScePsm.sprx (FW 4.03), without
// the Mono glue (font_icalls.cpp). Built on the FreeType 2.4.11 / HarfBuzz 1.3.4 the
// firmware links statically (third_party/fonts.cmake).
//
// Native object model (libScePsm_dyn.elf):
//   * Font object (40 bytes, vtable off_4A24D0): +8 refcount, +32 font entry. Handles live
//     in the font table xmmword_541300 (shared handle generator, psm_common.h).
//   * Font entry (80 bytes), shared by every Font with the same (name, sizeFixed, style,
//     weight, attribute | 0x100); kept in the list qword_53FCA8 under the lock qword_53FCB8:
//       +0 refcount, +4 status, +8 name ("__System__" / "__SystemArib__"), +32 size (26.6),
//       +36 style, +40 weight, +44 attribute, +48 font stack, +56 glyph-metrics cache
//       (list, at most 100 entries kept), +64 cache count, +72 / +76 two floats (vertical
//       nudges for ':' and '|' with FontAttribute.EmojiMetricsStyleUI3; no writer traced,
//       they keep their creation value 0).
//   * Font stack (56 bytes, vtable off_4A24F0): +8 faces, +16 face count (8, or 9 for
//     __SystemArib__), +20 Arib flag, +24..+39 copy of entry +32..+47, +40 ascent,
//     +44 descent, +48 center.
//   * Face (56 bytes, vtable off_4A2540), one per alias-table row: +8 hb_face, +16 hb_font
//     (hb-ft), +24 alias index, +28 style, +32 weight, +36 ascent, +40 descent,
//     +44 size (26.6), +48 font file record.
#pragma once
#include <cstddef>
#include <cstdint>
#include <string>

namespace ssn::psm::text {

// Managed structs, bit-for-bit (Core.dll).
struct CharMetrics {  // Sce.PlayStation.Core.Imaging.CharMetrics (44 bytes)
  float x, y, width, height, horizontal_bearing_x, horizontal_bearing_y, horizontal_advance;
  int32_t reserved[4];
};
static_assert(sizeof(CharMetrics) == 44);
struct FontMetrics {  // Sce.PlayStation.Core.Imaging.FontMetrics
  int32_t ascent, descent, line_spacing, center;
};

// Font::NewFromAliasSizeStyle (0x2e7760) / NewFromAliasSizeStyleWeight (0x2e78e0) -> sub_298480.
int32_t font_new_from_alias(uint32_t alias, int32_t size, uint32_t style, uint32_t weight, uint32_t attribute,
                            uint32_t* handle);
// Font::NewFromFilenameSizeStyle (0x2e7590) -> sub_2982B0.
int32_t font_new_from_filename(const std::string& filename, int32_t size, uint32_t style, uint32_t* handle);

void font_add_ref(uint32_t handle);  // AddRefNative 0x2e7a70
void font_release(uint32_t handle);  // ReleaseNative 0x2e7ae0

int32_t font_get_name(uint32_t handle, std::string* name);         // GetName 0x2e7bd0
int32_t font_get_size(uint32_t handle, int32_t* size);             // GetSize 0x2e7c90
int32_t font_get_size_fixed(uint32_t handle, int32_t* size_fixed); // GetSizeFixed 0x2e7d40
int32_t font_get_style(uint32_t handle, uint32_t* style);          // GetStyle 0x2e7df0
int32_t font_get_weight(uint32_t handle, uint32_t* weight);        // GetWeight 0x2e7ea0
int32_t font_get_metrics(uint32_t handle, FontMetrics* metrics);   // GetMetrics 0x2e7f50

// A UTF-16 managed string: chars (NUL-terminated, as Mono strings are) and length.
struct Utf16 {
  const uint16_t* chars = nullptr;
  int32_t length = 0;
};

// GetTextWidthNative 0x2e8050 (metrics: null or an array of metrics_length entries).
int32_t font_get_text_width(uint32_t handle, const Utf16* text, int32_t offset, int32_t len, int32_t* width,
                            CharMetrics* metrics, int32_t metrics_length);
// GetTextMetricsNative 0x2e81c0.
int32_t font_get_text_metrics(uint32_t handle, const Utf16* text, int32_t offset, int32_t len, int32_t fixed_font_flag,
                              int32_t disable_emoji, CharMetrics* metrics, int32_t metrics_length);
// MeasureTextMetricsNative 0x2e83a0 (not implemented in libScePsm either).
int32_t font_measure_text_metrics(uint32_t handle, int32_t metrics_length, const Utf16* text, int32_t offset,
                                  int32_t length);

int32_t register_char_metrics(uint32_t alias, int32_t code, const CharMetrics& metrics, uint32_t attribute);  // 0x2e84f0
int32_t unregister_char_metrics(uint32_t alias, int32_t code, uint32_t attribute);                            // 0x2e89b0
// NotifySystemConfigurationChangeNative() 0x2e90f0 / (string) 0x2e9190.
void notify_system_configuration_change(const std::string& locale);

// Image::DrawTextNative 0x2e34e0, after the image side (format / pixel slot) is resolved.
struct DrawTarget {
  uint8_t* pixels = nullptr;  // state +16 (null: image not decoded -> the layout only measures)
  size_t size = 0;            // state +24
  uint32_t internal_format = 0;  // impl +72 (4 Rgba8888, 14 A8; others are refused)
  int32_t width = 0, height = 0;  // impl +96
};
// Font lookup (0x80B30005), text / range checks, the format check, then the layout
// routine — the part of DrawTextNative after its image lookup.
int32_t draw_text(uint32_t font_handle, const DrawTarget& target, const int32_t color[4], const int32_t position[2],
                  const Utf16* text, uint32_t offset, uint32_t len, int32_t fixed_font_flag, int32_t direction,
                  int32_t disable_emoji);

}  // namespace ssn::psm::text
