# RN bridge: the JavaScriptCore executor layer

The PS5 ShellUI runs its React Native apps (RNPS: Home, Settings, …) through managed code in
`ReactNative.PUI.dll`. The managed `JSCJavaScriptExecutor` and `PerformanceModule` do not touch
JavaScriptCore directly; they call a thin C layer exported by `libNativeExtensions.sprx`, which
wraps libSceJsc's JSC C API. SceShellNative reproduces that layer in
`psmhost/libNativeExtensions/jsc/` over the same C API of a Windows JavaScriptCore built from
Sony's published JSC-400 source (the source of libSceJsc for FW 4.00–4.51).

Reference binary: `libNativeExtensions.sprx` FW 4.03. Addresses below are offsets in
`libNativeExtensions.elf`.

## Pieces

| File | Firmware routine(s) |
|---|---|
| `jsc_common.h/.cpp` | marshalling structs, RNPS heap (0x14d70/0x14de0/0x14f30), string copies (0xcb80, 0xcc60), `CreateJSError` (0xffe0), `ConvertJSONStrToJSValueRef` (0x10620), snprintf helpers, `SetObjectProperty` (0xd910) |
| `jsc_executor.cpp` | `JSCExecutor*` exports (0xce70–0xeb30) and the per-context builtins |
| `jsc_global_hooks.cpp` | `JSCExecutorInstallGlobalHooks` (0xef20) and its five JS shims |
| `jsc_razor.cpp` | global `razor` (0x146a0–0x14c30) |
| `jsc_performance.cpp` | `CreatePerformanceModule`, hook tables and the `performance.*` shims (0x12bc0–0x143c0) |
| `decrypt_rnps_bundle.cpp` | `DecryptRnpsBundle` (0x16370, 0x16a20) |
| `jsc_sysmodule.cpp` | `/dl_sysmodule:SCE_SYSMODULE_INTERNAL_JSC` imports forwarded to JavaScriptCore.dll |
| `jsc.cmake` | include dirs, import library and runtime DLL copy |

All entries register through `SSN_PINVOKE` under the library string the managed `DllImport`
uses: `libNativeExtensions.sprx` for the executor and performance entries,
`NativeExtensions.sprx` for `DecryptRnpsBundle`, and `/dl_sysmodule:SCE_SYSMODULE_INTERNAL_JSC`
for the four JSC entry points.

## Data passed across the boundary

- **JSCExecutorString** `{ char* ptr; bool isStatic; }` (16 bytes). Native code stores either a
  static literal (`isStatic = 1`, never freed) or a string from the module's private
  "RNPS native" heap. Managed frees heap strings with `JSCExecutorFreeJSCStr`, and writes
  strings back to native (results and errors of managed callbacks) with
  `JSCExecutorCopyToJSCStr`, which allocates from the same heap. The console uses an mspace; the
  host uses a growable private Win32 heap created on first use.
- **JSCExecutorExceptionWrapper** `{ JSCExecutorString message; bool isJSException; }` — the flag
  at +16 distinguishes a JS exception (message is the exception's JSON with indent 2; managed
  throws `JSException`) from a native error literal (managed throws `RNPSNativeHookException`).
- **Executor** — 112 bytes from `operator new`: `+0` `"__fbBatchedBridge"`, `+8` copy of the
  managed `GlobalHooks` (7 pointers: UserArg, ConsoleLog, GetNativeVersionStr,
  NativeBundleRegistryLoad, NativeCallSyncHook, NativeFlushQueueImmediate, NativeRequire),
  `+0x40` the global context, `+0x48` cached global object, `+0x50` cached `__fbBatchedBridge`,
  `+0x58/+0x60/+0x68` cached `callFunctionReturnFlushedQueue`,
  `invokeCallbackAndReturnFlushedQueue`, `flushedQueue`. The bridge methods are looked up once
  and never invalidated.
- **NativeHookParameters** `{ ClassName; ParentObject; PrivateData }` and hook table entries
  `{ name; callAsFunction; finalize }` (24 bytes) for `JSCExecutorInstallNativeHooks`.
