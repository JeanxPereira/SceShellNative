"""Attribute each declared icall to the native binary that contains its registration string;
mono runtime icalls are matched against upstream mono 2019-10 icall-def.h.
usage: SPIKE_TAG=.. python attribute_icalls.py [<dir-with-native-binaries> [extra binaries...]]
(default: $SPIKE_KEK/system_ex/common_ex/lib and the ShellUI eboot.bin)
needs $SPIKE_OUT/icall-def-2019-10.h (override with SPIKE_ICALL_DEF):
  curl -o icall-def-2019-10.h https://raw.githubusercontent.com/mono/mono/2019-10/mono/metadata/icall-def.h"""
import sys, os, re, json, glob, collections
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import asmmodel, bconfig
from inventory import native_icall_names
tag = asmmodel.TAG
inv = json.load(open(bconfig.out(f'inventory_{tag}.json')))
# upstream mono icall table
txt = open(os.environ.get('SPIKE_ICALL_DEF', bconfig.out('icall-def-2019-10.h'))).read()
mono_keys = set(); cur = None
for line in txt.splitlines():
    m = re.match(r'\s*ICALL_TYPE\s*\(\s*\w+\s*,\s*"([^"]+)"', line)
    if m: cur = m.group(1); continue
    m = re.match(r'\s*(?:NOHANDLES\s*\(\s*)?(?:ICALL|HANDLES|HANDLES_REUSE_WRAPPER)\s*\(\s*\w+\s*,\s*"([^"]+)"', line)
    if m and cur: mono_keys.add(cur + '::' + m.group(1).split('(')[0])
bins = []
for d in (sys.argv[1:] or [bconfig.COMMON_EX_LIB, bconfig.EBOOT]):
    if os.path.isdir(d): bins += [f for f in glob.glob(os.path.join(d, '*')) if (f.endswith('.sprx') and '.dll' not in f and '.exe' not in f) or f.endswith('.bin') or f.endswith('.elf')]
    else: bins.append(d)
where = collections.defaultdict(list)
for b in bins:
    for n in native_icall_names(b): where[n].append(os.path.basename(b))
seen = {}
for ic in inv['icalls']:
    seen.setdefault(ic['key'], ic)
rows = []
cnt = collections.Counter()
for k, ic in seen.items():
    src = where.get(k, [])
    if k in mono_keys: cat = 'mono-runtime(icall-def.h)'
    elif src: cat = 'native:' + '+'.join(sorted(set(src)))
    else: cat = 'UNRESOLVED'
    ic['provider'] = cat
    cnt[cat] += 1
    rows.append(ic)
json.dump(rows, open(bconfig.out(f'icalls_attributed_{tag}.json'), 'w'), indent=1)
print('unique icalls', len(rows))
for c, n in cnt.most_common(): print(f'  {n:5d}  {c}')
un = [r for r in rows if r['provider'] == 'UNRESOLVED']
print('UNRESOLVED by assembly:', collections.Counter(r['asm'] for r in un).most_common(15))
print('sample unresolved:', [r['key'] for r in un][:25])
