# libScePsm embedded shaders

How the PS5 ShellUI's `libScePsm.sprx` (FW 4.03) stores, loads, reflects and feeds its 72 built-in
shaders, as needed to reimplement them on PC. Every statement below is backed by the decompiled
library (addresses in `work/ida/libScePsm_dyn.elf`), the blobs themselves, or the managed
`Sce.PlayStation.Core.dll`. Where only the data was observed (the library never interprets it),
the text says *inferred*. Where something is still unknown it says so.

Tools and outputs:

| file | what |
|---|---|
| `work/shaders/NN_<name>.ags.bin` | the 72 blobs, extracted from the table at `0x4654E0` |
| `tools/shaders/ags_info.py` | parser for the container, header and reflection (this document's format, in code) |
| `work/shaders/reflection.txt` | `ags_info.py` over all 72 blobs (`--tree`) plus the 70 EmbeddedShader programs |
| `launcher/shaders/psm/*.vert/.frag` | GLSL 4.50 transcriptions (list in section 9.1) |

Regenerate the dump with

```
python -I tools/shaders/ags_info.py work/shaders --tree --psm-elf work/ida_shader/libScePsm_dyn.elf \
       --core-cs work/decomp/Sce.PlayStation.Core.cs > work/shaders/reflection.txt
```

Disassemble a blob with
`deps/dl/rga/x/utils/lc/opencl/bin/llvm-objdump.exe -d --triple=amdgcn-amd-amdhsa --mcpu=gfx1013 <blob>`
(`gfx1013` decodes the code cleanly; `.shader_text` ends at `s_endpgm`, followed by `s_code_end` padding
and a trailing data block that the disassembler shows as garbage).

---

## 1. From EmbeddedShader to blobs

`ShaderManager.LoadShaderNative(EmbeddedShader id, out handle)` is the icall at `0x2D4C90`:

* `id >= 0x46` fails with `"EmbeddedShader %u is out of range"`. The first call prints
  `"[PSM] InitializeShaderAssets rev=0"` (`sub_2EEF0`).
* `(vsId, psId) = dword_461ED0[2*id], dword_461ED0[2*id+1]`.
* `sub_2EF10(shaderId)` (`shaderId <= 0x47`) returns `{name, data, size}` from the 24-byte entries of
  `off_4654E0`; the extracted file prefix `NN_` is this index.
* A 224-byte program object is allocated and built by `sub_1FAF80(prog, vsData, vsSize, psData, psSize)`;
  on success it is registered in the program handle table with a fresh handle.

The full id -> (vs, ps) table with names is at the end of `reflection.txt`. The ones the UI
draws with most: `SolidFill 0 = (basic_vv, solid_fill_p)`, `SolidFillSimple 1 = (solid_fill_simple_vv,
solid_fill_simple_p)`, `Texture 2 = (basic_vv, rgba_p)`, `TextureMask 7 = (basic_vv, mask_p)`,
`A8Texture 11 = (basic_vv, a8_p)`.

## 2. Container: AMDGPU ELF ("AGS")

Each blob is an `ELFCLASS64` little-endian `ET_EXEC` with `e_machine = 0xE0` (AMDGPU) and five
sections:

| section | content | read by libScePsm |
|---|---|---|
| `.shader_text` | RDNA (gfx10) machine code, 256-byte aligned | `sub_D1E0` copies it to GPU memory (`sub_1D10`, align 256) |
| `.shader_header` | AGC `Shader` struct (below) | `sub_D1E0` copies it and calls `sceAgcCreateShader` (NID `f3dg2CSgRKY`, through `sub_15D0B0`) |
| `.metadata` | `sl00` reflection tree (section 4) | `sub_D010`/`sub_D500` (hash), `sub_1F82A0` (uniforms) |
| `.cfg` | another `sl00` tree (control-flow / source info, contains e.g. `"float4 main(VS_OUTPUT)"`) | never (no `".cfg"` string in the library) |
| `.shstrtab` | section names | |

Sections are found by name with `strcmp` over the section header table (`sub_D1E0`, `sub_D010`).
`sub_1F82A0` also accepts a second container whose first dwords are the magic values
`776161112 / 808335669 / 1163080240` (an archive holding both stages, searched for a `0x50534F`
entry); none of the 72 embedded blobs uses it, so it is not documented further here.

## 3. `.shader_header`: the AGC `Shader` struct

`sceAgcCreateShader` (NID `f3dg2CSgRKY`, `libSceAgc.sprx` `0xC440`) turns this blob into the runtime
`Shader*` (stored at stage object `+96`). The layout below is what the firmware itself reads: the
`libSceAgc.sprx` of the dump (`system/common/lib`, converted with `tools/ida/ps5_to_elf.py`,
database `work/ida_agc/libSceAgc.elf.i64`; addresses below prefixed `agc`) and the libScePsm
readers. Every function of `libSceAgc` was decompiled and searched for
accesses to these offsets; a field marked *not read* is not read by any of that code nor by the
libScePsm readers listed here, so only its value in the blobs is known.

`agc 0xC440` checks the magic and version, requires the code pointer to be 0, then makes the pointer
fields absolute: each is **self-relative** (field address + stored value; 0 = null), and so are the
five pointers at the start of the user-data block. It then stores the code address at `+0x10` and
patches it into the program-address register pair of the stage, which it finds in the SH register
list by the stage type at `+0x5A`.

| off | type | field | firmware reader |
|---|---|---|---|
| 0x00 | u32 | magic `0x34333231` ("1234") | `agc 0xC440` (else error `0x8A6C0003`) |
| 0x04 | u32 | version `0x18` | `agc 0xC440` (else `0x8A6C0004`) |
| 0x08 | ptr | user data (below) | `agc 0xC440` relocates it; libScePsm `sub_C930`, `sub_156260`, `sub_1F82A0` |
| 0x10 | ptr | code address, 0 in the blob | `agc 0xC440` requires 0 (else `0x8A6C001F`) and writes the code address |
| 0x18 | ptr | context registers `{u32 offset, u32 value}[]` | libScePsm hashes them (`sub_D010`/`sub_D500`/`sub_CB40`: FNV-1a over bytes 2,1,0 of offset and value, stored at stage `+104`), compares them (`sub_CB00`) and emits them (`sub_155190` -> `sub_1563F0`) |
| 0x20 | ptr | SH registers `{u32 offset, u32 value}[]` | `agc 0xC440` patches the code address into the stage's program-address pair; libScePsm emits them (`sub_155190` -> `sub_156540`) |
| 0x28 | ptr | "specials" block | `agc 0xD560` (`MqAdbRMdNz4`), `0xE410` (`D9sr1xGUriE`), `0xC840` (`nApJjpKNBl4`), `0xCE90` read the qwords at `+0`, `+8`, `+32`, `+40` and the dword at `+12` of the pointed block |
| 0x30 | ptr | input semantics `u32[]` | `agc 0xD560`, `0xD9B0` (`pdEV7bI6COI`), `0xDC90`, `0xDEF0` (`HV4j_E0MBHE`), `0xE1C0` (pixel-shader inputs, see below) |
| 0x38 | ptr | output semantics `u32[]` | the same functions (outputs of the stage that feeds the pixel shader) |
| 0x40 | u32 | header size (bytes) | libScePsm `sub_CB40` copies that many header bytes |
| 0x44 | u32 | code size (bytes) | libScePsm `sub_CB40` copies that many code bytes |
| 0x48 | u32 | 0 in 71 blobs, 7 in one | *not read* |
| 0x4C | i32 | 6 in all blobs | `agc 0xC440` reads only its sign: a negative value is accepted only when the imported predicate `HMnVBVUyajk` returns true (else `0x8A6C0042`) |
| 0x50 | u32 | input semantic count | `agc 0xD560`/`0xD9B0`/`0xDC90`/`0xDEF0`/`0xE1C0`; libScePsm uses it as the vertex-buffer table length (`sub_B010`, `shader+80`) |
| 0x54 | u16 | 0 in all blobs | *not read* |
| 0x56 | u16 | output semantic count | the `agc` functions above (`shader+86`) |
| 0x58 | u16 | 48 in all blobs | *not read* (48 bytes is what the specials readers above cover) |
| 0x5A | u8 | stage type | `agc 0xC440` accepts 0, 1, 2, 3, 6, 7 (each with its own program-address register); `agc 0xC7C0`/`0xC800`/`0xC840`/`0xCE90` accept only the pairs 4+6 and 5+7; libScePsm `sub_1562F0` / `sub_15D0E0` / `sub_15D120` require 2 for the vertex stage, 1 for the pixel stage, 3 for the optional stage before 2. The blobs use 1 (pixel) and 2 (the vertex stage, built as a merged/NGG stage in the ISA) |
| 0x5B | u8 | context register count | libScePsm `sub_D010`, `sub_D500`, `sub_CB00`, `sub_CB40`, `sub_155190` |
| 0x5C | u8 | SH register count | `agc 0xC440`; libScePsm `sub_155190` |

User data (at `+0x08`):

| off | field | firmware reader |
|---|---|---|
| 0x00 | `u16 *direct_resource_offset`, indexed by direct-resource number; `0xFFFF` = unused | libScePsm `sub_156260(shader, n)`: returns the user-data dword range of resource `n`, 1 dword long for `n` in {0, 6, 7, 9}, `*(u16*)(ud+0x2A)` dwords for `n` = 1, 2 dwords otherwise |
| 0x08..0x20 | `u16 *sharp_resource_offset[4]` | libScePsm `sub_C930` (`+8/+16/+24/+32`) |
| 0x28 | u16, 0 in all blobs | *not read* |
| 0x2A | u16, 0 in all blobs | libScePsm `sub_156260`: dword size of direct resource 1 |
| 0x2C | u16, 11 in all blobs | *not read*; the tool uses it as the length of the direct-resource list |
| 0x2E..0x34 | `u16 sharp_resource_count[4]` | libScePsm `sub_C930` (`+46/+48/+50/+52`); `sub_1F82A0` gates the metadata walk on `+52` |

Each sharp entry is `offset_dw:15 | flag:1`: the user-data dword where the descriptor goes, and bit 15
set for 4-dword descriptors (V#, S#) / clear for 8-dword T#s. `sub_C930` compacts the four lists into
the stage object as `{u16 dword, u8 slot, u8 isTexture}` where `slot` is the list index and
`isTexture = (bit15 == 0)`:

| class | stage object | count | meaning (from `BindShaderResources`, `sub_B010`) |
|---|---|---|---|
| 0 | `+144` | `+124` | **t** registers: texture/buffer SRVs. Source `cmd+520+64*stage+8*slot` (PS: `SetTexturePs`, `cmd+584`). Copies `*(u8*)(obj+120)` bytes of the descriptor at `obj+88`; an unbound texture slot falls back to the default texture, an unbound buffer is a fatal assert |
| 1 | `+176` | `+128` | **u** registers (RW). Source `cmd+712+32*stage+8*slot` |
| 2 | `+224` | `+136` | **samplers**. 16 bytes from `cmd+0x430+128*stage+16*slot` (`SetSamplerDirect`) |
| 3 | `+192` | `+132` | **constant buffers**. 16-byte V# at `buffer+88` from `cmd+872+64*stage+8*slot` (`SetConstantBufferPs` = `cmd+936`, `SetConstantBufferVs` = `cmd+1000`) |

`stage` is 1 for PS, 2 for VS (the debug strings in `sub_B010` print `PsShader`/`VsShader`). User-data
dwords `< 32` go to the user-SGPR block, the rest to the extended user data at `dword-32`.

Where user-data dword 0 lands is fixed by the hardware stage, not by the header: in the ISA, PS
resources start at `s0`, the vertex stage (binary type 2, a merged/NGG stage) at `s8`. For the
five transcribed shaders:

| shader | resource | metadata slot | user-data dword | SGPRs |
|---|---|---|---|---|
| basic_vv | `__GLOBAL_CB__` | b0 | 0 | `s[8:11]` |
| basic_vv | vertex-buffer table pointer (direct resource 8) | — | 4 | `s[12:13]` |
| solid_fill_p | `__GLOBAL_CB__` / `globalConstantBuffer` | b0 / b7 | 0 / 4 | `s[0:3]` / `s[4:7]` |
| rgba_p, a8_p | `s_Texture` / `Sampler0` | t0 / s0 | 0 / 8 | `s[0:7]` / `s[8:11]` |
| rgba_p, a8_p | `__GLOBAL_CB__` / `globalConstantBuffer` | b0 / b7 | 12 / 16 | `s[12:15]` / `s[16:19]` |
| mask_p | `s_Texture`, `s_MaskTexture` | t0, t1 | 0, 8 | `s[0:7]`, `s[8:15]` |
| mask_p | `Sampler0`, `Sampler1` | s0, s1 | 16, 20 | `s[16:19]`, `s[20:23]` |
| mask_p | `__GLOBAL_CB__` / `globalConstantBuffer` | b0 / b7 | 24 / 28 | `s[24:27]` / `s[28:31]` |

The direct-resource index 8 is the vertex-buffer descriptor table: `sub_B010` asks `sub_156260(shader, 8)`
for its user-data dwords, and the ISA of `basic_vv` loads the pointer from exactly those SGPRs. The other
direct indices are unused by the 72 blobs; the firmware code examined gives no names for them.

Semantic entries are 32-bit words. The only firmware code that interprets them is the pixel-shader
input mapping in `libSceAgc` (`agc 0xD9B0` `pdEV7bI6COI`, and the same loop in `0xD560`, `0xDC90`,
`0xDEF0`, `0xE1C0`): for each pixel-shader input `i` it looks for the output of the feeding stage with
the same low byte and builds one 32-bit control value per input (high dword of the 8-byte entry it
writes; the low dword is `i` + a base):

| bits | read from | effect in `agc 0xD9B0` |
|---|---|---|
| 0–7 | input and output | match key (`semantic`); for `basic_vv` vertex inputs it equals the attribute index |
| 8–12 | output | copied to control bits 0–4 when the input is matched (the output's parameter slot) |
| 20–21 | input (and the AND of input and matched output) | when non-zero the input takes the 16-bit path: input bit 20 -> control bit 24, bit 21 -> bit 25, bit 19 set; matched: bit 5 set when the AND's bit 20 is clear, bit 20 set when the AND's bit 21 is clear; unmatched: bits 5 and 20 set |
| 22, 24 | input | either one set -> control bit 10 when matched; on the 32-bit path also bit 5 (which an unmatched input always gets) |
| 28–29 | input | -> control bits 8–9 |
| 30–31 | input | -> control bits 21–22 (16-bit inputs only) |

Bits 13–19, 23 and 25–27 are *not read* by that code (nor by anything else examined); input semantics
of the vertex stage are not interpreted at all (libScePsm only uses their count). In the blobs bits
16–19 hold the element count (4/4/2 for POSITION/COLOR/TEXCOORD of `basic_vv`, cross-checked against
the metadata and the vertex-fetch code in the ISA), and the second byte of the vertex inputs is 9, 13,
17 (`basic_vv`); neither is used by the firmware.

## 4. `.metadata`: the `sl00` reflection tree

### 4.1 Encoding

```
+0  char[4] "sl00"
+4  u32     root node offset
+8  u32     total size
+12 u32     0
+16 ...     schemas and nodes, addressed by offset from the start of the section
```

All integers are LEB128. A **node** is

```
uleb   schema offset
uleb   next sibling     (only if schema flags bit 0)
uleb   first child      (only if schema flags bit 1)
...    one payload per field descriptor of the schema, in schema order
```

and a **schema** is

```
uleb   (type_id << 2) | flags
{u8 lo, u8 hi}*   field descriptor: tag = lo | (hi & 0x0F) << 8, kind = hi >> 4
                  the list ends at a descriptor whose first byte is 0
```

Payload by kind (the skip rule is the same in `sub_1F82A0`, `sub_1F9E30`, `sub_1FAA30`, `sub_CD20`):

| kind | payload | meaning |
|---|---|---|
| 1 | uleb | unsigned (`sub_1FAA30`) |
| 2 | sleb (sign from bit 6 of the last byte) | signed (`sub_1FAA30`) |
| 3 | uleb | offset of a NUL-terminated string (`sub_1F82A0`, field 0x1E) |
| 4 | uleb | offset of another node (`sub_1F9E30`, field 0x24) |
| 5 | none | boolean true (`sub_1FAA30` returns 1) |
| 6 | uleb, uleb | pair |
| 7 | 4 raw bytes | |

Fields absent from a node's schema are absent (0/false). Children and siblings form singly linked
lists; `sub_D8A0(md, node, t)` returns the first child of `node` with `type_id == t`.

### 4.2 Node types and fields

| type | name | evidence |
|---|---|---|
| 0x01 | root | header `+4`; field 0x06 = 0x10000 in all blobs (meaning unknown) |
| 0x02 | shader | `sub_D8A0(root, 2)` |
| 0x03 | resource list | `sub_D8A0(shader, 3)`; error string `"[PSM] Failed sceShaderGetResourceList(md)"` |
| 0x04 | stage input | inferred: names `input.*` |
| 0x05 | stage output / return value | inferred: names `main*` |
| 0x07 | constant buffer | inferred: slot matches sharp class 3 |
| 0x08 | sampler | inferred: slot matches sharp class 2 |
| 0x1A | texture | inferred: slot matches sharp class 0 |
| 0x1B | type list | inferred: holds every type node |
| 0x13 | scalar/vector type | inferred |
| 0x14 | matrix type | inferred |
| 0x15 | struct | `sub_1F9E30` collects it (when it has a name) |
| 0x17 | texture type | `sub_1F9E30` follows its 0x24 |
| 0x18 | constant-buffer type | `sub_1F9E30` follows its 0x24 (to the struct) |
| 0x19 | struct member | `sub_1F82A0` reads its 0x1E / 0x21 / 0x25 |
| 0x11, 0x12 | (type wrappers) | `sub_1F9E30` follows their 0x24; no instance in the 72 blobs |

| tag | kind | meaning | evidence |
|---|---|---|---|
| 0x03 | 1 | shader hash | `sub_CD20` -> stage `+92`; printed by `SetPsShader`/`SetVsShader` as `hash=%08x`; also appears as the value of an SH-register entry in the header |
| 0x1E | 3 | name | `sub_1F82A0`, `sub_1F9E30` |
| 0x21 | 1 | member byte offset | `sub_1FAA30` -> uniform entry `+48` |
| 0x24 | 4 | type reference | `sub_1F9E30` |
| 0x25 | 1 | byte size (member, struct, cbuffer type) | `sub_1F82A0` -> uniform entry `+16`/`+24` |
| 0x1B | 1/2 | resource slot / declaration index | inferred (equals the sharp-table slot) |
| 0x1F | 3 | semantic name | inferred |
| 0x1A | 1/2 | semantic index | inferred (`ClipPos` is TEXCOORD1) |
| 0x19 | 2 | system value code | inferred: 255 plain varying, 13 on `main.Position`, 20 on the PS return |
| 0x27 | 5 | member is referenced | inferred: set on 481/483 members; the two without it (`u_GlobalIntensity` in `vglow_p`/`vglow_r8_p`) are exactly the ones those shaders never load |
| 0x29 | 1/2 | base type / kind | inferred: 1 = 32-bit float on vectors and matrices; 5 and 9 = 32-bit integer-class scalars (`u_Flag` is 9, `EffectMode`/`uType`/`u_horizontal` are 5) — **which of int/uint/bool each is was not determined**; 7/8 on texture types; 5 on cbuffer types |
| 0x2A / 0x2B | 1 | rows / columns (components) | inferred |
| 0x4A | 3 | compiler version (`3.0.0.0 build 878 revision d926e84f`) | |
| 0x50 | 1 | VGPR count | inferred: equals `(SPI_SHADER_PGM_RSRC1.VGPRS + 1) * 4` in the checked blobs |
| other type-2 fields (0x02, 0x04, 0x05, 0x09, 0x42, 0x48, 0x51–0x54, 0x63, 0x68, 0x6A–0x6D) | | unknown | printed raw |

### 4.3 What the 72 blobs contain

All shaders are written against the same conventions (see `reflection.txt` for each one):

* Every stage has `__GLOBAL_CB__` at **constant-buffer slot 0** except the four `yuv422_packed*_p`
  shaders, which have no constant buffer at all.
* 44 pixel shaders also declare **`globalConstantBuffer` at slot 7**: `u_GlobalAlpha` (+0, float) and
  `u_GlobalIntensity` (+4, float), 8 bytes. Whenever it is present, `__GLOBAL_CB__` is present too and
  comes first in the resource list.
* Textures and samplers are numbered independently. In the transcribed shaders t-slot `n` is sampled with
  sampler slot `n` (`s_Texture`/`Sampler0`, `s_MaskTexture`/`Sampler1`), but not everywhere: e.g.
  `rgba_x2_scale_*_p` have a texture and no sampler, and `yuv420_semiplanar*_p` / `rgba_hdr_pq2sdr_lut*_p`
  have two textures and one sampler — check the ISA of a shader before assuming a pairing.
* Vertex shaders read `input.Position` (POSITION), `input.Color` (COLOR), `input.TexCoord` (TEXCOORD)
  and write `main.Color`, `main.TexCoord`, `main.ClipPos` (TEXCOORD1) and `main.Position`.

## 5. Building a program (`sub_1FAF80` -> `sub_1F82A0`)

`sub_1FAF80` copies both blobs (`sub_1FFBF0` + memcpy) and calls `sub_1F82A0(prog, vs, vsSize, ps, psSize)`.
Program object fields used below:

| off | content |
|---|---|
| +40 / +48 / +52 | uniform entry array (56 bytes each) / count / capacity |
| +64 / +72 | attribute entry array / count — **never filled for these blobs** (zeroed by the constructor; no writer in the program code; `GetAttributeCount` returns `+72`) |
| +160 / +168 | PS / VS stage objects (`sub_1940` -> `sub_D010`, `sub_1840` -> `sub_D500`) |
| +176 + 24*s | stage constant buffer: `+0` PBuffer, `+8` CPU copy, `+16` byte size, `+20` dirty flag; `s = 0` PS, `s = 1` VS |

For each stage, **PS first, then VS**:

1. Find `.metadata`. If `user_data->sharp_resource_count[3] == 0` (no constant buffers) the stage gets
   no uniforms.
2. `shader = sub_D8A0(root, 2)`, `list = sub_D8A0(shader, 3)`.
3. For every resource in the list, follow its 0x24 type with `sub_1F9E30`, which steps through types
   0x11/0x12/0x17/0x18 and collects each named struct (0x15) once (recursing into member types).
4. Take **only the first collected struct** — in practice `__GLOBAL_CB__`, because it is the first
   constant buffer in every list. `globalConstantBuffer` therefore never becomes program uniforms.
5. For each member (child) of that struct, in order:
   * name = field 0x1E, with a trailing `[...]` removed;
   * if a uniform of that name already exists (i.e. the VS reuses a PS name) the load fails with
     `"VsShader and PsShader cannot share uniform variable of the same name '%s'"`; none of the 70
     programs hits this;
   * append an entry: `+0` index, `+4` binding = -1, `+8` texture unit = -1, `+12` type = 0,
     `+16` size (0x25), `+20` stage (2 = PS, 1 = VS), `+24` stride = size, `+28` array count = 1,
     `+32` name, `+40` 0, `+48` byte offset (0x21).
6. Stage CB size = offset + size of the **last** member. A zeroed CPU copy of that size is allocated and
   a GPU PBuffer `{1, size}` is created (`sub_1230`; failure prints `"Failed to create PBuffer"`).

After the VS pass every entry gets `+0 = its index`; entries with type `>= 0x8001` (samplers) would get
a free texture unit, which never happens here because the type stays 0.

### 5.1 What the ShaderProgram icalls return

| icall | address | returns |
|---|---|---|
| `GetUniformCount` | `0x1E9C80` | `prog+48`: PS members followed by VS members |
| `FindUniform(name)` | `0x1E9EA0` | first index whose entry name `strcmp`s equal, else -1 |
| `GetUniformName/Type/Size/Texture(i)` | `0x1EA9C0` / `0x1EA740` / `0x1EAC80` / `0x1EAF00` | name, `+12` (= 0 = `ShaderUniformType.None`), `+28` (= array count 1, **not** the byte size), `+8` (= -1) |
| `GetAttributeCount` | `0x1E9D90` | `prog+72` = 0 |
| `SetUniformBinding(i, name)` | `sub_1FB490` | sets the entry's `+4` binding and rebuilds the index map `+0` (bound entries first at their index, the rest in order) |

So for `Texture = (basic_vv, rgba_p)` the indices are
`0 u_FadeA, 1 u_FadeB, 2 u_TargetRect, 3 u_Radius, 4 u_Flag` (PS CB0 offsets 0/16/32/48/52) and
`5 u_WorldMatrix, 6 u_Color` (VS CB0 offsets 0/64). The PUI code looks uniforms up by name
(`"u_WorldMatrix"`, `"u_Color"`, `"u_FadeA"`, ... in `Sce.PlayStation.PUI.cs`).

### 5.2 `SetUniformValue` (`0x1EB040` / `0x1EB1A0` -> `sub_1FBAA0`)

The managed `ShaderUniformType` value `t` encodes the element size
`(4*(t & 3) + 4) * (((t >> 4) & 3) + 1)` bytes (Float = 4, Float4 = 16, Float4x4 = 64, Int = 4).
For uniform `i`, array index `to`, `count` elements starting at source element `from`:

* entry `e = entries[entries[i].+0]`; requires `to < e.count(+28)`;
* destination = stage CPU copy (`prog+184` for PS entries, `prog+208` for VS — chosen by `e.+20 & 1`)
  at byte `(e.offset(+48) + e.stride(+24) * to)` rounded down to a multiple of 4;
* length = `min(count * elemSize, e.size(+16) - e.stride * to)`; source must hold `elemSize*(from+count)`;
* the stage's dirty flag is set and `byte_53F65F |= 1`.

Values are copied as raw bytes, so the CPU copy is a byte-exact image of the HLSL/PSSL `__GLOBAL_CB__`
(std140-compatible here: all members are 4-byte scalars, 16-byte vectors or a column-major 4x4).

## 6. Per draw (`sub_1F32A0`, called by `PsmGraphicsContext.DrawArrays` 0x1E7280 and `.Update` 0x1E6C50)

* **Global CB**: `SetConstantBufferVs(7, ctx+368)` and `SetConstantBufferPs(7, ctx+368)` on every call.
* **Program CBs**: if a stage's dirty flag is set, the PBuffer is mapped (`sub_9C30`) and the whole CPU
  copy is memcpy'd into it; then `SetPsShader`, `SetVsShader`, `SetConstantBufferPs(0, prog+176)`,
  `SetConstantBufferVs(0, prog+200)`.
* **Textures**: for texture unit `i < min(8, count)`, `SetTexturePs(i, tex+104)` and
  `SetSamplerDirect(1 /*PS*/, i, tex+112)`: managed texture unit `i` is PS t-slot `i` and sampler slot `i`.
* **Vertex buffers**: `SetVertexBuffers(0, vb+48, vb+240)` binds the streams of the PSM vertex buffer to
  slots 0..n-1. At bind time (`sub_B010`, VS only) a table of `num_input_semantics` 16-byte V#s is built,
  entry `i` = the V# (`+88`) of slot `i` (or a default buffer), and its address is written to the
  user-data dwords of direct resource 8. The shader fetches attribute `i` with
  `buffer_load_format_* v, vIndex, table[i], 0 idxen` (`basic_vv`: entries at `+0x00`, `+0x10`, `+0x20`),
  so **attribute location = input semantic index = vertex-buffer stream index**, and the element format
  is whatever the stream's V# says (the shader does not encode it). The internal layout of the managed
  VertexBuffer object (`+240` array) was not reversed further. *TODO.*

## 7. `globalConstantBuffer`

* Created in `PsmGraphicsContext.Create` (icall `0x1E5D50`): `sub_1230(..., ctx+368, {1, 8}, &init)`
  with `init = 0x3F8000003F800000`, i.e. `u_GlobalAlpha = 1.0`, `u_GlobalIntensity = 1.0`.
* Written only by `PsmGraphicsContext.UpdateGlobalConstantBuffer(handle, ref T, size)` (icall
  `0x1F1C80`): if `0 <= size <= buffer size` the buffer is mapped and `size` bytes are memcpy'd.
* Managed wrapper: `GraphicsContext.UpdateGlobalContantBuffer<T>(ref T value, int size)` (sic). A string
  search of all FW 4.03 managed assemblies in `work/il403` finds the name only in
  `Sce.PlayStation.Core.dll`, and the decompiled Core has no caller — so in this firmware the buffer
  keeps its initial `{1.0, 1.0}` unless something outside those assemblies calls it. *Not verified at
  runtime.*

## 8. Shader-side conventions (from the ISA)

* **Interpolants**: PS attribute `n` is the PS input semantic `n`; it is matched to the VS output with the
  same semantic id (VS output semantic 0 -> `exp param0` = `main.Color`, semantic 1 -> `exp param1` =
  `main.TexCoord` in `.xy` and `main.ClipPos` in `.zw`). The VS->PS linking itself is done by AGC, not by
  libScePsm; this mapping is what the semantic tables and the ISA agree on. PS interpolation: perspective,
  pixel center (`SPI_PS_INPUT_ENA = 0x2`, `SPI_PS_IN_CONTROL = 2` interpolants).
* **Colour output**: `SPI_SHADER_COL_FORMAT = 4` (32_ABGR FP16 export) with `v_cvt_pkrtz_f16_f32`, so the
  PS result is rounded toward zero to fp16.
* **`u_Flag` tail** (solid_fill_p, a8_p, rgba_p, mask_p and the other `_p` shaders that declare
  `u_FadeA/u_FadeB/u_TargetRect/u_Radius/u_Flag`): bit 2 = rounded-rectangle coverage against
  `u_TargetRect`/`u_Radius` (with the clip-space y scaled by 0.5625 and antialiased over one coarse ddx of
  `ClipPos.x`), bit 1 = fade ramp `f*f*(3-2f)` from `u_FadeA`/`u_FadeB`, bit 0 = also apply the coverage to
  RGB (premultiplied) and multiply RGB by `u_GlobalAlpha` as well as `u_GlobalIntensity`. The exact
  formula is in the header of `launcher/shaders/psm/solid_fill_p.frag`.
* **Texture pre-filter** (`rgba_p`, `mask_p`): when the hardware LOD exceeds 0.2 a 2x2 box filter on the
  even texel grid is blended in by `saturate(lod)`; above 1.2 a 16-tap 4x4 box is blended in by
  `saturate(lod - 1)`. Details in `rgba_p.frag`.

## 9. The GLSL transcriptions

`launcher/shaders/psm/` holds `basic_vv.vert`, `solid_fill_p.frag`, `rgba_p.frag`, `a8_p.frag`,
`mask_p.frag`. They are written register by register: each statement carries the byte offset of the
instruction it comes from, the register names are kept, and the control flow is the original
scalar-branch / EXEC-mask structure. All compile with `glslangValidator -V` (Vulkan 1.4 SDK).

Bindings (the host convention, `psmhost/libScePsm/graphics/shader_program.h`): everything in `set = 0`;
VS `__GLOBAL_CB__` at binding 0, PS `__GLOBAL_CB__` at binding 1, `globalConstantBuffer` (slot 7) at
binding 2, each with explicit `layout(offset = N)` equal to the metadata offsets; combined image samplers
at binding `3 + <t slot>` (t-slot `n` with sampler slot `n`, checked in each ISA); vertex inputs at
`location = <input semantic index>` (= vertex-buffer stream); varyings at `location = <param index>`.
A `float2` member at a 4-mod-8 offset (`u_ShimmerParam` +116 in `focus_ui3_area_p`, `u_NoiseChangeParam`
+84 in `focus_ui3_line_p`) cannot be a std140 `vec2`; it is declared as two `float`s `_x`/`_y`.

PS interpolants: the ISA's `attrN` is PS input-semantic entry `N`, whose value is the VS param index.
Where a pixel shader has a single interpolant with semantic 1 (`focus_ui3_p`, `hblur_p`, `vblur_p`,
`vglow_r8_p`: `SPI_PS_IN_CONTROL = 1`), its `attr0` is declared at `location = 1`.

Known differences from the hardware, repeated in each file:

* The vertex stage and the pixel stage have separate CB slot spaces in the original
  (`SetConstantBufferVs` / `SetConstantBufferPs`); `binding = 0` of `basic_vv.vert` and of the fragment
  shaders are different buffers and must be remapped when both go into one pipeline layout.
* NGG plumbing (`s_sendmsg GS_ALLOC_REQ`, `exp prim`) is omitted.
* `v_mad_f32`/`v_mac_f32` are unfused; the GLSL locals are `precise` to stop contraction into fma.
  `v_rcp_f32` is the 1-ulp hardware reciprocal (GLSL `1.0/x`). `v_min_f32` and the clamp modifier have
  defined NaN behaviour, GLSL `min`/`clamp` do not.
* The quad-DPP difference `lane1 - lane0` is written `dFdxCoarse`; identical on AMD, may pick another
  lane pair elsewhere.
* `image_get_lod` component 1 is written `textureQueryLod(...).y`. *TODO(verify)* on hardware.
* The T# width/height (mip 0 of the resource) are written `textureSize(tex, 0)`, equal while the view's
  base mip is 0.
* The pre-filter taps sample with implicit derivatives under a narrowed EXEC mask; quads that are only
  partially inside the branch use stale coordinates on hardware and undefined derivatives in GLSL.
* Clip-space/viewport conventions of the PS5 are not part of these shaders; a Vulkan host handles its own
  Y direction and depth range outside them.

### 9.1 What is transcribed

| EmbeddedShader | vs | ps | notes |
|---|---|---|---|
| 0 SolidFill, 48/49/53-56 Basemat variants | `basic_vv` | `solid_fill_p` | |
| 1 SolidFillSimple | `solid_fill_simple_vv` (POSITION only, no params) | `solid_fill_simple_p` (no interpolants) | |
| 2 Texture | `basic_vv` | `rgba_p` | |
| 3 TextureNoMip | `basic_vv` | `rgba_nomip_p` | |
| 7 TextureMask | `basic_vv` | `mask_p` | |
| 11 A8Texture | `basic_vv` | `a8_p` | |
| 24 BlurH / 25 BlurV | `blur_vv` (POSITION + TEXCOORD = stream 2; param0 = 0) | `hblur_p` / `vblur_p` | 13-tap, weights in the files |
| 26 GlowH / 27 GlowV / 28 GlowR8V | `glow_vv` (= `basic_vv` code with `u_World`/`u_FilterColor`) | `hglow_p` / `vglow_p` / `vglow_r8_p` | alpha-only 13-tap |
| 50 LineFocus | `focus_ui3_line_vv` (param2 = St.xy, FrameSt.xy) | `focus_ui3_line_p` | demote; t0 colour table + t1 noise |
| 51 AreaFocus | `focus_ui3_area_vv` (param2 = St.xy, 0, 0) | `focus_ui3_area_p` | demote; t0 colour table + t1 noise |
| 52 FocusUI3 | `basic_vv` | `focus_ui3_p` | 2D simplex noise, line and area paths |
| 57 SolidFillLightMat | `basic_vv` | `solid_fill_light_mat_p` | `s_memrealtime` dither |
| 64 SolidFillBasematFullScreenGradation | `basic_vv` | `solid_fill_basemat_full_screen_gradation_p` | vertical ramp `c.a * y²(3-2y) + u_startAlpha` with the same `s_memrealtime` dither; the Control Center backdrop draws two of these between its solid bars |

Not yet transcribed: the other `mask_*` variants (same structure as `mask_p` plus the colour-space tails
of `focus_ui3_area_p`, but with a different register allocation, so each needs its own pass), `yuv*`,
`rgba_*` variants, `gls_*`, `sector_mask_p`, `focus_frame_2_p`, `function_focus_p`, `lod_checker_p`,
`hdr_calibration_p`, `idx8_p`, `r8*`, `a8_shadow*`, `rgb1_p`, `hub_basemat_p`, `ellipse_gradation_p`,
`solid_fill_basemat_gradation_p`, `gls_effect_vv`.

Features some of these need from the Vulkan device (beyond the base layout):

* `demote` (`focus_ui3_area_p`, `focus_ui3_line_p`): the ISA removes killed lanes only from the export
  mask (`s_andn2_b64 s[live], s[live], exec`) and keeps them running in WQM, i.e. helper semantics.
  SPIR-V `DemoteToHelperInvocation` needs `VkPhysicalDeviceVulkan13Features::shaderDemoteToHelperInvocation`.
* `s_memrealtime` (`solid_fill_light_mat_p`, low 23 bits seed a dither): written
  `clockRealtime2x32EXT()` (`GL_EXT_shader_realtime_clock`, SPIR-V `ShaderClockKHR`), which needs
  `VK_KHR_shader_clock` with `shaderDeviceClock`. The counter frequency differs from the PS5's (not known).
* `colour-space mode` (`u_Flag` bits 20..23, `focus_ui3*_p`): 1 = PQ encode, 2 = gamma 1/2.2 of
  0.025 x (gamut-converted `pow(c, 2.35)`); the 3x3 matrix and constants are in `focus_ui3_area_p.frag`.

Observation for section 6: `blur_vv` has two input semantics (0 and 2) but loads the TEXCOORD V# from
table offset `0x20` (entry 2), so the table the shader indexes is addressed by semantic index, not by
position in the input-semantic list; a table of `num_input_semantics` entries would be too short.
*TODO(verify)* how `sub_B010` sizes and fills the table for such shaders.

## 10. ShellUI eboot shaders: the LightParticle layer (mechanical transcription)

The background layer's particle shaders live in the ShellUI eboot (NPXS40087, table
`{name, data, size}` at `0xCC95F8`; blobs extracted to `work/ida_bglayer/shaders/NNN_<name>.bin`):
#68 `particle_c` (compute, 64 lanes, 0x7440 bytes of code, ~5000 instructions dominated by an
inlined 3D noise), #70 `particle_vv`, #69 `particle_p`, and #72/#71 `large_particle_vv/p` (the
large particles of the boot animations, modes 3/4; `large_particle_p` samples `Particle0/1.gnf`).

