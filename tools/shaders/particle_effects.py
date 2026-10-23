"""Parse the LightParticle effect blobs embedded in the ShellUI eboot (table 0xD60E10 names,
0xD60E50 data, 0xBB0D80 sizes) following sub_99AC0 / sub_99DC0 / sub_9C030 / sub_9C150.
usage: python -I tools/shaders/particle_effects.py <eboot.elf> [name]
"""
import struct, sys

d = open(sys.argv[1], "rb").read()
ph, = struct.unpack_from("<Q", d, 0x20); n, = struct.unpack_from("<H", d, 0x38)
segs = []
for i in range(n):
    o = ph + i * 56
    t, = struct.unpack_from("<I", d, o)
    off, va = struct.unpack_from("<QQ", d, o + 8); fs, = struct.unpack_from("<Q", d, o + 32)
    if t == 1: segs.append((va, fs, off))
def rd(va, k):
    for v, fs, off in segs:
        if v <= va < v + fs: return d[off + va - v: off + va - v + k]

SETV = [5, 6, 10, 11, 14, 16, 17, 18, 19, 20]
INTV = [7, 8, 12, 13, 15]
TYPES = {1: "u64", 2: "u32", 3: "u16", 4: "i64", 5: "i32", 6: "i16", 7: "f64", 8: "f32", 10: "u32mask", 11: "vsharp"}

class R:
    def __init__(s, b): s.b, s.p = b, 0
    def take(s, k):
        assert s.p + k <= len(s.b), "overrun"
        r = s.b[s.p:s.p + k]; s.p += k; return r
    def u32(s): return struct.unpack("<I", s.take(4))[0]

def parse(blob):
    r = R(blob)
    name = r.take(r.u32()).split(b"\0")[0].decode()
    ver = r.u32()
    hdr = [r.u32() for _ in range(8)]
    vec = {}
    for v in SETV + INTV: vec[v] = [dict() for _ in range(r.u32())]
    vec[9] = [dict() for _ in range(r.u32())]
    meta = [dict() for _ in range(r.u32())]
    for v in SETV:
        for e in vec[v]:
            e["time"], e["nt"], e["no"] = struct.unpack("<fII", r.take(12))
    for v in INTV:
        for e in vec[v]:
            e["start"], e["end"], e["nt"], e["no"] = struct.unpack("<ffII", r.take(16))
    for e in vec[9]:
        e["raw"] = r.take(16); e["nt"] = struct.unpack_from("<I", e["raw"], 12)[0]
    for e in meta:
        e["raw"] = r.take(36); e["l1"], e["l2"] = struct.unpack_from("<II", e["raw"], 28)
    for v in SETV + INTV + [9]:
        for e in vec[v]:
            e["targets"] = list(struct.unpack("<%di" % e["nt"], r.take(4 * e["nt"])))
    tail = struct.unpack("<5f", r.take(20))
    for v in SETV:
        for e in vec[v]:
            e["ops"] = []
            for _ in range(e["no"]):
                t, pad, val, off = struct.unpack("<IIQQ", r.take(24))
                e["ops"].append((t, pad, val, off))
    for v in INTV:
        for e in vec[v]:
            e["ops"] = []
            for _ in range(e["no"]):
                t, pad, a, b, off = struct.unpack("<IIQQQ", r.take(32))
                e["ops"].append((t, pad, a, b, off))
    for e in vec[9]:
        e["a0"], e["a8"], e["a4"] = r.u32(), r.u32(), r.u32()
    for e in meta:
        e["s1"] = r.take(e["l1"]); e["s2"] = r.take(e["l2"])
    return name, ver, hdr, vec, meta, tail, r.p

def fval(t, v):
    if t == 8: return "%r" % struct.unpack("<f", struct.pack("<I", v & 0xffffffff))[0]
    if t == 7: return "%r" % struct.unpack("<d", struct.pack("<Q", v))[0]
    return "0x%x" % v

# 0xD60E50 holds relocated pointers (the file has R_X86_64_RELATIVE slots): values from the analysed DB.
DATA = [0xBB0DC0, 0xBB2D70, 0xBB4B70, 0xBB72E0, 0xBB9B40, 0xBBC250, 0xBBEBB0]
for i in range(7):
    va = DATA[i]
    size, = struct.unpack("<Q", rd(0xBB0D80 + 8 * i, 8))
    blob = rd(va, size)
    name, ver, hdr, vec, meta, tail, used = parse(blob)
    want = len(sys.argv) < 3 or sys.argv[2] == name
    print("== blob %d %s @%x size %d used %d ver %d hdr %s tail %s" % (i, name, va, size, used, ver, hdr, tail))
    if not want: continue
    for v in SETV + INTV + [9]:
        for e in vec[v]:
            if v in SETV:
                print("  set[%d] t=%r targets=%s" % (v, e["time"], e["targets"]))
                for (t, pad, val, off) in e["ops"]:
                    print("      +%d %s = %s%s" % (off, TYPES.get(t, t), fval(t, val), "" if pad == 0 else " pad=%x" % pad))
            elif v in INTV:
                print("  interp[%d] %r..%r targets=%s" % (v, e["start"], e["end"], e["targets"]))
                for (t, pad, a, b, off) in e["ops"]:
                    print("      +%d %s %s -> %s" % (off, TYPES.get(t, t), fval(t, a), fval(t, b)))
            else:
                print("  v9 raw=%s targets=%s a=%x,%x,%x" % (e["raw"].hex(), e["targets"], e["a0"], e["a4"], e["a8"]))
    for e in meta:
        print("  meta %s %r %r" % (e["raw"][:28].hex(), e["s1"], e["s2"]))
