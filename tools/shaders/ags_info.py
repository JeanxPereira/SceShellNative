"""Dump an embedded libScePsm shader blob (AGS = AMDGPU ELF produced by the PS5 shader compiler).

usage:
    python -I ags_info.py [--tree] [--psm-elf libScePsm_dyn.elf --core-cs Sce.PlayStation.Core.cs] blob...

Prints, for each blob:
  * the ELF sections (.shader_text / .shader_header / .metadata / .cfg)
  * the .shader_header (AGC `Shader` struct: registers, semantics, user-data resource layout)
  * the .metadata reflection ("sl00" tree): constant buffers + members, textures, samplers,
    stage inputs/outputs, entry point
With --psm-elf (and optionally --core-cs for the enum names) it also prints the EmbeddedShader
program table (libScePsm 0x461ED0) and, per program, the uniform list exactly as libScePsm's
program builder exposes it through the ShaderProgram icalls.

Everything decoded here is cited to the libScePsm (FW 4.03) code that reads it; fields the
library never reads are printed raw and their meaning is marked "inferred" or "unknown".
Addresses refer to work/ida/libScePsm_dyn.elf.

Key readers in libScePsm:
  sub_1FAF80  program ctor: copies the VS/PS blobs, calls sub_1F82A0
  sub_1F82A0  program builder: loads PS (sub_1940 -> sub_D010) and VS (sub_1840 -> sub_D500)
              through sceAgcCreateShader (NID f3dg2CSgRKY, via sub_D1E0/sub_15D0B0), then walks
              each stage's .metadata to build the uniform table and the stage constant buffer
  sub_D8A0    sl00: find child of a node by type id
  sub_CD20    sl00: read field 3 of the type-2 node (shader hash, stored at shader+92)
  sub_1F9E30  sl00: follow type references (field 0x24) and collect struct (type 0x15) nodes
  sub_1FAA30  sl00: read field 0x21 (member byte offset) of a node
  sub_C930    reads Shader->user_data sharp-resource tables into the stage object
  sub_B010    BindShaderResources: copies bound descriptors into user-data dwords per those tables
"""
import argparse
import os
import re
import struct
import sys

# ----------------------------------------------------------------------------------------- ELF


def elf_sections(d):
    if d[:4] != b'\x7fELF':
        raise ValueError('not an ELF')
    shoff, = struct.unpack_from('<Q', d, 0x28)
    shentsize, shnum, shstrndx = struct.unpack_from('<HHH', d, 0x3A)
    sh = [struct.unpack_from('<IIQQQQIIQQ', d, shoff + i * shentsize) for i in range(shnum)]
    stroff = sh[shstrndx][4]
    out = []
    for s in sh:
        name = d[stroff + s[0]:d.index(b'\0', stroff + s[0])].decode()
        out.append({'name': name, 'type': s[1], 'flags': s[2], 'off': s[4], 'size': s[5], 'align': s[8]})
    return out


def section(d, secs, name):
    # sub_D1E0 / sub_D010 / sub_1F82A0 look sections up by name (strcmp over the section table)
    for s in secs:
        if s['name'] == name:
            return d[s['off']:s['off'] + s['size']]
    return None

