# boundary tools

Static analysis of the managed -> native boundary of the PS5 ShellUI process
(Mono/PSM managed assemblies, `libScePsm`, `libmonosgen-2.0`, `eboot.bin`, PS5
system libraries). The scripts inventory every InternalCall and P/Invoke the
managed code declares, attribute each one to the native image that provides it,
and compute which of them are reachable from given entry points.

## Requirements

- Python 3.11+ and `dnfile` (`pip install dnfile`). Run with `python -I`.
- Managed assemblies with real IL, as plain PE files. Extract them from the FW 4.03
  dump with `tools/sprx_pe.py`:

  ```
  python -I tools\sprx_pe.py <dump root>\system_ex\common_ex\lib work\il403
  python -I tools\sprx_pe.py <dump root>\system_ex\app\NPXS40087\psm\Application work\il403
  ```

- The decrypted native images (`system/`, `system_ex/`) of the same dump.
- For `attribute_icalls.py`: upstream Mono's icall table, saved in the output directory:

  ```
  curl -o work\boundary\icall-def-2019-10.h https://raw.githubusercontent.com/mono/mono/2019-10/mono/metadata/icall-def.h
  ```

## Configuration

Locations come from environment variables (see `bconfig.py`):

| variable | default | meaning |
|---|---|---|
| `SPIKE_ROOT` | `work\il403` | managed IL set: flat dir of `*.dll`/`*.exe`, or a dir with `common\` and `Application\` |
| `SPIKE_TAG` | `fw403` | label in output and cache file names |
| `SPIKE_OUT` | `work\boundary` | output directory (json, txt, model cache) |
| `SPIKE_KEK` | (required) | decrypted dump root; natives are taken from `system\common\lib`, `system\priv\lib`, `system_ex\common_ex\lib` and `system_ex\app\NPXS40087\eboot.bin` |

A second firmware (for example the stripped FW 7.00 set) is analysed by pointing
`SPIKE_ROOT` at it and setting another `SPIKE_TAG`; nothing requires it.

## Scripts

Library modules:

- `bconfig.py`: the locations above.
- `asmmodel.py`: loads every assembly under `SPIKE_ROOT` with dnfile into a plain
  Python model (types, methods, flags, member/type refs, ImplMap, exported types) and
  decodes IL bodies into token-bearing ops. Cached in `model_<tag>.pkl`; running
  it directly rebuilds the cache.
- `nid.py`: PS4/PS5 symbol NID of a name (`python -I nid.py sceKernelLoadStartModule`).
- `ps5elf.py`: minimal PS5 ELF dynamic symbol reader. `read_syms(path)` returns
  `(exports, imports)` as NID sets; `read_syms.last` holds module/library tables
  and imports grouped by `module/library`. Run directly to print counts per file.

Pipeline (run in this order; each step reads the previous step's output from `SPIKE_OUT`):

1. `inventory.py [libScePsm.sprx libmonosgen-2.0.sprx]`: all icall and P/Invoke
   declarations, checked against the icall name strings in the two natives.
   Writes `inventory_<tag>.json/.txt`.
2. `attribute_icalls.py [dirs/files...]`: assigns each unique icall to the native
   binary that contains its registration string, or to the Mono runtime when listed
   in `icall-def-2019-10.h`. Default inputs: `common_ex\lib` and the ShellUI `eboot.bin`.
   Writes `icalls_attributed_<tag>.json`.
3. `attribute_pinvokes.py [dirs/files...]`: assigns each unique (lib, entry) P/Invoke
   to an exporting system library (by NID) or to a plain-name string in `eboot.bin`.
   Default inputs: `eboot.bin` plus the three system library directories.
   Writes `pinvokes_attributed_<tag>.json`.
4. `callgraph.py <scenario>`: RTA-style static call graph over the IL from chosen
   roots, listing reachable icalls/P/Invokes with their providers. Scenarios:
   `full`, `main_only`, `boot0`, `boot012` (default), `boot_all` (app.exe `Main`
   plus BootRegistry run items by timing), `pui_core` (PUI framework only:
   `Application`/`PlatformApplication` create and run), `shell_boot` (`Main` plus the
   three BootRegistry assemblies). String literals naming an `<ns>.AssemblyEntry`
   seed its `Start` method. Writes `reach_<tag>_<scenario>.json`.
5. `report_reach.py <tag> <scenario> [-v]`: text summary of a reach file, icalls by
   provider/namespace and P/Invokes by library; `-v` lists every non-Mono icall.

Standalone checks:

- `aot_boundary.py [<root>|-] [img1,img2,...]`: native-level boundary. Collects the
  NID imports of the ShellUI image set (eboot + `common_ex\lib\*.sprx` +
  `psm\Application\*.sprx`, or `<root>\native\eboot.bin`, `<root>\common`,
  `<root>\Application` for a staged layout), drops what the set exports itself and
  groups the rest by providing module/library. The optional list restricts the set
  (output gets a `_core` suffix). Writes `aot_boundary_<name>[_core].json`.
- `ilcheck.py <dir>...`: per directory, whether managed images (plain or inside
  `.sprx`) carry real IL bodies (distinct body RVAs) or only stubs.
- `stubstats.py`: the same question for the loaded `SPIKE_ROOT` set, with an IL
  size histogram.
- `compare_sources.py [assembly names...]`: compares key assemblies across sources
  (version, method count, distinct bodies, coverage of the FW 7.00 method set, MVID).
  Sources: `SPIKE_FW700_ROOT` (optional reference set), `SPIKE_ROOT`,
  `SPIKE_EXTRACTED` (optional), and the dump's `common_ex\lib`.
- `cmp_internalthread.py [<ps5 mscorlib> <desktop mscorlib>]`: field name and
  signature diff of `System.Threading.InternalThread`/`Thread` between the PS5
  mscorlib and a desktop Mono 6.8 mscorlib (`SPIKE_DESKTOP_MSCORLIB`).
- `summarize_run.py <spikelog.txt>`: classifies the boundary entry points hit in a
  runtime trace (tab-separated lines whose 4th field is `icall <key>` or
  `pinvoke <name> [<lib>!<entry>]`; `COUNT` lines are skipped)
  with the static attribution files of `SPIKE_TAG`.

## Example (FW 4.03)

```
python -I asmmodel.py
python -I inventory.py
python -I attribute_icalls.py
python -I attribute_pinvokes.py
python -I callgraph.py pui_core
python -I report_reach.py fw403 pui_core -v
python -I aot_boundary.py
```

Expected on the 4.03 set: 183 assemblies / 200178 methods; 2149 unique icalls
(1061 eboot.bin, 566 Mono runtime, 384 libScePsm, 94 libScePsmUtil, 21 libSceNKWeb,
4 libmonosgen, 19 unresolved); 3057 unique P/Invokes; `pui_core` reaches 33111
methods, 518 icalls and 168 P/Invokes.
