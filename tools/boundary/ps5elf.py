"""Minimal PS5 ELF dynamic symbol reader.
read_syms(path) -> (exports, imports) as sets of NID strings; read_syms.last has libs/mods/needed/imports_by_lib."""
import struct

A64 = 'ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+-'
NUL = bytes([0])

def _dec(x):
    n = 0
    for ch in x: n = n * 64 + A64.index(ch)
    return n

def read_syms(path):
    b = open(path, 'rb').read()
    if b[:4] != b'\x7fELF': return None
    phoff = struct.unpack_from('<Q', b, 0x20)[0]; phnum = struct.unpack_from('<H', b, 0x38)[0]
    segs = []; dyn = None
    for i in range(phnum):
        pt, fl, off, va, pa, fsz, msz, al = struct.unpack_from('<IIQQQQQQ', b, phoff + i * 56)
        if pt == 2: dyn = (off, fsz)
        if pt == 1 and fsz: segs.append((va, off, fsz))
    if not dyn: return None
    def v2o(v):
        for va, off, fsz in segs:
            if va <= v < va + fsz: return off + (v - va)
        return None
    tags = []
    for j in range(dyn[1] // 16):
        t, v = struct.unpack_from('<qQ', b, dyn[0] + j * 16)
        if t == 0: break
        tags.append((t, v))
    td = {}
    for t, v in tags: td.setdefault(t, v)
    st = v2o(td[5]); sy = v2o(td[6]); ha = v2o(td[4])
    def sname(o):
        e = b.find(NUL, st + o); return b[st + o:e].decode('latin1')
    libs, mods, needed = {}, {}, []
    for t, v in tags:
        if t == 0x61000049: libs[v >> 48] = sname(v & 0xffffffff)
        if t == 0x61000045: mods[v >> 48] = sname(v & 0xffffffff)
        if t == 1: needed.append(sname(v))
    last = {'libs': libs, 'mods': mods, 'needed': needed, 'imports_by_lib': {}}
    read_syms.last = last
    nchain = struct.unpack_from('<I', b, ha + 4)[0]
    exp, imp = set(), set()
    for k in range(nchain):
        st_name, st_info, st_other, st_shndx, st_value, st_size = struct.unpack_from('<IBBHQQ', b, sy + k * 24)
        name = sname(st_name)
        if not name: continue
        parts = name.split('#')
        (exp if st_shndx != 0 else imp).add(parts[0])
        if st_shndx == 0 and len(parts) >= 3:
            try: ln = mods.get(_dec(parts[2]), '?') + '/' + libs.get(_dec(parts[1]), '?')
            except ValueError: ln = '?'
            last['imports_by_lib'].setdefault(ln, set()).add(parts[0])
    return exp, imp

if __name__ == '__main__':
    import sys
    for p in sys.argv[1:]:
        r = read_syms(p); print(p, r and (len(r[0]), 'exports', len(r[1]), 'imports'))
