# URL config resolver

The ShellUI decides which app serves a URL (`pssettings:`, `pshomeui:main`, `cdlg:message`...)
through the URL config: a JSON manifest that maps URL prefixes to apps and apps to their
launch configuration (for React Native apps, `{"rnps": {"component", "titleId"}}`).
Managed code reaches it through `Sce.Vsh.UrlConfigResolver.dll`, whose P/Invoke library
`libSceUrlConfigResolverWrapper` is published by the ShellUI eboot (NPXS40087, `{name, fn}`
table at 0xcc4d60). The entries of that table are the eboot's imports of
`/system/common/lib/libSceUrlConfigResolver.sprx`, so each managed call lands directly on
the export of the same name.

Host implementation: `psmhost/libSceUrlConfigResolver/` (`url_config_resolver.cpp`, plus
`sce_json.*` for the libSceJson2 behaviour it depends on).

## Who owns the configuration

| Party | Role |
|---|---|
| SceShellCore | Creates the shared memory `SceUrlConfigResolverShm` at boot (0x775ed0, vsh `common_lib/atomic_buffer`, 0x10000 bytes per half, created read-write) and keeps it. Never writes it. |
| ShellUI, `SharedStoreUriManager` (Sce.Vsh.ShellUI.Legacy) | On first start, takes the cached download (`/<sandbox>/priv/cache/urlconfig/urlconfig_cache.json`, verified by URL hash and salted SHA-256) or, if absent, the **OfflineConfig manifest** embedded in `Sce.Vsh.ShellUI.AppSystem.dll` (resource `Sce.Vsh.ShellUI.AppSystem.resource.manifest.json`), and publishes `{"urlconfig": <manifest>, "macro": {"<NPENV>", "<SYS_VER>", "<DEVICE_ID>"}}` with `sceUrlConfigResolverUpdate`. Later downloads from `https://urlconfig.[<env>.]api.playstation.com/ps5/<ver>/manifest` replace it. |
| libSceUrlConfigResolver (every reader process) | Parses the published text on demand and answers queries. It has **no built-in configuration**: the `enableDefault` flag only selects the `"default"` entry *inside* the published scheme table. |

The cache-file path in the library is used only for `sceCoredumpAttachUserFile(0xf801, path)`
(diagnostics); the library never reads the file.

Until a configuration is published the shared memory holds zeros, the parse fails, and every
query returns `0x80848101` (libSceJson2 parse error). The ShellUI already queries before
`SharedStoreUriManager.Initialize` runs (`BootHelper.GetSchemeConfig` from `BootManager.EvalBase`);
it catches the resulting `PsmNativeException` and keeps its built-in scheme table.

## Shared memory

`atomic_buffer` layout: header `{magic 0xa1544cbdffe34255, sequence, half size}` and two
halves. A writer copies into the half not in use (`(seq & 1) == 0 ? 1 : 0`) and increments
the sequence; a reader copies half `seq & 1` (retrying if the sequence moved) and records the
sequence. "Updated" means the sequence differs from the last one read. Text larger than one
half (`strlen + 1 > 0x10000`) is rejected with `0x80ba8002`.

The host keeps one in-process buffer that exists from the start, standing in for the one
SceShellCore creates before the ShellUI runs.

## Loading (0x12d0)

On the first query: register the cache path with the coredump service, load sysmodules
0xe7 and 0x8000001f, read the NP environment string with `sceRegMgrGetStr(0x19800000, buf, 17)`
(an error is returned as is; a 17-byte value gives `0x80ba8081`), open the shared memory
read-only. Then, whenever the sequence changed (and once initially), copy 0x10000 bytes,
parse `strlen` of them with `sce::Json::Parser`, and validate:

| Check | Error |
|---|---|
| root is an object | 0x80ba8082 |
| `urlconfig.scheme` present (truthy) / object | 0x80ba8083 / 0x80ba8084 |
| `scheme[<env>]`, else `scheme.default`, present / object | 0x80ba8085 / 0x80ba8086 |
| `urlconfig.app` present / object | 0x80ba808a / 0x80ba808b |
| every `app` member is an object | 0x80ba808d |
| `jsc-heap-size-soft-limit`, when truthy, is an unsigned integer | 0x80ba8092 |

It then builds a hash table *URL prefix -> string* from the chosen scheme object and
*app name -> object* from `app`, keeps `jsc-heap-size-soft-limit` (default 104857600) and
`release` (when a non-empty string). The result of the load is remembered and returned by
every query until the shared memory changes again.

"Truthy" is `sce::Json::Value::operator bool`: null false; boolean its value; numbers non-zero;
string non-empty; array/object true.

## Resolving a URL (0x4ff0)