# ------------------------------------------------------------------------------ .shader_header
# AGC `Shader` struct, as the firmware reads it. "agc" = libSceAgc.sprx of the dump
# (work/ida_agc/libSceAgc.elf.i64), the rest = libScePsm (see docs/psm-shaders.md section 3).
#   +0x00 magic 0x34333231, +0x04 version 0x18     agc 0xC440 sceAgcCreateShader (NID f3dg2CSgRKY)
#   +0x08 user_data ptr                            agc 0xC440 relocates; sub_C930 / sub_156260 / sub_1F82A0
#   +0x10 code ptr (0 in the blob)                 agc 0xC440 requires 0, stores the code address
#   +0x18 context registers, +0x5B count           sub_D010/sub_D500/sub_CB40 hash, sub_155190 emits
#   +0x20 SH registers, +0x5C count                agc 0xC440 patches the program address; sub_155190 emits
#   +0x28 specials block                           agc 0xD560 / 0xE410 / 0xC840 / 0xCE90
#   +0x30/+0x50 input semantics + count            agc 0xD9B0 (PS input mapping) etc.; sub_B010 (count)
#   +0x38/+0x56 output semantics + count           agc 0xD9B0 etc.
#   +0x40 header size, +0x44 code size             sub_CB40 copies that many bytes
#   +0x4C i32                                      agc 0xC440 checks only its sign
#   +0x5A stage type                               agc 0xC440 (accepts 0,1,2,3,6,7); sub_1562F0 (2 = VS, 1 = PS)
#   +0x48 u32, +0x54 u16, +0x58 u16                not read by the firmware code examined (printed raw)
# All pointer fields are self-relative (value + address of the field): agc 0xC440 makes them
# absolute when libScePsm hands it the header (sub_D1E0 -> sub_15D0B0).

SHADER_TYPES_SEEN = {1: 'pixel (PS)', 2: 'vertex stage compiled as NGG/GS (VS)'}  # from the blobs + ISA


def rel_ptr(h, off):
    v, = struct.unpack_from('<q', h, off)
    return None if v == 0 else off + v


def parse_header(h):
    r = {}
    r['magic'], r['version'] = struct.unpack_from('<II', h, 0)
    r['user_data'] = rel_ptr(h, 0x08)
    r['cx'] = rel_ptr(h, 0x18)
    r['sh'] = rel_ptr(h, 0x20)
    r['specials'] = rel_ptr(h, 0x28)
    r['in_sem'] = rel_ptr(h, 0x30)
    r['out_sem'] = rel_ptr(h, 0x38)
    (r['header_size'], r['shader_size'], r['h48'], r['h4c'], r['num_in'], r['h54'],
     r['num_out'], r['h58'], r['type'], r['num_cx'], r['num_sh']) = struct.unpack_from('<IIIiIHHHBBB', h, 0x40)
    r['cx_regs'] = [struct.unpack_from('<II', h, r['cx'] + 8 * i) for i in range(r['num_cx'])] if r['cx'] else []
    r['sh_regs'] = [struct.unpack_from('<II', h, r['sh'] + 8 * i) for i in range(r['num_sh'])] if r['sh'] else []
    r['in'] = list(struct.unpack_from('<%dI' % r['num_in'], h, r['in_sem'])) if r['in_sem'] and r['num_in'] else []
    r['out'] = list(struct.unpack_from('<%dI' % r['num_out'], h, r['out_sem'])) if r['out_sem'] and r['num_out'] else []
    ud = r['user_data']
    if ud is not None:
        # User data: +0 direct_resource_offset (u16[], sub_156260), +8..+0x20 sharp_resource_offset[4]
        # (u16[]) with counts +0x2E..+0x34 (sub_C930 reads +8/+16/+24/+32 and +46/+48/+50/+52; sub_1F82A0
        # gates the metadata walk on +52, sharp class 3 = constant buffers); +0x2A = dword size of direct
        # resource 1 (sub_156260). +0x28 and +0x2C are not read by the firmware code examined; +0x2C
        # (11 in every blob) is used here only as the length of the direct-resource list.
        dro = rel_ptr(h, ud)
        sharp = [rel_ptr(h, ud + 8 + 8 * k) for k in range(4)]
        ud28, direct1, dcount, c0, c1, c2, c3 = struct.unpack_from('<7H', h, ud + 0x28)
        r['ud_28'], r['direct1_dw'] = ud28, direct1
        r['direct'] = list(struct.unpack_from('<%dH' % dcount, h, dro)) if dro and dcount else []
        r['sharp'] = []
        for k, c in enumerate((c0, c1, c2, c3)):
            r['sharp'].append(list(struct.unpack_from('<%dH' % c, h, sharp[k])) if sharp[k] and c else [])
    return r


