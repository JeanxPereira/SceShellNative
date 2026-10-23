// Sce.Vsh.SQLiteAux natives (DllImport "sqlite_aux"), exported by name from the ShellUI
// eboot (FW 4.03) next to its static SQLite 3.35.5 (third_party/sqlite.cmake).
#include <cstdint>
#include <cstring>
#include <string>

#include <sqlite3.h>

#include "core/log.h"
#include "host/boundary.h"

namespace ssn::sqlite_aux {
namespace {

// Host diagnostics: SQLite's error log (SQLITE_CONFIG_LOG) into the launcher log, installed
// before the shell opens a database.
void on_sqlite_log(void*, int code, const char* msg) { log::warn("sqlite", "({}) {}", code, msg ? msg : ""); }
const int g_log_installed = sqlite3_config(SQLITE_CONFIG_LOG, on_sqlite_log, nullptr);

}  // namespace

// SqliteErrorToSceErrorCodeNative(int sqliteCode, out int sceCode) — eboot 0x4a6680:
// 0 -> 0; every primary code 1..28 and the extended codes SQLite 3.35.5 defines (the set
// below, read off the function's decision tree) -> 0x81880000 + code, result 0; anything
// else -> sceCode = result = 0x8188FFFF (SCE_SQLITE_ERROR_CONVERT_ERRORCODE).
SSN_PINVOKE("sqlite_aux", "SqliteErrorToSceErrorCodeNative", int32_t, ssn_SqliteAux_SqliteErrorToSceErrorCodeNative,
            (int32_t code, int32_t* sce)) {
  static constexpr int32_t kExtended[] = {
      257,  261,  262,  264,  266,  267,  270,  275,  279,  283,  284,  513,  516,  517,  518,  520,  522,
      523,  526,  531,  539,  769,  773,  776,  778,  779,  782,  787,  1032, 1034, 1038, 1043, 1288, 1290,
      1294, 1299, 1544, 1546, 1550, 1555, 1802, 1811, 2058, 2067, 2314, 2323, 2570, 2579, 2826, 2835, 3082,
      3338, 3594, 3850, 4106, 4362, 4618, 4874, 5130, 5386, 5642, 5898, 6154, 6410, 6666, 6922, 7178, 7434,
      7690, 7946, 8202};
  constexpr int32_t kConvertError = static_cast<int32_t>(0x8188FFFFu);
  if (code == 0) {
    *sce = 0;
    return 0;
  }
  bool known = code >= 1 && code <= 28;
  for (int32_t k : kExtended) known = known || k == code;
  if (!known) {
    *sce = kConvertError;
    return kConvertError;
  }
  *sce = static_cast<int32_t>(0x81880000u + static_cast<uint32_t>(code));
  return 0;
}

// SqliteAuxInterruptNative(db) — eboot 0x4a69f0 -> sqlite3_interrupt (0x3d4190).
SSN_PINVOKE("sqlite_aux", "SqliteAuxInterruptNative", void, ssn_SqliteAux_SqliteAuxInterruptNative, (sqlite3 * db)) {
  sqlite3_interrupt(db);
}

namespace {

// The eboot's application SQL function table (qword_EE2AC0..EE2AC8, 40-byte entries, filled
// by 0xBB60 from 0xD4C030): {name, nArg, eTextRep, xFunc, pApp getter, xDestroy}.
//   "library_canonicalize", 1, SQLITE_UTF8, xFunc 0x4A3040, pApp 0x4A31C0 (the shell's
//   collator 0x4A3260 with options 1=0, 6=1, 5=1), xDestroy 0x4A3250 (none).
// 0x4A3040: one argument; NULL -> NULL; else the collator's GetCanonicalForm (vtable +24) of
// the UTF-8 text, or "Collator::GetCanonicalForm() returned an error".
void library_canonicalize(sqlite3_context* ctx, int argc, sqlite3_value** argv) {
  if (argc != 1) {
    sqlite3_result_error(ctx, "invalid number of arguments", -1);
    return;
  }
  if (!sqlite3_value_text(argv[0])) {
    sqlite3_result_null(ctx);
    return;
  }
  host::note_unimplemented("eboot collator canonical form (0x4A3260 GetCanonicalForm, library_canonicalize) not reproduced");
  sqlite3_result_error(ctx, "Collator::GetCanonicalForm() returned an error", -1);
}

struct SqlFunction {
  const char* name;
  int n_arg;
  int text_rep;
  void (*x_func)(sqlite3_context*, int, sqlite3_value**);
};
constexpr SqlFunction kFunctions[] = {{"library_canonicalize", 1, SQLITE_UTF8, library_canonicalize}};

}  // namespace

// EnableFunctionNative(db, name) — eboot 0x4a2f10: looks the name up in the table above
// (strncmp, 255) and registers each match with sqlite3_create_function_v2 (0x3d4450);
// empty table -> 21, null argument -> 2, no match -> 12 ("[SQLiteAux] %s not found").
SSN_PINVOKE("sqlite_aux", "EnableFunctionNative", int32_t, ssn_SqliteAux_EnableFunctionNative, (sqlite3 * db, const char* name)) {
  if (!db || !name) return 2;
  bool found = false;
  for (const auto& f : kFunctions) {
    if (std::strncmp(f.name, name, 255) != 0) continue;
    const int r = sqlite3_create_function_v2(db, f.name, f.n_arg, f.text_rep, nullptr, f.x_func, nullptr, nullptr, nullptr);
    if (r != 0) {
      log::warn("sqlite", "[SQLiteAux] {:x}", r);
      return r;
    }
    found = true;
  }
  if (found) return 0;
  log::warn("sqlite", "[SQLiteAux] {} not found", name);
  return 12;
}

}  // namespace ssn::sqlite_aux
