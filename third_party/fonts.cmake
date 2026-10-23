# The text stack libScePsm.sprx (FW 4.03) links statically, built from the official
# release sources vendored next to this file:
#
#   freetype-2.4.11/  freetype-2.4.11.tar.bz2 (download.savannah.gnu.org/releases/freetype/
#                     freetype-old/), sha256 ef9d0bcb64647d9e5125dc7534d7ca371c98310fec87677c410f397f71ffbe3f
#                     Kept: include/, src/, README, docs/{LICENSE,FTL,GPLv2}.TXT (unmodified).
#   harfbuzz-1.3.4/   harfbuzz-1.3.4.tar.bz2 (www.freedesktop.org/software/harfbuzz/release/),
#                     sha256 718aa6fcadef1a6548315b8cfe42cc27e926256302c337f42df3a443843f6a2b
#                     Kept: src/, COPYING, AUTHORS, README, NEWS (unmodified).
#   fonts_config/     the libScePsm module list (freetype/config/ftmodule.h override).
#
# Why these versions (libScePsm_dyn.elf, IDA):
#   FreeType 2.4.11 — FT_New_Library (0x19a2f0) writes version 2/4/11; FT_Add_Default_Modules
#     (0x19e8a0) adds autofitter, truetype, type1, cff, t1cid, pfr, type42, winfonts, psaux,
#     psnames, pshinter, raster1, sfnt, smooth, smooth-lcd, smooth-lcdv (no pcf/bdf).
#   HarfBuzz 1.3.4 — only the "ot" shaper (no "fallback" string), ucdn Unicode functions,
#     hb-ft. Version pinned inside 1.3.x by:
#       >= 1.3.1: the Padauk GDEF blacklist (gdef/gpos/gsub lengths 1046/17112/71788,
#                 1330/57938/109904) in _hb_ot_layout_create (0x29fdd0);
#       >= 1.3.3: hb_ot_layout_t carries math_blob/math (+24/+56 cleared in 0x29fdd0);
#       >= 1.3.4: guess_v_origin_minus_h_origin calls get_font_h_extents with the
#                 y_scale * 0.8 fallback (qword_350DA8 = 0.8, in _hb_ot_shape 0x2a4740 and
#                 0x2b5330) — 1.3.3 used y_scale there;
#       <  1.4.0: hb_font_t is 152 bytes without variation coordinates.
#
# Usage (launcher/CMakeLists.txt):
#   include("${CMAKE_SOURCE_DIR}/third_party/fonts.cmake")
#   target_link_libraries(SceShellNative PRIVATE ssn_psm_fonts)
# ssn_psm_fonts carries the include directories and SSN_HAVE_PSM_TEXT, which switches on
# psmhost/libScePsm/text/ (without it those sources compile to nothing and the managed
# Font icalls stay logging stubs).

include_guard(GLOBAL)

set(SSN_FREETYPE_DIR "${CMAKE_CURRENT_LIST_DIR}/freetype-2.4.11")
set(SSN_HARFBUZZ_DIR "${CMAKE_CURRENT_LIST_DIR}/harfbuzz-1.3.4")
set(SSN_FONTS_CONFIG_DIR "${CMAKE_CURRENT_LIST_DIR}/fonts_config")

# --- FreeType 2.4.11 (docs/INSTALL.ANY: base files + one file per module) -------------------
add_library(ssn_freetype STATIC
  "${SSN_FREETYPE_DIR}/src/base/ftsystem.c"
  "${SSN_FREETYPE_DIR}/src/base/ftinit.c"
  "${SSN_FREETYPE_DIR}/src/base/ftdebug.c"
  "${SSN_FREETYPE_DIR}/src/base/ftbase.c"
  "${SSN_FREETYPE_DIR}/src/base/ftbbox.c"
  "${SSN_FREETYPE_DIR}/src/base/ftglyph.c"
  "${SSN_FREETYPE_DIR}/src/base/ftbitmap.c"
  "${SSN_FREETYPE_DIR}/src/base/ftlcdfil.c"
  "${SSN_FREETYPE_DIR}/src/base/ftmm.c"
  "${SSN_FREETYPE_DIR}/src/base/ftsynth.c"
  "${SSN_FREETYPE_DIR}/src/autofit/autofit.c"
  "${SSN_FREETYPE_DIR}/src/truetype/truetype.c"
  "${SSN_FREETYPE_DIR}/src/type1/type1.c"
  "${SSN_FREETYPE_DIR}/src/cff/cff.c"
  "${SSN_FREETYPE_DIR}/src/cid/type1cid.c"
  "${SSN_FREETYPE_DIR}/src/pfr/pfr.c"
  "${SSN_FREETYPE_DIR}/src/type42/type42.c"
  "${SSN_FREETYPE_DIR}/src/winfonts/winfnt.c"
  "${SSN_FREETYPE_DIR}/src/psaux/psaux.c"
  "${SSN_FREETYPE_DIR}/src/psnames/psnames.c"
  "${SSN_FREETYPE_DIR}/src/pshinter/pshinter.c"
  "${SSN_FREETYPE_DIR}/src/raster/raster.c"
  "${SSN_FREETYPE_DIR}/src/sfnt/sfnt.c"
  "${SSN_FREETYPE_DIR}/src/smooth/smooth.c")
