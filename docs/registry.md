# System registry (libSceRegMgr)

The PS5 keeps its system settings in a key/value registry: language, initial-setup state,
power options, NP environment, per-user settings and so on. The ShellUI reads and writes it
all the time. This document describes how the console does it and how the launcher
reproduces it in `psmhost/libSceRegMgr/`.

Sources: `libSceRegMgr.sprx` and the ShellUI `eboot.bin` (FW 4.03), the managed
`Sce.Vsh.Registry.dll` (FW 4.03), and the decrypted 7.00 manufacturing kernel
(`700_t_manu_kernel.elf`, from the "Prototype Globe" devkit dump). No 4.03 kernel is
available, so kernel behaviour is quoted from 7.00. Kernel addresses below are from that
image; IDA databases are in `work/ida_reg/` (`regmgr.elf.i64`, `k700/kernel700.elf.i64`).

## How it works on the console

```
ShellUI (managed)          Sce.Vsh.Registry.GetInt(key, out v)
  -> eboot icall           Sce.Vsh.Registry::GetIntNative   (eboot 0x4c5760)
  -> libSceRegMgr          sceRegMgrGetInt(key, &v)         (NID mPYKD12UDQI, 0x270)
  -> system call           regmgr_call(#532)(cmd 2, key, &status, &v, 4)
  -> kernel                regmgr_call handler 0xffffffff80ca9c90 -> GetInt 0xffffffff80ca2070
```

`libSceRegMgr.sprx` holds no data and makes no checks of its own. Each of its 72 exports
packs the arguments into `regmgr_call` (libkernel import `7NwggrWJ5cA`) and returns the
status the kernel wrote; only when the system call itself fails does it return
`sceKernelError(errno)`. The store, the entry definitions, the defaults and every check
are in the kernel. The kernel keeps the values in memory and writes them to its files on
the system storage (`system.reg` / `system.nvs` and friends).

### Commands used by the shell

| export (NID) | lib address | regmgr_call | kernel routine |
|---|---|---|---|
| sceRegMgrGetVersion (q_lPgqZniW4) | 0xd0 | 0, arg 1 | 0xffffffff80ca3270 |
| sceRegMgrGetVersionForPS4 (7yIpFl9KknY) | 0x130 | 0, arg 2 | 0xffffffff80ca3270 |
| sceRegMgrSetInt (khaYelw1Ytc) | 0x200 | 1 | 0xffffffff80ca1f20 |
| sceRegMgrGetInt (mPYKD12UDQI) | 0x270 | 2 | 0xffffffff80ca2070 |
| sceRegMgrSetStr (ffInidSqRss) | 0x2e0 | 3 | 0xffffffff80ca2190 |
| sceRegMgrGetStr (CTplLrrndUg) | 0x340 | 4 | 0xffffffff80ca22e0 |
| sceRegMgrSetBin (eXxrXkrLXa4) | 0x3a0 | 5 | 0xffffffff80ca2540 |
| sceRegMgrGetBin (NqxMleeTiLs) | 0x400 | 6 | 0xffffffff80ca2650 |
| sceRegMgrGetInt/Str/BinInitVal (i64ig3BF6f8, jqyhb1oMgHw, dhuH8HjNhUY) | 0x4d0.. | 7/8/9 | — |
| sceRegMgrSetInitLevel (YRPHqS8TcnI) | 0x600 | 10 | 0xffffffff80ca2a90 |
| sceRegMgrResetVal (eL7sMZaIdac) | 0x670 | 11 | 0xffffffff80ca2cb0 |
| sceRegMgrCheckError (LZBANaFbPqM) | 0x6e0 | 12 | 0xffffffff80ca3590 |
| sceRegMgrIsInitOK (rebo0q4yREE) | 0x1090 | 17 | 0xffffffff80ca3620 |
| sceRegMgrSrvGetRegion (sywg_RnhZMA) | 0x910 | 18 | 0xffffffff8080bd70 |
| sceRegMgrSrvGetRegionStr / CnvRegionStr / CnvRegionInt | 0x970/0x9d0/0xa30 | 19/20/21 | 0xffffffff8080bde0.. |
| sceRegMgrSrvGetMachineType (_04zv_0zNJkg) | 0xa90 | 22 | 0xffffffff8080bfc0 |
| sceRegMgrSrvGetQAFforReg (wQlp94zzrWg) | 0xaf0 | 23 | 0xffffffff8080c060 |
| sceRegMgrGetInitError (nfAfS7f2gR0) | 0x10f0 | 33 | 0xffffffff80ca36e0 |
| sceRegMgrSrvGetRealMachineType (x0oTVOySvTU) | 0xb50 | 36 | — |
| sceRegMgrStrncpy / Memset / Setbin | 0x21b0/0x21d0/0x21f0 | none (local helpers) | — |

