# SQLite, as the ShellUI eboot (FW 4.03) links it statically and exports its sqlite3_* entry
# points by name to Mono: the DllImport("sqlite3") calls of Sce.Vsh.SQLite resolve against
# the eboot itself. Built here from the official amalgamation vendored next to this file:
#
#   sqlite-3.35.5/  sqlite-amalgamation-3350500.zip (www.sqlite.org/2021/), sha256
#                   b49409ef123e193e719e2536f9b795482a69e61a9cc728933739b9024f035061
#                   Kept (unmodified): sqlite3.c, sqlite3.h, sqlite3ext.h.
#
# Why this version/configuration (eboot.elf, IDA):
#   * sqlite3_libversion (0x3d1fa0) "3.35.5"; sqlite3_sourceid (0x3da830)
#     "2021-04-19 18:32:05 1b256d97b553...0c9e98fa" + "alt2": the release's source id with the
#     amalgamation's line-count marker, i.e. the console's sqlite3.c was regenerated/edited
#     (ENABLE_UPDATE_DELETE_LIMIT below needs a parser built from the canonical sources).
#   * the compile-option table (azCompileOpt, 0xcec480..): COMPILER=clang-12.0.0,
#     DEFAULT_CACHE_SIZE=(16), DEFAULT_FILE_FORMAT=(4), DEFAULT_LOOKASIDE=0,0,
#     DEFAULT_PAGE_SIZE=(0x1000), DEFAULT_SECTOR_SIZE=(512), DEFAULT_WAL_SYNCHRONOUS=(1),
#     ENABLE_ATOMIC_WRITE, ENABLE_COLUMN_METADATA, ENABLE_SNAPSHOT,
#     ENABLE_UPDATE_DELETE_LIMIT, LIKE_DOESNT_MATCH_BLOBS, THREADSAFE=1 — reproduced here.
# Not reproduced: the console's VFS and the mutex the eboot installs with
# sqlite3_config(SQLITE_CONFIG_MUTEX) (0x3b6680); the Windows VFS and mutexes stand in.
# UPDATE/DELETE ... LIMIT is not in the amalgamation's prebuilt grammar.
#
# Usage (launcher/CMakeLists.txt):
#   include("${CMAKE_SOURCE_DIR}/third_party/sqlite.cmake")
#   target_link_libraries(SceShellNative PRIVATE ssn_sqlite3)
# (psmhost/libSqliteAux calls into it.) The DLL lands next to SceShellNative.exe as sqlite3.dll; main.cpp registers it as the
# passthrough library "sqlite3".
set(SSN_SQLITE_DIR "${CMAKE_CURRENT_LIST_DIR}/sqlite-3.35.5")
add_library(ssn_sqlite3 SHARED "${SSN_SQLITE_DIR}/sqlite3.c")
target_include_directories(ssn_sqlite3 INTERFACE "${SSN_SQLITE_DIR}")
set_target_properties(ssn_sqlite3 PROPERTIES OUTPUT_NAME sqlite3
  RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/bin")
target_compile_definitions(ssn_sqlite3 PRIVATE
  "SQLITE_API=__declspec(dllexport)"
  SQLITE_DEFAULT_CACHE_SIZE=16
  SQLITE_DEFAULT_FILE_FORMAT=4
  SQLITE_DEFAULT_LOOKASIDE=0,0
  SQLITE_DEFAULT_PAGE_SIZE=0x1000
  SQLITE_DEFAULT_SECTOR_SIZE=512
  SQLITE_DEFAULT_WAL_SYNCHRONOUS=1
  SQLITE_ENABLE_ATOMIC_WRITE
  SQLITE_ENABLE_COLUMN_METADATA
  SQLITE_ENABLE_SNAPSHOT
  SQLITE_ENABLE_UPDATE_DELETE_LIMIT
  SQLITE_LIKE_DOESNT_MATCH_BLOBS
  SQLITE_THREADSAFE=1)
if(MSVC)
  target_compile_options(ssn_sqlite3 PRIVATE /W0)
endif()