def semantic_fields(v):
    # Bits as the PS input mapping in libSceAgc (agc 0xD9B0 pdEV7bI6COI and its copies) reads them:
    #   0-7   semantic: PS input matched to the feeding stage's output with the same byte
    #   8-15  hw_mapping: bits 8-12 of the matched output become the input's parameter slot
    #   20-21 f16: non-zero selects the 16-bit path (bit 20 / bit 21 -> one control bit each)
    #   22    flat: control bit 10 (bit 24 has the same effect)
    #   28-31 default: bits 28-29 -> control bits 8-9, bits 30-31 -> control bits 21-22 (16-bit path)
    # Bits 16-19 are not read by the firmware; in the blobs they hold the element count (cross-checked
    # against the metadata and the ISA). Bits 23 and 25-27 are not read; bit 24 is printed on its own.
    return ('semantic=%d hw_mapping=%d elems=%d f16=%d flat=%d bit23=%d bit24=%d bit25=%d bit26=%d default=%d'
            % (v & 0xFF, (v >> 8) & 0xFF, (v >> 16) & 0xF, (v >> 20) & 3, (v >> 22) & 1, (v >> 23) & 1,
               (v >> 24) & 1, (v >> 25) & 1, (v >> 26) & 1, ((v >> 28) & 3) | ((v >> 30) & 3) << 2))


SHARP_CLASSES = [
    # class index -> how libScePsm BindShaderResources (sub_B010) fills it
    ('t', 'texture/buffer SRV: 32 or 16 bytes from the object bound with SetTexturePs (cmd+584+8*slot for PS); '
          'missing texture -> default texture'),
    ('u', 'RW resource: descriptor from cmd+712+32*stage+8*slot'),
    ('s', 'sampler: 16-byte S# from SetSamplerDirect (cmd+0x430+128*stage+16*slot)'),
    ('b', 'constant buffer: 16-byte V# from SetConstantBufferPs/Vs (cmd+872+64*stage+8*slot)'),
]

# user SGPR where user-data dword 0 lands. Not stored in the header that libScePsm reads; observed in the
# ISA of these blobs: PS resources are addressed from s0, the NGG vertex stage (type 2) from s8.
USER_SGPR_BASE = {1: 0, 2: 8}

# ------------------------------------------------------------------------------------- sl00

# Field payload kinds (high nibble of the 2nd descriptor byte). The skip logic is identical in every
# reader (sub_1F82A0, sub_1F9E30, sub_1FAA30, sub_CD20): kind 5 = no payload, kind 7 = 4 raw bytes,
# kind 6 = two LEB128, anything else = one LEB128. Value decoding: kind 1 = ULEB (sub_1FAA30 v19==1),
# kind 2 = SLEB (sign from bit 6 of the last byte, sub_1FAA30 v19==2), kind 5 = 1/true (sub_1FAA30 returns 1).
# Kind 3 is used as "offset of a NUL-terminated string from the section start" (field 0x1E in sub_1F82A0:
# v190 = value + base) and kind 4 as "offset of another node" (field 0x24 handed to sub_1F9E30).
KIND_NAMES = {1: 'uleb', 2: 'sleb', 3: 'str', 4: 'ref', 5: 'flag', 6: 'uleb2', 7: 'raw4'}