- **JSCMemoryUsageStatistics** — seven `u64` returned by value: heapSize, heapCapacity,
  extraMemorySize, objectCount, protectedObjectCount, globalObjectCount,
  protectedGlobalObjectCount, read from `JSGetMemoryUsageStatistics` (the property-name strings
  are created once per process).

Managed callbacks are plain function pointers (Mono-marshalled delegates). A callback reports
failure by writing a JSON string `{"message": ...}` into its `error` out-string; the shim parses it
with `JSValueMakeFromJSONString` and throws it, falling back to a plain string value.

## Executor lifecycle

`JSCExecutorCreate(isInternalDevelopmentMode)`:

1. `JSClassCreate` of a global-object class whose only callback is `setProperty`.
2. `JSGlobalContextCreateInGroup(NULL, class)`; executor allocated and zeroed.
3. Builtins, in order: `__Blob` (constructor), global `eval` replacement, global `razor`. Any
   failure releases the context and returns NULL.
4. `JSGlobalContextSetDebuggerRunLoopWithCurrentRunLoop` binds the inspector run loop to the
   creating thread (the app's JS queue thread). `JSCExecutorDelete` unsets it and releases the
   context.

The managed constructor then names the context, registers the heap finalizer, defines
`navigator` and `rnps` as `{}`, calls `JSCExecutorInstallGlobalHooks`, evaluates `__DEV__` and
its console polyfill, and later installs the performance hooks, `__fbBatchedBridgeConfig`
(`JSCExecutorSetGlobalVariableFromJSON`) and calls `__fbBatchedBridgeConfigUpdateNativeModules`
(`JSCExecutorCallGlobal`, optional).

## JS surface installed natively

| Name | Behaviour |
|---|---|
| global object `setProperty` | assigning `onunhandledrejection` evaluates a forwarding function `(promise, reason) => global.onunhandledrejection({promise, reason})` and registers it with `JSGlobalContextSetUnhandledRejectionCallback`; the store itself still happens |
| `new __Blob(id)` | returns a `__BlobHandle` owning a copy of `id`; construction calls managed `BlobModule` increment, finalization calls remove |
| `eval(x)` | always throws `{ "message": "eval(x)\neval() is disabled. Please consider different implementation." }`; `new Function` is untouched |
| `razor.start/stop/pushMarker/popMarker` | Razor CPU capture and markers; start/stop/push act only in internal development mode, popMarker always calls through |
| `nativeCallSyncHook(m, method, args)` | exactly 3 arguments; ids truncated to int, args as JSON; returns the managed result parsed as JSON |
| `nativeFlushQueueImmediate(queue)` | one argument (other counts are ignored); queue as JSON |
| `bundleRegistryLoad(name)` | string argument; managed loads and evaluates the bundle; returns undefined |
| `nativeRequire(id[, bundle])` | RAM-bundle module load; any other argument count throws "Incompatible version detected" with the JS and native versions |
| `rnps._nativeLoggingHook(msg, level)` | exactly 2 arguments; console output |
| `performance.*`, `nativePerformanceNow` | getEntries, getEntriesByName, getEntriesByType, clearResourceTimings, setResourceTimingBufferSize, now, mark, clearMarks, measure, clearMeasures; argument validation and error texts as on the console |

Native-originated errors use the template `{ "message": "%s" }` with the text spliced in
unescaped; a text containing quotes therefore fails to parse and is thrown as the raw string.

## Bridge calls

`CallFunctionReturnFlushedQueue`, `InvokeCallbackAndReturnFlushedQueue` and `FlushedQueue` call
the cached `__fbBatchedBridge` method with `(module, method, JSON.parse(args))`,
`(callbackId, JSON.parse(args))` or no arguments, and return the result as JSON with indent 2.
`GetJSVersion` returns `__ReactNativeVersion` the same way. `EvaluateScript` evaluates source
text (no bytecode cache exists in this layer) with the given source URL at line 0.

## Threading

One context per RNPS app; every entry point runs on that app's JS thread (the managed side
asserts it). There is no locking in the layer. `JSRunLoop` and
`JSRunLoopIterateCurrentRunLoop` matter only when the remote inspector is enabled
(`JSRemoteInspectorServerStart`).

