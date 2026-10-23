"""Check whether the loaded assembly set carries real IL bodies or only stubs
(stripped FW 7.00 images: every method RVA points at the same 1-byte 'ret' body).
usage: SPIKE_ROOT=.. SPIKE_TAG=.. python stubstats.py"""
import os, sys, collections, dnfile; sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import asmmodel
asms = asmmodel.load_all()
tot = real = 0; hist = collections.Counter(); distinct = collections.Counter()
for n, A in asms.items():
    wb = [m for m in A['methods'] if m['rva']]
    tot += len(wb); real += sum(1 for m in wb if m.get('ilsize', 0) > 1)
    for m in wb: hist[m.get('ilsize')] += 1
    distinct[len(set(m['rva'] for m in wb))] += 1
print("assemblies", len(asms), "methods", sum(len(a['methods']) for a in asms.values()))
print("methods with RVA", tot, "with IL > 1 byte", real)
print("ilsize histogram", hist.most_common(5))
print("distinct-RVA-per-assembly histogram", distinct.most_common(5))
for f in [os.path.join('Application', 'app.exe'), os.path.join('common', 'mscorlib.dll'), os.path.join('common', 'ReactNative.PUI.dll')]:
    p = os.path.join(asmmodel.ROOT, f)
    if not os.path.exists(p): p = os.path.join(asmmodel.ROOT, os.path.basename(f))  # flat layout
    pe = dnfile.dnPE(p)
    md = pe.net.mdtables.MethodDef.rows
    rv = [r.Rva for r in md if r.Rva]
    b = pe.get_data(rv[0], 4)
    print(f, "first body bytes", b.hex(), "distinct rvas", len(set(rv)))