They are transcribed mechanically instead of by hand:

```
python -I tools/shaders/ags_disasm.py work/ida_bglayer/shaders/068_particle_c.bin work/ida_particles/sh/068_particle_c.ann
python -I tools/shaders/isa2glsl.py particle_c work/ida_bglayer/shaders/068_particle_c.bin \
       work/ida_particles/sh/068_particle_c.ann launcher/shaders/bglayer/particle/particle_c.comp
```

`ags_disasm.py` runs llvm-objdump (gfx1013) on `.shader_text` (the blobs mark it with an OBJECT
symbol, retyped to FUNC so it is decoded as code) and annotates literals with their float value.
It uses `SSN_LLVM_OBJDUMP`, else the RGA download under `deps/dl/rga`, else the newest installed
ROCm / HIP SDK (`C:\Program Files\AMD\ROCm\<ver>\bin\llvm-objdump.exe`). Newer llvm-objdump prints
exports as `exp pos0 v1, ...`; the tool writes them in the RGA form `exp pos0, v1, ...` that
`isa2glsl.py` parses and the committed transcriptions quote, so the outputs regenerate
byte-identically with either disassembler (checked for `particle_c/vv/p` and `gtc_bc7`).
`isa2glsl.py` emits one GLSL statement per instruction, each carrying its byte offset and text:

* registers are `uint`s holding bits; float instructions reinterpret them; `v_mad/v_mac/v_madmk/
  v_madak` are an unfused multiply and add in `precise` helpers, `v_fma_f32` is `fma()`;
* one invocation is one lane: EXEC and the 64-bit lane masks (VCC, `s[n:n+1]`) are `bool`s,
  VGPR writes are predicated on EXEC, SALU always runs;
* control flow is a loop over `switch (pc)` with the basic blocks as cases. Branches that test
  "any lane" (`s_cbranch_execz/vccz`, SCC of 64-bit mask ops) use `subgroupAny()`, so a host
  subgroup follows the path of the hardware wave and scalar state stays uniform (SPIR-V 1.6,
  `--target-env vulkan1.3`);
* memory: the user-data V# is the SRT uniform block (binding 0), the CB its pointer at +0 refers to
  is binding 1, buffer V#s loaded from that CB map to storage buffers by CB offset (bindings 2/3)
  with the V# NUM_RECORDS/STRIDE bounds check of OOB_SELECT 0; `s_getpc`-relative `tbuffer` loads
  read the constant tables appended after the code (copied into the GLSL as const arrays);
* `.long` pairs llvm-objdump cannot decode (SDWA `v_mov_b32` with a source modifier) are decoded
  by the tool; the NGG prologue of the vertex shaders (primitive allocation/export) is skipped.