Managed entry points:

* `Sce.Vsh.Registry` internal calls, registered by the eboot: `SetIntNative` 0x4c5750,
  `GetIntNative` 0x4c5760, `SetStrNative` 0x4c5770, `GetStrNative` 0x4c5880,
  `SetBinNative` 0x4c5900, `GetBinNative` 0x4c5930, `ResetValNative` 0x4c5960,
  `CheckErrorNative` 0x4c5970, `GetRegionNative` 0x4c5980, `GetQAFforRegNative` 0x4c59a0,
  `GetMachineTypeNative` 0x4c59c0. Set/GetInt, ResetVal, CheckError and GetMachineType
  are plain thunks; the string, binary, region and QAF wrappers are described in
  `registry_icalls.cpp`.
* `Sce.Vsh.RegistryInternal` P/Invokes into `libSceRegistry` (the same exports):
  SetInitLevel, GetVersion, GetVersionForPS4, CheckError, IsInitOK, GetInitError, Strncpy,
  Memset, Setbin, SrvGetRegion, SrvGetRegionStr, SrvCnvRegionStr, SrvCnvRegionInt,
  SrvGetMachineType, SrvGetQAFforReg, SrvGetRealMachineType.

The shell's helper `UT.Registry.GetInt(key, fallback)` returns the fallback only when the
call fails; with a working registry the stored value is what counts.

## Entry definitions

Each entry has a 32-bit key, a type (0 integer, 1 string, 2 binary), a size (4 for
integers; strings include the terminating NUL; at most 2048) and an initial value. Keys
are grouped by category in the top bits (`0x02......` SYSTEM, `0x19......` NP, ...).
Some entries are arrays: one definition for N consecutive slots, e.g. the 16 users
(`USER_01_16_*`, slot n at `base + (n - 1) * 0x10000`).

### In Sce.Vsh.Registry.dll (4.03)

The managed assembly carries the SDK header `sce_regmgr_entry.h` as constants of class
`Sce.Vsh.Registry`:

| member | meaning |
|---|---|
| `SCE_REGMGR_ENT_KEY_<name>` (const) | key |
| `SCE_REGMGR_ENT_KEY_<name>(uint a)` (method) | array entry: `SCE_REGMGR_ENT_NUM(a, count, stride, base, invalid)` = `base + (a - 1) * stride` for `a` in 1..count, else `invalid` |
| `SCE_REGMGR_ENT_TYP_<name>` | type |
| `SCE_REGMGR_ENT_SIZ_<name>` | size |
| `SCE_REGMGR_ENT_INI_<name>` | initial value: an integer, or the string `"sceRegMgrRiniI<key>()"` for values the kernel picks per region |
| `SCE_REGMGR_ENT_CAT_<name>`, `_MIN_`, `_MAX_` | category keys and array bounds |
| `SCE_REGMGR_ERROR_*` | status codes |

4.03 defines 1185 entries (787 single, 398 arrays) = **7224 distinct keys** (two names,
`VIDEOOUT_enable_cec` and `VIDEOOUT_enable_one_touchplay`, share `0x0A080000`). INI
values: 675 integers, 24 region-dependent; the remaining 218 integers and every string and
binary entry have none in the header.

### In the kernel: registry.db0

