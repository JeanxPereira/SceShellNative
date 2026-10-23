# App launch: how ShellUI starts its React Native apps

ShellUI (FW 4.03) runs its React Native apps — Control Center (NPXS40003), Home (NPXS40002),
the notification overlay (NPXS40011), Settings, InitialSetup and the rest — **inside its own
process**. Starting one is a PUI scene load, not a process launch. SceShellCore's launcher
(`SceLncService`) does not start or announce these apps. What ShellUI does need from
SceShellCore on this path are three other services, all of which live in SceShellCore:

- the **user service**, for the logged-in and foreground users;
- the **RNPS app manager**, for the install record of each RNPS title;
- the **launcher service**, for the LncUtil event queue that ShellUI polls every frame.

SceShellNative has no SceShellCore. `psmhost/shellcore/` answers the client calls ShellUI
makes into those services, using the answers the real services give.

Reference binaries (FW 4.03): `eboot.bin` (ShellUI), `libSceSystemService.sprx` (LncUtil
client), `libSceUserService.sprx`, `libSceRnpsAppMgr.sprx`, `libSceIpmi.sprx`,
`SceShellCore.elf`. Addresses are offsets in those modules once `tools/ida/ps5_to_elf.py`
has converted them.

## The managed path

1. **Boot.** `BootManager.EvalBase` (Sce.Vsh.ShellUI.Legacy) runs these steps in order:
   - waits for `BackgroundInitFinishedBeforeBasePlugin`;
   - calls `BootHelper.Boot("<scheme>:launched")` for each base scheme (`psbase`,
     `psnotification`, `pshomeui`, …);
   - calls `PluginManager.Load("ControlCenterPlugin", "", cb)`.

   The callback `cb` sets `BootEvent.BasePluginLoadFinished`. `EvalInitialSetup` waits for
   that event before it calls `BootHelper.Boot("psinitialsetup:launched")`.
2. **Scheme to plugin.** `BootHelper.Boot` (Sce.Vsh.ShellUI.AppSystem) maps the scheme to a
   plugin through `PluginList`, and through the URL config's `rnps` entry for `psrn`. The two
   cases are:
   - **Not a ShellApp:** `PluginManager.Load(plugin, uri)`.
   - **A ShellApp** (`ReactNativePlugin`, `InitialSetupPlugin`): `ShellAppManager.Add(new ShellApp(...))`.
     `ShellApp.OnInitApp` then also ends in `PluginManager.Load`.

   No LncUtil call is involved.
3. **Plugin load.** `PluginManager.Load` creates the plugin scene through its `IPluginFactory`
   and queues it with `SceneBase.LoadAsync`. The PUI `SceneManager` loads queued scenes one at
   a time (`SceneQ : Loaded ...` in the log). If one scene's `OnLoading` never finishes, every
   scene queued after it waits too. The load callback runs from `ProcessLoaded` once the scene
   has loaded.
4. **BasePlugin.** BasePlugin is queued before ControlCenterPlugin. Its constructor posts a
   task that runs `UT.Np.Utility.Start()` and `UT.NetCtl.Utility.Start()` and then sets
   `m_isLoadTaskFinished`. `OnLoading` returns `false` until that flag is set and the base
   resources have been prefetched.
   - `Np.Utility.Start` creates a `PlusState` for every user in `UserService.LoginUserIdList`.
     Each one calls `NpManager.sceNpManagerIntAddPlusMemberTypeCallback`.
   - The login list comes from the `UserServiceWrapper._GetLoginUserIdList` icall.
   - **Failure mode:** if that list holds duplicates or bogus ids, NpManager's callback
     dictionary throws. The task then never sets `m_isLoadTaskFinished`, and BasePlugin and
     everything behind it (ControlCenterPlugin, so also `BasePluginLoadFinished`) never load.
   - `BasePlugin.OnLoaded` then runs `PLayerTransition.Initialize`. That builds a 1×1 black
     texture through `Image.ToBuffer` → `Image::GetPixelDataSize/GetPixelData` (libScePsm
     0x2dfe60/0x2dfba0).
5. **RN app scene.** A plugin that shows an RN app gets a `ReactApplicationStackScene` from
   `ISceneFactory.Create(url, ...)` (Sce.Vsh.ShellUI.ReactNativeShellApp `SceneFactory`). The
   URL resolves through `UrlConfigResolver.Utility.GetRnpsAppConfig` to
   `{component, titleId}`. For example, `pscontrolcenter:main` → `app:rnps-control-center` →
   NPXS40003.
