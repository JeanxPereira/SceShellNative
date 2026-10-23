# Declared host values and policies

Where the console's answer depends on something a PC cannot have (a console identity, a
connected service, data missing from the dump), the launcher uses an explicit host value
or policy. Each one is marked `HOST VALUE` / `HOST POLICY` in the code and logged once
(`[stub]` lines). The policies that change behaviour:

* **Video-out vblank** — `PsmGraphicsDevice.WaitVBlank` (libScePsm 0x1e8c60) waits on the
  port's vblank event queue. The PC has no PS5 video out, so the host's vblanks are a clock at
  the refresh rate the port reports (59.94 Hz), and `WaitVBlank` sleeps until the next tick.
  The shell polls `WaitVBlank` + `ReceiveSetModeEvent` in a loop, so without this the loop spins.
* **RNPS bundle decryption (`/dev/rnps`)** — the console decrypts an app's `RNPSHEDR` bundle
  in place through `/dev/rnps` (libNativeExtensions DecryptRnpsBundle 0x16370 -> ioctl
  0xC0105203). A dump may carry what that ioctl produced on a console for each container
  (`<dump root>/rnps/<apps|bgs>/<titleId>/<bundle>_out/<bundle stem>_`, e.g.
  `application.ps.bundle_out/application.ps_`: the payload plaintext, exactly the header's
  real payload size, header +0x24). HOST VALUE: the host's `/dev/rnps` answers the ioctl for a
  container whose bytes equal that title's container in the dump by writing that output over the
  payload (header +0x1C), result 0; the SBL signature check is not reproduced. Everything after
  it is the firmware's own path: `JavaScriptBundleDecryptor` keeps `RequireEncryptedBundles =
  true` and its certificate-chain check, and `FileJavaScriptBundleLoader` hands the plaintext to
  `JSIndexedUnbundle` when it is a RAM bundle (magic 0xFB0BD1E5: module table, startup code;
  modules are evaluated on demand through `nativeRequire`, libNativeExtensions 0xf7d0 -> managed
  `JSCJavaScriptExecutor.NativeRequire`). Any other container gets what the console gets when the
  device cannot be opened (0x800F1213).
* **RNPS app with no readable bundle** — when an installed React Native app named by the
  URL config has no readable JS in the dump (an `RNPSHEDR` bundle without a decryption output,
  or nothing), `sceUrlConfigResolverGetAppConfig` answers the URL's configuration without its
  `"rnps"` member, so the shell treats the URL as not served by an RNPS app instead of failing
  ("Cannot fallback to original bundle" ends the process). Every caller handles that form
  (`Utility.GetRnpsAppConfig` -> null, `BootHelper.GetSchemeConfig` keeps the scheme's plugin,
  the home's AppModule `SceneFactory.Create` -> no scene). The former answer, no entry at all
  (0x80ba8087), is not caught by `SceneFactory.Create`, which the FW 4.03 home (683.x) reaches
  for the Media Gallery hub (NPXS40075, encrypted in the 4.03 dump). No bundle content is made up.
* **RNPS version check** (`--rnps-version-policy warning`, opt-in) — Prep makes
  `RenderScene.CreateAppConfig` store `VersionCheckFailureMode.Warning` instead of
  `Exception`, so bundles built for another RNPS version (e.g. the 0.59.6-680.72 `--rnps`
  bundles) run; the reported versions and their comparison are unchanged. Not needed with the
  FW 4.03 dump's own bundles (RNPS 0.59.6-683.x: `RNPSVersionCheck` compares major, minor, patch
  and the prerelease up to its first '.', 683 = 683; no "FW Version Mismatch" is traced).
* **RNPS version banner** (`--hide-version-banner`, opt-in; cosmetic and temporary) — in
  Warning mode `ReactNative.PUI` `RNPSVersionCheck.ShowWarning` puts a red
  "FW Version Mismatch: JS … != native …" label over the app's scene. With this option Prep
  makes `ShowWarning` return at once (host policy `rnps-version-banner=hidden`, part of the
  cache stamp); the mismatch is still traced (`Tracer.Warn`) and the check is unchanged.
  Without it the banner stays, as the firmware draws it.
* **RNPS app trees** (`--rnps`, for plaintext bundles from elsewhere) — the console's `/system_ex/rnps/apps/<titleId>` holds the
  app's manifest, appdb and assets next to an encrypted bundle. The launcher composes
  `<cache>/rnps/apps/<titleId>` from the dump's app directory with the `--rnps` files over it
  (hard links or copies; nothing generated), and Prep stores `false` into
  `JavaScriptBundleDecryptor._isEncryptionRequired` (the shell hard-codes
  `RequireEncryptedBundles = true`) so the plaintext bundles load; both bundle branches stay.
* **Firmware build of the eboot and SceShellCore** — the eboot data the host reads at run time
  (BGLayer effect blobs and sizes, IES / colour / preset / gradation tables, transition
  parameters, NGS2 rack options) is cited at its address in the FW 4.03 retail build
  (J01736823). The FW 4.03 devkit build (J01736824, same sources, relinked) holds the same bytes
  elsewhere; `eboot_bytes` (`psmhost/eboot/bglayer_gpu.cpp`) identifies the dump's eboot by its
  GNU build-id and translates each address through that build's table (every entry checked
  byte for byte and against the referencing code in both builds, `work/ida_r7diff/report.md`
  §3a). An unknown build reads nothing (logged). Likewise `shellcore::devkit_build()` (the
  dump's `SceShellCore.elf` build-id) selects the devkit build's behaviour where it differs in
  what the host reproduces: `tbl_version` `build_target` written / expected as 1 (devkit
  0xBEB250 / 0xBC9B30) and no NPXS29998 case in appInstallRNPSTitleDirMain (devkit 0x2334F0).
* **Devkit activation** — `sceSblDevActGetRemainingTime` (eboot 0x4A9920 -> 0x4AA2B0) opens
  `/dev/devact`; the PC has none, so it returns the console's open-failure result 0x800F0013
  (and `SblDevActHasPassCodeNative`, eboot 0x4AA530, answers false, its open-failure answer).
* **Install paths for managed code** — host directories handed to the firmware's managed code
  (RNPS install paths) are `/`-rooted without a drive letter (`host::managed_path`): the shell
  round-trips them through `Uri.AbsolutePath`, which turns `E:/x` into `/E:/x`. They resolve on
  the process's current drive.
* **Background layer** — `ShellUIBackgroundLayer` (`psmhost/eboot/bglayer*`) reproduces the eboot's
  BackgroundLayer for the Light background (ThemeColourIndex 0x40..0x4F): `UpdateNative` updates
  it and draws the eboot's `light_p` pass (`launcher/shaders/bglayer`, IES textures and colour
  table read from the dump's `NPXS40087/eboot.bin`) into the display buffer before the UI renders,
  as on the console (`BGLayerPlugin` keeps `ClearFrameBufferAutomatically = false`). The
  LightParticle layer (`bglayer_particle*`) runs as on the console: the effect timelines embedded
  in the eboot (`spread_expanded`, `coldboot`, ...) drive the emitter parameter blocks, the eboot's
  compute shader `particle_c` simulates the 6000-particle pool, `particle_vv`/`particle_p` draw it
  into a 10_11_11_FLOAT target that `light_p` composites. The three shaders are transcribed
  instruction by instruction by `tools/shaders/isa2glsl.py` (see `docs/psm-shaders.md` §10); the
  CPU side reproduces the layer state machine, the xorshift128+ PRNG with its static seed, the
  brightness/cross-fade of the extra layer and the blend states. The boot animations (ThemeColourIndex
  69 ColdBoot -> mode 3, 70 WarmBoot -> mode 4, effect `coldboot`) add the large particles: two more
  emitters per generation simulated by `particle_c` and drawn additively by `large_particle_vv/p`
  with `/system_ex/vsh_asset/Sce.Vsh.ShellUI.BGLayer.Particle0/1.gnf`, which the layer's resource
  job (0x96240) loads on a worker thread (a missing file gives the job's own 1x1 black texture).
  The Transition layer (`bglayer_transition*`) shows background images (e.g. the home's
  `/system_ex/vsh_asset/bg_NPXS40071.dds` behind the Game Library tile): MapTextureNative wraps the
  libScePsm texture the managed side loads, SetBGTransitionNative / SetBGBasematNative and the
  per-frame state machine (0xA53F0) are reproduced, the layers before it render into its capture
  texture for the old image, and the eboot's transition shaders (single, cross_fade, ripple,
  slide_in, game_splash, basic_p; `launcher/shaders/bglayer/transition`, transcribed by isa2glsl)
  draw it with the basemat GradationParameter buffer (spline 0xA7390). Not reproduced:
  `ripple_with_logo_p` (LaunchingGame with an overlay logo; that draw is skipped and logged). Host
  values: the transition shaders' `s_memrealtime` dither seed as for `light_p`; the gradation
  spline's `vrcpps` is the host CPU's (Zen 2 may differ in the low bits after the Newton step).
  Not reproduced yet, each logged once: VR (the BGM and boot sounds: see *BGLayer sounds*). The wave themes (ThemeColourIndex groups 0..3) are not reachable
  in FW 4.03: the managed BGLayer only sets ThemeColourIndex 65..79 and PresetColourIndex 4, and
  no FW 4.03 assembly calls `SetThemeColourIndex`. Host
  values: the `s_memrealtime` dither seed of `light_p` comes from the host performance counter;
  the unidentified import `kiA9bZhbnAg(3)` behind the 10-minute background dimming reads 0.
  The particle layer's two end-of-pipe labels (+1976 / +1984) are GPU-written as on the
  console: Render (0x971B0) stores 1 into the frame's label and records a release_mem
  (`sceAgcCbReleaseMem`) that stores 0 once the frame's particle work has drained (host: a
  barrier + `vkCmdFillBuffer` into a host-visible slot). IsBusy (0x97470) and the generation
  switch (0x97560 / 0x96FF0 / 0x94020) read them while the previous frame may still be on the
  GPU (see the frame synchronisation above), so at the cold-boot `coldboot` -> `spread_expanded`
  switch the old generation gets its 7 s (dword_AFFB7C) instead of ending at once.
  The particle shaders need `vertexPipelineStoresAndAtomics`.
* **Video** — `PsmVideoPlayer` (libScePsmUtil) is reproduced down to the media framework:
  handle table, command queue and worker (0x11790), state machine (0xee80), status and
  video-event queues, error fields, MediaFw event callback (0x16500 -> 0x190c0). Below it,
  `sceMediaFw*` is an IPC client of the BE core process (NPXS40039 `becore.elf`), which
  demuxes/decodes and renders into a compositor canvas. That process is not on the host:
  `psmhost/libScePsmUtil/media_fw.cpp` stands in for it — the dump's file is decoded with
  Windows Media Foundation (Source Reader, RGB32; `psbutton_press.mp4` is HEVC Main 1000x1000,
  which needs the system HEVC decoder), frames are published at their presentation times to a
  host compositor canvas (`psmhost/libSceComposite/canvas.cpp`) and the media-framework events
  are raised in a host-chosen order (listed in `media_fw.cpp`). Audio is not decoded. libScePsm's
  canvas textures (pixel-buffer type 3, `CanvasTexture2DNative`) copy the canvas's newest frame
  before a draw samples them. A file missing from the dump makes `sceMediaFwAddSrc` fail with
  `0x80020002` (host choice) and the firmware takes its own error path (`initial_boot_movie.mp4`
  of `/preinst` is not in the dumps).
* **ShellUI boot arguments** (`--cold-boot`, `psmhost/eboot/boot_args.cpp`) — SceShellCore launches
  ShellUI with `--cold-boot` when it starts it at run level 80 (power-on; shellcore 0x768650) and
  with no arguments at run level 100 (main on standby). The eboot hands main's argv to the managed
  side (`lx::lxUT::GetBootArgumentsCount` / `GetBootArgument`, 0x9150 / 0x9190). Which launch the
  host plays is a HOST POLICY: by default the run-level 100 launch (no arguments); with
  `--cold-boot` the power-on launch after a clean shutdown, which makes BootManager.IsColdBoot true
  and with it the health warning and the cold boot animation (ColdBootAnimation, the LightParticle
  `coldboot` effect with the large particles). HOST VALUE: argv[0] is
  `/system_ex/app/NPXS40087/eboot.bin`.
* **Platform charset** — the PS5 runtime is a Unix Mono (`Auto` = UTF-8); Prep points
  `Marshal.*Auto` calls and `CharSet.Auto` P/Invokes of the firmware at their Ansi forms.
* **Device bus** (`psmhost/libSceMbus`) — the SceSysCore mbus server's device table holds one
  device: the input layer's emulated DualSense as SceSysCore's addDevice would register a
  USB DualSense (device id 0x10001, VID 1356 / PID 3302, type 22, bus 0, controller
  function only, no owner, no charge-state source). The server's condition matching and
  record conversion and the eboot's LoginMgrProxy `_GetDeviceInfoList` are reproduced over it.
  The device is attached (SCE_MBUS_EVENT_DEVICE_ADDED queued to every client event handle)
  when the pad is turned on; a bind (`_Login`) queues SCE_MBUS_EVENT_DEVICE_OWNER_CHANGED
  (where SceSysCore posts it is not traced). libScePsm's controller scanner and the eboot's
  LoginMgr watch thread read those events.
* **USB keyboard** (`launcher/src/platform/input`, `psmhost/eboot/ime_keyboard.cpp`, `docs/ime.md`) —
  the PC keyboard is also one USB keyboard at the boundary of the eboot's statically linked
  libSceKeyboard: always connected, HID usages from the Windows scan codes, LEDs from the PC's
  lock keys. While the system IME holds a keyboard handle (an on-screen-keyboard session is
  open) the window's keys go to it, not to the DualSense key table; the firmware turns them into
  text and edit operations as for a USB keyboard on the console. Logged once.
* **DualSense power** (`launcher/src/platform/input`) — the emulated DualSense is off at boot,
  as a DualSense is: only its PS button (keyboard Home, input-script `ps`) turns it on, and that
  press is not reported as button data; turning it on connects it to the bus. At the Welcome
  screen this is what "Press the PS button" acts on: the eboot LoginMgr reports the new,
  unassigned controller to ShellUI as `psaccounts:login?deviceId=…` (the user-selection screen
  for that controller). `--pad-on-at-boot` starts with the pad on (as when the console was
  woken with it); the LoginMgr then reports it when its event watch starts. XInput does not
  expose the Guide button, so an XInput pad cannot turn the emulated pad on.
* **Audio out** (`psmhost/libScePsm/audio/pss_audio_out.*`) — libScePsm's audio ports are
  sceAudioOut ports on the console: the main port (48 kHz, 2 channels, float, 256-frame
  grains) and the BGM port (sceAudioOut BGM type; 8 channels, reopened as 8 channels 16-bit
  for a direct ATRAC9 player). Each port's grains go to the default Windows render device
  through its own WASAPI shared-mode stream (Windows converts to the device format; the
  8-channel order L, R, C, LFE, Ls, Rs, Lb, Rb is rearranged into WAVEFORMATEXTENSIBLE 7.1
  surround order), and a grain is written once the device queue is under two device periods
  — the stand-in for sceAudioOutOutput's blocking. sceAudioOutSetVolume (the BGM direct
  player's volume, `(int)(volume * 32768)`) is applied to the samples (32768 = 0 dB); the
  port state (import Y3lXfCFEWFY) reads 0, so PortStatus is OK and BgmPlayer.PortUsable true.
  Driver thread start / stop (sub_18F820 / sub_18FBD0 / sub_18FB50) is a run flag on one host
  thread per port. With no device (or after a device error) the grains are dropped at 48 kHz,
  paced by the host clock, so headless runs need no audio hardware. Pad-speaker, personal
  (controller headset) and surround ports do not exist on the PC: players for them are not
  created (SoundPlayer.Play reports the audio-system error). Not reproduced yet (logged
  once): RIFF PCM / MP3 / ADPCM resources, the main port's looping mixer and pending seeks of
  mixed voices, HEVAG seeks to a non-zero position, a second BGM voice beside the direct
  player. A third port, the eboot's BGLayer sound port (see *BGLayer sounds*), uses the same
  host output; closing it (the eboot joins its audio thread) ends the port's thread.
* **BGLayer sounds (NGS2)** (`psmhost/eboot/bglayer_bgm.*`, `psmhost/libSceNgs2`) — the boot
  sounds (`sfx_coldboot` / `sfx_warmboot` / `sfx_initialboot` / `sfx_transition` .at9 of
  `/system_ex/vsh_asset`, read from the dump) play through the BGLayer's SoundPlayer: the eboot
  natives InitializeBgmNative .. DumpBgmDataNative (0x8E840 .. 0x8E9E0, manager 0x8F1B0 .. 0x8FC70,
  per-frame volume ramps 0x8F3E0 from UpdateNative) and the eboot's "NGS2 Music System"
  (0x8FE40 .. 0x915A0: one system of 512-frame grains at 48 kHz, a mastering rack with one
  8-channel mastering voice, a sampler rack with one sampler voice per sound patched to it, an
  audio thread rendering 8-channel float grains) over libSceNgs2. libSceNgs2 is the dump's
  `libSceNgs2.native.sprx` (sysmodule 11 for a native app), reproduced for this use: the API
  (system / rack / voice creation, sceNgs2VoiceControl turned into internal commands, command
  validation / application, state flags, ParseWaveformData for RIFF ATRAC9), the render loop
  (event application and parameter commit 0x13D70, voice processing 0x15DE0 in list order,
  input patches 0x17620 with the default channel matrices and gain ramps, module passes, stop /
  pause fades, the status merge 0x135A0), and the modules the path runs — a module runs only once
  a command has set one of its parameters, so the sampler voice runs only the Player (block
  queue and reader, decoder setup / process, the resampler, which at ratio 1.0 is a copy with a
  3-sample delay) and the mastering voice only ChannelConverter (8 -> 8 identity) and Output
  (float32 interleaved render buffer). Tables (default matrices, resampler FIR, ATRAC9 frame
  sizes, format GUIDs) are read from the dump's module at run time. HOST VALUES: the sampler's
  ATRAC9 frames are decoded by Sony's software decoder inside libSceNgs2.native (0x36070 /
  0x364C0 / 0x3A8D0) on the console; it is not transcribed and LibAtrac9 (above) decodes in its
  place, its IMDCT output (16-bit scale, double) rounded to float and scaled by 2^-15 with the
  firmware's clamp, as the firmware's output type 288 asks. The eboot's port
  (sceAudioOutOpen(SYSTEM user, MAIN, 512 frames, 48 kHz, FLOAT_8CH), volume 0x8000) is a host
  audio-out port ("ngs2", see *Audio out*) whose thread calls the eboot's render callback. The
  engine keeps its objects on the host heap instead of the eboot's allocator (malloc / free
  handlers, not called). Each logged once. Not reproduced (logged once when reached): other
  waveform types (PCM, VAG / ADPCM, MP3), loops and block repeats, resampling at ratios other
  than 1 (transcribed, not verified), the other effect modules' DSP, user matrices / panners,
  streams, reverb / submixer / custom racks, the AJM decoder class (sampler racks with option
  +0xCC). On the cold-boot path (`--cold-boot`) sfx_coldboot plays from the grain after its
  load; its NGS2 output was checked bit-exact against an independent LibAtrac9 decode of the file
  (`work/ida_r6ngs2/ref`). The dumps used here have no sfx_initialboot / sfx_transition: those
  loads take the firmware's failure path (LoadBgmNative -1, the managed "SoundPlayer Status
  0xFFFFFFFF").
* **ATRAC9 decoding** (`psmhost/libScePsm/audio/audiodec_atrac9.*`) — libScePsm's ATRAC9
  decoder (PSS type 3, sub_191C50) drives libSceAudiodec (sceAudiodecCreateDecoder 0x191D80,
  sceAudiodecDecode 0x192200, ClearContext 0x191D30 / 0x192190, DeleteDecoder 0x1920C0),
  which decodes on the console's ACP/AJM hardware. Windows has none: LibAtrac9 by Alex Barney
  (github.com/Thealexbarney/LibAtrac9, commit efca2e3af35562a09a9bb6deed90e45b4b824dc4, MIT,
  vendored unmodified in `third_party/LibAtrac9-efca2e3`) decodes in its place, one 256-sample
  frame per decode call as the firmware sizes the PCM item; the last frame of a superframe
  reports the superframe's padding as used (its fixed size, the fmt block align), and a
  context clear is a fresh LibAtrac9 instance. Logged once. Decoded output was checked
  bit-exact against a standalone LibAtrac9 decode of the same files; ffmpeg's ATRAC9 decoder
  gives the same samples with the opposite polarity (±1 LSB), which of the two matches
  Sony's decoder is not known.
* **Console paths in managed File.Exists** (policy `console-files=bgm`, `managed/Runtime`
  ConsoleFiles, Prep ApplyConsoleFilesBgm) — the PC's file system has no `/system_ex`; PUI's
  BgmPlayerWrapper checks its BGM file with File.Exists before `new Bgm(path)` (whose native
  side opens console paths through the firmware roots). Its File.Exists calls are pointed at
  a helper that looks rooted paths up in the firmware roots (and checks anything else as
  given). Without it the home and onboarding BGM are dropped ("PlayBgmPlayer File Not
  Found").
* **Voice and Agent** (`psmhost/libSceVnaInternal`) — no Voice and Agent daemon (SceShellCore's
  "Voice and Agent", NPXS40094) runs on the host: its IPMI server `SceVnaIpcServer` and event
  flags `SceVnaStatVOIN` / `SceVnaVoLang` do not exist, the console's state before ShellCore
  launches the daemon. The libSceVnaInternal client takes its own path for that:
  `sceVnaInitialize` (0xD40) connects without waiting and gets the kernel's "no such server"
  (ESRCH, passed through by libSceIpmi as 0x80020003) with `*context = 0`; every per-context
  call finds no context and answers 0x816F8007 (NOT_INITIALIZED); `sceVnaUtilInitialize`
  cannot open the event flags (ESRCH). The managed side does what the firmware does with
  that: the agent core logs the error and retries once a second from its error state; no
  context is ever polled, so no daemon crash is reported. `sceVnaGetErrorSeverity` (a pure
  error table, not reached) stays a stub.
* **HDMI monitor** (`psmhost/libSceAvSetting/av_monitor.cpp`) — the PC has no HDMI sink and
  no AV-setting server. The host declares one generic monitor on the HDMI port, the sink of
  the declared video out: CEA-861 VIC 16 (1920x1080p, 148.35 MHz at 59.94 Hz) plus the
  mandatory 720x480p (VIC 2 / 3), 24-bit RGB only (limited range on CE formats, no
  quantisation-range bit), one short audio descriptor (2-channel LPCM, 32/44.1/48 kHz,
  16/20/24 bit, FL/FR), no YCbCr, deep colour, HDR, 3D, VRR or HDCP 2.2. Only the
  `SceAvControlMonitorInfo` fields the firmware reads are filled. Device id 0x7000 (the id
  libSceAvSetting accepts for the HDMI device), connected, no secondary device, output path
  0, HDCP enabled (1.x), no deep-colour / VRR restriction; the server's video-out mode is
  the mode the firmware's own resolution picks for 1080p 59.94 Hz on that monitor (RGB,
  limited, CERGB, 24 bpp). `IsVideoOutModeSupportedByMonitorInfo` (0x29E0 -> 0x58D0),
  `IsAudioOutModeSupportedByMonitorInfo` (0x2940 -> 0x7960) and
  `IsHdrSupportedByMonitorInfo` are libSceAvSetting's code, transcribed in
  `mode_support.cpp` and checked against the module's own machine code on generated inputs
  (530k video, 120k audio, no difference). The output-mode queries (`GetCurrentOutputMode*`, `CheckVideoOutMode`, PU type)
  stay stubs.
* **Compositor rendering time** — `sceCompsoitorGetProcessRenderingTime` (libSceComposite
  0x4560) answers the host renderer's GPU frame time (Vulkan timestamps, ns) of the last
  retired frame.
* **Game Live Streaming** — no GLS service runs on the host, so SceBootStatusFlags never gets
  SCE_BOOT_STAT_GLS (0x800): the eboot's GLS client takes its own "not ready / no session"
  results (0x80A00003 / 0x80A00004).
* **Network hardware** (`psmhost/libSceNetCtl`) — the libSceNetCtl client (callback tables,
  event bits, CheckCallback dispatch) is reproduced; the netctl server answers as a console
  with no network: state disconnected, scans complete at once with no networks, disconnect
  requests complete at once, results 0. Since no connection is ever made, `GetInfo` answers
  SceShellCore's "not connected" (0x80412108, info zeroed), the connection elapsed time is 0 and
  the InetStat state 0. The kernel's interface list (`sceNetGetIfList`, libSceNet 0x4B0) has no
  eth / dbg / wlan interface, so a named lookup finds nothing (0 entries, buffer not written);
  the loopback record lo0 is not traced (a lookup of it, or of the whole list, is logged and
  answers 0 entries). The netctl server's check of the caller's authority id treats ShellUI as
  an ordinary caller (its id is in the SELF auth info, not in a decrypted dump).
* **USB storage** (`psmhost/libSceUsbStorage`) — `getDeviceList3` (libSceUsbStorage 0x37c0 ->
  SceShellCore UsbDeviceManager::getDeviceList 0x2C0870) answers as a console with no USB
  storage attached: count 0, result 0 (also `sceUsbStorageGetDeviceList`, 0xb350).
* **Console identity** — OpenPSID (`machdep.openpsid_for_sys`) reads as 16 zero bytes; the
  ShellCore VM statistics block counts no console processes. The IDPS product code that
  `sceKernelIsCEX` / `IsDevKit` / `IsCronos` test (libkernel 0x1e190 / 0x1df90 / 0x1e460) is a
  retail code (0x8402): a retail CEX console.
  The ShellUI process's ucred carries the kernel's system attribute (ucred+96 bit 62; the SELF
  auth info is not in a decrypted dump), so the `machdep.rcmgr_*` QA flags and
  `machdep.curr_manumode` answer 0 (kernel 0xFFFFFFFF80E34180.., 0xFFFFFFFF80E33A20).
  The PC has no ICC or board id: `machdep.icc.hw_info` / `hw_model` (`sceKernelGetHwSerialNumber`
  / `GetHwModelName`, libkernel_sys 0x24C30 / 0x24CF0) fail as the kernel's board-id failure
  does (ENXIO, 0x80020006; 7.00 kernel handlers 0xFFFFFFFF80C02BB0 / 0xFFFFFFFF80C02CD0), and
  there is no `/dev/dipsw`, so libSceDipsw answers its zeroed switch table:
  `sceKernelIsDevelopmentMode` / `IsAssistMode` 0.
* **Secure and network clocks** — there is no `/dev/srtc`, so `sceSblSrtcGetCurrentSecureTick`
  fails with 0x800F0513 (as libScePsm SystemParameters key 19 already does); the network clock
  was never synchronised (registry DATE_rtc_net2 holds the never-synced value), so
  `sceRtcGetCurrentNetworkTick` answers 0x80B50001.
* **System state** — SceShellCore's SystemStateMgr sets the boot trigger code from the ICC
  power-up cause (0x6C8350). The PC has no ICC: the host takes that function's failed-call path,
  trigger code 1 and state 1000 (WORKING), in the `SceSystemStateMgrInfo` event flag that
  `sceSystemStateMgrGetCurrentState` / `GetTriggerCode` (libSceSystemService 0x19DF0 / 0x19EA0)
  read.
* **PSN accounts** — the NP manager's user contexts (SceShellCore 0x8B1480, one per registered
  user) have no account data: `sceNpManagerIntGetSigninState` answers NOT_SIGNED_UP,
  `GetCachedAccountInfo` and `GetAccountCountryA` 0x8055000A, and `GetAccountType` type 1, as
  for a local user that never signed up. The parental check of the home
  (`psmhost/eboot/np_papc.cpp`) follows from that: a type-1 user is held to the system's game
  age limit (registry `SECURITY_PARENTAL_game_age_limit`, 21 by default), compared with the
  title's `ageLevel`.
* **NpCommon JSON errors** (`psmhost/eboot/np_papc.cpp`) — the status codes libSceNpCommon's
  streaming parser returns for malformed text are not traced; the host returns 0x80550602 for
  a malformed `ageLevel` or parental whitelist.
* **Thread scheduling** — PS5 thread priorities and schedule policies (`NativeThread.Priority` ->
  libScePsm 0x300f90 scePthreadSetprio; `ThreadUtils_SetSchedulePolicy` -> libNativeExtensions
  0x15330) are not mapped onto Windows threads; the calls answer 0 and Windows schedules the
  threads as usual. Thread names (`ThreadUtils_SetThreadName`) go to SetThreadDescription.
* **NativeFile.CreateWithSystemPermission** (libScePsm 0x2f2f90) creates the file or directory
  where the managed File API resolves the same path; the POSIX mode of table 0x462100 has no
  NTFS counterpart and is not applied.
* **Bluetooth radio** (`psmhost/eboot/bt_device_manager.cpp`) — the host `/dev/bt` accepts the
  open and inquiry ioctls of the eboot client (0x4E7110..0x4E7770), finds no device, and answers
  a stop with `SCE_BT_EVENT_TYPE_INQUIRY_STOP` (status 0).
* **Timezone** — the kernel timezone behind `sceRtcGetCurrentClockLocalTime` (libSceRtc 0x320) is
  the PC's UTC offset, the local time the shell's managed clock also uses.
* **Yoga** — `YGNodeLayoutGetMargin/Border/Padding` follow the firmware build (edges up to
  `End` accepted) instead of the 1.5 DLL, which rejects `End`; the other entries pass through.
* **BCL members** — calls to `Dictionary<K,V>.GetValueOrDefault` (present in the PS5's
  mobile-profile mscorlib, absent from the desktop BCL) go to corefx-equivalent helpers in
  `SceShellNative.Runtime`; Prep lists any other missing BCL member in `bcl-missing.txt`.
* **Users** (`psmhost/shellcore/user_service`, `login_mgr_server`, `eboot/login_mgr_login`) —
  SceShellCore's SceUserService keeps persistent users in the registry slots
  `USER_01..16_*`; the host reads and writes those keys through the registry store, so users
  persist in `<cache>/registry.txt`, with their home directories under
  `<cache>/users/<console path>`. New user ids follow ShellCore's allocator with the host's
  random source; the login-order counter starts at the users' highest `last_login_order`;
  the login event reaches ShellUI as the eboot forwards it (user-service event callback
  0x4A6A30 registered by initializeFrameworkDependentModules -> PSM event 40); guest users,
  the other user events, telemetry and the per-user region parameters are not reproduced
  (each logged once).
* **M.2 slot** — `SceAutoMounterUIM2Evf` is created with bit 0 set: the state after the
  AutoMounter has reported the (empty) M.2 slot with a "no dialog" result.
  `sceAutoMounterM2GetInternalStatus` answers as SceShellCore does for that slot (0x2A79C0): not
  present, availability NOT_CONNECTED (the boot mount's "not found" 0x80F40007 mapped by
  0x2B2DA0).
* **Host title platforms** (`psmhost/shellcore/app_db_promote.cpp`) — SceShellCore gives a
  title's `_ps_platform` from its category (0 PS5, 1 PS4, 2 PS3) and, for a few system titles,
  from a fixed table (0xB62F30; NPXS40144 and NPXS40148 get -1, no platform). The host adds
  entries to that table from `<cache>/host/ps_platform.json` (`{"<titleId>": <platform>}`), for
  titles a host tool installs into `/user/app` that are not console games. With -1 the home draws
  no PS5/PS4/PS3 mark on the tile, as for those system titles. Without the file nothing changes.
* **Application processes** (`psmhost/shellcore/lnc_host_app.cpp`, docs/app-launch.md) — the
  PC cannot run a PS5 executable, so SceShellCore's `spawnApp` starts the program a title names
  in `<cache>/host/apps.json` (`"exe"` with `"args"` and `"cwd"`, or a `"uri"` for the PC's
  shell). The application runs while that process runs and, with `"watch"`, while a process
  from that directory runs (the host waits up to 120 s for one to appear); then the launcher's
  `onAppExit` follows. A title without an entry fails to spawn with 0x80020002.
* **Application ids** (`psmhost/shellcore/lnc_service.cpp`) — the kernel's numbering
  (`sceKernelBeginAppMount2`) is not traced; the host numbers applications 1, 2, ..., ShellUI
  first. ShellUI treats appIds as opaque.
* **SQLite** — the eboot's static SQLite 3.35.5 (exported to Mono as `sqlite3`) is the
  upstream 3.35.5 amalgamation built with the eboot's compile options
  (`third_party/sqlite.cmake`); the console's VFS and mutex hooks are not reproduced (Windows
  VFS). `sqlite_aux` (error-code conversion, interrupt) follows the eboot.
* **File-system sandbox word** — the shell builds its `/system_data` paths as
  `"/" + SystemParameters.FileSystemSandboxRandomWord + "/..."`; the host's word is
  `<cache>/system_data` in drive-less rooted form, so those files live in the cache.
* **No console identity / services** — no NP, region and machine type
  0, video out 1080p 59.94 Hz, `SceBootStatusFlags` starting at 0, a DualSense fed from
  the keyboard/XInput.

