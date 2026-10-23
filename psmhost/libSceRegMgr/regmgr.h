// Host system registry: the store behind libSceRegMgr.sprx (FW 4.03).
//
// On the console libSceRegMgr is a thin client: every export packs its arguments into
// the regmgr_call system call (#532, libkernel import 7NwggrWJ5cA) and the kernel owns
// the store, the entry definitions and every value check. This module stands in for that
// kernel side. See docs/registry.md for the system and the sources of each value.
//
// All functions return the kernel's status codes (SCE_REGMGR_ERROR_*, 0x800D....) exactly
// as regmgr_call hands them back to the client, or 0 on success.
#pragma once
#include <cstddef>
#include <cstdint>
#include <string>

namespace ssn::regmgr {

// SCE_REGMGR_ERROR_* (Sce.Vsh.Registry.dll constants; codes the 7.00 kernel returns).
constexpr int32_t kOk = 0;
constexpr int32_t kErrUninitialized = static_cast<int32_t>(0x800D0201);  // store not loaded
constexpr int32_t kErrParam = static_cast<int32_t>(0x800D0202);          // null buffer
constexpr int32_t kErrPrmRegId = static_cast<int32_t>(0x800D0203);       // unknown key
constexpr int32_t kErrPrmType = static_cast<int32_t>(0x800D0207);        // type mismatch
constexpr int32_t kErrPrmSize = static_cast<int32_t>(0x800D0208);        // size mismatch

// SCE_REGMGR_TYPE_*.
enum Type : uint8_t { kInteger = 0, kString = 1, kBinary = 2 };

// Where the persisted values live (the console's system.reg); call once at start-up with a
// file in the launcher cache directory. Without it, values written by the shell last for
// the session only.
void set_store_path(const std::string& path);

// Optional: the plaintext registry definition file (registry.db0) that the kernel embeds
// (XOR-obfuscated) and reads at boot. It supplies the defaults Sce.Vsh.Registry.dll does
// not carry (strings, binaries and most integers without an SDK INI value). The launcher
// never ships it; see docs/registry.md for how it is obtained.
void set_db0_path(const std::string& path);
// Host: called once the Mono runtime is up (the definitions are read from Sce.Vsh.Registry
// through it). Native threads that reach the registry earlier wait for it.
void set_runtime_ready();

// sceRegMgrGetInt / SetInt / GetStr / SetStr / GetBin / SetBin (regmgr_call 2/1/4/3/6/5).
int32_t get_int(uint32_t key, int32_t* value);
int32_t set_int(uint32_t key, int32_t value);
int32_t get_str(uint32_t key, char* buf, size_t size);
int32_t set_str(uint32_t key, const char* str, size_t size);
int32_t get_bin(uint32_t key, void* buf, size_t size);
int32_t set_bin(uint32_t key, const void* data, size_t size);

// sceRegMgrResetVal (regmgr_call 11): back to the default value (key 0 = every key).
int32_t reset_val(uint32_t key, int32_t init_level);

// The value of each listed key back to its default, with one store write; keys the
// registry does not define are skipped. Returns the number of keys reset. (For writers that
// store each key's sceRegMgrGet*InitVal value, e.g. SceShellCore filling a new user slot.)
int32_t reset_vals(const uint32_t* keys, size_t count);

// sceRegMgrCheckError (regmgr_call 12): classifies a regmgr status as
// SCE_REGMGR_CHKERR_BROKEN (1), _SYSTEM (2) or _MISTAKE (3); 0 for 0.
int32_t check_error(int32_t error);

// sceRegMgrGetVersion / GetVersionForPS4 (regmgr_call 0 with 1 / 2).
int32_t get_version();
int32_t get_version_for_ps4();

// sceRegMgrIsInitOK (regmgr_call 17) / sceRegMgrGetInitError (regmgr_call 33).
int32_t is_init_ok();
int32_t get_init_error();

// Entry metadata (for diagnostics): false when the key is not defined.
struct EntryInfo {
  Type type;
  uint16_t size;
  std::string name;  // SCE_REGMGR_ENT_KEY_<name> in Sce.Vsh.Registry.dll
};
bool describe(uint32_t key, EntryInfo* info);

}  // namespace ssn::regmgr
