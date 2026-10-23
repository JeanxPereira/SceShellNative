"""Inventory of managed->native boundary: InternalCall methods + P/Invokes, cross-checked with native icall name strings.
Usage: SPIKE_ROOT=... SPIKE_TAG=fw403|fw700 python inventory.py [<libScePsm.sprx> <libmonosgen.sprx>]
(defaults: both from $SPIKE_KEK/system_ex/common_ex/lib)
Writes $SPIKE_OUT/inventory_<tag>.json and $SPIKE_OUT/inventory_<tag>.txt
"""
import sys, os, re, json, collections
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import asmmodel, bconfig

ICALL_RE = re.compile(rb'([A-Za-z_][A-Za-z0-9_.`/+]*\.[A-Za-z0-9_`/+]+::[A-Za-z_.<>][A-Za-z0-9_.<>]*)(\(([^)\x00]*)\))?')

def native_icall_names(path):
    b = open(path, 'rb').read()
    out = set()
    for m in ICALL_RE.finditer(b):
        out.add(m.group(1).decode())
    return out

def method_fullname(A, mi):
    m = A['methods'][mi - 1]
    t = A['types'][m['type'] - 1]['full'] if m['type'] else '?'
    return t, m['name']

def classify_pinvoke(lib):
    l = lib.lower()
    if l in ('__internal',): return 'iv_eboot_internal'
    return None

def main():
    tag = asmmodel.TAG
    if len(sys.argv) > 2: psm, monosgen = sys.argv[1], sys.argv[2]
    else: psm, monosgen = (os.path.join(bconfig.COMMON_EX_LIB, 'libScePsm.sprx'),
                           os.path.join(bconfig.COMMON_EX_LIB, 'libmonosgen-2.0.sprx'))
    asms = asmmodel.load_all()
    psm_names = native_icall_names(psm)
    mono_names = native_icall_names(monosgen)
    icalls = []; pinv = []
    for an, A in asms.items():
        for i, m in enumerate(A['methods'], 1):
            if m['icall']:
                t, n = method_fullname(A, i)
                key = t + '::' + n
                icalls.append({'asm': an, 'type': t, 'method': n, 'key': key, 'nparams': m['nparams'],
                               'in_psm': key in psm_names, 'in_monosgen': key in mono_names})
            if i in A['implmap']:
                t, n = method_fullname(A, i)
                lib, ent = A['implmap'][i]
                pinv.append({'asm': an, 'type': t, 'method': n, 'lib': lib, 'entry': ent})
    # dedupe icalls by key (same icall may be declared in several assemblies, e.g. facades)
    by_key = collections.OrderedDict()
    for ic in icalls: by_key.setdefault(ic['key'], ic)
    res = {'icalls': icalls, 'pinvokes': pinv,
           'psm_icall_strings': sorted(psm_names), 'monosgen_icall_strings': sorted(mono_names)}
    json.dump(res, open(bconfig.out(f'inventory_{tag}.json'), 'w'), indent=1)
    # ---- text summary
    L = []
    def p(*a): L.append(' '.join(str(x) for x in a))
    def domain(ic):
        if ic['type'].startswith('Sce.') or ic['asm'].startswith('Sce.') or ic['asm'].startswith('ReactNative'): return 'sony'
        return 'mono'
    p(f'# inventory {tag}: {len(asms)} assemblies')
    p(f'icall declarations: {len(icalls)}  unique icall keys: {len(by_key)}')
    cnt = collections.Counter(domain(ic) for ic in by_key.values())
    p('by domain:', dict(cnt))
    sony = [ic for ic in by_key.values() if domain(ic) == 'sony']
    mono = [ic for ic in by_key.values() if domain(ic) == 'mono']
    p(f'sony icalls found as string in libScePsm: {sum(ic["in_psm"] for ic in sony)}/{len(sony)}; in libmonosgen: {sum(ic["in_monosgen"] for ic in sony)}')
    p(f'mono icalls found as string in libmonosgen: {sum(ic["in_monosgen"] for ic in mono)}/{len(mono)} (note: mono 6.8 icall table stores names split by class; low hit rate expected)')
    p(f'mono icalls found as string in libScePsm: {sum(ic["in_psm"] for ic in mono)}')
    psm_unmatched = sorted(set(psm_names) - set(by_key))
    p(f'libScePsm icall-like strings: {len(psm_names)}; not declared by any managed icall: {len(psm_unmatched)}')
    for x in psm_unmatched[:60]: p('   psm-only:', x)
    p('\n## icalls per assembly / namespace (unique keys)')
    per = collections.Counter((ic['asm'], ic['type'].rsplit('.', 1)[0] if '.' in ic['type'] else '') for ic in by_key.values())
    for (a, ns), c in sorted(per.items(), key=lambda x: (-x[1])):
        p(f'  {c:5d}  {a}  [{ns}]')
    p('\n## sony icalls NOT in libScePsm (implemented elsewhere: eboot/other prx?)')
    for ic in sony:
        if not ic['in_psm']: p('   ', ic['asm'], ic['key'])
    p('\n## P/Invoke per native lib')
    pl = collections.Counter(x['lib'] for x in pinv)
    for lib, c in pl.most_common(): p(f'  {c:5d}  {lib}')
    open(bconfig.out(f'inventory_{tag}.txt'), 'w', encoding='utf-8').write('\n'.join(L))
    print('\n'.join(L[:40]))

if __name__ == '__main__':
    main()
