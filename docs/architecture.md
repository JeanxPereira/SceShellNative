# How SceShellNative works

The pipeline from a firmware dump to a running shell: the managed code and its runtime, Prep, the host boundary, the platform pieces and the graphics backend.

## Managed code and runtime

* The FW 4.03 managed assemblies carry full IL (later firmware ships IL-stripped,
  AOT-only images). The PS5 runtime is Mono from upstream branch 2019-10
  (`MONO_CORLIB_VERSION ABB721D6-…`), so the official desktop Mono 6.8 runs it.
* The PS5 `mscorlib` is not loadable by the desktop runtime (its `InternalThread`
  layout differs), so the firmware's copies of the Mono class libraries are replaced
  by the desktop BCL. Third-party libraries the firmware ships (Rx) are kept.

## Prep (`managed/Prep`)

`SceShellNative.Prep.exe` runs inside the launcher's own Mono runtime (in a child
process, `SceShellNative.exe --prep`) whenever the cache does not match the current
firmware and host boundary. It:

1. carves the .NET PE out of every `*.dll.sprx` / `*.exe.sprx`;
2. drops firmware copies of Mono class libraries and retargets references from the
   mobile profile (2.0.5.0, `7cec85d7bea7798e`) to the desktop identities;
3. classifies every native entry point against the host boundary table:
   * **host** — an InternalCall stays an InternalCall (registered with
     `mono_add_internal_call`); a P/Invoke is pointed at the launcher's own export
     (`__Internal`, symbol `ssn_<lib>_<entry>`);
   * **library** — a P/Invoke into a passthrough native library is left as is;
   * **stub** — the body is replaced by `SceShellNative.Boundary.Unimplemented("<id>")`
     followed by a default result (`""` for strings and `out string`, zero otherwise).
4. writes `boundary.tsv` listing every entry point and its disposition.

The launcher logs the first call of every stub (`[stub] #n …`) and the hit counts at
exit, so the log is always the up-to-date list of what the shell needs next.

## Host boundary (`launcher/src/host`, `psmhost/`)

Implementations are grouped by the firmware module whose behaviour they reproduce
and register themselves:

```cpp
SSN_ICALL("Sce.PlayStation.Core.Graphics.PsmGraphicsContext::Clear", Clear);
SSN_PINVOKE("libSceBootManager", "SetBootEvent", void, ssn_BootManager_SetBootEvent, (uint64_t e)) { … }
```

Every implementation cites the address of the firmware function it reproduces
(FW 4.03 module offsets) and summarises its behaviour. Values and behaviours come
from the firmware (decompiled IL, disassembly of the native modules, resource
files); where a detail is not traced yet the code says so and logs it.

## Platform pieces

| Area | Firmware it reproduces | Notes |
|---|---|---|
| Graphics | libScePsm `PsmGraphicsContext/Device`, `PixelBuffer`, `Texture`, `FrameBuffer`, `VertexBuffer`, `ShaderProgram` | Vulkan backend, see below |
| Shaders | libScePsm's 72 embedded AGC shaders; the eboot's BGLayer shaders | `docs/psm-shaders.md`; transcribed: basic_vv, solid_fill_p, rgba_p, a8_p, mask_p, ...; eboot light_p, particle_c/vv/p, large_particle_vv/p |
| Resources | libScePsm `CxmlResources` (RCOF/CXML `.rco`) | textures, strings, sound scripts |
| Images | libScePsm `Image` codecs | PNG (libpng-equivalent decode), SVG (nanosvg 1f17a7e + Sony changes) |
| Text | libScePsm `Font` / `Image.DrawText` | FreeType 2.4.11 + HarfBuzz 1.3.4, the versions libScePsm links |
| Registry | libSceRegMgr / kernel regmgr | `docs/registry.md`; defaults from `Sce.Vsh.Registry` (+ optional `registry.db0`), persisted |
| Kernel | event flags, system software version | `psmhost/libkernel`, `psmhost/libSysBridge` |
| Shell natives | eboot BootManager + init thread, LncProxy | `psmhost/eboot` |
| System services | ShellCore user service, RNPS app manager, LncUtil event queue; URL config resolver | `docs/app-launch.md`, `docs/url-config-resolver.md` |
| React Native | libNativeExtensions JSCExecutor over Sony's JavaScriptCore (JSC-400) | `docs/rn-bridge.md`, `tools/jsc/` |
| Audio | libScePsm `Sound` / `SoundPlayer` and `Bgm` / `BgmPlayer` over its PSS audio system (resources, voices, stream buffers, "ScePsP:AudioStream" decode thread, per-grain mixer, BGM port with its direct player, decoder loop / seek) | `psmhost/libScePsm/audio`; VAG resources through the firmware's HEVAG decoder (tables read from the dump's libScePsm.sprx); RIFF/ATRAC9 resources through the firmware's ATRAC9 decoder over libSceAudiodec, which is LibAtrac9 efca2e3 (MIT, `third_party/atrac9.cmake`) on the PC; main and BGM ports -> host audio device |
| BGLayer sounds | eboot SoundPlayer natives and "NGS2 Music System" over libSceNgs2.native (system, mastering / sampler racks, voices, commands, render loop, Player / ChannelConverter / Output modules, ParseWaveformData) | `psmhost/eboot/bglayer_bgm.*`, `psmhost/libSceNgs2`; sampler ATRAC9 frames by LibAtrac9 (host value); the eboot's FLOAT_8CH port -> host audio device |

## Graphics

The PSM graphics API the shell drives (`Sce.PlayStation.Core.Graphics`) is an
immediate-mode, GLES2-style API: state + `DrawArrays` into the current frame buffer,
`SwapBuffers` to present. The Vulkan backend (`launcher/src/gfx`) records a frame into
one command buffer using dynamic rendering, renders the PSM screen into an offscreen
1920×1080 target (the display buffer `GetScreenInfo` reports) and blits it to the
window's swapchain on `SwapBuffers`. Frames are synchronised as libScePsm's `SwapBuffers`
(0x1e6d40) does on the console: it waits for the previous frame (sub_2BC0 ->
`sceCompositorWaitPostEvent`), submits the new one (sub_78F0) and returns without waiting for
it, so frame N may still run on the GPU while the shell runs frame N+1. The host keeps two
frame slots (command buffer, fence, half of the upload ring, captures): `present()` paces
(headless: the ~60 Hz timer; windowed: the FIFO acquire), waits for frame N-1's fence, submits
frame N and returns; captures of a frame are written once it has retired. On the GPU the frames
still run one after the other (each command buffer starts with a full memory barrier).
libScePsm's shaders are compiled AGC (RDNA)
binaries embedded in the module; the host uses Vulkan transcriptions of them
(`launcher/shaders/psm`), see `docs/psm-shaders.md`.

Images the shell asks to be block-compressed (`Image.Convert(…, DXTCompressByGPU)`, e.g. the
home's tile icons, which it converts to BC7) go through libScePsm's GPU texture compressor.
The compressor shaders are embedded in libScePsm as RDNA binaries, one per block format
(table 0x4A23C0). The BC7 one is transcribed by `tools/shaders/isa2glsl.py` into
`launcher/shaders/psm/gtc/gtc_bc7.comp`. The host runs it as one synchronous compute dispatch
per image (`psmhost/libScePsm/imaging/image_gtc.cpp`) and reads back the 128-bit blocks. The
other formats' compressors are not transcribed yet. See `docs/psm-shaders.md` §11.

