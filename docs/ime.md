# The system IME (on-screen keyboard and USB keyboard)

A text field in a ShellUI React Native app (for example the name field of Settings → Users and
Accounts → Users → Add User, `keyboardType: "online-id"`, `maxLength: 16`, NPXS40009) is a PUI
`TextField`. Editing it opens the **system IME**, which on the PS5 lives in the ShellUI process
itself: the eboot hosts the IME server, and the managed `Sce.Vsh.ShellUI.Ime` assembly draws the
on-screen keyboard (OSK). Apps in other processes reach the same server over IPMI.

Reference binaries: the FW 4.03 devkit eboot (`NPXS40087/eboot.bin`, build J01736824, the standard
firmware; the retail build J01736823 holds the same code, 0xF40 lower in the ImeProxy block and
0x4B40 lower in this rodata), `libScePsmUtil.sprx`, `libSceIme.sprx`, `libScePsm.sprx`. Addresses
below are devkit-eboot addresses or module offsets.

## Path of a text field

```
PUI TextField.BeginEdit -> TextFieldEditIme -> Sce.PlayStation.Core.Ime.ImeClient.Open
  (window type Default -> GetDefaultWindowType() = Inline)
libScePsmUtil  ImeClient::StartNative 0x2290 -> 0x2DE0 -> LibImeStart 0x5980      psmhost/libScePsmUtil/ime_client.cpp
               handler 0x5850 (event list), GetEventNative2 0x6AA0 (SCE id -> ImeEvent id)
libSceIme      sceImeVshOpen 0x9110, sceImeVshUpdate 0xaab0, sceImeVshClose 0xe300   psmhost/libSceIme/ime_vsh_client.cpp
               (validation 0x8B30, 456-byte internal param, record -> SceImeEvent)
   | IPMI "SceImeService" (host: direct calls)
eboot          service 0x198E00: connect 0x1991B0, sync methods 0x199820,          psmhost/eboot/ime_server.cpp
               poll method 4 (command 0 + 0x19B420), commands 0x19DB10
               framework 0x10CE90: open 0x11B9F0 -> PSM event 270 {EventId 21}
               unit manager 0x13F090: SendChangeUnitEvent 0x123F80 -> PSM event 270 {EventId 2}
               OnUnitLoadFinish 0x11AC50, UI ready 0x11B7C0 -> client event 0 (OPEN)
managed OSK    ImeUIFramework.OnImeOpen / OnChangeUnit / StartOsk -> ImeProxy.OnChangeUIStatus(Ready)
               key press -> ImeNativeCall.OnImeEventNative(1, ImeButtonEvent JSON) 0x144CF0
eboot          PostButtonEvent 0x113EA0 -> unit queue -> SceImeOskBasicLatinUnit.OnDecide 0x16CFF0
               -> backend type 27 (0x16DD00) -> framework insert 0x111070 / backspace 0x10F1F0 / ...
               -> client events 514 (insert) / 515 (backspace) / 768.. (caret) / 5 (enter) / 4 (close)
PUI            ImeEvent 6 InsertTextByIme, 9 BackSpace, 12.. caret, 3 OnPressEnter, 2 EndEdit
```

`BackgroundInit(InitIme)` (0xD210 case 7) creates the framework, the UI singleton, the service and
the "SceImeUpdate" thread. That thread is woken once per vblank: the eboot registers 0x146E00 as
libScePsm's vblank callback (`XmlFvKZlm7k`, called at the end of `PsmGraphicsDevice.WaitVBlank`).
Each wake it runs the service's command queue, the keyboard, and (10 of 11 wakes) the unit
manager.

## What is reproduced

* The inline OSK session end to end for the input types whose OSK unit is
  `SceImeOskBasicLatinUnit` (unit types 1 BasicLatin, 5 SocialName, 7 OnlineId; 0x13D130):
  open, the unit context and activation, the UI-ready events, button events (characters,
  Space, Backspace, Delete, caret moves, Home / End / PageUp / PageDown, Enter, Close), the
  type filter (types 1 / 3 / 258 keep code points <= 0x7E, libSceIme `sceImeFilterText`),
  context updates (PSM event 204), panel position / form (PSM event 206, client events 0 / 3),
  close (PSM event 270 {EventId 22}).