## Host differences

- **DecryptRnpsBundle**: the console decrypts in place through `/dev/rnps` (ioctl `0xC0105203`,
  SBL/SAMU). Managed `JavaScriptBundleDecryptor.Decrypt` passes the whole container (header
  included) and afterwards reads the plaintext at the header's payload offset (+0x1C) with its real
  payload size (+0x24; +0x20 is the encrypted, 16-byte aligned size). The host has no SBL. When the
  dump carries the console's decryption output for the container
  (`<dump root>/rnps/<apps|bgs>/<titleId>/<bundle>_out/<bundle stem>_`), the host's `/dev/rnps`
  writes it over the payload and returns 0 (HOST VALUE, `rnps_decryption.h`; the container must be
  byte-identical to the dump's and the output exactly the real payload size). Otherwise it keeps the
  16-byte alignment check and returns `0x800F1213`, the console's result when `/dev/rnps` cannot be
  opened. The FW 4.03 dump's outputs are RAM bundles (magic 0xFB0BD1E5), which the managed
  `FileJavaScriptBundleLoader` turns into a `JSIndexedUnbundle` (12-byte header: magic, module
  count, startup-code size; 8-byte {offset, length} entries relative to the end of the table;
  startup code first) and whose modules the startup code loads through `nativeRequire` (0xf7d0,
  above) -> `JSCJavaScriptExecutor.NativeRequire` -> `EvalModule`; `host`/`base_dll` module ids
  resolve through the `BundleRegistry` (NPXS40141 `host.ps.bundle`, `base_dll.ps.bundle`).
- **RNPS heap creation**: the original creates its mspace on first use without synchronisation
  (0x14d70/0x14de0); the host creates its heap exactly once, because its first use comes from up to
  twelve app-pool JS threads at once (two heaps would mean frees into the wrong heap).
- **Stack bounds**: JS threads are Mono threads with a fully committed stack; the JSC-400 Win64 build
  takes the bounds from `GetCurrentThreadStackLimits` (`tools/jsc/PATCHES.md` P4).
- **CurlGetDefaultUA** (`curl.cpp`): `"RNPS/<major>.<minor:02> (PlayStation 5)"` from the compiled SDK
  version (`sceKernelGetProsperoCompiledSdkVersion`, the high half of the eboot's process-parameter
  `sdk_version`): `RNPS/4.03 (PlayStation 5)`.
- **Razor**: `sceRazorCpu*` have no host counterpart; the stand-ins return SCE_OK and are logged
  through `note_unimplemented`.
- **JSRemoteInspectorServerStart** returns the bound port (`uint16_t`); managed declares `bool`,
  so the forwarder returns the port zero-extended to 32 bits.
- **`long` in diagnostics**: formats using `%lx`/`%lu` on the console use the `long long`
  conversions on Windows so the printed text is unchanged.
- **Faithful defects**: the layer keeps the original's behaviour where it is fragile — strings
  are copied out of `JSValueToStringCopy`/`JSValueCreateJSONString` results without a null check
  (a throwing `toString` crashes, as on the console); `eval()` with no argument reads `argv[0]`;
  `JSCExecutorGetJSCodeCovarage` hands managed an `operator new[]` buffer that managed frees on
  the RNPS heap; `SetObjectProperty`'s unused parent-script path stores on a null object.
- `JSCExecutorGetJSCodeCoverage` (3 arguments) is declared by managed code but not exported by
  the firmware; it is not provided.

## Case study: Settings > Sound after Users and Accounts

Symptom: in a Settings instance that was relaunched (after Users and Accounts / Family, which
run NPXS40009 as a separate ShellApp), opening Sound throws `null is not an object (evaluating
'e.deviceInfoListForPadSpeaker')`. The JS is the firmware bundle and every step listed here is
firmware code; the host-side candidates checked are listed with the evidence that rules them out.

### Mechanism (firmware code)

1. Relaunch is unconditional. `BootHelper.Boot(uri, Option.AddApp)` (Legacy 23125) →
   `ShellAppManager.Add` → `SuspendAll` → `ShellApp.OnSuspendApp` → `PluginManager.Unload`; the
   RN app is terminated in `RenderScene.OnUnloading`. No memory or capacity query on that path.
2. The relaunched instance restores its navigation from `ShellApp.getProperty("redux").nav`
   (the ShellApp keeps its properties across suspend; only `OnExitApp` clears them), so
   `isHistoricalBack` is true (application bundle, index.js module, line 41421).
3. The main screen's `_onPerformanceTiming` (line 86367) returns early when `isHistoricalBack`
   is true. On a first launch its `"interactive"` event sends the LoadTime telemetry, whose
   `addLoadTimeInfo` reads `AppState.currentState` (line 84409) and so builds the JS AppState
   wrapper (`host.ps.bundle` module `p7dS6yI0`). The wrapper's constructor calls
   `getCurrentAppState`; its answer (`"active"` vs the initial `"uninitialized"`) is emitted as a
   synthetic `appStateDidChange` while nobody listens. In a relaunched instance this does not
   happen, so the wrapper is built by Sound's `componentDidMount` (line 184275) itself.
4. Native side: `AppStateModule.getCurrentAppState` → `DispatcherHelpers.RunOnDispatcher` →
   `FrameTask.CallOnce` → the next `FrameTaskManager.Update` (one per `UISystem.Update`, i.e.
   one per frame).
5. Sound fills `_padSpeakerInfo` only after about seven sequential promise round trips
   (`getAudioPeripheralInfoList`, `isPersonalAudioPeripheralInUse` ×2, `getMorpheusUser`,
   `getCaesarUser`, then the `Promise.all` of `getDeviceInfoForVolume`). Every one of them is an
   asynchronous `[ReactMethod]` promise run on the module's TaskQueueBridge (no blocking
   synchronous method on this path). If the AppState answer arrives first, the synthetic
   `"active"` event reaches Sound's `_onAppStateChanged`, which calls
   `getVolumePadSpeakerUpdateInfo(null)`.

