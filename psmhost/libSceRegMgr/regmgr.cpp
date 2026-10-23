// Host system registry (stands in for the kernel side of libSceRegMgr.sprx, FW 4.03).
//
// Sources (docs/registry.md has the full description):
//  * Entry definitions — Sce.Vsh.Registry.dll (FW 4.03, the managed copy of the SDK's
//    sce_regmgr_entry.h, read at run time from the user's dump through Mono):
//      SCE_REGMGR_ENT_KEY_<n> (const)  key id
//      SCE_REGMGR_ENT_KEY_<n>(uint a)  ranged key: SCE_REGMGR_ENT_NUM(a, count, stride, base, invalid)
//                                      = base + (a - 1) * stride for a in 1..count
//      SCE_REGMGR_ENT_TYP_<n>          0 int, 1 string, 2 binary
//      SCE_REGMGR_ENT_SIZ_<n>          bytes (strings include the NUL)
//      SCE_REGMGR_ENT_INI_<n>          initial value; an int, or the string
//                                      "sceRegMgrRiniI<key>()" (computed per region)
//  * Defaults the DLL does not carry — registry.db0, the definition file the kernel embeds
//    (7.00 manu kernel: 111056 bytes at 0xffffffff816D9F50, XOR 16-byte key at
//    0xffffffff81308C50) and parses at boot in 0xffffffff80ca3b50 (line parser
//    0xffffffff80cab660). Optional, supplied by the user (set_db0_path).
//  * Checks and status codes — the 7.00 kernel's regmgr_call handler 0xffffffff80ca9c90
//    and its Get/Set routines (addresses at each function below). No 4.03 kernel is
//    available; the client library and the managed constants are 4.03.
#include "libSceRegMgr/regmgr.h"

#include <algorithm>
#include <cstring>
#include <filesystem>
#include <format>
#include <fstream>
#include <map>
#include <condition_variable>
#include <mutex>
#include <sstream>
#include <string_view>
#include <unordered_map>
#include <vector>

#include <mono/metadata/appdomain.h>
#include <mono/metadata/assembly.h>
#include <mono/metadata/attrdefs.h>
#include <mono/metadata/blob.h>
#include <mono/metadata/class.h>
#include <mono/metadata/loader.h>
#include <mono/metadata/metadata.h>
#include <mono/metadata/threads.h>

#include "core/log.h"
#include "host/boundary.h"