# Node type ids (schema varint >> 2).  "lib" = libScePsm branches on it; others inferred from content.
TYPE_NAMES = {
    0x01: 'root',                     # header +4 points at it (sub_CD20 / sub_1F82A0 start there)
    0x02: 'shader',                   # lib: sub_D8A0(md, root, 2)
    0x03: 'resource_list',            # lib: sub_D8A0(md, shader, 3)  ("sceShaderGetResourceList")
    0x04: 'input',                    # inferred (names input.*)
    0x05: 'output',                   # inferred (names main.*)
    0x07: 'cbuffer',                  # inferred (names, slot = sharp class 3 index)
    0x08: 'sampler',                  # inferred (Sampler0..; slot = sharp class 2 index)
    0x11: 'type_0x11',                # lib: followed through field 0x24 (no instance in the 72 blobs)
    0x12: 'type_0x12',                # lib: followed through field 0x24 (no instance in the 72 blobs)
    0x13: 'vector',                   # inferred (0x29 base type, 0x2B components)
    0x14: 'matrix',                   # inferred (0x29 base type, 0x2A rows, 0x2B columns)
    0x15: 'struct',                   # lib: collected by sub_1F9E30 when it has a name (0x1E)
    0x17: 'texture_type',             # lib: followed through 0x24 (element type)
    0x18: 'cbuffer_type',             # lib: followed through 0x24 (-> struct)
    0x19: 'member',                   # lib: children of the struct read with 0x1E/0x21/0x25
    0x1A: 'texture',                  # inferred (s_Texture..; slot = sharp class 0 index)
    0x1B: 'type_list',                # inferred (holds all type nodes)
}
TAG_NAMES = {
    0x03: 'hash',          # lib: sub_CD20 -> shader obj+92, printed as "hash=%08x" by SetPsShader/SetVsShader
    0x06: 'version?',      # unknown (root, 0x10000 in all blobs)
    0x19: 'sysval?',       # inferred: 255 on plain varyings, 13 on main.Position, 20 on the PS return
    0x1A: 'sem_index',     # inferred: TEXCOORD index (ClipPos=1)
    0x1B: 'slot',          # inferred: resource slot / declaration index (matches the sharp tables)
    0x1E: 'name',          # lib: sub_1F82A0 / sub_1F9E30
    0x1F: 'semantic',      # inferred
    0x21: 'offset',        # lib: sub_1FAA30 -> uniform byte offset (entry+48)
    0x24: 'type',          # lib: node reference followed by sub_1F9E30
    0x25: 'size',          # lib: sub_1F82A0 v171 -> uniform byte size (entry+16 / +24)
    0x27: 'used',          # inferred: flag present on 481/483 members; the two without it (u_GlobalIntensity in
                           # vglow_p / vglow_r8_p) are exactly the members those shaders never s_buffer_load
    0x29: 'base',          # inferred: 1 = float (used with f32 ALU), 5 / 9 = 32-bit integer class,
                           #           7 / 8 on texture types, 5 (sleb) on cbuffer types
    0x2A: 'rows',          # inferred
    0x2B: 'cols',          # inferred
    0x4A: 'compiler',      # inferred (version string)
    0x50: 'vgprs',         # inferred; equals (PGM_RSRC1.VGPRS+1)*4 in the blobs checked
}


def uleb(b, p):
    r = s = 0
    while True:
        c = b[p]
        p += 1
        r |= (c & 0x7F) << s
        s += 7
        if c < 0x80:
            return r, p


def sleb(b, p):
    r = s = 0
    while True:
        c = b[p]
        p += 1
        r |= (c & 0x7F) << s
        s += 7
        if c < 0x80:
            if c & 0x40:
                r -= 1 << s
            return r, p


def cstr(b, o):
    return b[o:b.index(b'\0', o)].decode('latin1')


