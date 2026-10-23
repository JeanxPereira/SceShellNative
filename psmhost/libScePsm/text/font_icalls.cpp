// Sce.PlayStation.Core.Imaging.Font icalls and Image::DrawTextNative (libScePsm.sprx,
// FW 4.03): Mono glue over font_engine.cpp. Names are the ones libScePsm registers
// (work/ida/psm_icall_table.tsv); addresses are the icall functions.
#if defined(SSN_HAVE_PSM_TEXT)

#include <cstdint>
#include <string>

#include <mono/metadata/object.h>

#include "common/mono_util.h"
#include "host/boundary.h"
#include "libScePsm/error.h"
#include "libScePsm/imaging/image_core.h"
#include "libScePsm/imaging/psm_common.h"
#include "libScePsm/text/font_engine.h"
#include "libSceRegMgr/regmgr.h"

namespace ssn::psm::text {
namespace {

Utf16 utf16_of(MonoString* s) {
  Utf16 u;
  u.chars = reinterpret_cast<const uint16_t*>(mono_string_chars(s));  // KrITF2YFQH0_F_G
  u.length = mono_string_length(s);                                  // qrLv6QEOWhA_F_G
  return u;
}

// NewFromFilenameSizeStyle(string, int, FontStyle, out int) — 0x2e7590 (null name -> 0x80B30002).
int32_t NewFromFilenameSizeStyle(MonoString* filename, int32_t size, uint32_t style, uint32_t* handle) {
  if (!filename) return kErrorArgumentNull;
  return font_new_from_filename(utf8(filename), size, style, handle);
}
SSN_ICALL("Sce.PlayStation.Core.Imaging.Font::NewFromFilenameSizeStyle(string,int,Sce.PlayStation.Core.Imaging.FontStyle,int&)",
          NewFromFilenameSizeStyle);

// NewFromAliasSizeStyle(FontAlias, int, FontStyle, out int) — 0x2e7760: weight 0, attribute 0.
int32_t NewFromAliasSizeStyle(uint32_t alias, int32_t size, uint32_t style, uint32_t* handle) {
  return font_new_from_alias(alias, size, style, 0, 0, handle);
}
SSN_ICALL("Sce.PlayStation.Core.Imaging.Font::NewFromAliasSizeStyle(Sce.PlayStation.Core.Imaging.FontAlias,int,Sce.PlayStation.Core.Imaging.FontStyle,int&)",
          NewFromAliasSizeStyle);

// NewFromAliasSizeStyleWeight(FontAlias, int, FontStyle, FontWeight, uint, out int) — 0x2e78e0.
int32_t NewFromAliasSizeStyleWeight(uint32_t alias, int32_t size, uint32_t style, uint32_t weight, uint32_t attribute,
                                    uint32_t* handle) {
  return font_new_from_alias(alias, size, style, weight, attribute, handle);
}
SSN_ICALL("Sce.PlayStation.Core.Imaging.Font::NewFromAliasSizeStyleWeight(Sce.PlayStation.Core.Imaging.FontAlias,int,Sce.PlayStation.Core.Imaging.FontStyle,Sce.PlayStation.Core.Imaging.FontWeight,uint,int&)",
          NewFromAliasSizeStyleWeight);

// AddRefNative(int) — 0x2e7a70.
void AddRefNative(uint32_t handle) { font_add_ref(handle); }
SSN_ICALL("Sce.PlayStation.Core.Imaging.Font::AddRefNative(int)", AddRefNative);

// ReleaseNative(int) — 0x2e7ae0.
void ReleaseNative(uint32_t handle) { font_release(handle); }
SSN_ICALL("Sce.PlayStation.Core.Imaging.Font::ReleaseNative(int)", ReleaseNative);

// GetName(int, out string) — 0x2e7bd0.
int32_t GetName(uint32_t handle, MonoString** name) {
  std::string n;
  const int32_t r = font_get_name(handle, &n);
  if (r == 0) store_ref(name, new_string(n));
  return r;
}
SSN_ICALL("Sce.PlayStation.Core.Imaging.Font::GetName(int,string&)", GetName);

// GetSize(int, out int) — 0x2e7c90.
int32_t GetSize(uint32_t handle, int32_t* size) { return font_get_size(handle, size); }
SSN_ICALL("Sce.PlayStation.Core.Imaging.Font::GetSize(int,int&)", GetSize);

// GetSizeFixed(int, out int) — 0x2e7d40.
int32_t GetSizeFixed(uint32_t handle, int32_t* size) { return font_get_size_fixed(handle, size); }
SSN_ICALL("Sce.PlayStation.Core.Imaging.Font::GetSizeFixed(int,int&)", GetSizeFixed);

// GetStyle(int, out FontStyle) — 0x2e7df0.
int32_t GetStyle(uint32_t handle, uint32_t* style) { return font_get_style(handle, style); }
SSN_ICALL("Sce.PlayStation.Core.Imaging.Font::GetStyle(int,Sce.PlayStation.Core.Imaging.FontStyle&)", GetStyle);

// GetWeight(int, out FontWeight) — 0x2e7ea0.
int32_t GetWeight(uint32_t handle, uint32_t* weight) { return font_get_weight(handle, weight); }
SSN_ICALL("Sce.PlayStation.Core.Imaging.Font::GetWeight(int,Sce.PlayStation.Core.Imaging.FontWeight&)", GetWeight);

// GetMetrics(int, out FontMetrics) — 0x2e7f50.
int32_t GetMetrics(uint32_t handle, FontMetrics* metrics) { return font_get_metrics(handle, metrics); }
SSN_ICALL("Sce.PlayStation.Core.Imaging.Font::GetMetrics(int,Sce.PlayStation.Core.Imaging.FontMetrics&)", GetMetrics);

CharMetrics* metrics_of(MonoArray* a) {
  return a ? reinterpret_cast<CharMetrics*>(mono_array_addr_with_size(a, sizeof(CharMetrics), 0)) : nullptr;
}

// GetTextWidthNative(int, string, int, int, out int, CharMetrics[]) — 0x2e8050.
int32_t GetTextWidthNative(uint32_t handle, MonoString* text, int32_t offset, int32_t len, int32_t* width,
                           MonoArray* metrics) {
  const Utf16 t = text ? utf16_of(text) : Utf16{};
  return font_get_text_width(handle, text ? &t : nullptr, offset, len, width, metrics_of(metrics),
                             metrics ? int32_t(mono_array_length(metrics)) : 0);
}
SSN_ICALL("Sce.PlayStation.Core.Imaging.Font::GetTextWidthNative(int,string,int,int,int&,Sce.PlayStation.Core.Imaging.CharMetrics[])",
          GetTextWidthNative);

// GetTextMetricsNative(int, string, int, int, int fixedFontFlag, int disableEmoji, CharMetrics[]) — 0x2e81c0.
int32_t GetTextMetricsNative(uint32_t handle, MonoString* text, int32_t offset, int32_t len, int32_t fixed_font_flag,
                             int32_t disable_emoji, MonoArray* metrics) {
  const Utf16 t = text ? utf16_of(text) : Utf16{};
  // The native dereferences the array unchecked; a null array is reported as an overflow.
  return font_get_text_metrics(handle, text ? &t : nullptr, offset, len, fixed_font_flag, disable_emoji,
                               metrics_of(metrics), metrics ? int32_t(mono_array_length(metrics)) : -1);
}
SSN_ICALL("Sce.PlayStation.Core.Imaging.Font::GetTextMetricsNative(int,string,int,int,int,int,Sce.PlayStation.Core.Imaging.CharMetrics[])",
          GetTextMetricsNative);

// MeasureTextMetricsNative(int, GlyphMetrics[], int, string, int, int) — 0x2e83a0.
int32_t MeasureTextMetricsNative(uint32_t handle, MonoArray* metrics, int32_t metrics_index, MonoString* text,
                                 int32_t offset, int32_t length) {
  (void)metrics_index;
  const Utf16 t = text ? utf16_of(text) : Utf16{};
  return font_measure_text_metrics(handle, metrics ? int32_t(mono_array_length(metrics)) : -1, text ? &t : nullptr,
                                   offset, length);
}
SSN_ICALL("Sce.PlayStation.Core.Imaging.Font::MeasureTextMetricsNative(int,Sce.PlayStation.Core.Imaging.GlyphMetrics[],int,string,int,int)",
          MeasureTextMetricsNative);

// RegisterCharMetricsNative(FontAlias, int, ref CharMetrics, FontAttribute) — 0x2e84f0.
int32_t RegisterCharMetricsNative(uint32_t alias, int32_t code, const CharMetrics* metrics, uint32_t attribute) {
  return register_char_metrics(alias, code, *metrics, attribute);
}
SSN_ICALL("Sce.PlayStation.Core.Imaging.Font::RegisterCharMetricsNative(Sce.PlayStation.Core.Imaging.FontAlias,int,Sce.PlayStation.Core.Imaging.CharMetrics&,Sce.PlayStation.Core.Imaging.FontAttribute)",
          RegisterCharMetricsNative);

// UnregisterCharMetricsNative(FontAlias, int, FontAttribute) — 0x2e89b0.
int32_t UnregisterCharMetricsNative(uint32_t alias, int32_t code, uint32_t attribute) {
  return unregister_char_metrics(alias, code, attribute);
}
SSN_ICALL("Sce.PlayStation.Core.Imaging.Font::UnregisterCharMetricsNative(Sce.PlayStation.Core.Imaging.FontAlias,int,Sce.PlayStation.Core.Imaging.FontAttribute)",
          UnregisterCharMetricsNative);

// NotifySystemConfigurationChangeNative() — 0x2e90f0: the language tag of
// sceSystemServiceParamGetInt(0x02020000 = SCE_SYSTEM_SERVICE_PARAM_ID_LANG) (table
// off_4A22B0; "en-US" when the call fails or the id is above 29).
int32_t NotifySystemConfigurationChangeNative() {
  static const char* const kTags[30] = {
      "ja-JP", "en-US", "fr-FR", "es-ES", "de-DE", "it-IT", "nl-NL", "pt-PT", "ru-RU", "ko-KR",
      "zh-Hant", "zh-Hans", "fi-FI", "sv-SE", "da-DK", "nb-NO", "pl-PL", "pt-BR", "en-GB", "tr-TR",
      "es-419", "ar-AE", "fr-CA", "cs-CZ", "hu-HU", "el-GR", "ro-RO", "th-TH", "vi-VN", "id-ID",
  };
  int32_t id = -1;
  const bool ok = regmgr::get_int(0x02020000, &id) == 0;
  notify_system_configuration_change((ok && uint32_t(id) <= 29u) ? kTags[id] : "en-US");
  return 0;
}
SSN_ICALL("Sce.PlayStation.Core.Imaging.Font::NotifySystemConfigurationChangeNative()",
          NotifySystemConfigurationChangeNative);

// NotifySystemConfigurationChangeNative(string) — 0x2e9190.
int32_t NotifySystemConfigurationChangeNativeLocale(MonoString* locale) {
  notify_system_configuration_change(utf8(locale));
  return 0;
}
SSN_ICALL("Sce.PlayStation.Core.Imaging.Font::NotifySystemConfigurationChangeNative(string)",
          NotifySystemConfigurationChangeNativeLocale);

// Image::DrawTextNative(int, string, uint, uint, ref ImageColor, int, ref ImagePosition,
// int fixedFontFlag, int direction, int disableEmoji) — 0x2e34e0. Image lookup, font
// lookup, text checks, colour clamped to [0, 255] (dword_350224), format check, then the
// layout routine on the image's pixel slot.
struct DrawArgs {
  uint32_t font;
  MonoString* text;
  uint32_t offset, len;
  int32_t color[4];
  int32_t position[2];
  int32_t fixed_font_flag, direction, disable_emoji;
};

int32_t draw_on_image(const imaging::PixelTarget& t, void* ctx) {
  const DrawArgs& a = *static_cast<const DrawArgs*>(ctx);
  if (!t.has_impl) {  // the native reads the impl unchecked
    host::note_unimplemented("libScePsm Image.DrawText on an image without impl");
    return kErrorImageSystem;
  }
  const Utf16 text = a.text ? utf16_of(a.text) : Utf16{};
  DrawTarget d;
  d.pixels = t.pixels;
  d.size = t.size;
  d.internal_format = t.internal_format;
  d.width = t.width;
  d.height = t.height;
  return draw_text(a.font, d, a.color, a.position, a.text ? &text : nullptr, a.offset, a.len, a.fixed_font_flag,
                   a.direction, a.disable_emoji);
}

int32_t DrawTextNative(uint32_t image, MonoString* text, uint32_t offset, uint32_t len, const int32_t* color,
                       uint32_t font, const int32_t* position, int32_t fixed_font_flag, int32_t direction,
                       int32_t disable_emoji) {
  DrawArgs a{font, text, offset, len, {}, {position[0], position[1]}, fixed_font_flag, direction, disable_emoji};
  for (int i = 0; i < 4; ++i) a.color[i] = color[i] < 0 ? 0 : (color[i] > 255 ? 255 : color[i]);
  return imaging::draw_into_pixels(image, &draw_on_image, &a);
}
SSN_ICALL("Sce.PlayStation.Core.Imaging.Image::DrawTextNative(int,string,uint,uint,Sce.PlayStation.Core.Imaging.ImageColor&,int,Sce.PlayStation.Core.Imaging.ImagePosition&,int,int,int)",
          DrawTextNative);

}  // namespace
}  // namespace ssn::psm::text

#endif  // SSN_HAVE_PSM_TEXT
