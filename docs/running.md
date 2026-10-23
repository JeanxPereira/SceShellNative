# Running

```
build\bin\SceShellNative.exe --firmware <dump root> [--overlay <dump root>]...
                             [--rnps <dir>] [--registry-db0 <file>] [--cache <dir>]
                             [--rnps-version-policy warning] [--hide-version-banner]
                             [--headless] [--capture-dir <dir> --capture-every N | --capture-frames a,b,c]
                             [--input-script "<events>"] [--exit-after-frame N]
                             [--pad-on-at-boot] [--cold-boot]
```

* `--firmware`: decrypted dump root containing `system/` and `system_ex/` (the
  managed set must be FW 4.03, which still carries IL). The console mounts `/system` and
  `/system_ex` from one of two update slots; a root that has no plain `system/` (`system_ex/`)
  but exactly one slot directory (`system_a`/`system_b`, `system_ex_a`/`system_ex_b`) maps the
  console path to it (nothing on disk is renamed). A root may also hold the console's RNPS
  decryption outputs under `rnps/` (see [RNPS bundle decryption](host-values.md)).
* `--overlay`: further roots searched for console paths the first root lacks (e.g. a
  dump that has `system_ex/vsh_asset`, or `preinst/common/font` for the system fonts).
* `--rnps`: directory with installed RNPS (React Native) apps, `apps/<titleId>/`
  (plaintext bundles supplied by the user; never part of this repository). Not needed when the
  dump carries its own decryption outputs.
* `--rnps-version-policy warning`, `--hide-version-banner`: see [Declared host values and
  policies](host-values.md).
* `--registry-db0`: the kernel's registry default table (`registry.db0`, decoded from a
  kernel image with `tools/registry/extract_db0.py`); without it, defaults come only from
  the firmware's `Sce.Vsh.Registry` definitions. The console registry itself (what
  `system.reg` is on the console: initial setup state, language, …) is kept in
  `<cache>/registry.txt`; see `docs/registry.md`.

Unattended runs (no window, nothing takes the focus):

* `--headless`: no window, surface or swapchain; the 1920x1080 display buffer is rendered
  offscreen and frames are paced by a ~60 Hz timer (as the FIFO swapchain paces them in a
  window). The prep child runs without a console window. Keyboard input is off (no window);
  an attached XInput pad still counts.
* `--capture-dir <dir>` with `--capture-every N` and/or `--capture-frames a,b,c`: writes
  those presented frames (counted from 1) as `<dir>/frame_<n>.png` (works with or without
  `--headless`).
* `--input-script "f300:cross;f420:down;f500:ps:10"`: presses a DualSense control at the
  end of frame N and releases it after the hold (default 42 frames, ~700 ms at 60 Hz;
  `:<frames>` sets it). Events go through the keyboard's key table (`docs/input.md`), so
  `ps` is exactly what Home does. Names: `cross circle square triangle up down left right
  l1 r1 l2 r2 l3 r3 options create`(=`share`)` ps touchpad mute`, and the stick directions
  `lup ldown lleft lright rup rdown rleft rright`. `f700:text:Jean` types printable ASCII on the
  USB keyboard (one character every 10 frames, through the firmware's default layout) and
  `f900:key:enter` presses one keyboard key (`enter escape backspace tab space delete home end
  pageup pagedown left right up down shift ctrl alt`, optional `:<hold>`); both reach the
  firmware only while the IME holds a keyboard handle (`docs/ime.md`).
* `--exit-after-frame N`: ends the process after frame N is presented (pending captures
  are written first).
* `--cold-boot`: ShellUI is launched as at power-on (see [ShellUI boot arguments](host-values.md)): the health
  warning and the cold boot animation play before the login / Welcome screen.
* `--pad-on-at-boot`: the emulated DualSense is already on at boot (see [DualSense power](host-values.md)).
  Without it the first `ps` (Home) turns the pad on; from a cache with one user and no
  auto-login, `f900:ps;f1200:cross` reaches the home (Welcome -> user selection -> home).

Standard configuration: the complete FW 4.03 dump (`system_b/`, `system_ex_b/` and `rnps/` with
the decryption outputs) as the firmware, plus a root with `/preinst/common/font` for the system
fonts (the 4.03 dump has no `/preinst`). No `--rnps` and no `--rnps-version-policy`:

```
SceShellNative.exe --firmware <4.03 dump> --overlay <root with preinst/common/font>
                   --registry-db0 <registry.db0> [--cold-boot]
```

The former configuration (a 4.03 dump with `system/` and `system_ex/`, FW 7 overlays for
`vsh_asset` and the fonts, `--rnps` with 680.72 plaintext bundles and
`--rnps-version-policy warning`) still works.

```
cd build\bin
SceShellNative.exe --headless --exit-after-frame 900 --capture-dir C:\tmp\cap --capture-frames 600,900
                   --input-script "f600:cross" --firmware <dump root> [...]
```

Diagnostics: `SSN_CAPTURE_DIR` + `SSN_CAPTURE_FRAMES=1,60,…` write those presented
frames as BMP; `SSN_TRACE_DRAWS=<frame,...>` logs every DrawArrays of those frames
(program, state, bound textures with their debug names, constant buffers; add
`SSN_TRACE_DRAWS_FULL=1` for all vertex streams and indices);
`SSN_THREAD_DUMP_AFTER=<s>` prints every managed thread's stack;
`SSN_MONO_TRACE=<spec>` is Mono's `--trace=<spec>` (e.g. `T:Ns.Type,M:Ns.Type:Method`; calls go
to stdout);
`SSN_VK_VALIDATION=1` enables the Vulkan validation layer;
`SSN_AUDIO_DUMP=<file.wav>` writes every grain of the main audio port (48 kHz stereo float WAV),
of the BGM port to `<file>.bgm.wav` and of the BGLayer's NGS2 port to `<file>.ngs2.wav` (48 kHz,
8-channel float; `work/ida_r6ngs2/ref/cmp_ngs2.py` compares it with a reference decode), and
`SSN_AUDIO_NULL=1` drops the grains instead of opening the audio device. The log lines
`[bglayer] sound N loaded / PLAY / unloaded (frame F)` and `[ngs2] rack … voice … starts in render
R` / `released after render R` (R = 512-frame grains of the NGS2 port) time the sounds. Logs go to
`<cache>/launcher.log` and `<cache>/prep.log`.

