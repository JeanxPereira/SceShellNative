# Input: the PC keyboard as a DualSense

ShellUI (FW 4.03) reads its controller entirely through libScePsm's input stack. SceShellNative
reproduces that stack from the decompiled libScePsm code, from the libScePad boundary up. It
feeds the stack from one emulated DualSense, built from the PC keyboard and, if one is
attached, the first XInput pad. The only host decision is which key drives which DualSense
control (see [Key mapping](#key-mapping)).

Reference binaries (FW 4.03): `libScePsm.sprx`, which has the icalls and the PSS input layer
(database `work/ida/libScePsm_dyn.elf.i64`; this work used a copy in `work/ida_input/`);
`libSceSysBridge.sprx`, which holds the `libSysBridgePad` P/Invokes; and the ShellUI
`eboot.bin`. The addresses below are offsets in those modules.

## Who reads the pad

- **PUI `UIGamePad`** (Sce.PlayStation.PUI) reads every key and stick the UI sees. It calls
  `GamePad.GetData(deviceIndex)` once per frame and `InputManager.GetControllerInfo(0)`.
  It also calls `InputManager.GetControllerFocusStatus()`. It derives the stick-as-D-pad
  input, masks and repeats in managed code.
- **AppMain `PadManager`** (Sce.Vsh.ShellUI.AppSystem, `PadInternalDriver` registers
  `PadManager.Update` on the main task). Each frame and for each port it calls
  `InputManager.GetSystemButtonsState(port, …)`. This gives the PS, Share, Mute,
  PushToTalk and EnterTuning system buttons, plus the normal buttons it uses for PS+□/R/✕/△.
  `PSButtonListener` and Control Center consume the result.
- **Scene manager** (AppSystem). On each layer change it sets the device-index behaviour.
  The shell sets `MergeAllConnectedDevice` at boot; some scenes use `SpecificUser`. It also
  sets `SystemPadButtonMeaning` and calls `Sce.Vsh.SysBridge.Pad.scePadDisableTransportButtons`.
- PUI also calls `Touch.GetData(0)`, `Motion.GetData(0)` and `RemoteControl.GetData(0)`.

## Traced data path

```
libScePad  ScePadData {buttons SCE_PAD_BUTTON_*, lx ly rx ry, l2 r2, …}   <- host device
   |  scePadDeviceOpen / scePadGetControllerInformation / scePadGetCapability
"ScePsP:CtrlScanner"  sub_1832D0 -> sub_188AF0 -> sub_186F90 (Mbus scan, Open Pad)
   |  sub_183970 -> sub_188DD0 -> sub_1891F0 (orbis-controller init: model, dead zones, caps)
   |  event 23 -> sub_2DBFE0: sub_17B280 (callbacks), sub_17B5B0 (feature mask 0xDD),
   |                          sub_1FD520 (InputManager slot), prev-button reset
"ScePsP:InputCtrl"    sub_186AC0 (vblank, >= 12 ms) -> sub_1887F0 (scePadRead* per slot)
   |  sub_1888F0 -> sub_189BA0: SCE bit -> PSS button index, edge callbacks
   |                             stick bytes -> floats with dead zone
PSS state             sub_181B00 / sub_181940 / sub_181A20 -> 0x4fd2a0 (24 B x 8)
   |                  remote 0x525750, touch 0x5260b0, motion 0x5255e0
InputManager          sub_1FCF90 "ScePssCtrlSelector" (24 slots, behaviour, active device)
   |                  sub_1FD430 (slot accepted), sub_1FD360 (ControllerInfo)
icalls                GamePad::GetDataNative 0x2dad80, InputManager::* 0x2dc760..0x2dd110
   |
Sce.PlayStation.Core / Sce.PlayStation.Orbis (managed) -> PUI UIGamePad, AppMain PadManager
```

### ScePadData to PSS button index (`sub_189BA0`)

The converter checks the bits in this order. For each change it calls the button callback
(`sub_181B00`) with the index, the SCE mask and the new state. The PSS state bit is
`1 << index`.

| SCE_PAD bit | control | PSS index | GamePadButtons (0x2dad80) |
|---|---|---|---|
| 0x00010000 | PS | 18 | (system: `Ps`) |
| 0x00000001 | Create/Share | 19 | (system: `Share`) |
| 0x00000008 | Options | 8 | Start |
| 0x00000010 | Up | 0 | Up |
| 0x00000040 | Down | 1 | Down |
| 0x00000080 | Left | 2 | Left |
| 0x00000020 | Right | 3 | Right |
| 0x00000400 | L1 | 10 | L |
| 0x00000100 | L2 | 11 | L2 |
| 0x00000002 | L3 | 12 | L3 |
| 0x00000800 | R1 | 13 | R |
| 0x00000200 | R2 | 14 | R2 |
| 0x00000004 | R3 | 15 | R3 |
| 0x00004000 | Cross | 4 | Cross |
| 0x00002000 | Circle | 5 | Circle |
| 0x00008000 | Square | 6 | Square |
| 0x00001000 | Triangle | 7 | Triangle |
| 0x00100000 | Touch pad click | 20 | TouchPad |
| 0x00020000 | — | 21 | Enter \| Move |
| 0x00200000 | Mute | 24 | (system: `UniversalMute`) |
| 0x00800000 | — | 25 | (system: `PushToTalk`) |
| +0x58 bits 0–4 | extension unit | 26–30 | (system: volume / balance / tuning) |
| 0x80000000 | Intercepted | 31 | Intercepted |
| +0x5C/+0x5D = 0x0D / 0x0F | Enter/Back keys (remote models only, feature 0x400) | 22 / 23 | Enter / Back |

`GamePad.GetData` then adds Enter and Back from Cross and Circle when either is down. The
choice depends on `dword_53FC88` (SystemPadButtonMeaning): 2 means Circle is Enter;
otherwise Cross is Enter and Circle is Back.

**Sticks.** Each stick byte goes through `f(raw, dz)`, where `dz` is the dead zone from
`scePadGetControllerInformation`:

- If `raw < 128 - dz`, the result is `raw / (128 - dz) - 1`.
- If `raw > hi`, where `hi = dz < 0x7F ? dz | 0x80 : 0xFF`, the result is
  `(raw - hi) / (hi ^ 0xFF)`.
- Otherwise the result is 0.

The stick callback runs while either the old or the new value is non-zero. It clamps the
value to [-1, 1].

**Device selection** (`GamePad.GetData(0)`). With `MergeAll*` behaviour the InputManager
keeps an *active* device, which starts at −1. Each read compares every accepted slot's
`buttons & 0xF3FFFF` (and its remote buttons) with the slot's previous value. The last
slot with newly pressed input becomes active. While no device is active the read returns
zero data. That is why the first frame of input is the frame that selects the pad.

## Icalls and P/Invokes implemented

libScePsm (`psmhost/libScePsm/input/input_icalls.cpp`, `pss_input.cpp`):

| entry | address |
|---|---|
| `Sce.PlayStation.Core.Input.GamePad::GetDataNative(int,GamePadData&)` | 0x2dad80 |
| `Sce.PlayStation.Core.Input.Motion::GetDataNative(int,MotionData&)` | 0x2dba00 |
| `Sce.PlayStation.Core.Input.Motion::ResetOrientationNative(int)` | 0x2dbbe0 |
| `Sce.PlayStation.Core.Input.RemoteControl::GetDataNative(int,RemoteControlData&)` | 0x2dbcd0 |
| `Sce.PlayStation.Core.Input.Touch::GetDataNative(int,TouchData[],int&)` | 0x2da930 |
| `Sce.PlayStation.Orbis.Input.InputManager::SetBypassSwitchNative(bool)` | 0x2dc760 |
| `…InputManager::GetBypassSwitchNative(bool&)` | 0x2dc7c0 |
| `…InputManager::SetDeviceIndexBehaviorNative(DeviceIndexBehavior,long,bool&)` | 0x2dc820 → sub_1FD1D0 |
| `…InputManager::GetDeviceIndexBehaviorNative(DeviceIndexBehavior&,long&)` | 0x2dc870 |
| `…InputManager::GetControllerInfoNative(int,bool,ControllerInfo&,int&)` | 0x2dc900 |
| `…InputManager::GetSystemButtonsStateNative(int,ControllerInfo&,SystemButtons&,GamePadButtons&)` | 0x2dca80 |
| `…InputManager::GetInvalidOperationByOtherUsersNative(bool&,int&)` | 0x2dcf90 |
| `…InputManager::GetControllerFocusStatusNative(ControllerFocusStatus&)` | 0x2dd030 |
| `…InputManager::SetVibration(int,PadVibrationParam&)` | 0x2dd0d0 → scePadSetVibration |
| `Sce.PlayStation.Orbis.Input.DebugTools::scePadReadStateNative(int,uint&)` | 0x2dd110 → scePadReadState |
| `Sce.PlayStation.Orbis.Environment.SystemParameters::GetSystemPadButtonMeaning(int&)` | 0x2d4860 |
| `…SystemParameters::SetSystemPadButtonMeaning(int)` | 0x2d4880 |
| `…SystemParameters::IsValidUserIdNative` | 0x2d48a0 |

`Sce.Vsh.SysBridge.Pad` (`psmhost/libSysBridge/pad.cpp`). In libSceSysBridge, the
`libSysBridgePad` table at 0x78a80 points each name at the libScePad import of the same
name. These calls therefore *are* the pad driver boundary:

- `scePadGetCapability` returns `SCE_HID_CONTROLLER_CAPABILITY_BOND`, the firmware's own
  enum value for a DualSense.
- `scePadDisableVibration`, `scePadSetLightBarBaseBrightness`, `scePadSetLightBarBlinking`,
  `scePadDisableTriggerEffect`, `scePadSetVibrationStrength`, `scePadSetTriggerEffectStrength`,
  `scePadSetAutoPowerOffCount`, `scePadDisableTrackingLedBatterySaver` and
  `scePadDisableTransportButtons` are accepted. They are settings of hardware that the host
  pad does not have.
- `scePadSetButtonRemappingInfo` and `scePadEnablePlayPauseEmulation` are accepted and
  logged through `host::note_unimplemented`. The host pad does not apply them.

## The host device (physical boundary)

`launcher/src/platform/input.{h,cpp}` holds the emulated pad's ScePadData fields:
buttons, the four stick bytes, the triggers and the connection state. The window thread
writes them through `on_key` (wired in `main.cpp`). The InputCtrl thread reads them. Its
identity, as the scanner and libScePad would report it, is in `pss_input.h`:

| value | host | note |
|---|---|---|
| Mbus model | 22 → internal model 10 (`DUALSENSE`) | Gives HardwareType `Bond` (dword_40FC20) and feature mask 0xDD (dword_3D8B10). |
| scePadDeviceOpen type | 0x10 (dword_3D8ADC) | |
| device id | 0x10001 | Host-chosen. |
| pad handle | 1 | Host-chosen. |
| owner user id | the mbus record's owner (+200) | −1 until a user logs in with the pad; the scanner picks up the bind (see below). |
| connection | 0 (local) | |
| stick dead zones | 0 | XInput input is dead-zoned on the host side. |
| capability | `SCE_HID_CONTROLLER_CAPABILITY_BOND` | Becomes `ControllerInfo.Capability` through `sub_1891F0`. |

Keyboard input counts only while the SceShellNative window has the focus (both the DualSense
keys and the USB keyboard of `docs/ime.md`). Losing focus
releases every key. Scripted key events (`--input-script`, `launcher/src/platform/input_script.cpp`)
go through the same key table but are kept apart from the window's keys, so they do not
depend on the focus (headless runs have no window). The XInput pad is read at every poll through `xinput1_4.dll`, which is
loaded at run time. `SetVibration` drives its motors.

### Power, connection and owner

The emulated DualSense starts **off**, as a DualSense does while the console boots
(`--pad-on-at-boot` starts it on). Only the PS button turns it on (keyboard Home, script
`ps`); that press is swallowed until PS is released. Turning it on attaches the device to the
host mbus table and queues SCE_MBUS_EVENT_DEVICE_ADDED (`psmhost/libSceMbus`).

- **libScePsm controller scanner** (`sub_1832D0` -> `sub_188AF0`): one pass over the bus
  (`sub_186F90`), then a blocking wait for the next sceMbus event, then another pass. A
  device not yet in a slot is opened (Open Pad, PSM input event 23 -> `sub_2DBFE0`); for an
  open slot whose record owner (+200) changed, the device's user id (+176) is updated and its
  info callback (`sub_17B4D0`, +352) posts input event 28, whose listener refreshes the
  InputManager slot (`sub_1FD520`). So `ControllerInfo.UserId` follows the login.
- **eboot LoginMgr** (`psmhost/eboot/login_mgr_watch.cpp`): after `_StartEventWatch` the watch
  thread reports unassigned devices; `onDeviceAdded` (0x1CFD40) queues a controller that no
  logged-in user auto-assigns, and PostEvent (0x1C9A30) sends
  `psaccounts:login?deviceId=65537&deviceUniqueId=0&isBusChange=false&needBondUpdate=false&needNextGenVrControllerUpdate=false`
  to the managed LaunchByUriAsync delegate: the user-selection screen for that controller.
  Logging in there binds the pad (`_Login` -> mbus bind -> OWNER_CHANGED).

The Welcome RN app (NPXS40009) itself ignores PS (`onPSButtonPressedOnLoginLayer` returns for
entry "welcome"), and the managed LoginMgr's event 182 handler only acts for a CEC remote
(user 255): "Press the PS button" works through the controller being turned on and added.

### PS on the home

PadManager (`GetSystemButtonsState`) posts PSM event 5 with `{DeviceId, UserId}` of the pad.
PSButtonListener hands it to the Control Center (`ControlCenterAction` -> RNApps
`EventManager.OnPSButton` -> `MenuManager.Open(userId)`), which opens only for a user in
`UT.UserService.LoginUserIdList`. That list grows on PSM event 40, which the eboot posts from
its user-service event callback (0x4A6A30, registered by `initializeFrameworkDependentModules`
0xD050 -> `_EnablePsmEvent` 0x4A6A20) when SceUserService reports a login. With the pad bound
to the logged-in user, PS opens the Control Center (NPXS40003).

A second PS press closes it by the same path: `EventManager.OnPSButton` (RNApps) sees
`MenuManager.IsOpened(userId)` and calls `MenuManager.Close(userId, false, {reason: "PSButton"})`,
which, because the CC's `ExitHandlerModule.ClosingEventRequired` is set, sends the JS closing
event; the app answers with its close request (`OnRequestClose` -> `PostClose`: opacity 0, not
terminated), with a 4 s `UITimer` fallback. Verified: the CC is gone ~25 frames after the press.

Press length matters exactly as on the console (`PadManager.PadInfo` config for PS: long press
400 ms, or 1000 ms with the accessibility "slow" setting; double press 160 ms / clear 260 ms):
a press shorter than 400 ms is `OnPSButton` (event 5); the input script's default hold of 42
frames (~700 ms) is a *long* press (`OnPSButtonLong`, event 6: go to the home), which never opens
or closes the Control Center. Script PS presses as `ps:12` (200 ms).

## Key mapping

This is the only host decision.

| DualSense | keyboard | XInput |
|---|---|---|
| D-pad | arrow keys | D-pad |
| ✕ Cross | Enter, Z | A |
| ○ Circle | Backspace, Esc, X | B |
| □ Square | C | X |
| △ Triangle | V | Y |
| L1 / R1 | Q / E | LB / RB |
| L2 / R2 (digital and full analog) | 1 / 3 | LT / RT (analog; the digital bit is set above 30/255) |
| L3 / R3 | F / G | left / right stick click |
| Options | Tab | Start |
| Create (Share) | F1 | Back |
| PS | Home (also turns the pad on) | — (the XInput API does not expose the Guide button) |
| Touch pad click | T | — |
| Mute | M | — |
| Left stick | W A S D | left stick |
| Right stick | I J K L | right stick |

With the shell's default button meaning, ✕ also produces `Enter` and ○ produces `Back`
inside `GamePad.GetData`. That derivation is the firmware's, not the host's.

## Evidence

`managed/Tests/PadSmoke.cs` drives the firmware's own `Sce.PlayStation.Core` and
`Sce.PlayStation.Orbis` the way the shell does. It sets `MergeAllConnectedDevice`, then
each frame calls `GamePad.GetData(0)` and `InputManager.GetSystemButtonsState(0)`. Key
presses were injected into the window. The run log is `work/evidence/input_padsmoke_run.txt`:

```
PadSmoke: device 0x10001 type=Bond connected=True user=-1 cap=GamePad, TouchPad, Vibrator, MotionSensor, AnalogJoystickLeft, AnalogJoystickRight, Lightbar, Presure, Speaker, HsJack handle=1
PadSmoke: focus=Unknown
PadSmoke: frame 243 Buttons=[Down] Down=[Down] Up=[0] LX=0 active type=Bond
PadSmoke: frame 252 Buttons=[0] Down=[0] Up=[Down] LX=0 active type=Bond
PadSmoke: frame 269 Buttons=[Cross, Enter] Down=[Cross, Enter] Up=[0] LX=0 active type=Bond
PadSmoke: frame 294 Buttons=[0] Down=[0] Up=[0] LX=1 active type=Bond
PadSmoke: frame 320 system Ps=True normal=[0] connected=True
```

The real shell no longer reports any input icall or `libSysBridgePad` entry as a stub, and
none of them throws. It calls `SetDeviceIndexBehavior: mode=MergeAllConnectedDevice`
through the implemented icall.

## Not traced / not reproduced

- **PsmInitParam input flags.** The four flags are bytes +8..+0xB of the init parameter
  (GamePad, Touch, Motion and RemoteControl). Init copies them into `word_540A28/2A`. The
  initial `SystemPadButtonMeaning` (+0x1E8) arrives the same way. The defaults come from
  the PsmInitParam constructor, libScePsm export `FqHN0elWA6E`. That export is not
  resolved in the database, and the eboot (`sub_E500`) does not override these fields.
  - The host turns GamePad on, because the PUI reads every key through it.
  - The host turns Touch, Motion and RemoteControl off. Their icalls then take the traced
    "disabled" path: zeroed data, result 0.
  - With this device, Touch and RemoteControl would return the same data either way.
    Motion would need sensor data that the host does not have.
  - The meaning starts at 0 (conform with the system setting).
  - The first use is logged as `[stub] … PsmInitParam input flags …`.
- **Motion, touch and remote converters** (`sub_1888F0` motion callback, `sub_18A8D0`,
  `sub_18AC00`). These are not reproduced. The host DualSense has no sensor or touch data.
  The remote converter only runs for remote models (feature 0x400), so for this device the
  touch and remote state tables stay empty, as they would on hardware with no touches.
- **PSM input events.** The button and stick callbacks also post events (category 2,
  ids 19–22) through `sub_17C270`/`sub_17C490`. The only listener traced for category 2 is
  `sub_2DBFE0`, which ignores those ids. The posts are not reproduced.
- **VirtualPadManager (slots 8–23), GamePadRemotePlay, HardwareKeyboard (`libSceKbEmulate`
  pad emulation) and PadDevice.Enable/Disable** are not implemented. They remain logging
  stubs. The traced readers handle slots 8–23 as "no virtual pad" (their table entries
  stay −1).
- **libScePad answers for device ids or handles the host does not own.** The host returns
  `SCE_PAD_ERROR_INVALID_ARG` / `SCE_PAD_ERROR_INVALID_HANDLE`. The driver's real answers
  were not traced.
- `scePadGetVersionInfo` and `scePadGetUsbDetectionState` are not implemented. The shell
  has not called them so far.
