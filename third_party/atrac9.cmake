# LibAtrac9 (Alex Barney, MIT licence), the ATRAC9 decoder that stands in for Sony's
# libSceAudiodec on the PC. HOST VALUE: the firmware has no ATRAC9 decoder of its own —
# libScePsm's PSS ATRAC9 decoder (type 3, sub_191C50 / vtable off_49C3E8) creates and drives
# an sceAudiodec ATRAC9 instance (sceAudiodecCreateDecoder 0x191D80, sceAudiodecDecode
# 0x192200, sceAudiodecClearContext 0x191D30 / 0x192190, sceAudiodecDeleteDecoder 0x1920C0),
# which decodes on the console's ACP/AJM hardware; Windows has no ATRAC9 decoder.
# psmhost/libScePsm/audio/audiodec_atrac9.cpp maps that interface onto this library.
#
#   LibAtrac9-efca2e3/  github.com/Thealexbarney/LibAtrac9 at commit
#                       efca2e3af35562a09a9bb6deed90e45b4b824dc4 (2025-03-14, "Update
#                       Makefile (#10)"). Kept (unmodified; every file hashes to the upstream
#                       git blob): LICENSE (MIT) and C/src/*.c, C/src/*.h — the C
#                       implementation. Not kept: the C# port, the Visual Studio project, the
#                       Makefile.
#
# The library is built static with the upstream flags' intent (C99, no SIMD). libatrac9.h
# marks its API __declspec(dllimport) under MSVC unless COMPILING_DLL is defined; it is
# defined for the library and its users so the static symbols resolve directly.
#
# Usage (launcher/CMakeLists.txt):
#   include("${CMAKE_SOURCE_DIR}/third_party/atrac9.cmake")
#   target_link_libraries(SceShellNative PRIVATE ssn_atrac9)
set(SSN_ATRAC9_DIR "${CMAKE_CURRENT_LIST_DIR}/LibAtrac9-efca2e3/C/src")
file(GLOB SSN_ATRAC9_SOURCES CONFIGURE_DEPENDS "${SSN_ATRAC9_DIR}/*.c")
add_library(ssn_atrac9 STATIC ${SSN_ATRAC9_SOURCES})
target_include_directories(ssn_atrac9 INTERFACE "${SSN_ATRAC9_DIR}")
target_compile_definitions(ssn_atrac9 PUBLIC COMPILING_DLL)
if(MSVC)
  target_compile_options(ssn_atrac9 PRIVATE /W0)
endif()