namespace ssn::regmgr {
namespace {

// ---------------------------------------------------------------------------------------
// Entry definitions from Sce.Vsh.Registry.dll

struct Definition {
  Type type = kInteger;
  uint16_t size = 0;
  bool has_key = false;
  uint32_t key = 0;
  bool ranged = false;
  uint32_t count = 0, stride = 0;  // ranged keys
  bool has_ini = false;            // numeric SCE_REGMGR_ENT_INI_<n>
  int32_t ini = 0;
  bool region_ini = false;         // "sceRegMgrRiniI<key>()"
};

// ECMA-335 II.23.2 compressed unsigned integer (blob length prefix).
uint32_t blob_size(const uint8_t*& p) {
  if ((p[0] & 0x80) == 0) return *p++;
  if ((p[0] & 0xC0) == 0x80) {
    const uint32_t v = ((p[0] & 0x3Fu) << 8) | p[1];
    p += 2;
    return v;
  }
  const uint32_t v = ((p[0] & 0x1Fu) << 24) | (uint32_t(p[1]) << 16) | (uint32_t(p[2]) << 8) | p[3];
  p += 4;
  return v;
}

// Literal field value: the constant blob (length prefix + value) from mono_field_get_data.
bool literal_u32(MonoClassField* f, uint32_t* v) {
  const int t = mono_type_get_type(mono_field_get_type(f));
  if (t != MONO_TYPE_I4 && t != MONO_TYPE_U4) return false;
  const auto* p = reinterpret_cast<const uint8_t*>(mono_field_get_data(f));
  if (!p) return false;
  if (blob_size(p) < 4) return false;
  std::memcpy(v, p, 4);
  return true;
}

bool literal_string(MonoClassField* f, std::string* s) {
  if (mono_type_get_type(mono_field_get_type(f)) != MONO_TYPE_STRING) return false;
  const auto* p = reinterpret_cast<const uint8_t*>(mono_field_get_data(f));
  if (!p) return false;
  const uint32_t bytes = blob_size(p);
  s->clear();
  for (uint32_t i = 0; i + 1 < bytes; i += 2) {
    const uint16_t c = uint16_t(p[i] | (p[i + 1] << 8));
    s->push_back(c < 0x80 ? char(c) : '?');  // the INI strings are ASCII
  }
  return true;
}

// IL of a ranged key method, e.g. SCE_REGMGR_ENT_KEY_USER_01_16_NP_env(uint a):
//   ldarg.0  ldarg.1  ldc.i4[.s] count  ldc.i4 stride  ldc.i4 base  ldc.i4 invalid
//   call SCE_REGMGR_ENT_NUM  ret
bool ranged_args(MonoMethod* m, uint32_t out[4]) {
  MonoMethodHeader* h = mono_method_get_header(m);
  if (!h) return false;
  uint32_t size = 0, max_stack = 0;
  const uint8_t* il = mono_method_header_get_code(h, &size, &max_stack);
  int n = 0;
  bool call = false, ok = true;
  for (uint32_t i = 0; il && i < size && ok;) {
    const uint8_t op = il[i];
    if (op == 0x02 || op == 0x03) { ++i; continue; }  // ldarg.0 / ldarg.1
    uint32_t v = 0;
    bool ld = true;
    if (op >= 0x16 && op <= 0x1E) { v = uint32_t(op - 0x16); i += 1; }                  // ldc.i4.0..8
    else if (op == 0x15) { v = 0xFFFFFFFFu; i += 1; }                                   // ldc.i4.m1
    else if (op == 0x1F && i + 1 < size) { v = uint32_t(int32_t(int8_t(il[i + 1]))); i += 2; }  // ldc.i4.s
    else if (op == 0x20 && i + 4 < size) { std::memcpy(&v, il + i + 1, 4); i += 5; }    // ldc.i4
    else ld = false;
    if (ld) {
      if (n < 4) out[n] = v;
      ++n;
      continue;
    }
    if (op == 0x28 && i + 4 < size) { call = true; i += 5; continue; }  // call
    if (op == 0x2A) break;                                              // ret
    ok = false;
  }
  mono_metadata_free_mh(h);
  return ok && call && n == 4;
}

// ---------------------------------------------------------------------------------------
// registry.db0 (kernel definition file), only the defaults are taken from it

struct Db0Value {
  uint32_t howto = 0;  // field 19: 0 none, 1 literal value, 0x80|n computed by the kernel
  std::string value;   // field 20 to the end of the line, trailing ':' removed
};

// The kernel's number parser 0xffffffff80cab4e0: leading spaces, "0x" hex or optional '-'
// and decimal digits, stops at the first other character.
int64_t db0_number(std::string_view s) {
  size_t i = 0;
  while (i < s.size() && s[i] == ' ') ++i;
  if (i + 1 < s.size() && s[i] == '0' && (s[i + 1] | 0x20) == 'x') {
    int64_t v = 0;
    for (i += 2; i < s.size(); ++i) {
      const char c = s[i];
      int d;
      if (c >= '0' && c <= '9') d = c - '0';
      else if (c >= 'a' && c <= 'f') d = c - 'a' + 10;
      else if (c >= 'A' && c <= 'F') d = c - 'A' + 10;
      else break;
      v = v * 16 + d;
    }
    return v;
  }
  int64_t sign = 1;
  if (i < s.size() && s[i] == '-') { sign = -1; ++i; }
  int64_t v = 0;
  for (; i < s.size() && s[i] >= '0' && s[i] <= '9'; ++i) v = v * 10 + (s[i] - '0');
  return v * sign;
}

// Reads the [REG-BAS] section. Lines are "<key>=<f0>:...:<f19>:<value>:"; field 3 is the
// type, 4 the size, 18 the element count, 19 how the value is initialised, 20 the value.
// The [REG-<region>], [REG-DEX] and [REG-TOL] sections override entries by region and
// machine type (0xffffffff80ca3b50: "BAS" always, the console's region code, "DEX"/"TOL"
// on development/test kits) — the host has no region or kit, so only BAS applies.
std::unordered_map<uint32_t, Db0Value> load_db0(const std::string& path) {
  std::unordered_map<uint32_t, Db0Value> out;
  std::ifstream f(path, std::ios::binary);
  if (!f) {
    log::error("regmgr", "registry.db0 not readable: {}", path);
    return out;
  }
  std::string line;
  bool bas = false;
  while (std::getline(f, line)) {
    if (!line.empty() && line.back() == '\r') line.pop_back();
    if (line.empty() || line[0] == '#') continue;
    if (line[0] == '[') {
      bas = line.rfind("[REG-BAS", 0) == 0;
      continue;
    }
    if (!bas) continue;
    const size_t eq = line.find('=');
    if (eq == std::string::npos || eq == 0) continue;
    const std::string_view k(line.data(), eq);
    if (!std::all_of(k.begin(), k.end(), [](char c) { return c >= '0' && c <= '9'; })) continue;  // trailing pad
    size_t pos = eq + 1;
    std::string_view fields[20];
    bool ok = true;
    for (int i = 0; i < 20 && ok; ++i) {
      const size_t c = line.find(':', pos);
      if (c == std::string::npos) { ok = false; break; }
      fields[i] = std::string_view(line).substr(pos, c - pos);
      pos = c + 1;
    }
    if (!ok) continue;
    Db0Value v;
    v.howto = uint32_t(db0_number(fields[19]));
    v.value = line.substr(pos);
    if (!v.value.empty() && v.value.back() == ':') v.value.pop_back();
    out[uint32_t(db0_number(k))] = std::move(v);
  }
  return out;
}

// ---------------------------------------------------------------------------------------
// The store

enum class DefaultSource { Ini, Db0, Db0None, Untraced };

struct Entry {
  Type type = kInteger;
  uint16_t size = 0;
  std::string name;
  std::vector<uint8_t> def;  // default value (size bytes)
  std::vector<uint8_t> val;  // current value (size bytes)
  DefaultSource source = DefaultSource::Untraced;
  std::string untraced_why;
  bool written = false;  // set since the store was created (persisted)
};

struct State {
  std::mutex mutex;
  bool loaded = false;
  bool load_failed = false;
  std::string store_path;
  std::string db0_path;
  std::map<uint32_t, Entry> entries;
  int32_t version = kErrUninitialized;
  int32_t version_ps4 = kErrUninitialized;
};

State& state() {
  static State s;
  return s;
}

void put_int(std::vector<uint8_t>& b, int32_t v) {
  b.assign(4, 0);
  std::memcpy(b.data(), &v, 4);
}

bool hex_value(std::string_view s, std::vector<uint8_t>& out, size_t size) {
  // db0 binary literal: b'<hex pairs> (0xffffffff80cab660, high nibble first)
  if (s.size() < 2 || s[0] != 'b' || s[1] != '\'') return false;
  s.remove_prefix(2);
  if (s.size() % 2) return false;
  out.assign(size, 0);
  auto nib = [](char c) -> int {
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
  };
  for (size_t i = 0; i + 1 < s.size() && i / 2 < size; i += 2) {
    const int hi = nib(s[i]), lo = nib(s[i + 1]);
    if (hi < 0 || lo < 0) return false;
    out[i / 2] = uint8_t(hi * 16 + lo);
  }
  return true;
}

void apply_default(Entry& e, const Definition& d, uint32_t key, const std::unordered_map<uint32_t, Db0Value>& db0,
                   const std::unordered_map<uint32_t, uint32_t>& range_base) {
  e.def.assign(e.size, 0);
  if (d.has_ini && e.type == kInteger) {
    put_int(e.def, d.ini);
    e.source = DefaultSource::Ini;
    return;
  }
  // db0 lists a ranged entry once, under its base key.
  const auto rb = range_base.find(key);
  const auto it = db0.find(rb != range_base.end() ? rb->second : key);
  if (it == db0.end()) {
    e.source = DefaultSource::Untraced;
    e.untraced_why = db0.empty() ? (d.region_ini ? "region-dependent INI, no registry.db0" : "no INI in Sce.Vsh.Registry, no registry.db0")
                                 : "not in registry.db0 [REG-BAS]";
    return;
  }
  const Db0Value& v = it->second;
  if (v.howto == 0) {  // no initial value: the kernel's zero-filled cache
    e.source = DefaultSource::Db0None;
    return;
  }
  if (v.howto != 1) {  // 0x80|n: filled in by a kernel routine
    e.source = DefaultSource::Untraced;
    e.untraced_why = std::format("registry.db0 init method {:#x} (computed by the kernel)", v.howto);
    return;
  }
  e.source = DefaultSource::Db0;
  if (e.type == kInteger) {
    put_int(e.def, int32_t(db0_number(v.value)));
  } else if (e.type == kString) {
    const size_t n = std::min(v.value.size(), size_t(e.size ? e.size - 1 : 0));
    std::memcpy(e.def.data(), v.value.data(), n);
  } else if (!hex_value(v.value, e.def, e.size)) {
    e.def.assign(e.size, 0);
    e.source = DefaultSource::Untraced;
    e.untraced_why = "registry.db0 binary value not parsed";
  }
}

bool load_definitions(std::map<uint32_t, Entry>& entries, std::unordered_map<uint32_t, uint32_t>& range_base,
                      std::map<std::string, Definition>& defs) {
  MonoImageOpenStatus status = MONO_IMAGE_OK;
  MonoAssembly* asm_ = mono_assembly_load_with_partial_name("Sce.Vsh.Registry", &status);
  if (!asm_) {
    log::error("regmgr", "Sce.Vsh.Registry not loadable (status {})", int(status));
    return false;
  }
  MonoClass* klass = mono_class_from_name(mono_assembly_get_image(asm_), "Sce.Vsh", "Registry");
  if (!klass) {
    log::error("regmgr", "Sce.Vsh.Registry class not found");
    return false;
  }
  constexpr std::string_view kPrefix = "SCE_REGMGR_ENT_";
  void* iter = nullptr;
  while (MonoClassField* f = mono_class_get_fields(klass, &iter)) {
    if (!(mono_field_get_flags(f) & MONO_FIELD_ATTR_LITERAL)) continue;
    const std::string_view name = mono_field_get_name(f);
    if (name.substr(0, kPrefix.size()) != kPrefix || name.size() < kPrefix.size() + 5) continue;
    const std::string_view kind = name.substr(kPrefix.size(), 4);  // "KEY_", "TYP_", ...
    const std::string rest(name.substr(kPrefix.size() + 4));
    uint32_t u = 0;
    if (kind == "KEY_" && literal_u32(f, &u)) { defs[rest].has_key = true; defs[rest].key = u; }
    else if (kind == "TYP_" && literal_u32(f, &u)) defs[rest].type = Type(u);
    else if (kind == "SIZ_" && literal_u32(f, &u)) defs[rest].size = uint16_t(u);
    else if (kind == "INI_") {
      std::string s;
      if (literal_u32(f, &u)) { defs[rest].has_ini = true; defs[rest].ini = int32_t(u); }
      else if (literal_string(f, &s) && s.rfind("sceRegMgrRiniI", 0) == 0) defs[rest].region_ini = true;
    }
  }
  iter = nullptr;
  constexpr std::string_view kKey = "SCE_REGMGR_ENT_KEY_";
  while (MonoMethod* m = mono_class_get_methods(klass, &iter)) {
    const std::string_view name = mono_method_get_name(m);
    if (name.substr(0, kKey.size()) != kKey) continue;
    uint32_t a[4];
    if (!ranged_args(m, a)) {
      log::warn("regmgr", "ranged key method {} not understood", name);
      continue;
    }
    Definition& d = defs[std::string(name.substr(kKey.size()))];
    d.ranged = true;
    d.count = a[0];
    d.stride = a[1];
    d.key = a[2];
  }
  for (const auto& [name, d] : defs) {
    if (!d.has_key && !d.ranged) continue;  // categories (SCE_REGMGR_ENT_CAT_*) and MIN/MAX only
    if (d.size == 0 && d.type != kInteger) continue;
    const uint32_t n = d.ranged ? d.count : 1;
    for (uint32_t i = 0; i < n; ++i) {
      const uint32_t key = d.key + i * d.stride;
      Entry& e = entries[key];
      e.type = d.type;
      e.size = d.type == kInteger ? 4 : d.size;
      e.name = d.ranged ? std::format("{}({})", name, i + 1) : name;
      if (d.ranged) range_base[key] = d.key;
    }
  }
  return true;
}

// Persisted values: one line per written entry, "<key hex> <type> <value hex>".
void load_store(State& s) {
  if (s.store_path.empty()) return;
  std::ifstream f(s.store_path);
  if (!f) return;
  std::string line;
  int n = 0;
  while (std::getline(f, line)) {
    std::istringstream ls(line);
    std::string key_hex, hex;
    int type = -1;
    if (!(ls >> key_hex >> type >> hex)) continue;
    const uint32_t key = uint32_t(std::stoul(key_hex, nullptr, 16));
    auto it = s.entries.find(key);
    if (it == s.entries.end() || it->second.type != type || hex.size() != size_t(it->second.size) * 2) {
      log::warn("regmgr", "store: dropping {} (no longer matches the definition)", key_hex);
      continue;
    }
    std::vector<uint8_t> v(it->second.size);
    for (size_t i = 0; i < v.size(); ++i) v[i] = uint8_t(std::stoul(hex.substr(i * 2, 2), nullptr, 16));
    it->second.val = std::move(v);
    it->second.written = true;
    ++n;
  }
  log::info("regmgr", "store: {} values from {}", n, s.store_path);
}

void save_store(const State& s) {
  if (s.store_path.empty()) return;
  const std::filesystem::path p(s.store_path);
  const std::filesystem::path tmp = p.string() + ".tmp";
  {
    std::ofstream f(tmp, std::ios::trunc);
    if (!f) {
      log::error("regmgr", "store: cannot write {}", tmp.string());
      return;
    }
    for (const auto& [key, e] : s.entries) {
      if (!e.written) continue;
      std::string hex;
      for (uint8_t b : e.val) hex += std::format("{:02x}", b);
      f << std::format("{:08x} {} {} {}\n", key, int(e.type), hex, e.name);
    }
  }
  std::error_code ec;
  std::filesystem::rename(tmp, p, ec);
  if (ec) log::error("regmgr", "store: cannot replace {}: {}", p.string(), ec.message());
}

std::mutex g_ready_mutex;
std::condition_variable g_ready_cv;
bool g_runtime_ready = false;

// Called with the mutex held. False when the store cannot exist yet (Mono not running or
// the definitions assembly missing) — callers then return SCE_REGMGR_ERROR_UNINITIALIZED,
// the kernel's answer before the registry is initialised.
bool ensure_loaded(State& s) {
  if (s.loaded) return true;
  // Host: the definitions come from the Sce.Vsh.Registry metadata through Mono, while the
  // console's registry is reachable from any thread at any time (it is the kernel's). A
  // native thread that Mono does not know (no current domain) waits for the runtime, is
  // attached for the load and detached after it.
  if (!mono_domain_get()) {
    std::unique_lock rl(g_ready_mutex);
    g_ready_cv.wait(rl, [] { return g_runtime_ready; });
  }
  if (s.load_failed || !mono_get_root_domain()) return false;
  std::map<std::string, Definition> defs;
  std::unordered_map<uint32_t, uint32_t> range_base;
  MonoThread* attached = mono_domain_get() ? nullptr : mono_thread_attach(mono_get_root_domain());
  const bool defined = load_definitions(s.entries, range_base, defs);
  if (attached) mono_thread_detach(attached);
  if (!defined) {
    s.load_failed = true;
    return false;
  }
  std::unordered_map<uint32_t, Db0Value> db0;
  if (!s.db0_path.empty()) db0 = load_db0(s.db0_path);
  int untraced = 0;
  for (auto& [key, e] : s.entries) {
    const std::string base_name = e.name.substr(0, e.name.find('('));
    apply_default(e, defs[base_name], key, db0, range_base);
    e.val = e.def;
    if (e.source == DefaultSource::Untraced) ++untraced;
  }
  // GetVersion / GetVersionForPS4 return the version the kernel takes from the db0 header
  // ([VERSION=], [VER_PS4=]); in the 7.00 kernel those equal the defaults of
  // REGISTRY_version (0x01100000) and REGISTRY_version_ps4 (0x01010000), whose 4.03
  // values are the INI constants of the same entries.
  if (defs.count("REGISTRY_version") && defs["REGISTRY_version"].has_ini) s.version = defs["REGISTRY_version"].ini;
  if (defs.count("REGISTRY_version_ps4") && defs["REGISTRY_version_ps4"].has_ini) s.version_ps4 = defs["REGISTRY_version_ps4"].ini;
  s.loaded = true;
  log::info("regmgr", "{} keys defined by Sce.Vsh.Registry; registry.db0 {} ({} BAS entries); {} defaults not traced",
            s.entries.size(), s.db0_path.empty() ? "not given" : s.db0_path, db0.size(), untraced);
  load_store(s);
  return true;
}

// Entry lookup and checks of the kernel (0xffffffff80ca83d0): unknown key -> PRM_REGID,
// other type -> PRM_TYPE; integers and binaries need size == entry size; strings need
// size <= entry size when setting and size >= entry size when getting. (The per-process
// permission checks of the same routine are not modelled: the shell is a system process.)
enum class Op { Get, Set };
int32_t lookup(State& s, uint32_t key, Type type, size_t size, Op op, Entry** out) {
  auto it = s.entries.find(key);
  if (it == s.entries.end()) return kErrPrmRegId;
  Entry& e = it->second;
  if (e.type != type) return kErrPrmType;
  if (type == kString) {
    if (op == Op::Set ? e.size < size : e.size > size) return kErrPrmSize;
  } else if (e.size != size) {
    return kErrPrmSize;
  }
  if (!e.written && e.source == DefaultSource::Untraced && op == Op::Get)
    host::note_unimplemented(std::format("libSceRegMgr default of {:#010x} {} not traced ({}); reads zero", key, e.name,
                                         e.untraced_why));
  *out = &e;
  return kOk;
}

void store_value(State& s, Entry& e, const void* data, size_t n) {
  std::fill(e.val.begin(), e.val.end(), 0);
  std::memcpy(e.val.data(), data, std::min(n, e.val.size()));
  e.written = true;
  save_store(s);
}

}  // namespace

void set_store_path(const std::string& path) {
  State& s = state();
  std::lock_guard lock(s.mutex);
  s.store_path = path;
  if (s.loaded) load_store(s);
}

void set_db0_path(const std::string& path) {
  State& s = state();
  std::lock_guard lock(s.mutex);
  s.db0_path = path;
  if (s.loaded) log::warn("regmgr", "registry.db0 set after the store was built; applies on the next start");
}

void set_runtime_ready() {
  {
    std::lock_guard l(g_ready_mutex);
    g_runtime_ready = true;
  }
  g_ready_cv.notify_all();
}

// sceRegMgrGetInt -> regmgr_call(2): kernel 0xffffffff80ca2070.
int32_t get_int(uint32_t key, int32_t* value) {
  State& s = state();
  std::lock_guard lock(s.mutex);
  if (!ensure_loaded(s)) return kErrUninitialized;
  if (!value) return kErrParam;
  Entry* e = nullptr;
  if (const int32_t r = lookup(s, key, kInteger, 4, Op::Get, &e)) return r;
  std::memcpy(value, e->val.data(), 4);
  return kOk;
}

// sceRegMgrSetInt -> regmgr_call(1): kernel 0xffffffff80ca1f20.
int32_t set_int(uint32_t key, int32_t value) {
  State& s = state();
  std::lock_guard lock(s.mutex);
  if (!ensure_loaded(s)) return kErrUninitialized;
  Entry* e = nullptr;
  if (const int32_t r = lookup(s, key, kInteger, 4, Op::Set, &e)) return r;
  store_value(s, *e, &value, 4);
  return kOk;
}

// sceRegMgrGetStr -> regmgr_call(4): kernel 0xffffffff80ca22e0. Copies at most the entry
// size; a value filling the whole entry is cut to size - 1 characters plus the NUL.
int32_t get_str(uint32_t key, char* buf, size_t size) {
  State& s = state();
  std::lock_guard lock(s.mutex);
  if (!ensure_loaded(s)) return kErrUninitialized;
  if (!buf) return kErrParam;
  Entry* e = nullptr;
  if (const int32_t r = lookup(s, key, kString, size, Op::Get, &e)) return r;
  const size_t n = std::min(size, size_t(e->size));
  if (n == 0) return kOk;
  const char* src = reinterpret_cast<const char*>(e->val.data());
  const size_t len = strnlen(src, n);
  if (len >= n) {
    std::memcpy(buf, src, n - 1);
    buf[n - 1] = 0;
  } else {
    std::memcpy(buf, src, len + 1);
  }
  return kOk;
}

// sceRegMgrSetStr -> regmgr_call(3): kernel 0xffffffff80ca2190. Stores strnlen(str, size) + 1
// bytes; that length may exceed neither size nor the entry size.
int32_t set_str(uint32_t key, const char* str, size_t size) {
  State& s = state();
  std::lock_guard lock(s.mutex);
  if (!ensure_loaded(s)) return kErrUninitialized;
  if (!str) return kErrParam;
  Entry* e = nullptr;
  if (const int32_t r = lookup(s, key, kString, size, Op::Set, &e)) return r;
  const size_t n = strnlen(str, size) + 1;
  if (n > size || n > e->size) return kErrPrmSize;
  store_value(s, *e, str, n);
  return kOk;
}

// sceRegMgrGetBin -> regmgr_call(6): kernel 0xffffffff80ca2650.
int32_t get_bin(uint32_t key, void* buf, size_t size) {
  State& s = state();
  std::lock_guard lock(s.mutex);
  if (!ensure_loaded(s)) return kErrUninitialized;
  if (!buf) return kErrParam;
  Entry* e = nullptr;
  if (const int32_t r = lookup(s, key, kBinary, size, Op::Get, &e)) return r;
  std::memcpy(buf, e->val.data(), size);
  return kOk;
}

// sceRegMgrSetBin -> regmgr_call(5): kernel 0xffffffff80ca2540.
int32_t set_bin(uint32_t key, const void* data, size_t size) {
  State& s = state();
  std::lock_guard lock(s.mutex);
  if (!ensure_loaded(s)) return kErrUninitialized;
  if (!data) return kErrParam;
  Entry* e = nullptr;
  if (const int32_t r = lookup(s, key, kBinary, size, Op::Set, &e)) return r;
  store_value(s, *e, data, size);
  return kOk;
}

// sceRegMgrResetVal -> regmgr_call(11): kernel 0xffffffff80ca2cb0 resets through
// 0xffffffff80ca2e20 (not decompiled further): restoring the default value is the traced
// part; the init-level filter and the exact status codes are not.
int32_t reset_val(uint32_t key, int32_t init_level) {
  State& s = state();
  std::lock_guard lock(s.mutex);
  if (!ensure_loaded(s)) return kErrUninitialized;
  host::note_unimplemented(std::format("libSceRegMgr ResetVal init-level filter (level {}) not traced", init_level));
  auto reset = [](Entry& e) {
    e.val = e.def;
    e.written = false;
  };
  if (key == 0) {  // SCE_REGMGR_RESETVAL_ALL
    for (auto& [k, e] : s.entries) reset(e);
  } else {
    auto it = s.entries.find(key);
    if (it == s.entries.end()) return kErrPrmRegId;
    reset(it->second);
  }
  save_store(s);
  return kOk;
}

int32_t reset_vals(const uint32_t* keys, size_t count) {
  State& s = state();
  std::lock_guard lock(s.mutex);
  if (!ensure_loaded(s)) return kErrUninitialized;
  int32_t n = 0;
  for (size_t i = 0; i < count; ++i) {
    auto it = s.entries.find(keys[i]);
    if (it == s.entries.end()) continue;
    it->second.val = it->second.def;
    it->second.written = false;
    ++n;
  }
  save_store(s);
  return n;
}

// sceRegMgrCheckError -> regmgr_call(12): kernel 0xffffffff80ca3590, with the registry
// initialised normally.
int32_t check_error(int32_t error) {
  {
    State& s = state();
    std::lock_guard lock(s.mutex);
    if (!ensure_loaded(s)) error = kErrUninitialized;
  }
  if (error == 0) return 0;
  const uint32_t a = uint32_t(error);
  uint32_t v = (a & 0x7FF27F00u) | 0x800D0000u;
  if ((a & 0xFFFF0000u) != 0x800D0000u) v = a;
  if (((v & 0xFFFFFB00u) | 0x400u) != 0x800D0400u) return ((v & 0xFFFFFB00u) == 0x800D0100u) ^ 3;
  return 1;
}

// regmgr_call(0) -> kernel 0xffffffff80ca3270 (see ensure_loaded for the values).
int32_t get_version() {
  State& s = state();
  std::lock_guard lock(s.mutex);
  return ensure_loaded(s) ? s.version : kErrUninitialized;
}

int32_t get_version_for_ps4() {
  State& s = state();
  std::lock_guard lock(s.mutex);
  return ensure_loaded(s) ? s.version_ps4 : kErrUninitialized;
}

// regmgr_call(17) -> kernel 0xffffffff80ca3620: 0 when initialised with no pending
// recovery/initialisation state (SCE_REGMGR_ISINIT_RECOVER -1, _INIT -2, _SAFEMODE -3 are
// console storage states the host store never enters), UNINITIALIZED before.
int32_t is_init_ok() {
  State& s = state();
  std::lock_guard lock(s.mutex);
  return ensure_loaded(s) ? 0 : kErrUninitialized;
}

// regmgr_call(33) -> kernel 0xffffffff80ca36e0: 0 when initialised normally.
int32_t get_init_error() {
  State& s = state();
  std::lock_guard lock(s.mutex);
  return ensure_loaded(s) ? 0 : kErrUninitialized;
}

bool describe(uint32_t key, EntryInfo* info) {
  State& s = state();
  std::lock_guard lock(s.mutex);
  if (!ensure_loaded(s)) return false;
  auto it = s.entries.find(key);
  if (it == s.entries.end()) return false;
  if (info) *info = {it->second.type, it->second.size, it->second.name};
  return true;
}

}  // namespace ssn::regmgr