Layout the eboot feeds them (bglayer_particle.cpp): SRTCs `{ptr, time, step, kill rate, 0, gen}`
(28 bytes) with the 248-byte emitter block; SRTVsPs `{ptr, time, step, gen}` (20 bytes) with the
320-byte draw block (+0x13C = emitter index); V#s at block +0/+16 (CS: permutation, particles;
VS/PS: particles, permutation). The particle record is 68 bytes; 6000 records.

Large particles (`large_particle_vv/p`, RenderInternal 0x96860 after the regular draws): the
236-byte draw block `SRTLargeParticleVsPs` (count at +0xAC, particle index = +0xB0 + +0xB4 x n),
SRT time without the +0x7B4 offset, blend control +1920 = ONE / ONE / ADD on colour and alpha
(xmmword_B8B1F0). `large_particle_p` loads two T#s from the block (+0x20 / +0x40, the
`Particle0/1.gnf` textures Prepare copies in) with `s_load_dwordx16` and samples them with an S#
built from immediates (`s[16:19]` = {0x92, 0, 37 << 20, 0}: clamp, bilinear, no mip). For this the
tool gained:

* `p_textures`: T#s stored in P (tracked through `s_load_dwordx8/16` like the V#s) become
  `sampler2D` bindings; the host binds the textures with the VkSampler of the same S#;