class Node:
    """sl00 node: ULEB schema offset; ULEB next-sibling if schema flag bit0; ULEB first-child if bit1;
    then one payload per field descriptor of the schema. Schema: ULEB (type<<2 | flags) followed by
    2-byte descriptors (tag = b0 | (b1 & 0xF) << 8, kind = b1 >> 4) up to a descriptor whose first byte
    is 0. (sub_D8A0 walks next/child exactly like this; sub_1FAA30 the field list.)"""

    def __init__(self, b, off):
        self.off = off
        so, p = uleb(b, off)
        flags, sp = uleb(b, so)
        self.type = flags >> 2
        self.next = self.child = 0
        if flags & 1:
            self.next, p = uleb(b, p)
        if flags & 2:
            self.child, p = uleb(b, p)
        self.fields = {}
        self.order = []
        while b[sp] != 0:
            tag = b[sp] | ((b[sp + 1] & 0xF) << 8)
            kind = b[sp + 1] >> 4
            sp += 2
            if kind == 5:
                v = True
            elif kind == 7:
                v = struct.unpack_from('<I', b, p)[0]
                p += 4
            elif kind == 6:
                a, p = uleb(b, p)
                c, p = uleb(b, p)
                v = (a, c)
            elif kind == 2:
                v, p = sleb(b, p)
            elif kind == 3:
                v, p = uleb(b, p)
                v = cstr(b, v)
            else:
                v, p = uleb(b, p)
            self.fields[tag] = v
            self.order.append((tag, kind))
        self.b = b

    def children(self):
        c = self.child
        while c:
            n = Node(self.b, c)
            yield n
            c = n.next

    def get(self, tag, default=None):
        return self.fields.get(tag, default)

    def ref(self, tag=0x24):
        v = self.fields.get(tag)
        return Node(self.b, v) if v else None


def sl_root(b):
    if b[:4] != b'sl00':
        raise ValueError('not sl00')
    root_off, size, zero = struct.unpack_from('<III', b, 4)
    return Node(b, root_off), size


def find_child(n, t):
    # sub_D8A0: first child whose type id == t
    for c in n.children():
        if c.type == t:
            return c
    return None


def type_str(n):
    if n is None:
        return '?'
    base = n.get(0x29)
    bn = {1: 'float'}.get(base, 'base%s' % base)
    if n.type == 0x13:
        k = n.get(0x2B, 1)
        return bn if k == 1 else '%s%d' % (bn, k)
    if n.type == 0x14:
        return '%s%dx%d' % (bn, n.get(0x2A), n.get(0x2B))
    if n.type == 0x17:
        return 'texture(kind %s) of %s' % (base, type_str(n.ref()))
    if n.type == 0x18:
        return 'cbuffer of %s' % (type_str(n.ref()))
    if n.type == 0x15:
        return 'struct %s (%d bytes)' % (n.get(0x1E), n.get(0x25, 0))
    return 'type%#x' % n.type


def dump_tree(n, ind, out):
    fs = ' '.join('%s=%s' % (TAG_NAMES.get(t, '%#x' % t),
                             ('@%#x' % n.fields[t]) if k == 4 else ('%#x' % n.fields[t]) if t == 3 else repr(n.fields[t]))
                  for t, k in n.order)
    out.append('%s@%#x %s [%s]' % ('  ' * ind, n.off, TYPE_NAMES.get(n.type, 'type%#x' % n.type), fs))
    for c in n.children():
        dump_tree(c, ind + 1, out)


def collect_structs(n, acc):
    """sub_1F9E30: follow field 0x24 through types 0x11/0x12/0x17/0x18; a type-0x15 node that has a
    name (0x1E) is appended once, then its children's 0x24 types are walked too."""
    while n is not None:
        if n.type in (0x11, 0x12, 0x17, 0x18):
            n = n.ref()
            continue
        if n.type == 0x15:
            if 0x1E in n.fields and n.off not in [x.off for x in acc]:
                acc.append(n)
            for c in n.children():
                collect_structs(c.ref(), acc)
        return


def program_struct(md_root):
    """The struct libScePsm turns into program uniforms for one stage (sub_1F82A0): walk every child of
    the resource list through sub_1F9E30 and take the FIRST collected struct."""
    shader = find_child(md_root, 2)
    rl = find_child(shader, 3) if shader else None
    acc = []
    if rl:
        for r in rl.children():
            collect_structs(r.ref(), acc)
    return acc[0] if acc else None