# fonts_config first: its freetype/config/ftmodule.h replaces the stock module list.
target_include_directories(ssn_freetype PUBLIC "${SSN_FONTS_CONFIG_DIR}" "${SSN_FREETYPE_DIR}/include")
target_compile_definitions(ssn_freetype PRIVATE FT2_BUILD_LIBRARY _CRT_SECURE_NO_WARNINGS)
if(MSVC)
  target_compile_options(ssn_freetype PRIVATE /w)
endif()

# --- HarfBuzz 1.3.4 (src/Makefile.sources: HB_BASE + HB_OT + HB_FT + HB_UCDN) ---------------
set(SSN_HB_SRC "${SSN_HARFBUZZ_DIR}/src")
add_library(ssn_harfbuzz STATIC
  # HB_BASE_sources
  "${SSN_HB_SRC}/hb-blob.cc"
  "${SSN_HB_SRC}/hb-buffer-serialize.cc"
  "${SSN_HB_SRC}/hb-buffer.cc"
  "${SSN_HB_SRC}/hb-common.cc"
  "${SSN_HB_SRC}/hb-face.cc"
  "${SSN_HB_SRC}/hb-font.cc"
  "${SSN_HB_SRC}/hb-ot-tag.cc"
  "${SSN_HB_SRC}/hb-set.cc"
  "${SSN_HB_SRC}/hb-shape.cc"
  "${SSN_HB_SRC}/hb-shape-plan.cc"
  "${SSN_HB_SRC}/hb-shaper.cc"
  "${SSN_HB_SRC}/hb-unicode.cc"
  "${SSN_HB_SRC}/hb-warning.cc"
  # HB_OT_sources
  "${SSN_HB_SRC}/hb-ot-font.cc"
  "${SSN_HB_SRC}/hb-ot-layout.cc"
  "${SSN_HB_SRC}/hb-ot-map.cc"
  "${SSN_HB_SRC}/hb-ot-shape.cc"
  "${SSN_HB_SRC}/hb-ot-shape-complex-arabic.cc"
  "${SSN_HB_SRC}/hb-ot-shape-complex-default.cc"
  "${SSN_HB_SRC}/hb-ot-shape-complex-hangul.cc"
  "${SSN_HB_SRC}/hb-ot-shape-complex-hebrew.cc"
  "${SSN_HB_SRC}/hb-ot-shape-complex-indic.cc"
  "${SSN_HB_SRC}/hb-ot-shape-complex-indic-table.cc"
  "${SSN_HB_SRC}/hb-ot-shape-complex-myanmar.cc"
  "${SSN_HB_SRC}/hb-ot-shape-complex-thai.cc"
  "${SSN_HB_SRC}/hb-ot-shape-complex-tibetan.cc"
  "${SSN_HB_SRC}/hb-ot-shape-complex-use.cc"
  "${SSN_HB_SRC}/hb-ot-shape-complex-use-table.cc"
  "${SSN_HB_SRC}/hb-ot-shape-normalize.cc"
  "${SSN_HB_SRC}/hb-ot-shape-fallback.cc"
  # HB_FT_sources
  "${SSN_HB_SRC}/hb-ft.cc"
  # HB_UCDN_sources
  "${SSN_HB_SRC}/hb-ucdn.cc"
  "${SSN_HB_SRC}/hb-ucdn/ucdn.c")
# hb-ucdn/ is public too: libScePsm reads the same statically linked ucdn tables directly
# (bidi class test at the top of the layout routine 0x298a50).
target_include_directories(ssn_harfbuzz PUBLIC "${SSN_HB_SRC}" "${SSN_HB_SRC}/hb-ucdn")
# "ot" is the only shaper libScePsm carries (HB_SHAPER_LIST at 0x29fce0, no "fallback").
target_compile_definitions(ssn_harfbuzz PRIVATE HAVE_OT HAVE_UCDN NOMINMAX WIN32_LEAN_AND_MEAN _CRT_SECURE_NO_WARNINGS)
set_target_properties(ssn_harfbuzz PROPERTIES CXX_STANDARD 14 CXX_STANDARD_REQUIRED ON)
target_link_libraries(ssn_harfbuzz PUBLIC ssn_freetype)
if(MSVC)
  target_compile_options(ssn_harfbuzz PRIVATE /w /EHs-c-)
endif()

# --- what the launcher links ------------------------------------------------------------------
add_library(ssn_psm_fonts INTERFACE)
target_link_libraries(ssn_psm_fonts INTERFACE ssn_harfbuzz ssn_freetype)
target_compile_definitions(ssn_psm_fonts INTERFACE SSN_HAVE_PSM_TEXT=1)