* `image_sample` (2D, any dmask) on those textures; with `sample_lod0` it is written
  `textureLod(.., 0.0)`, exact here because both textures have one mip level and identical
  min/mag filters, and defined inside the transcription's non-uniform control flow;
* `v_mad_u64_u32` with `vcc` as the 64-bit addend (`large_particle_vv` 0x158; an unsupported
  addend now stops the tool instead of emitting `None`);
* the `cvt_i32_f32_` helper is emitted only when used (as before `gtc_bc7`), so the earlier
  transcriptions regenerate unchanged.

```
python -I tools/shaders/ags_disasm.py work/ida_bglayer/shaders/071_large_particle_p.bin <ann>
python -I tools/shaders/isa2glsl.py large_particle_p work/ida_bglayer/shaders/071_large_particle_p.bin <ann> \
       launcher/shaders/bglayer/particle/large_particle_p.frag
```
(and `large_particle_vv` from `072_large_particle_vv.bin` into `large_particle_vv.vert`).

## 10b. ShellUI eboot shaders: the Transition layer (background images)

The Transition layer (Initialize 0xA3CB0) draws background images with #44 `single_p`,
#5 `cross_fade_p`, #46 `ripple_p`, #48 `slide_in_p`, #49 `game_splash_p` (and #47
`ripple_with_logo_p`, not transcribed) over `basic_vv`; the old-image capture 0xA48D0 copies with
#0 `basic_p`. Their resources are all in user data: T#s and S#s (the XF texture's sampler, S#
{0x92, 0x00F00000, 0x05500000, 0}), c0 (the per-shader parameters: opacity / progress, ripple
0xD61220, slide 0xD612C0) and c1 (the basemat: two 80-byte GradationParameter built by 0xA8670).
The shaders also read T# words as values (the texture width decides whether alpha is forced to 1)
and seed a dither with `s_memrealtime`. isa2glsl gained for them:

* `ud_cbs`: constant buffers whose V# is in user data -> uniform blocks (`s_buffer_load` by
  register);
* `ud_textures`: `image_sample` through a T# in user data -> `sampler2D` bindings;
* `ud_values` / `ud_dwords`: the user SGPRs start with the user-data dwords from a uniform block;
  the host fills the T# WIDTH / HEIGHT fields of the bound textures there;
* `s_memrealtime` -> a push constant (`memrealtime`), as in `light_p`;
* `v_med3_f32` (`ripple_p`), and `attr_comps` to declare an interpolant as narrow as the vertex
  output that feeds it (`basic_vv` exports a vec2).

```
python -I tools/shaders/isa2glsl.py single_p work/ida_bglayer/shaders/044_single_p.bin <ann> \
       launcher/shaders/bglayer/transition/single_p.frag
```
(likewise `cross_fade_p` 005, `ripple_p` 046, `slide_in_p` 048, `game_splash_p` 049, `basic_p` 000.)

## 11. libScePsm GPU texture compressor (GTC): BC7 by transcription

`Image::ConvertNative` with `ImageConvertOption.DXTCompressByGPU` (0x2e10a0, option 4) maps
the target ImageFormat 2..8 to the block formats (Dxt1 16, Dxt3 18, Dxt5 20, Bc4U 33, Bc5U 34,
Bc6U 35, Bc7U 36), allocates a raw impl of that format over the size rounded up to 4
(sub_200910), and fills it with sub_296B70 ("GTC"). GTC binds the RGBA8888 frame as an
8_8_8_8 UNORM texture (data format 0xFAC00000038) and runs one of the compressor shaders of
the table at 0x4A23C0 (4 qwords per format: BC1 0x4177D0, BC2 0x422A10, BC3 0x425A20, BC4
0x4291D0, BC5 0x42A510, BC6 0x42C160, BC7 0x430970; BC1..3 also bind t1 from 0x4221E0).