def program_uniforms(struct_node, stage_bit):
    """Uniform entries built by sub_1F82A0 from one struct, in member order. Name has a trailing
    "[...]" removed (strchr '[' when the name ends in ']'). entry: +16 size (0x25), +20 stage (1 = VS,
    2 = PS), +24 stride (= size), +28 count (= 1), +48 offset (0x21)."""
    out = []
    end = 0
    for m in struct_node.children():
        name = m.get(0x1E, '')
        if len(name) >= 2 and name.endswith(']') and '[' in name:
            name = name[:name.index('[')]
        size = m.get(0x25, 0)
        off = m.get(0x21, 0)
        out.append({'name': name, 'size': size, 'offset': off, 'stage': stage_bit, 'type': type_str(m.ref())})
        end = size + off       # v268 = size + offset of the LAST member -> stage CB byte size
    return out, end


def reflect(md):
    root, size = sl_root(md)
    shader = find_child(root, 2)
    rl = find_child(shader, 3) if shader else None
    res = {'root': root, 'size': size, 'shader': shader, 'cbuffers': [], 'textures': [], 'samplers': [],
           'inputs': [], 'outputs': [], 'other': []}
    if rl:
        for r in rl.children():
            {7: res['cbuffers'], 0x1A: res['textures'], 8: res['samplers'], 4: res['inputs'],
             5: res['outputs']}.get(r.type, res['other']).append(r)
    return res

# ----------------------------------------------------------------------------------- printing