Measured on the host (Mono trace): the AppState answer arrives 0–16.7 ms after the call (the
frame phase; e.g. 6.1 ms), the chain needs about 8–10 ms, so the crash is usual but not certain
(the crash-relaunch loop sometimes gets through).

### Host candidates ruled out

| Candidate | Evidence |
|---|---|
| AppState value / timing in the native module | IL as shipped (ReactNative.PUI `AppStateModule`, `DispatcherHelpers`, `FrameTask`); `OnFG` comes from `RenderScene.UpdateAppState` (Vsh.Common), no host input. |
| Main-loop pacing | Every frame: ~0.7 ms of work, ~15.9 ms in `SwapBuffers` (host 60 Hz pacing); `FrameTaskManager.Update` runs ~0.3 ms after it. `PsmGraphicsContext::WaitVBlank` (libScePsm 0x1e77b0) was an unimplemented stub and is now reproduced, but ShellUI calls it about once per run. |
| Thread priorities | Main thread 520 (eboot procparam 0xd42060 +0x28 → 0xd4c06c), JS / native-modules threads 615, Vsh.Common queue 612. `Runtime_SetThreadPriority` (libScePsm 0x300f90) is `scePthreadSetprio(self, v)`; `ThreadUtils_SetSchedulePolicy` (0x15330) leaves the policy unchanged for SCHED_OTHER. The main thread outranks the chain, so on the console contention favours the AppState answer; the host stubs (no-ops) cannot make the chain lose. |
| Bridge flush / callback delivery | `nativeFlushQueueImmediate` is installed under the same name and calls the managed hook synchronously exactly as 0xf440 does; the host bundle's MessageQueue uses it with the 5 ms threshold (host.ps.bundle line 7072). The bridge-call exports are thin wrappers; batching and queues are managed IL (`SingleBackgroundMessageQueueThread`). |
| Unload on suspend | See Mechanism 1: unconditional. |