* The `ImeProxy` icalls the session reaches, in their initialised branches (`ime_proxy.cpp`).

Not reproduced (logged once when reached): the other OSK units and their conversion engines
(European scripts with prediction, Japanese, Korean, Chinese, number pad, voice / remote-play
plugins), the ImeDialog server (the dialog window type), text-to-speech, the user dictionary,
selection mode, the event cache of PostButtonEvent, the per-user input-language list and the
last-unit memory (the session's language is the system language, 0x14B650).

## USB keyboard

The eboot links libSceKeyboard statically (`/dev/hid`, mbus device class 6). When an OSK session
is ready the IME opens the user's keyboard (0x121E50 -> 0x1A5BD0 -> OpenKeyboard 0x1B4790 ->
`sceKeyboardOpen(userId, 0, 0)`, option REPEAT) and closes it with the session (0x1A5E80). Each
SceImeUpdate tick the keyboard manager reads it and makes key events (0x1AE4C0 / 0x1AE680):
status `3 | modifiers << 8 | (LED & 7) << 16`, modifier pseudo-keys 0xE0..0xE7, key-down /
key-up / repeat (delay 16, rate 4 ticks), the character from the layout table (normal, or
shifted with a Shift modifier; CHARACTER_VALID cleared for the non-character keycodes at
0xBEDB50). The IME then (0x118540 / 0x118900 / 0x114C60):

| key | firmware behaviour |
|---|---|
| any key down / repeat | client event 543 = keyboard (ReceiveUserAction) |
| first key-down that is not Enter / Escape | hardware-keyboard mode: client event 14 = 2, PSM event 23 {State 2 KeyboardStart} (the managed OSK hides its key panel), control 0x20000 |
| printable character | type filter, then a Character button: the text is inserted |
| Enter (character 0x0A) | the Enter button: client event 5, PUI `OnPressEnter` |
| Escape | the Close button: client event 4, PUI `EndEdit` |
| Backspace (0x08) / Shift+Backspace | Backspace (515) / delete all (131078) |
| Delete, arrows, Home, End, PageUp, PageDown | 131077, caret moves 768..771, 772..777 (Ctrl+Home / End: top / bottom) |
| Ctrl / GUI + character | nothing |
| a controller button in keyboard mode | back to the OSK: client event 14 = 1, PSM event 23 {State 3}, control 0x20001 (0x1147B0) |

The firmware does **not** block the controller while a keyboard is used: the OSK keeps pad control
(the managed `IsDisableController` only answers for the companion app / `DisableInputDevice`).

HOST VALUE (`launcher/src/platform/input.cpp`, logged once): the PC keyboard is also one USB
keyboard, always connected, its keys reported as HID usages from the Windows scan codes (a
position mapping; the firmware applies the layout) and its LEDs following the PC's lock keys.
While the firmware holds a keyboard handle (an OSK session is open), a key pressed in the window
goes to that keyboard and not to the DualSense key table (`docs/input.md`), so Z X C V Q E W A S D
… type text instead of pressing pad buttons — on the console those are keyboard keys too. A key
held when the handle opens stays a pad key until released. With no handle open the PC keyboard
drives the DualSense as before; the firmware's own keyboard-as-pad path (KeyboardMonitor ->
libSceKbEmulate -> libScePsm virtual pad, eleven keys) is not reproduced.

Not reproduced (logged once): the keyboard type -> layout table (0xBEDE00; the default layout
0xCD5F78, read from the dump, is used), CapsLock and NumLock keypad remaps, AltGr and dead keys,
the switch to a unit of the keyboard's language (0x1175F0), Shift+arrow selection, user repeat
settings, the KeyboardForPsm server.

## Input script

`--input-script` types on the declared USB keyboard:

* `f<frame>:text:<characters>` — printable ASCII, one character every 10 frames (held 4), each
  through the key (and Shift) of the firmware's default layout that produces it.
* `f<frame>:key:<name>[:<hold>]` — `enter escape backspace tab space delete home end pageup
  pagedown left right up down shift ctrl alt`.

Like the window's keys, these only reach the firmware while a keyboard handle is open.
