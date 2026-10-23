"""Check whether managed images (plain .dll/.exe or PE embedded in .sprx) carry real IL bodies.
usage: python ilcheck.py <dir> [<dir>...]"""
import sys, os, struct, glob, io, collections
import dnfile

def extract_pe(b):
    i = b.find(b'MZ\x90\x00')
    while i >= 0:
        try:
            pe_off = struct.unpack_from('<I', b, i + 0x3c)[0]
            if b[i + pe_off:i + pe_off + 4] == b'PE\0\0':
                return b[i:]
        except Exception: pass
        i = b.find(b'MZ\x90\x00', i + 1)
    return None

def check(data):
    pe = dnfile.dnPE(data=data)
    md = getattr(pe.net.mdtables, 'MethodDef', None)
    rows = md.rows if md else []
    rv = [r.Rva for r in rows if r.Rva]
    sizes = collections.Counter()
    for r in set(rv):
        try:
            h = pe.get_data(r, 12)
            sz = h[0] >> 2 if h[0] & 3 == 2 else struct.unpack_from('<I', h, 4)[0]
        except Exception: sz = -1
        sizes[sz] += 1
    return len(rows), len(rv), len(set(rv)), sum(1 for s in sizes if s > 1)

dirs = sys.argv[1:] if __name__ == '__main__' else []   # compare_sources imports extract_pe only
for d in dirs:
    files = sorted(glob.glob(os.path.join(d, '*.dll')) + glob.glob(os.path.join(d, '*.exe')) +
                   glob.glob(os.path.join(d, '*.dll.sprx')) + glob.glob(os.path.join(d, '*.exe.sprx')))
    agg = [0, 0, 0, 0, 0, 0]
    real_files = []
    for f in files:
        b = open(f, 'rb').read()
        if f.endswith('.sprx'):
            b = extract_pe(b)
            if b is None: continue
        try:
            n, wb, distinct, nontriv = check(b)
        except Exception as e:
            continue
        agg[0] += 1; agg[1] += n; agg[2] += wb; agg[3] += distinct
        if distinct > 1: real_files.append((os.path.basename(f), distinct))
    print(f"{d}\n  images={agg[0]} methods={agg[1]} withRVA={agg[2]} distinctBodies={agg[3]} imagesWith>1distinctBody={len(real_files)} {real_files[:8]}")
