"""Attribute P/Invoke targets to providers: by NID export in PS5 sprx/system libs, or by plain-name string in eboot.bin
(eboot exports nothing -> it must register a name table / fallback).  usage: SPIKE_TAG=.. python attribute_pinvokes.py [<dirs/files...>]
(default: the ShellUI eboot.bin + $SPIKE_KEK/system/common/lib, system/priv/lib, system_ex/common_ex/lib)"""
import sys, os, json, glob, collections, re
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import asmmodel, bconfig
from nid import nid
from ps5elf import read_syms
tag = asmmodel.TAG
inv = json.load(open(bconfig.out(f'inventory_{tag}.json')))
bins = []
for d in (sys.argv[1:] or [bconfig.EBOOT] + bconfig.SYSTEM_LIB_DIRS):
    if os.path.isdir(d):
        bins += [f for f in glob.glob(os.path.join(d, '*')) if (f.endswith('.sprx') or f.endswith('.prx') or f.endswith('.elf') or f.endswith('.bin') or f.endswith('.self')) and '.dll.' not in f and '.exe.' not in f]
    else: bins.append(d)
exports = {}
strings_eboot = None
for f in bins:
    try:
        r = read_syms(f)
    except Exception: r = None
    if r:
        for n in r[0]: exports.setdefault(n, []).append(os.path.basename(f))
ebootp = [f for f in bins if f.endswith('eboot.bin')]
eb = open(ebootp[0], 'rb').read() if ebootp else b''
def in_eboot(name):
    return (b'\0' + name.encode() + b'\0') in eb
res = []
by_lib = collections.defaultdict(collections.Counter)
uniq = {}
for p in inv['pinvokes']:
    uniq.setdefault((p['lib'], p['entry']), p)
for (lib, ent), p in uniq.items():
    prov = exports.get(nid(ent))
    if prov: where = 'export:' + '+'.join(sorted(set(prov))[:2])
    elif in_eboot(ent): where = 'eboot-string'
    else: where = 'unresolved'
    p['provider'] = where; res.append(p)
    by_lib[lib][where.split(':')[0] if where.startswith('export') else where] += 1
    by_lib[lib]['_prov:' + where] += 1
json.dump(res, open(bconfig.out(f'pinvokes_attributed_{tag}.json'), 'w'), indent=1)
print('unique (lib,entry):', len(res), 'binaries scanned', len(bins), 'exported NIDs', len(exports))
tot = collections.Counter(r['provider'].split(':')[0] if r['provider'].startswith('export') else r['provider'] for r in res)
print('totals', dict(tot))
for lib, c in sorted(by_lib.items(), key=lambda x: -sum(v for k, v in x[1].items() if not k.startswith('_'))):
    provs = [k[6:] for k in c if k.startswith('_prov:')]
    print(f"  {lib:60s} export={c['export']:4d} eboot={c['eboot-string']:4d} unres={c['unresolved']:4d}  {provs[:3]}")