The console passes, and a PC emulator crashed like the host: both PC runs used the 680.72
bundles. See *Resolved* below.

### Resolved: the JS bundle version

The JS that ran was not the JS a 4.03 console runs. The plaintext bundles used until then
(every copy then in hand) are RNPS
`0.59.6-680.72`; the 4.03 managed side is `0.59.6-683.80`, and every launch logged
`FW Version Mismatch: JS 0.59.6-680.72 != native 0.59.6-683.80` (they only ran with
`--rnps-version-policy warning`).

The complete FW 4.03 devkit dump (`full_4_03_devkit_dump_dec`) carries the console's decryption
outputs of its own bundles: Settings (NPXS40008, manifest `4.0.0+19589`, RNPS `0.59.6-683.36`) and
the host / base_dll bundles (NPXS40141, `0.59.6-683.80`). With it as the firmware (no `--rnps`, no
version policy) no mismatch is traced, and Sound opens after Users and Accounts / Family in the
relaunched Settings instance:

| Setup (same launcher build) | Runs | Sound after Users / Family |
|---|---|---|
| 4.03 dump, Settings 683.36, `--cold-boot` | 4 (s2, s3, s4, s6) | opens every time; no `deviceInfoListForPadSpeaker` error |
| 4.03 dump, Settings 683.36, no `--cold-boot` | 1 (s5) | opens |
| kek + `--rnps` 680.72 + warning policy | 1 (o1, control) | crashes (`null is not an object (evaluating 'e.deviceInfoListForPadSpeaker')`, 18 traces, crash-relaunch loop) |

Each run walks the 13 categories in their original order (`work/ida_r4nav/gen.py 3300 560 13`;
Settings is relaunched twice, after Users and Accounts and after Family, as before). Evidence:
`work/evidence/r7_dump403_sound_after_users_family_old_vs_new.png`,
`r7_dump403_sound_runs_s3_s4_s5_s6.png`, `r7_dump403_settings13_run_s2_part1/2.png`.

The fix is in the JS. Both Settings bundles are RAM bundles; the pad-speaker helpers are module 120
in 680.72 and module 105 in 683.36, Sound's screen is module 1880 / 1950. Sound's
`_onAppStateChanged` and `componentDidMount` are unchanged (it still calls
`getVolumePadSpeakerUpdateInfo(this._padSpeakerInfo)` on every `"active"`, with
`_padSpeakerInfo` still null when the synthetic AppState event wins the race). 683.36 makes the
two helpers that `getVolumePadSpeakerUpdateInfo` calls null-safe:

```
680.72 (module 120):
_getDeviceInfoWithFunctionSpeakerEnabled = function(e) {
    return e.deviceInfoListForPadSpeaker.find(function(e) { return e.enabledFunctions.includes("padSpeaker") })
}
_isVolumePadSpeakerEnabled = function(e) { var n = e.deviceInfoListForPadSpeaker, ...

683.36 (module 105):
_getDeviceInfoWithFunctionSpeakerEnabled=function(e){return e?e.deviceInfoListForPadSpeaker.find(...):null}
_isVolumePadSpeakerEnabled=function(e){if(!e)return!1;var n=e.deviceInfoListForPadSpeaker,...
```

With a null `_padSpeakerInfo` the 683.36 path yields `{updateData: {itemId: "id_volume_pad_speaker",
enabled: false, value: 0}, userParam: undefined}` instead of throwing. The race itself (AppState
answer vs. the device-info chain) is unchanged and was never a host defect; the native-side
candidates listed above stay ruled out.

## Build

`jsc.cmake` adds the JSC-400 Win64 build (`deps/jsc/build`): public and private header
directories, `lib64/JavaScriptCore.lib`, and copies JavaScriptCore.dll, WTF.dll, CFLite.dll,
pthreadsVC2.dll and the ICU 67 DLLs next to `SceShellNative.exe`. The PlayStation-only API
(`JSContextRefPrivatePlayStation.h`, `JSRemoteInspectorServer.h`) is part of that build's private
headers.
