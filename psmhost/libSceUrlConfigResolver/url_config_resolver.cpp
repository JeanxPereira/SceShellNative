// URL config resolver (libSceUrlConfigResolver.sprx, FW 4.03), exported to managed code as
// "libSceUrlConfigResolverWrapper" (Sce.Vsh.UrlConfigResolver.dll).
//
// The ShellUI eboot (NPXS40087) publishes that P/Invoke library through the {name, fn}
// table at 0xcc4d60 whose entries are the module's own imports, so each managed entry is
// the libSceUrlConfigResolver export of the same name (NID = name). Addresses below are
// offsets in libSceUrlConfigResolver.sprx.
//
// Data flow on the console:
//   * SceShellCore creates the shared memory "SceUrlConfigResolverShm" at boot (0x775ed0:
//     vsh common_lib atomic_buffer opened with create flags, 0x10000 bytes per half) and
//     keeps it for its lifetime; it never writes it.
//   * The ShellUI (Sce.Vsh.ShellUI.Legacy SharedStoreUriManager) builds the configuration
//     {"urlconfig": <manifest>, "macro": {"<NPENV>", "<SYS_VER>", "<DEVICE_ID>"}} — the
//     manifest is the urlconfig cache, a download, or the OfflineConfig manifest embedded in
//     Sce.Vsh.ShellUI.AppSystem.dll — and publishes it with sceUrlConfigResolverUpdate.
//   * Every reader process parses the published text on demand and resolves URLs in it.
// The library holds no configuration of its own; until something is published the parse of
// the empty buffer fails and every query returns 0x80848101.
//
// Host model: the shared memory is an in-process object standing in for the one SceShellCore
// creates (same header magic/size); everything else follows the native code.
#include "libSceRegMgr/regmgr.h"
#include "host/context.h"
#include <cstdint>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <mutex>
#include <optional>
#include <string>
#include <unordered_map>

#include "host/boundary.h"
#include "libSceUrlConfigResolver/sce_json.h"
#include "libNativeExtensions/jsc/rnps_decryption.h"