6. **Bundle location.** `RenderScene.CreateAppConfig` (ReactNative.Vsh.Common) builds the
   app's configuration:
   - `RnpsAppMgrWrapper.GetAppInfo(titleId)`. A negative result throws `PsmNativeException`.
     That makes the scene try the recovery path (`RecoverUfsImage`, `RemoveApp`), and then
     fail with `Cannot fallback to original bundle (0x8186xxxx)`.
   - Reads `<path>/manifest.json`, and from it the `twinTurbo` app type.
   - Sets `JSBundleSource` = `<path>/application.ps.bundle` (TwinTurbo) or `<path>/main.jsbundle`.
   - The app takes a pre-warmed instance from the Turbo or TwinTurbo pool (`Creating app from
     TwinTurbo pool`). Its bridge loads the bundle (`DecryptRnpsBundle` for `RNPSHEDR` files)
     and evaluates it in JavaScriptCore (see `rn-bridge.md`).

## Native calls on the path

| Managed entry | Native (FW 4.03) | Served by on console | Host |
|---|---|---|---|
| `UserServiceWrapper._GetLoginUserIdList` (icall) | eboot 0x4a6bf0 → `sceUserServiceGetLoginUserIdList` (libSceUserService 0x2a50) | client cache of login events | `shellcore/user_service.cpp` |
| `UserServiceWrapper._GetForegroundUser` (icall) | import `sceUserServiceGetForegroundUser` (0x2f30 → 0x3f70; IPMI 0x3000C) | SceShellCore `SceUserService` (+0x58, 0x7001c0) | `shellcore/user_service.cpp` |
| `UserServiceWrapper._GetRegisteredUserIdList` (icall) | eboot 0x4a6c30 → 0x2b30 → 0x3fd0 → 0x8b0 (IPMI 0x3000F) | SceShellCore | `shellcore/user_service.cpp` (server not traced) |
| `Image::GetPixelDataSize/GetPixelData` (icalls) | libScePsm 0x2dfe60 / 0x2dfba0 | libScePsm | `libScePsm/imaging/image_icalls.cpp` |
| `RnpsAppMgrWrapper.GetAppInfo` (`libSceRnpsAppMgrWrapper!sceRnpsAppMgrGetAppInfo`) | eboot table 0xcfec40 → libSceRnpsAppMgr 0x16a0 (IPMI 0x30009, 1227-byte reply) | SceShellCore 0x5d63b0 → 0x5be840 → 0x5be4a0 | `shellcore/rnps_app_mgr.cpp` |
| `RnpsAppMgrWrapperGetPath` (`NativeExtensions.sprx`) | libNativeExtensions 0x163e0 | same RNPS app manager | `libNativeExtensions/rnps_app_mgr.cpp` |
| `EventMonitor.UpdateEvent/GetEvent` (`libSceLncUtil!LncProxy::*`) | eboot 0x8da0 / 0x8dd0 → `sceLncUtilGetEventForShellUI` (libSceSystemService 0x6470 → 0x9000) | SceShellCore `SceLncService` event queue | `eboot/lnc_proxy.cpp` + `shellcore/lnc_service.cpp` |
| `LncUtilWrapper.LaunchAppParamInit` (`libSceLncUtil!LncProxy::LaunchAppParamInit`) | eboot 0x8e00 | eboot | `eboot/lnc_proxy.cpp` |

The other `libSceLncUtilWrapper` entries ShellUI calls at boot are still logging stubs: `GetAppStatus`,
`GetAppStatusListForShellUIReboot`, `SetAppFocus`, `SetControllerFocus` and
`GetGpuCrashFullDumpAppStatus`. In the eboot's {name, fn} table (0xce9b20..) they are the
imports of the same name in libSceSystemService. The RN path does not depend on them.

## What the host's SceShellCore services answer

The host models a console where no user is logged in, which is the state ShellUI boots in
before InitialSetup creates the first user.

### User service

- **Login user list.** Four slots, all `SCE_USER_SERVICE_USER_ID_INVALID` (-1); result 0.
  The client copies its cached vector of logged-in users and pads with -1 (0x2a50).
- **Foreground user.** -1, result 0. SceShellCore's `SceUserService` constructor
  (0x6e5790) sets the foreground user at +0x58 to -1, and the getter (0x7001c0) returns it
  with result 0. A null out pointer gives 0x80960005.