Each compressor is an NGG vertex shader. Each vertex is one 4x4 block: blockId is the vertex
index. The shader reads `srcTex` (t0, s[8:15]) with `image_load` and stores the 128-bit block to
the `u0` image (s[16:23]) with `image_store`. The home tiles use BC7. The blobs are extracted to
`work/ida_icon/sh/gtc_*.bin`:

```
python -I tools/shaders/ags_disasm.py work/ida_icon/sh/gtc_bc7.bin work/ida_icon/sh/gtc_bc7.ann
python -I tools/shaders/isa2glsl.py gtc_bc7 work/ida_icon/sh/gtc_bc7.bin work/ida_icon/sh/gtc_bc7.ann \
       launcher/shaders/psm/gtc/gtc_bc7.comp
```

The `gtc_bc7` entry of `isa2glsl.py` runs the vertex shader as a compute shader. Each
invocation is one vertex. The NGG prologue is skipped. EXEC starts as the lanes below the
vertex count (a push constant). Image descriptors are declared per user-data register.

The tool gained the instructions this shader needs:
- integer and conversion ops: `s_movk_i32`, `s_brev_b32`, `s_ashr_i32`, `v_add3_u32`, `v_lshl_or_b32`, `v_not_b32`, `v_min/max_u32/i32`, `v_med3_i32`, `v_mul_u32_u24/i32_i24`, `v_add/sub/subrev_co_ci_u32`, `v_cvt_f32_ubyte0`, `v_cvt_i32_f32`, `v_cvt_rpi_i32_f32`, `v_rcp_iflag_f32`, `v_fmac_f32`;
- `v_readfirstlane_b32`: subgroup ballot plus shuffle of the lowest EXEC lane;
- `v_movrels_b32` / `v_movreld_b32`: M0-relative VGPR indexing over a 64-register window, which covers the 16 RGBA texels;
- `image_load`, `image_store` and `image_get_resinfo` for 2D images.

The constant table the shader reads through an `s_getpc`-relative V# (`.shader_text+0x6080`,
NUM_RECORDS 0x800) is the single-colour endpoint table used for uniform blocks. The tool now copies
`table_words` words of it (512 here; the default stays 64). With only 64 words, uniform blocks came
out with black endpoints. `SSN_DUMP_GTC=<dir>` writes each input frame and its blocks for checking:
decoded against the source, the BC7 output's mean error is below 0.4 per channel. The encoder emits
modes 4, 5 and 6 only (single subset).

The host runs the shader through `psmhost/libScePsm/imaging/image_gtc.cpp`: one synchronous
dispatch per image (`gfx::Device::run_now`), reading back the block image row-major. Only BC7 is
transcribed. The other formats are logged as not reproduced.