The kernel builds its entry table at boot (0xffffffff80ca3b50) from a text file named
`registry.db0` that is part of the kernel image (7.00: 111056 bytes at
0xffffffff816D9F50, XORed with the 16-byte key at 0xffffffff81308C50; the name is resolved
in 0xffffffff8077e0b0). `work/ida_reg/extract_db0.py` decodes it from the 7.00 kernel.

```
[VERSION=7340115          registry version (GetVersion)
[VER_PS4=17104898         PS4 registry version (GetVersionForPS4)
[REG-BAS                  base definitions (1320 in 7.00)
<key>=<f0>:<f1>:...:<f19>:<value>:
...
[REG-TOL / [REG-DEX       test-kit / development-kit overrides
[REG-J1 / [REG-UC2 / ...  per-region overrides (KRT J1 UC2 AU3 CEK CEL KR2 E12 TW1 RU3 MX2 BR2 CN9)
```

Line parser 0xffffffff80cab660 (numbers by 0xffffffff80cab4e0: decimal with optional `-`,
or `0x` hex). Fields used here: 3 type, 4 size, 18 array count (bit 7 selects a 16-bit
instead of 8-bit key stride), 19 how the value is initialised, 20.. the value (to the end
of the line, final `:` removed):

* 19 = 1: the value is literal — a number, a string, or `b'<hex>` for binaries;
* 19 = 0: no initial value (the entry starts zeroed / empty);
* 19 = 0x80|n: the value is computed by a kernel routine (nickname, IDU/arcade modes,
  parental flags, ...).

The BAS section always applies; the console's region section and, on kits, DEX/TOL
override single entries (0xffffffff80ca3b50, `[REG-` handling).

The managed INI values and the 7.00 BAS literals agree everywhere except entries that
record the firmware's own version (`REGISTRY_version`, `REGISTRY_install`, ...). The 7.00
file also defines 138 keys 4.03 does not know and gives the user arrays 20 slots instead of
16.

## Checks and status codes

From the kernel lookup 0xffffffff80ca83d0 and the Get/Set routines:

| situation | status |
|---|---|
| registry not initialised | `0x800D0201` SCE_REGMGR_ERROR_UNINITIALIZED |
| null value pointer (Get*, SetStr, SetBin) | `0x800D0202` SCE_REGMGR_ERROR_PARAM |
| unknown key | `0x800D0203` SCE_REGMGR_ERROR_PRM_REGID |
| entry has another type | `0x800D0207` SCE_REGMGR_ERROR_PRM_TYPE |
| integer/binary size != entry size; GetStr size < entry size; SetStr size > entry size, or the string (with NUL) longer than size or the entry | `0x800D0208` SCE_REGMGR_ERROR_PRM_SIZE |
| copying from/to the caller failed | `0x800D020F` COPYIN / `0x800D0210` COPYOUT |

GetStr copies at most the entry size; a value that fills it is cut to size-1 characters
plus the NUL. SetStr stores `strnlen(str, size) + 1` bytes. `sceRegMgrCheckError` sorts a
status into SCE_REGMGR_CHKERR_BROKEN (1), _SYSTEM (2) or _MISTAKE (3). `sceRegMgrIsInitOK`
is 0 on a normal start, negative (`-1` recover, `-2` initialised, `-3` safe mode) after
storage problems — the shell then shows its registry error dialog. The same lookup also
enforces per-process access rights (`0x800D0119`) that matter only for games.

## Values the ShellUI boot reads

Fresh registry = the defaults. With the 4.03 definitions and the 7.00 `registry.db0`:

