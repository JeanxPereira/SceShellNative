# libjpeg-turbo, as libScePsm.sprx (FW 4.03) links it statically for its JPEG image codec
# (psmhost/libScePsm/imaging/image_jpeg.cpp), built from the official release sources
# vendored next to this file:
#
#   libjpeg-turbo-2.0.2/  libjpeg-turbo-2.0.2.tar.gz (github.com/libjpeg-turbo/libjpeg-turbo/
#                         releases/download/2.0.2/), sha256
#                         acb8599fe5399af114287ee5907aea4456f8f2c1cc96d26c28aebfdf5ee82fed
#                         Kept (unmodified): the library's j*.c / j*.h (not jcstest.c,
#                         jpegtran.c), jconfig.h.in, jconfigint.h.in, win/jconfig.h.in,
#                         LICENSE.md, README.ijg, README.md, ChangeLog.md.
#
# Why this version/configuration (libScePsm_dyn.elf, IDA):
#   * version string "2.0.2 (build 20191211)"; jpeg_CreateDecompress is called with
#     JPEG_LIB_VERSION 62 and sizeof(struct jpeg_decompress_struct) 632 (0x24eb30 -> 0x56e00),
#     i.e. the default libjpeg v6b API/ABI;
#   * jpeg_aritab is present (0x3d9fb0 in the file) -> arithmetic coding compiled in
#     (WITH_ARITH_DEC / WITH_ARITH_ENC, the upstream defaults);
#   * "JSIMD_FORCE..." strings -> the native build has the x86-64 SIMD extensions. They are
#     not built here (no NASM in this build); libjpeg-turbo's SIMD paths produce the same
#     output as the C paths for the defaults libScePsm uses (islow IDCT, fancy upsampling,
#     YCbCr->RGB), so decoded pixels are unaffected.
#
# Usage (launcher/CMakeLists.txt):
#   include("${CMAKE_SOURCE_DIR}/third_party/jpeg.cmake")
#   target_link_libraries(SceShellNative PRIVATE ssn_psm_jpeg)
# ssn_psm_jpeg carries the include directories (the vendored sources + the generated
# jconfig.h) and SSN_HAVE_PSM_JPEG, which switches on the JPEG codec in
# psmhost/libScePsm/imaging/image_jpeg.cpp (without it JPEG images keep failing with
# "Image unsupported" and a note_unimplemented line).

include_guard(GLOBAL)

set(SSN_LJT_DIR "${CMAKE_CURRENT_LIST_DIR}/libjpeg-turbo-2.0.2")
set(SSN_LJT_GEN "${CMAKE_BINARY_DIR}/third_party/libjpeg-turbo-2.0.2")

# jconfig.h / jconfigint.h as upstream CMakeLists.txt generates them on Windows
# (win/jconfig.h.in), with the upstream default options. A function keeps the template
# variables out of the including directory's scope.
function(ssn_ljt_configure)
  set(JPEG_LIB_VERSION 62)
  set(VERSION 2.0.2)
  set(LIBJPEG_TURBO_VERSION_NUMBER 2000002)
  set(BITS_IN_JSAMPLE 8)
  set(C_ARITH_CODING_SUPPORTED 1)
  set(D_ARITH_CODING_SUPPORTED 1)
  set(MEM_SRCDST_SUPPORTED 1)
  set(WITH_SIMD 0)
  set(BUILD 20191211)
  set(CMAKE_PROJECT_NAME libjpeg-turbo)
  set(SIZE_T ${CMAKE_SIZEOF_VOID_P})
  if(MSVC)
    set(INLINE __forceinline)
    set(HAVE_INTRIN_H 1)
    set(HAVE_BUILTIN_CTZL 0)
  else()
    set(INLINE "inline __attribute__((always_inline))")
    set(HAVE_INTRIN_H 0)
    set(HAVE_BUILTIN_CTZL 1)
  endif()
  if(WIN32)
    configure_file("${SSN_LJT_DIR}/win/jconfig.h.in" "${SSN_LJT_GEN}/jconfig.h")
  else()
    configure_file("${SSN_LJT_DIR}/jconfig.h.in" "${SSN_LJT_GEN}/jconfig.h")
  endif()
  configure_file("${SSN_LJT_DIR}/jconfigint.h.in" "${SSN_LJT_GEN}/jconfigint.h")
endfunction()
ssn_ljt_configure()

# Upstream JPEG_SOURCES + jaricom/jcarith/jdarith (arithmetic coding) + jsimd_none.c.
set(SSN_LJT_SOURCES
  jcapimin.c jcapistd.c jccoefct.c jccolor.c jcdctmgr.c jchuff.c jcicc.c jcinit.c jcmainct.c
  jcmarker.c jcmaster.c jcomapi.c jcparam.c jcphuff.c jcprepct.c jcsample.c jctrans.c jdapimin.c
  jdapistd.c jdatadst.c jdatasrc.c jdcoefct.c jdcolor.c jddctmgr.c jdhuff.c jdicc.c jdinput.c
  jdmainct.c jdmarker.c jdmaster.c jdmerge.c jdphuff.c jdpostct.c jdsample.c jdtrans.c jerror.c
  jfdctflt.c jfdctfst.c jfdctint.c jidctflt.c jidctfst.c jidctint.c jidctred.c jquant1.c
  jquant2.c jutils.c jmemmgr.c jmemnobs.c jaricom.c jcarith.c jdarith.c jsimd_none.c)
list(TRANSFORM SSN_LJT_SOURCES PREPEND "${SSN_LJT_DIR}/")

add_library(ssn_libjpeg_turbo STATIC ${SSN_LJT_SOURCES})
target_include_directories(ssn_libjpeg_turbo PUBLIC "${SSN_LJT_GEN}" "${SSN_LJT_DIR}")
target_compile_definitions(ssn_libjpeg_turbo PRIVATE _CRT_SECURE_NO_WARNINGS)
if(MSVC)
  target_compile_options(ssn_libjpeg_turbo PRIVATE /w)
endif()
# jcphuff.c passes an int * to _BitScanForward64 (unsigned long *, same size on Windows);
# recent clang makes that an error by default.
if(CMAKE_C_COMPILER_ID STREQUAL "Clang")
  target_compile_options(ssn_libjpeg_turbo PRIVATE -Wno-incompatible-pointer-types)
endif()

# --- what the launcher links ------------------------------------------------------------------
add_library(ssn_psm_jpeg INTERFACE)
target_link_libraries(ssn_psm_jpeg INTERFACE ssn_libjpeg_turbo)
target_compile_definitions(ssn_psm_jpeg INTERFACE SSN_HAVE_PSM_JPEG=1)