namespace ssn::urlcfg {
namespace {

using json::Type;
using json::Value;

// Error codes (SCE_URL_CONFIG_RESOLVER_ERROR_*, 0x80ba80xx; atomic_buffer 0x80ba800x).
constexpr int32_t E(uint32_t v) { return static_cast<int32_t>(v); }
constexpr int32_t kErrInvalidArgument = E(0x80ba8080);    // null argument
// 0x80ba8081: NP environment registry string fills all 17 bytes (not reachable on the host).
constexpr int32_t kErrRootNotObject = E(0x80ba8082);
constexpr int32_t kErrNoScheme = E(0x80ba8083);           // urlconfig.scheme missing/false
constexpr int32_t kErrSchemeNotObject = E(0x80ba8084);
constexpr int32_t kErrNoSchemeEnv = E(0x80ba8085);        // scheme[<env>] and scheme.default missing
constexpr int32_t kErrSchemeEnvNotObject = E(0x80ba8086);
constexpr int32_t kErrUrlNotFound = E(0x80ba8087);
constexpr int32_t kErrNotAppReference = E(0x80ba8089);    // scheme value without "app:"
constexpr int32_t kErrNoApp = E(0x80ba808a);              // urlconfig.app missing/false
constexpr int32_t kErrAppNotObject = E(0x80ba808b);
constexpr int32_t kErrAppNotFound = E(0x80ba808c);        // neither app[name] nor app.default
constexpr int32_t kErrAppEntryNotObject = E(0x80ba808d);
constexpr int32_t kErrNoAppEnv = E(0x80ba808e);           // app[name][<env>] and .default missing
constexpr int32_t kErrWrongType = E(0x80ba808f);          // resolved value of the wrong type
constexpr int32_t kErrBufferTooSmall = E(0x80ba8090);     // managed side retries with *outSize
constexpr int32_t kErrNotFound = E(0x80ba8091);           // macro / release missing
constexpr int32_t kErrHeapLimitType = E(0x80ba8092);      // jsc-heap-size-soft-limit not unsigned
// 0x80ba8000 / 0x80ba8001: shared memory not open / bad header magic (open failures; the host
// buffer always exists). An open failure is otherwise the shm_open errno via sceKernelError.
constexpr int32_t kErrShmTooLarge = E(0x80ba8002);

constexpr uint32_t kDefaultJscHeapSoftLimit = 104857600;  // 0x1060 / 0x12d0
constexpr size_t kShmHalfSize = 0x10000;                  // SceShellCore 0x775ed0
constexpr size_t kConfigBufferSize = 0x10000;             // reader copy (0x12d0 -> 0x6610)

// ---------------------------------------------------------------------------------------
// "SceUrlConfigResolverShm" (vsh common_lib atomic_buffer, impl.cpp): header
// {magic 0xa1544cbdffe34255, sequence, half size} followed by two halves. A writer copies
// into the half the readers are not using ((seq & 1) == 0 ? half 1 : half 0) and then
// increments the sequence (0x6820); a reader copies half (seq & 1) and retries while the
// sequence moves (0x6610); "updated" means the sequence differs from the last one read
// (0x6700). Here it exists from the start, as SceShellCore has created it before the
// ShellUI runs.
// ---------------------------------------------------------------------------------------
struct SharedConfigBuffer {
  std::mutex m;
  uint64_t seq = 0;
  char half[2][kShmHalfSize] = {};
};

SharedConfigBuffer& shm() {
  static SharedConfigBuffer s;
  return s;
}

// Writer side of sceUrlConfigResolverUpdate (0x6820).
int32_t shm_write(const char* data, size_t size) {
  auto& b = shm();
  std::lock_guard lock(b.m);
  if (size > kShmHalfSize) return kErrShmTooLarge;
  std::memcpy(b.half[(b.seq & 1) == 0 ? 1 : 0], data, size);
  ++b.seq;
  return 0;
}

uint64_t shm_sequence() {
  auto& b = shm();
  std::lock_guard lock(b.m);
  return b.seq;
}

// Reader copy (0x6610): `size` bytes of the current half; returns the sequence read.
uint64_t shm_read(char* out, size_t size) {
  auto& b = shm();
  std::lock_guard lock(b.m);
  std::memcpy(out, b.half[b.seq & 1], size);
  return b.seq;
}

// ---------------------------------------------------------------------------------------
// Reader state (0x430 initialises it; 0x12d0 loads it).
// ---------------------------------------------------------------------------------------
struct Resolver {
  std::mutex m;                       // qword_10040
  bool opened = false;                // LOBYTE(word_10048)
  bool dirty = true;                  // HIBYTE(word_10048): reload before the next query
  uint64_t last_seq = 0;              // atomic_buffer reader's last sequence (+56)
  std::optional<std::string> np_env;  // unk_20060: registry 0x19800000 (17 bytes)
  int32_t status = 0;                 // dword_20074: result of the last load
  Value root;                         // xmmword_20078: the parsed configuration
  std::shared_ptr<const std::unordered_map<std::string, std::string>> schemes;  // xmmword_20090
  std::shared_ptr<const std::unordered_map<std::string, Value>> apps;           // xmmword_200a8
  uint32_t jsc_heap_soft_limit = 0;   // dword_200c0
  std::shared_ptr<const std::string> release;  // xmmword_200c8
};

Resolver& resolver() {
  static Resolver r;
  return r;
}

struct Snapshot {
  Value root;
  std::shared_ptr<const std::unordered_map<std::string, std::string>> schemes;
  std::shared_ptr<const std::unordered_map<std::string, Value>> apps;
  std::shared_ptr<const std::string> release;
  std::optional<std::string> np_env;
};

// obj[env] when the registry value is known (otherwise treated as absent, see open_reader).
const Value& env_member(const Value& obj, const std::optional<std::string>& env) {
  static const Value kNull;
  return env ? obj.member(*env) : kNull;
}

// First-call set-up in 0x12d0: format "/<sandbox word>/priv/cache/urlconfig/
// urlconfig_cache.json" and register it with sceCoredumpAttachUserFile(0xf801, path)
// (diagnostics only), load sysmodules 0xe7 and 0x8000001f, read the NP environment with
// sceRegMgrGetStr(0x19800000, buf, 17) (failure is returned; a value filling all 17 bytes
// gives 0x80ba8081), then open the shared memory read-only (0x65f0 -> 0x6a00).
int32_t open_reader(Resolver& r) {
  // Registry key 0x19800000 is the NP environment (the ShellUI reads the same key for its
  // "<NPENV>" macro).
  char env[17] = {};
  if (int32_t rc = regmgr::get_str(0x19800000, env, sizeof env); rc != 0) return rc;
  if (std::strlen(env) >= 16 && env[16] != 0) return static_cast<int32_t>(0x80ba8081u);
  r.np_env = std::string(env);
  r.last_seq = shm_sequence();  // atomic_buffer Open records the current sequence (0x6900)
  r.opened = true;
  return 0;
}

// Validates the parsed document and builds the lookup tables (0x12d0 after the parse).
int32_t build_tables(Resolver& r, const Value& root) {
  if (root.type() != Type::Object) return kErrRootNotObject;
  const Value& urlconfig = root.member("urlconfig");
  const Value& scheme = urlconfig.member("scheme");
  if (!scheme.truthy()) return kErrNoScheme;
  if (scheme.type() != Type::Object) return kErrSchemeNotObject;
  const Value* scheme_env = &env_member(scheme, r.np_env);
  if (!scheme_env->truthy()) {
    scheme_env = &scheme.member("default");
    if (!scheme_env->truthy()) return kErrNoSchemeEnv;
  }
  if (scheme_env->type() != Type::Object) return kErrSchemeEnvNotObject;
  const Value& app = urlconfig.member("app");
  if (!app.truthy()) return kErrNoApp;
  if (app.type() != Type::Object) return kErrAppNotObject;

  // scheme map (0x23c0 / 0x3300): URL prefix -> string value (non-strings become "").
  auto schemes = std::make_shared<std::unordered_map<std::string, std::string>>();
  for (const auto& [k, v] : scheme_env->object()) schemes->emplace(k, v.string().c_str());
  // app map (0x3be0 / 0x4aa0): every member must be an object.
  auto apps = std::make_shared<std::unordered_map<std::string, Value>>();
  for (const auto& [k, v] : app.object()) {
    if (v.type() != Type::Object) return kErrAppEntryNotObject;
    apps->emplace(k, v);
  }

  uint32_t heap = kDefaultJscHeapSoftLimit;
  const Value& limit = urlconfig.member("jsc-heap-size-soft-limit");
  if (limit.truthy()) {
    if (limit.type() != Type::UInteger) return kErrHeapLimitType;
    heap = static_cast<uint32_t>(limit.uinteger());  // low dword of the stored value
  }
  std::shared_ptr<const std::string> release;
  const Value& rel = urlconfig.member("release");
  if (rel.truthy() && rel.type() == Type::String) release = std::make_shared<const std::string>(rel.string().c_str());

  r.schemes = std::move(schemes);
  r.apps = std::move(apps);
  r.jsc_heap_soft_limit = heap;
  r.release = std::move(release);
  return 0;
}

// 0x12d0: (re)loads the configuration when the shared memory changed and returns a
// snapshot of the current tables, or the status of the last load.
int32_t load(Snapshot* snap) {
  Resolver& r = resolver();
  std::lock_guard lock(r.m);
  if (!r.opened) {
    const int32_t err = open_reader(r);
    if (err < 0) return err;
  }
  if (shm_sequence() != r.last_seq) r.dirty = true;
  if (r.dirty) {
    static char buf[kConfigBufferSize];
    r.last_seq = shm_read(buf, kConfigBufferSize);
    Value doc;
    r.status = json::parse(doc, buf, strnlen(buf, kConfigBufferSize));
    r.dirty = false;
    if (r.status == 0) {
      r.root = doc;
      r.status = build_tables(r, r.root);
    }
  }
  if (r.status < 0) return r.status;
  if (r.root.type() != Type::Object) return kErrRootNotObject;
  if (snap) {
    snap->root = r.root;
    snap->schemes = r.schemes;
    snap->apps = r.apps;
    snap->release = r.release;
    snap->np_env = r.np_env;
  }
  return 0;
}

// 0x4ff0: resolves a URL to its app configuration value (the app entry's member for the NP
// environment, else its "default" member).
//   1. When the URL has a ':' that is not its last character, the text before the first '?'
//      is looked up, then repeatedly cut at its last '/' (a trailing '/' is dropped first)
//      and looked up again while it is non-empty.
//   2. Then the scheme (text before the first ':', or the whole URL) is looked up.
//   3. With enableDefault, the "default" entry.
// The entry must read "app:<name>"; <name> (or, failing that, "default") selects the app.
int32_t resolve(const char* url, bool enable_default, Snapshot& snap, Value& out) {
  if (!url) return kErrInvalidArgument;
  const int32_t err = load(&snap);
  if (err < 0) return err;
  const auto& schemes = *snap.schemes;
  const std::string s = url;
  const std::string* found = nullptr;
  auto lookup = [&](const std::string& key) {
    auto it = schemes.find(key);
    return it == schemes.end() ? nullptr : &it->second;
  };

  const size_t colon = s.find(':');
  if (colon != std::string::npos && colon != s.size() - 1) {
    std::string key = s.substr(0, s.find('?', colon + 1));
    while (!key.empty()) {
      if ((found = lookup(key))) break;
      const size_t slash = key.rfind('/');
      if (slash == std::string::npos) break;
      key.resize(slash);
    }
  }
  if (!found) found = lookup(s.substr(0, colon));
  if (!found && enable_default) found = lookup("default");
  if (!found) return kErrUrlNotFound;
  if (std::strncmp(found->c_str(), "app:", 4) != 0) return kErrNotAppReference;

  const auto& apps = *snap.apps;
  auto app = apps.find(found->c_str() + 4);
  if (app == apps.end()) app = apps.find("default");
  if (app == apps.end()) return kErrAppNotFound;
  out = env_member(app->second, snap.np_env);
  if (!out.truthy()) {
    out = app->second.member("default");
    if (!out.truthy()) return kErrNoAppEnv;
  }
  return 0;
}

// Copies s (with its terminator) to the caller's buffer: *out_size = length + 1, and
// 0x80ba8090 when that exceeds `size`.
int32_t copy_out(const std::string& s, char* buf, uint32_t size, uint32_t* out_size) {
  const size_t n = s.size() + 1;
  if (out_size) *out_size = static_cast<uint32_t>(n);
  if (n > size) return kErrBufferTooSmall;
  std::memcpy(buf, s.c_str(), n);
  return 0;
}

void clear_out(char* buf, uint32_t size, uint32_t* out_size) {
  if (buf) std::memset(buf, 0, size);
  if (out_size) *out_size = 0;
}

// 0x5bc0: "DeviceId=<macro <DEVICE_ID>>&sys_ver=<macro <SYS_VER>>" (both must be strings).
int32_t default_query(const Value& root, std::string& q) {
  q.clear();
  const Value& macro = root.member("macro");
  const Value& sys_ver = macro.member("<SYS_VER>");
  const Value& device_id = macro.member("<DEVICE_ID>");
  if (sys_ver.type() != Type::String || device_id.type() != Type::String) return kErrNotFound;
  q = "DeviceId=";
  q += device_id.string().c_str();
  q += "&sys_ver=";
  q += sys_ver.string().c_str();
  return 0;
}

// 0xdf0 (shared tail of GetDeviceId / GetSysVer): a string member of "macro".
int32_t get_macro(const char* key, char* buf, uint32_t size, uint32_t* out_size) {
  clear_out(buf, size, out_size);
  Snapshot snap;
  const int32_t err = load(&snap);
  if (err < 0) return err;
  const Value& v = snap.root.member("macro").member(key);
  if (v.type() != Type::String) return kErrNotFound;
  return copy_out(v.string(), buf, size, out_size);
}

}  // namespace

// int sceUrlConfigResolverUpdate(const char* config) — 0xd0 (NID DrRUD+rZV28).
// Opens the shared memory read-write without create (flags 2) on first use, writes the text
// with its terminator (more than 0x10000 bytes: 0x80ba8002) and then reloads this process's
// view (0x12d0, result ignored). Null config: 0x80ba8080.
SSN_PINVOKE("libSceUrlConfigResolverWrapper", "sceUrlConfigResolverUpdate", int32_t, ssn_UrlConfigResolver_Update,
            (const char* config)) {
  if (!config) return kErrInvalidArgument;
  const int32_t err = shm_write(config, std::strlen(config) + 1);
  if (err < 0) return err;
  load(nullptr);
  return 0;
}

// HOST POLICY — "bundle absent in the dump". On a console every RNPS app the URL
// config names is installed. A dump may lack an app's readable JS (only an encrypted
// RNPSHEDR bundle with no console decryption output in the dump, which needs the console's
// /dev/rnps to decrypt, or nothing at all). For such an app the host answers the URL's
// configuration without its "rnps" member, i.e. as if the URL were not served by an RNPS app,
// instead of letting the bundle load fail (RenderScene "Cannot fallback to original bundle",
// which ends the process). Every managed caller treats a configuration without "rnps" as an
// unavailable app: UrlConfigResolver.Utility.GetRnpsAppConfig returns null (as it does for an
// error), BootHelper.GetSchemeConfig keeps the scheme's plugin, and the AppModule
// SceneFactory.Create (ReactNative.Components.Vsh, the home's hub views) returns no scene. The
// earlier answer, no entry at all (0x80ba8087), is not handled by SceneFactory.Create (it
// does not catch), which the FW 4.03 home (rnps-home 683) reaches for the Media Gallery hub
// (psmediagalleryhub:main, NPXS40075). It is a declared host value, logged once per app; no
// bundle content is made up.
static bool rnps_bundle_absent(const Value& app) {
  const Value& rnps = app.member("rnps");
  if (rnps.type() != Type::Object) return false;
  const Value& tid = rnps.member("titleId");
  if (tid.type() != Type::String) return false;
  const std::string title = tid.string().c_str();
  const auto dir = std::filesystem::path(host::ctx().rnps_root) / "apps" / title;
  bool readable = false;
  for (const char* name : {"application.ps.bundle", "main.jsbundle", "host.ps.bundle"}) {
    std::ifstream f(dir / name, std::ios::binary);
    char magic[8] = {};
    if (!f || !f.read(magic, 8)) continue;
    // Plaintext JS, or an RNPSHEDR container the host's /dev/rnps can answer (the dump holds the
    // console's decryption output for it; libNativeExtensions DecryptRnpsBundle).
    if (std::memcmp(magic, "RNPSHEDR", 8) != 0 || native_ext::jsc::rnps_decryption_available(dir / name)) readable = true;
  }
  if (!readable) host::note_unimplemented("RNPS app " + title + ": bundle absent in the dump (no readable JS); its URL config answered without \"rnps\" (host policy)");
  return !readable;
}

// int sceUrlConfigResolverGetAppConfig(const char* url, bool enableDefault, char* config,
//                                      uint32_t size, uint32_t* outSize) — 0x2b0 (MTEBJdi3YNQ).
// The resolved app configuration serialized as JSON; it must be an object (0x80ba808f).
SSN_PINVOKE("libSceUrlConfigResolverWrapper", "sceUrlConfigResolverGetAppConfig", int32_t,
            ssn_UrlConfigResolver_GetAppConfig,
            (const char* url, uint8_t enable_default, char* config, uint32_t size, uint32_t* out_size)) {
  clear_out(config, size, out_size);
  Snapshot snap;
  Value v;
  const int32_t err = resolve(url, enable_default != 0, snap, v);
  if (err < 0) return err;
  if (v.type() != Type::Object) return kErrWrongType;
  if (rnps_bundle_absent(v)) {
    // The entry without its "rnps" app (see rnps_bundle_absent).
    json::Object o = v.object();
    o.erase("rnps");
    v = Value::make_object(std::move(o));
  }
  return copy_out(json::serialize(v), config, size, out_size);
}

// int sceUrlConfigResolverGetAppUrl(const char* appUrl, const char* locationUrl,
//     bool enableDefault, char* url, uint32_t size, uint32_t* outSize) — 0x4f0 (MxdDxN+kWA8).
// Resolves locationUrl (appUrl is not used); the value must be a string. Every key of the
// "macro" object is replaced in it (all occurrences, keys in order), then the default query
// is appended after '?' — or after '&' when the URL already has a query, or directly when
// its '?' is the last character.
SSN_PINVOKE("libSceUrlConfigResolverWrapper", "sceUrlConfigResolverGetAppUrl", int32_t, ssn_UrlConfigResolver_GetAppUrl,
            (const char* app_url, const char* location_url, uint8_t enable_default, char* url, uint32_t size,
             uint32_t* out_size)) {
  (void)app_url;
  clear_out(url, size, out_size);
  Snapshot snap;
  Value v;
  int32_t err = resolve(location_url, enable_default != 0, snap, v);
  if (err < 0) return err;
  if (v.type() != Type::String) return kErrWrongType;
  std::string s = v.string().c_str();
  // std::string::find/replace semantics, as in the firmware (an empty key would never end,
  // there as here; the ShellUI's macro keys are "<NPENV>", "<SYS_VER>", "<DEVICE_ID>").
  for (const auto& [k, val] : snap.root.member("macro").object()) {
    const std::string& rep = val.string();
    for (size_t pos = s.find(k); pos != std::string::npos; pos = s.find(k, pos)) {
      s.replace(pos, k.size(), rep);
      pos += rep.size();
    }
  }
  std::string q;
  err = default_query(snap.root, q);
  if (err < 0) return err;
  if (!q.empty()) {
    const size_t qm = s.find('?');
    if (qm == std::string::npos) s += '?';
    else if (qm != s.size() - 1) s += '&';
    s += q;
  }
  return copy_out(s, url, size, out_size);
}

// int sceUrlConfigResolverGetDefaultQueryParameter(char* buf, uint32_t size,
//                                                  uint32_t* outSize) — 0xc30 (7wCjXDc2cjI).
SSN_PINVOKE("libSceUrlConfigResolverWrapper", "sceUrlConfigResolverGetDefaultQueryParameter", int32_t,
            ssn_UrlConfigResolver_GetDefaultQueryParameter, (char* buf, uint32_t size, uint32_t* out_size)) {
  clear_out(buf, size, out_size);
  Snapshot snap;
  int32_t err = load(&snap);
  if (err < 0) return err;
  std::string q;
  err = default_query(snap.root, q);
  if (err < 0) return err;
  return copy_out(q, buf, size, out_size);
}

// int sceUrlConfigResolverGetDeviceId(char* buf, uint32_t size, uint32_t* outSize)
// — 0xdb0 (Y00mcdiTiOA): macro "<DEVICE_ID>".
SSN_PINVOKE("libSceUrlConfigResolverWrapper", "sceUrlConfigResolverGetDeviceId", int32_t,
            ssn_UrlConfigResolver_GetDeviceId, (char* buf, uint32_t size, uint32_t* out_size)) {
  return get_macro("<DEVICE_ID>", buf, size, out_size);
}

// int sceUrlConfigResolverGetSysVer(char* buf, uint32_t size, uint32_t* outSize)
// — 0xfc0 (Syu+Sbd7R4s): macro "<SYS_VER>".
SSN_PINVOKE("libSceUrlConfigResolverWrapper", "sceUrlConfigResolverGetSysVer", int32_t,
            ssn_UrlConfigResolver_GetSysVer, (char* buf, uint32_t size, uint32_t* out_size)) {
  return get_macro("<SYS_VER>", buf, size, out_size);
}

// bool sceUrlConfigResolverIsUpdated() — 0x1000 (C4AAV-A4pnA): the shared memory has been
// written since this process last read it (false before the first query opened it).
SSN_PINVOKE("libSceUrlConfigResolverWrapper", "sceUrlConfigResolverIsUpdated", uint8_t,
            ssn_UrlConfigResolver_IsUpdated, ()) {
  Resolver& r = resolver();
  std::lock_guard lock(r.m);
  return r.opened && shm_sequence() != r.last_seq;
}

// int sceUrlConfigResolverGetJscHeapSizeSoftLimit(uint32_t* softlimit) — 0x1060 (kBo7ItYgSPo).
// Stores 104857600 first, then urlconfig "jsc-heap-size-soft-limit" when a load succeeds.
SSN_PINVOKE("libSceUrlConfigResolverWrapper", "sceUrlConfigResolverGetJscHeapSizeSoftLimit", int32_t,
            ssn_UrlConfigResolver_GetJscHeapSizeSoftLimit, (uint32_t* softlimit)) {
  if (!softlimit) return kErrInvalidArgument;
  *softlimit = kDefaultJscHeapSoftLimit;
  const int32_t err = load(nullptr);
  if (err < 0) return err;
  Resolver& r = resolver();
  std::lock_guard lock(r.m);
  *softlimit = r.jsc_heap_soft_limit;
  return 0;
}

// int sceUrlConfigResolverGetReleaseVersion(char* buf, uint32_t size, uint32_t* outSize)
// — 0x1130 (a3kUt2XrhOs): urlconfig "release" (0x80ba8091 when absent).
SSN_PINVOKE("libSceUrlConfigResolverWrapper", "sceUrlConfigResolverGetReleaseVersion", int32_t,
            ssn_UrlConfigResolver_GetReleaseVersion, (char* buf, uint32_t size, uint32_t* out_size)) {
  clear_out(buf, size, out_size);
  Snapshot snap;
  const int32_t err = load(&snap);
  if (err < 0) return err;
  if (!snap.release) return kErrNotFound;
  return copy_out(*snap.release, buf, size, out_size);
}

}  // namespace ssn::urlcfg
