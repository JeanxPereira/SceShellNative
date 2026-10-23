"""Extract the .NET PE image embedded in a PS5 *.dll.sprx / *.exe.sprx (decrypted ELF).

The managed assembly is stored as a complete PE file inside one of the ELF's
segments. It is located by scanning for an 'MZ' whose e_lfanew points to a
'PE\0\0' signature with a CLI header (data directory 14) whose metadata
starts with 'BSJB'. The PE length is the end of its last raw section.

usage: python -I sprx_pe.py <in.sprx> <out.dll>
       python -I sprx_pe.py <in_dir> <out_dir>      (every *.dll.sprx / *.exe.sprx)
"""
import os, struct, sys


def find_pe(d):
    pos = 0
    while True:
        mz = d.find(b"MZ", pos)
        if mz < 0:
            return None
        pos = mz + 1
        if mz + 0x40 > len(d):
            continue
        lfanew = struct.unpack_from("<I", d, mz + 0x3C)[0]
        pe = mz + lfanew
        if lfanew > 0x1000 or d[pe:pe + 4] != b"PE\0\0":
            continue
        nsec = struct.unpack_from("<H", d, pe + 6)[0]
        optsz = struct.unpack_from("<H", d, pe + 20)[0]
        opt = pe + 24
        magic = struct.unpack_from("<H", d, opt)[0]
        dd = opt + (96 if magic == 0x10B else 112)
        cli_rva, cli_sz = struct.unpack_from("<II", d, dd + 14 * 8)
        if not cli_rva:
            continue
        secs = []
        so = opt + optsz
        for i in range(nsec):
            va, vsz = struct.unpack_from("<II", d, so + i * 40 + 12)[0], 0
            vsize, va, rawsz, rawptr = struct.unpack_from("<IIII", d, so + i * 40 + 8)
            secs.append((va, vsize, rawptr, rawsz))
        end = max(p + s for (_, _, p, s) in secs)
        img = d[mz:mz + end]
        if len(img) < end:
            continue
        def rva2off(r):
            for va, vs, p, s in secs:
                if va <= r < va + max(vs, s):
                    return r - va + p
            return None
        co = rva2off(cli_rva)
        if co is None:
            continue
        md_rva = struct.unpack_from("<I", img, co + 8)[0]
        mo = rva2off(md_rva)
        if mo is None or img[mo:mo + 4] != b"BSJB":
            continue
        return img
    

def main():
    src, dst = sys.argv[1], sys.argv[2]
    pairs = []
    if os.path.isdir(src):
        os.makedirs(dst, exist_ok=True)
        for n in sorted(os.listdir(src)):
            if n.endswith(".dll.sprx") or n.endswith(".exe.sprx"):
                pairs.append((os.path.join(src, n), os.path.join(dst, n[:-5])))
    else:
        pairs.append((src, dst))
    for s, o in pairs:
        img = find_pe(open(s, "rb").read())
        if img is None:
            print("no PE  ", os.path.basename(s)); continue
        open(o, "wb").write(img)
        print("%8d %s" % (len(img), os.path.basename(o)))


if __name__ == "__main__":
    main()