def describe_blob(path, tree=False):
    d = open(path, 'rb').read()
    secs = elf_sections(d)
    o = []
    o.append('=' * 100)
    o.append('%s  (%d bytes, ELF e_machine %#x, e_type %d)' % (os.path.basename(path), len(d),
             struct.unpack_from('<H', d, 18)[0], struct.unpack_from('<H', d, 16)[0]))
    o.append('  sections:')
    for s in secs:
        if s['name']:
            o.append('    %-15s off=%#06x size=%#06x align=%d' % (s['name'], s['off'], s['size'], s['align']))
    h = parse_header(section(d, secs, '.shader_header'))
    o.append('  .shader_header: magic=%#x version=%#x type=%d (%s) header_size=%d shader_size=%d '
             '+0x48=%d +0x4C=%d +0x54=%d +0x58=%d'
             % (h['magic'], h['version'], h['type'], SHADER_TYPES_SEEN.get(h['type'], '?'), h['header_size'],
                h['shader_size'], h['h48'], h['h4c'], h['h54'], h['h58']))
    o.append('    cx_registers (%d): %s' % (h['num_cx'], ' '.join('%#x=%#x' % r for r in h['cx_regs'])))
    o.append('    sh_registers (%d): %s' % (h['num_sh'], ' '.join('%#x=%#x' % r for r in h['sh_regs'])))
    for nm, lst in (('input', h['in']), ('output', h['out'])):
        for i, v in enumerate(lst):
            o.append('    %s semantic[%d] = %#010x  %s' % (nm, i, v, semantic_fields(v)))
    base = USER_SGPR_BASE.get(h['type'])
    if 'direct' in h:
        o.append('    user data: +0x28=%d direct1_size_dw=%d (user-data dword 0 = s%s, observed in ISA)'
                 % (h['ud_28'], h['direct1_dw'], base if base is not None else '?'))
        for i, v in enumerate(h['direct']):
            if v != 0xFFFF:
                note = ' (vertex-buffer descriptor table pointer: sub_B010 calls sub_156260(shader, 8) and '\
                       'fills entry i from vertex-buffer slot i)' if i == 8 else ''
                o.append('      direct resource %d -> dword %d%s%s' % (i, v, ' = s[%d:%d]' % (base + v, base + v + 1)
                                                                       if base is not None else '', note))
        for k, lst in enumerate(h['sharp']):
            for slot, v in enumerate(lst):
                if v == 0xFFFF:
                    continue
                dw, small = v & 0x7FFF, v >> 15
                n = 4 if small else 8
                sg = ' = s[%d:%d]' % (base + dw, base + dw + n - 1) if base is not None else ''
                o.append('      %s%d -> dword %d, %d dwords%s' % (SHARP_CLASSES[k][0], slot, dw, n, sg))
    md = section(d, secs, '.metadata')
    if md is None:
        o.append('  (no .metadata)')
        return '\n'.join(o)
    r = reflect(md)
    sh = r['shader']
    o.append('  .metadata: sl00, %d bytes, root @%#x' % (r['size'], r['root'].off))
    if sh:
        o.append('    shader: hash=%#010x compiler=%r other=%s' % (
            sh.get(3, 0), sh.get(0x4A), ' '.join('%s=%s' % (TAG_NAMES.get(t, '%#x' % t), sh.fields[t])
                                                  for t, k in sh.order if t not in (3, 0x4A))))
    first = program_struct(r['root'])
    o.append('    constant buffers:')
    for cb in r['cbuffers']:
        t = cb.ref()
        st = t.ref() if t is not None and t.type == 0x18 else t
        mark = '   <- program uniforms (first struct, sub_1F82A0)' if first is not None and st is not None and \
            st.off == first.off else ''
        o.append('      slot %d  %-22s %d bytes%s' % (cb.get(0x1B, -1), cb.get(0x1E), st.get(0x25, 0) if st else -1, mark))
        if st is not None:
            for m in st.children():
                o.append('        +%-4d %-26s %-10s %3d bytes%s' % (m.get(0x21, 0), m.get(0x1E), type_str(m.ref()),
                                                                     m.get(0x25, 0), '' if m.get(0x27) else '  (unused: no 0x27 flag)'))
    o.append('    textures:')
    for t in r['textures']:
        o.append('      slot %d  %-22s %s' % (t.get(0x1B, -1), t.get(0x1E), type_str(t.ref())))
    o.append('    samplers:')
    for t in r['samplers']:
        o.append('      slot %d  %s' % (t.get(0x1B, -1), t.get(0x1E)))
    for title, lst in (('inputs', r['inputs']), ('outputs', r['outputs'])):
        o.append('    %s:' % title)
        for t in lst:
            extra = ' '.join('%s=%s' % (TAG_NAMES.get(k, '%#x' % k), t.fields[k]) for k, kk in t.order
                             if k in (0x6A, 0x6B))
            o.append('      index %-2s %-18s %-9s sem_index=%-2s sysval?=%-4s %-8s %s' % (
                t.get(0x1B, '-'), t.get(0x1E), t.get(0x1F, '-'), t.get(0x1A, '-'), t.get(0x19, '-'),
                type_str(t.ref()), extra))
    for t in r['other']:
        o.append('    other resource: %s' % TYPE_NAMES.get(t.type, t.type))
    if tree:
        o.append('    tree:')
        lines = []
        dump_tree(r['root'], 3, lines)
        o.extend(lines)
    return '\n'.join(o)

# ---------------------------------------------------------------------------- program table


def elf_vaddr_reader(path):
    d = open(path, 'rb').read()
    phoff, = struct.unpack_from('<Q', d, 0x20)
    phes, phn = struct.unpack_from('<HH', d, 0x36)
    segs = []
    for i in range(phn):
        t, f, off, va, pa, fs, ms, al = struct.unpack_from('<IIQQQQQQ', d, phoff + i * phes)
        if t == 1:
            segs.append((va, fs, off))

    def read(va, n):
        for v, fs, off in segs:
            if v <= va < v + fs:
                return d[off + va - v: off + va - v + n]
        raise KeyError(hex(va))
    return read


