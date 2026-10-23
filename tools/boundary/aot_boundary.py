"""Option (b) boundary: external imports (NIDs) of the ShellUI process image set (eboot + common/*.sprx), grouped by
providing PS5 system module/library.
Usage: python aot_boundary.py [<root>|-] [<comma-separated image names to keep>]
  <root> with native/eboot.bin, common/*.sprx, Application/*.sprx (FW 7.00 staging layout), or '-'/omitted for the
  kek dump: NPXS40087/eboot.bin + system_ex/common_ex/lib/*.sprx + NPXS40087/psm/Application/*.sprx (name = $SPIKE_TAG).
Writes $SPIKE_OUT/aot_boundary_<name>[_core].json"""
import sys, os, glob, json, collections
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import bconfig
from ps5elf import read_syms
root = sys.argv[1] if len(sys.argv) > 1 else '-'
only = set(sys.argv[2].split(',')) if len(sys.argv) > 2 else None
if root != '-':
    name = os.path.basename(os.path.normpath(root))
    files = [os.path.join(root, 'native', 'eboot.bin')] + sorted(glob.glob(os.path.join(root, 'common', '*.sprx'))) + sorted(glob.glob(os.path.join(root, 'Application', '*.sprx')))
else:
    name = bconfig.TAG
    files = [bconfig.EBOOT] + sorted(glob.glob(os.path.join(bconfig.COMMON_EX_LIB, '*.sprx'))) + sorted(glob.glob(os.path.join(bconfig.PSM_APPLICATION, '*.sprx')))
own_exports = set(); imports = collections.defaultdict(set); per_file = {}
mods_in_set = set()
for f in files:
    if only and os.path.basename(f) not in only: continue
    r = read_syms(f)
    if not r: continue
    own_exports |= r[0]
    last = read_syms.last
    base = os.path.basename(f).replace('.sprx', '')
    mods_in_set.add(base)
    n = 0
    for ln, nids in last['imports_by_lib'].items():
        imports[ln] |= nids; n += len(nids)
    per_file[os.path.basename(f)] = (len(r[0]), n)
ext = {}
for ln, nids in imports.items():
    mod = ln.split('/')[0]
    if mod in mods_in_set or mod.replace('lib', '', 1) in mods_in_set: continue
    e = nids - own_exports
    if e: ext[ln] = e
tot = sum(len(v) for v in ext.values())
print('images', len(per_file), 'external import NIDs (unique per lib):', tot, 'libs:', len(ext))
allext = set().union(*ext.values()) if ext else set()
print('unique external NIDs overall', len(allext))
for ln, v in sorted(ext.items(), key=lambda x: -len(x[1])): print(f'  {len(v):5d}  {ln}')
# AOT managed images only
aot = {k: v for k, v in per_file.items() if '.dll' in k or '.exe' in k}
print('AOT managed images:', len(aot), 'total imports from them:', sum(v[1] for v in aot.values()))
big = sorted(per_file.items(), key=lambda x: -x[1][1])[:15]
print('largest importers:', big)
json.dump({k: sorted(v) for k, v in ext.items()}, open(bconfig.out('aot_boundary_%s%s.json' % (name, '_core' if only else '')), 'w'), indent=1)