1. If the URL contains `:` that is not its last character: take the text before the first
   `?` after it, look it up; while not found, cut it at its last `/` (a trailing `/` first)
   and retry, stopping when nothing is left or no `/` remains.
2. Look up the scheme (text before the first `:`, or the whole URL).
3. With `enableDefault`, look up `"default"`.
4. Not found: `0x80ba8087`. The value must start with `app:` (`0x80ba8089`).
5. Find the app by the name after `app:`, else the app `"default"` (`0x80ba808c`).
6. Result: `app[<env>]` if truthy, else `app.default` if truthy (`0x80ba808e`).

## Entry points

All buffer-returning entries zero the buffer and `*outSize` first, set `*outSize` to
length + 1, and return `0x80ba8090` when that exceeds `size` (the managed wrapper then
grows its `StringBuilder` and retries).

| Entry (NID) | Address | Behaviour |
|---|---|---|
| `sceUrlConfigResolverUpdate(config)` (DrRUD+rZV28) | 0xd0 | Open the shared memory read-write (no create) on first use, write `config` with its terminator, reload this process's view (result ignored), return 0. Null: 0x80ba8080. |
| `sceUrlConfigResolverGetAppConfig(url, enableDefault, buf, size, outSize)` (MTEBJdi3YNQ) | 0x2b0 | Resolve; the result must be an object (0x80ba808f); returns it serialized. |
| `sceUrlConfigResolverGetAppUrl(appUrl, locationUrl, enableDefault, buf, size, outSize)` (MxdDxN+kWA8) | 0x4f0 | Resolve `locationUrl` (`appUrl` is unused); the result must be a string. Replace every `macro` key in it, then append the default query after `?`, after `&` if a query exists, or directly if `?` is last. |
| `sceUrlConfigResolverGetDefaultQueryParameter(buf, size, outSize)` (7wCjXDc2cjI) | 0xc30 | `DeviceId=<macro <DEVICE_ID>>&sys_ver=<macro <SYS_VER>>`; both must be strings (0x80ba8091). |
| `sceUrlConfigResolverGetDeviceId(buf, size, outSize)` (Y00mcdiTiOA) | 0xdb0 | String `macro["<DEVICE_ID>"]` (0x80ba8091). |
| `sceUrlConfigResolverGetSysVer(buf, size, outSize)` (Syu+Sbd7R4s) | 0xfc0 | String `macro["<SYS_VER>"]` (0x80ba8091). |
| `sceUrlConfigResolverIsUpdated()` (C4AAV-A4pnA) | 0x1000 | Shared memory open and its sequence differs from the last one read. |
| `sceUrlConfigResolverGetJscHeapSizeSoftLimit(&limit)` (kBo7ItYgSPo) | 0x1060 | Store 104857600, then the configured value if the load succeeds. Null: 0x80ba8080. |
| `sceUrlConfigResolverGetReleaseVersion(buf, size, outSize)` (a3kUt2XrhOs) | 0x1130 | `urlconfig.release` (0x80ba8091). |

## JSON details (libSceJson2)

The resolver's output is produced by `sce::Json::Value::serialize`: no whitespace, object
members in byte order of their keys (objects are ordered maps; a repeated key keeps the last
value), strings escaped as `\b \t \n \f \r \" \/ \\` and `\u%04x` for other control bytes
and 0x7f, bytes >= 0x80 unchanged; integers `%ld` / `%lu`. The parser accepts one value with
leading tab/LF/CR/space and does not look past it; positive integers become unsigned
(type 3), negative ones signed (type 2), anything with `.`, `e`, `E`, `+` or an inner `-`
becomes real.

Example, with the OfflineConfig manifest published:
`GetAppConfig("pssettings:", false)` -> `{"rnps":{"component":"Main","titleId":"NPXS40008"}}`
(`pssettings` -> `app:rnps-settings`); `pssettings:accounts` -> NPXS40009;
`pshomeui:main` -> NPXS40002.

## Not reproduced

* **NP environment** (registry key 0x19800000): the host has no registry. The host treats the
  per-environment members as absent, so the `"default"` members are used — the only ones the
  OfflineConfig manifest has. Reported once through `note_unimplemented`. (A console whose
  registry read failed would instead return the registry error from every query.)
* **Real numbers**: the firmware's string-to-double routine (libSceJson2 0x75b0) is not
  transcribed (the C library's `strtod` is used), and the serializer's real format depends
  on how the process initialised libSceJson2 (`%1.17g` with InitParameter2, `%f`
  otherwise), which is not traced (`%f` is used). Both report through
  `note_unimplemented`; the manifest contains no real numbers.
* libSceJson2's optional access-error callbacks and the `inf`/`nan` extensions of the
  extended init mode are not modelled.
* Sysmodule loads and the coredump registration are no-ops on the host.