def enum_names(cs_path):
    names = {}
    if not cs_path:
        return names
    txt = open(cs_path, encoding='utf-8', errors='replace').read()
    m = re.search(r'enum EmbeddedShader\s*:\s*uint\s*\{(.*?)\}', txt, re.S)
    if not m:
        return names
    for line in m.group(1).splitlines():
        mm = re.match(r'\s*(\w+)\s*=\s*(\d+)u', line)
        if mm and int(mm.group(2)) not in names:
            names[int(mm.group(2))] = mm.group(1)
    return names


def describe_programs(psm_elf, blobs_dir, cs_path):
    """icall ShaderManager::LoadShaderNative (0x2d4c90): EmbeddedShader id < 0x46 -> (vs, ps) =
    dword pair at 0x461ED0; sub_2EF10 -> blob table 0x4654E0 {name, data, size}[72]; sub_1FAF80 builds
    the program. sub_1F82A0 walks the PS first, then the VS."""
    read = elf_vaddr_reader(psm_elf)
    pairs = [struct.unpack('<II', read(0x461ED0 + 8 * i, 8)) for i in range(0x46)]
    files = {}
    for f in os.listdir(blobs_dir):
        m = re.match(r'(\d+)_(.*)\.ags\.bin$', f)
        if m:
            files[int(m.group(1))] = (m.group(2), os.path.join(blobs_dir, f))
    names = enum_names(cs_path)
    o = ['=' * 100, 'EmbeddedShader programs (libScePsm 0x461ED0; uniform index order as built by sub_1F82A0)']
    for e, (vs, ps) in enumerate(pairs):
        o.append('%2d %-38s vs=%2d %-24s ps=%2d %s' % (e, names.get(e, '?'), vs, files[vs][0], ps, files[ps][0]))
        unis = []
        cb_sizes = {}
        err = None
        for stage_bit, sid in ((2, ps), (1, vs)):
            d = open(files[sid][1], 'rb').read()
            secs = elf_sections(d)
            h = parse_header(section(d, secs, '.shader_header'))
            if not h.get('sharp') or not h['sharp'][3]:
                continue      # sub_1F82A0: metadata walk only when sharp count[3] (+52) != 0
            st = program_struct(sl_root(section(d, secs, '.metadata'))[0])
            if st is None:
                continue
            lst, end = program_uniforms(st, stage_bit)
            cb_sizes['PS' if stage_bit == 2 else 'VS'] = end
            for u in lst:
                if any(x['name'] == u['name'] for x in unis):
                    err = u['name']
                    break
                unis.append(u)
            if err:
                break
        if err:
            o.append('     LOAD FAILS: "VsShader and PsShader cannot share uniform variable of the same name \'%s\'"' % err)
            continue
        o.append('     stage CB0 sizes: %s' % ', '.join('%s=%d' % kv for kv in cb_sizes.items()))
        for i, u in enumerate(unis):
            o.append('     [%d] %-26s %s CB0 +%-3d %3d bytes  %s' % (i, u['name'], 'PS' if u['stage'] == 2 else 'VS',
                                                                     u['offset'], u['size'], u['type']))
    return '\n'.join(o)


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('blobs', nargs='+')
    ap.add_argument('--tree', action='store_true', help='also dump the raw sl00 tree')
    ap.add_argument('--psm-elf', help='libScePsm_dyn.elf, to print the EmbeddedShader program table')
    ap.add_argument('--core-cs', help='decompiled Sce.PlayStation.Core.cs, for the EmbeddedShader names')
    a = ap.parse_args()
    paths = []
    for p in a.blobs:
        if os.path.isdir(p):
            paths += sorted(os.path.join(p, f) for f in os.listdir(p) if f.endswith('.ags.bin'))
        else:
            paths.append(p)
    for p in paths:
        print(describe_blob(p, a.tree))
    if a.psm_elf:
        print(describe_programs(a.psm_elf, os.path.dirname(paths[0]), a.core_cs))


if __name__ == '__main__':
    main()