| key | name | default | source |
|---|---|---|---|
| 0x02040000 | SYSTEM_initialize | 0 | INI. 0 means initial setup has not been done: the shell sets `IsInitialSetupRequired` (Legacy `ResetInitialSetupPhase`) |
| 0x02200000 (35651584) | SYSTEM_initialize_phase | 0 | INI |
| 0x02820E00 (42077696) | SYSTEM_POWER_shutdown_status | 0 | INI |
| 0x02020000 | SYSTEM_language | 1 (en-US) | region-dependent INI; 7.00 BAS literal 1 (J1: 0, KR2: 9, CN9: 11, E12/AU3/CEK/...: 18, BR2: 17, MX2: 20) |
| 0x19800000 | NP_env (string, 17) | "np" | db0 BAS literal ("sp-int" on DEX/TOL kits) |
| 0x020B0000 | SYSTEM_button_assign | 1 | region-dependent INI; db0 BAS 1 |
| 0x02820200/0300/0800 | SYSTEM_POWER_signin / remote / suspend_to_ram | 1 / 1 / 1 | region-dependent INI; db0 BAS |
| 0x14190B00 / 0x14190600 | NET_COMMON_current_profile / net_flag | 1 / 1 | INI |
| 0x027C0100 | SYSTEM_PLATFORMPRIVACY_eu_ws1 | -1 | db0 BAS |
| 0x02050000 | SYSTEM_nickname (string, 65) | not traced | computed by the kernel (db0 0x80) |
| 0x02860100 / 0x02860300 | SYSTEM_SPECIFIC_idu_mode / arcade_mode | not traced | computed by the kernel (db0 0x83 / 0x88) |

`sceRegMgrGetVersion` = 0x00400028 and `GetVersionForPS4` = 0x00900001: the kernel answers
with the db0 header versions, which in 7.00 equal the defaults of `REGISTRY_version`
(0x01100000) and `REGISTRY_version_ps4` (0x01010000); the 4.03 INI values of those entries
are used.

## The host implementation

`psmhost/libSceRegMgr/regmgr.{h,cpp}` is the kernel side; `registry_icalls.cpp` registers
the eboot icalls and the `libSceRegistry` P/Invokes on top of it.

* **Definitions** are read at run time from the user's firmware: on first use the store
  loads `Sce.Vsh.Registry` through Mono, takes the literal fields' constant blobs
  (`mono_field_get_data`) and decodes the array-key methods' IL. Nothing is compiled in.
* **Defaults**, per key: the 4.03 integer INI; otherwise the `[REG-BAS]` entry of a
  `registry.db0` given with `ssn::regmgr::set_db0_path()` (literal value, or zero for "no
  initial value"); otherwise zero, reported once through `host::note_unimplemented` when
  the shell reads the key. Without a db0, 2598 keys are in that last group (all strings
  and binaries); with the 7.00 db0, 117 (computed values and the keys 7.00 lacks).
  The launcher must not ship `registry.db0`: it is the user's to extract.
* **Persistence**: values the shell writes are saved, one line per key
  (`<key hex> <type> <value hex> <name>`), to the file given with
  `ssn::regmgr::set_store_path()` (meant for the launcher cache directory) and loaded on
  the next start, so a completed initial setup or a changed setting survives restarts like
  the console's `system.reg`. Deleting the file is a factory reset.
* **Checks and status codes** follow the kernel (table above). Region and kit sections of
  db0 are not applied: the host has neither.

C++ API (all return the kernel status codes): `get_int/set_int`, `get_str/set_str`,
`get_bin/set_bin`, `reset_val`, `check_error`, `get_version`, `get_version_for_ps4`,
`is_init_ok`, `get_init_error`, `describe`. Before Mono is running (or if the assembly is
missing) every call returns `0x800D0201`, the kernel's answer before initialisation.

### Not implemented

* Region, machine type, QAF (`SrvGetRegion`, `SrvGetMachineType`, `SrvGetRealMachineType`,
  `SrvGetQAFforReg`, the region-string conversions): console identity the host does not
  have. They report through `note_unimplemented` and return 0 (the eboot already maps
  unknown regions to 0 and any QAF other than 1 to off; machine type 0 is CEX).
* Defaults computed by the kernel (db0 init method 0x80|n) and region-dependent ones
  without a db0: zero, reported.
* `sceRegMgrSetInitLevel`: returns 0 like the kernel; what the level changes is not traced.
* `sceRegMgrResetVal`: restores the default (key 0 = all keys); the init-level filter and
  its status codes are not traced.
* Per-process access rights, change notifications/event flags, backup/restore, the
  `NonSys*`/`Cntl*`/`Drv*`/`Tool*`/log commands: not used by the shell, not provided.