- **Registered user list.** 20 slots of -1, result 0. ShellCore's handler for IPMI method
  0x3000F is not traced. The call reports through `note_unimplemented`.

### RNPS app manager: `sceRnpsAppMgrGetAppInfo`

| Condition | Result |
|---|---|
| service not initialised | 0x81860001 |
| null `titleId` or `info`, or invalid title id (`isValidTitleId`, 0x5c7ce0: nine characters, four upper-case letters then five digits) | 0x81860003 |
| title id not in `m_apps` | 0x81860008 |
| found | 0, and a 1227-byte record (below) |

Layout of the record that 0x5be4a0 fills:

| Offset | Field | Source |
|---|---|---|
| +0 | `titleId[10]` | strncpy |
| +10 | `version[64]` | the app's `manifest.json` `applicationVersion`, read with sce::Json (0x5bddd0); `""` if it does not parse |
| +74 | `preinstalled` | 1 when the install path contains `/system_ex/rnps/apps` or `/system_ex/rnps/bgs` (install roots table, 0x5c7b00) |
| +75 | `path[128]` | install path |
| +203 | `downloadUrl[1024]` | not traced (the service's app URL table); the host leaves it empty |

The host's `--rnps` root stands in for `/system_ex/rnps`, so every `<rnps>/apps/<titleId>`
directory counts as a preinstalled app. Paths are host paths, because the managed side opens
them with System.IO. They must stay under 128 bytes; `RnpsAppMgrWrapperGetPath` returns -1
for longer paths.

### Launcher service: the event queue

The LncUtil client (libSceSystemService 0x6b40) connects the IPMI client `SceLncIpcClient`
to the server `SceLncService`, with one message queue of 5736-byte messages.
`getEventForShellUI` (0x9000) pops one message with `IpmiClient::tryGetMessage(0, buf,
&size, 5736)` (libSceIpmi vtable +56, 0xdf0, syscall 594).

`EventMonitor.Update` reads an event only when `UpdateEvent` returns 0. Any other value
means "no event". On an empty queue the status is 0x80020023 (SCE_KERNEL_ERROR_EAGAIN):
libSceIpmi 0xdf0 -> 0x4a20 issues ipmimgr op 0x252 and turns the call's errno into
0x80020000 + errno (libkernel `D4yla3vx4tY`, 0x16130), then passes the status through except
0x80020002 -> 0x80020003, 0x8002000C / 0x8002000E -> 0x80020016 and 0x80020059 -> 0x8002000C.
In the kernel (only the FW 7.00 image is in hand) op 0x252 is 0xFFFFFFFF80CFB4B0 ->
getMessage 0xFFFFFFFF80CFAB30 with a zero wait flag -> the non-blocking receive
0xFFFFFFFF805E1A30, which answers EAGAIN (35) when the channel head (+136) is empty.

`shellcore::push_event_for_shellui` queues records for ShellUI. Nothing on the RN app path
produces any, because the RN apps are not launcher-managed. (SceShellCore's other
"PushEventForShellUI", rnps_app_mgr_event.cpp 0x5CC6A0, feeds the RNPS app manager's own
channel, `RnpsAppMgrWrapper._GetEventForShellUI`, not this queue.) Record layout (`LncEvent`,
Pack 4, mirrors `Sce.Vsh.LncUtil.EventMonitor.Event`):

| Offset | Field | Offset | Field |
|---|---|---|---|
| 0 | eventType | 4148 | appLocalPid |
| 4 | titleId[16] | 4152 | path[1024] |
| 20 | appId | 5176 | sceSysDir[256] |
| 24 | appType | 5432 | causeCode |
| 28 | appAttr | 5436 | existingAppId |
| 32 | launchRequestAppId | 5440 | checkFlag (u64) |
| 36 | userId | 5448 | contextId (u64) |
| 40 | args[4096] | 5456 | coredumpReason |
| 4136 | argsSize | 5460 | corefileName[256] |
| 4140 | errorCode | 5716 | exceptionCode |
| 4144 | errorData | 5720 | category[4] |
| | | 5724 | appCategoryType |
| | | 5728 | isCrashed |

The remaining 4 bytes (+5732) are not read by the managed side.

Event types: 1 AppLaunchStart, 2 AppLaunchEnd, 3 AppKillStart, 4 AppKillEnd,
5 AppSuspending, 6 AppSuspended, 7 AppResuming, 8 AppResumed, 17 CdlgActive,
18 CdlgDeactive, 241 CoredumpStart, 0x1001 AppFocusStart, 0x1002 AppFocusEnd,
0x1003 ControllerFocusStart, 0x1004 ControllerFocusEnd.

`LaunchAppParamInit` (eboot 0x8e00) fills `{size 0x28, userId -1, appAttr 0,
enableCrashReport 1, checkFlag 0, contextId 0, isSpeculativeLaunch 0}`.

## Launching an installed title

Pressing the cross button on a game tile ends in `AppLauncher.Start` (Sce.Vsh.ShellUI.AppSystem),
which calls `LncUtilWrapper.LaunchApp` on the launcher job queue and ignores its result.
Everything ShellUI then does follows the events SceShellCore queues and the answers to its
LncUtil queries. `psmhost/shellcore/lnc_service.cpp` models SceShellCore's `LncManager`
(lnc_manager.cpp; IPC handlers in lnc_util_ipc_server.cpp, 0x482580..0x48B260):

- **The application list.** ShellUI is in it from the start: SceShellCore launches it through
  the same `launchApp` (0x768650), and its param.json (`applicationCategoryType` 0x1000000)
  makes it category "gdf", type ShellUI (1). `getAppStatus` answers the caller's record,
  `getAppStatusList` all records but daemons and SceShellCore (at most 16), `getAppTitleId` and
  `getAppId` look a record up.
- **Category and type.** A PS5 param.json has no CATEGORY; the launcher derives it from
  `applicationCategoryType` (0x4147F0: 0 and the 0x1xxxx/0x2xxxx game values "gd", 0x1000000
  "gdf", 0x2000000 "gdd", 0x3000000 "gdg", 0x4000000 "gdi"), then the app type from the
  category (0x414210: "gd" BigApp 5, "gdf" ShellUI 1, "gdd" Daemon 2).
- **`launchApp`** (0x432630): the record (appId -1, the caller as parent), the title's
  param.json, `APP_LAUNCH_START`, the appId (sceKernelBeginAppMount2's result), `spawnApp`, the
  record added to the list (a big or mini app becomes the focused app: the kernel's spawn gives
  it every focus), `APP_LAUNCH_END`, and the appId as the result. A failure at any step sends
  `APP_LAUNCH_END` with the error.
- **`setAppFocus`** (0x43F110): `APP_FOCUS_START`, the kernel's focus, controller focus -1,
  `APP_FOCUS_END`; an unknown appId gives `APP_FOCUS_END` with 0x80940004.
  **`setControllerFocus`** (0x43FA20): `CONTROLLER_FOCUS_START`, the kernel's controller focus,
  `CONTROLLER_FOCUS_END`.
- **`onAppExit`** (0x436460): the record leaves the list and `APP_KILL_END` follows.

ShellUI hides itself on `APP_LAUNCH_END` of a big app and comes back on its `APP_KILL_END`.

The application's process is a HOST POLICY (`lnc_host_app.cpp`, see host-values.md): a title
listed in `<cache>/host/apps.json` starts a PC program; any other title fails at `spawnApp`
with 0x80020002, as a title without its executable would.

The console has one screen and one controller, and hands both to the application. On the PC
the application has its own window, so the host's window steps aside (HOST POLICY, see
host-values.md "Application window"): it minimizes when the program starts and is restored and
brought to the front when the application ends, before `APP_KILL_END`. While the application
runs and the host window is not in front, the XInput pad is not read (docs/input.md). A
minimized window has nothing to present to; frames are then paced by the ~60 Hz timer, as a
headless run.

Not reproduced: the workspace and title mounts, `preLaunchCheck`, `checkExistingApp`, the CPU
budget of extra audio devices, the application save, the mount roots, the parental check of
`setAppFocus`, the suspension of the previously focused app, and the kill and suspend paths.

## Not traced / open

- The kernel's appId numbering (`sceKernelBeginAppMount2`, syscall 706); the host numbers
  applications 1, 2, ... (host-values.md).
- `getAppStatusListForShellUIReboot` and the rest of the `libSceLncUtilWrapper` set.
- ShellCore's registered-user list handler (IPMI 0x3000F).
- The RNPS app record's `downloadUrl`, and how `m_apps` is populated: which files a
  directory must hold to count as installed.
- The DXT/BC branch of `Image::GetPixelDataSize/GetPixelData`, which writes a DDS header
  and then the compressed data.
