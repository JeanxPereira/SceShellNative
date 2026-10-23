"""Static call graph over IL (RTA-style virtual dispatch) -> reachable icalls / P/Invokes.
SPIKE_ROOT/SPIKE_TAG select the assembly set (needs real IL: fw403/legacy).
usage: python callgraph.py <scenario>   scenarios (see build()): full, main_only, boot0, boot012 (default), boot_all,
       pui_core, shell_boot.  Needs $SPIKE_OUT/icalls_attributed_<tag>.json and pinvokes_attributed_<tag>.json.
"""
import sys, os, json, collections, time
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import asmmodel, bconfig

asms = asmmodel.load_all()
TAG = asmmodel.TAG
ANAMES = list(asms.keys())

# ---------- global indexes
by_full = collections.defaultdict(list)          # fullname -> [(asm, tidx)]
for an, A in asms.items():
    for ti, t in enumerate(A['types'], 1):
        by_full[t['full']].append((an, ti))

def T(key): return asms[key[0]]['types'][key[1] - 1]

_tr_cache = {}
def resolve_typeref(an, idx, depth=0):
    ck = (an, idx)
    if ck in _tr_cache: return _tr_cache[ck]
    A = asms[an]; tr = A['typerefs'][idx - 1]
    full = (tr['ns'] + '.' if tr['ns'] else '') + tr['name']
    res = None; sc = tr['scope']
    if sc and sc[0] == 'TypeRef':
        enc = resolve_typeref(an, sc[1], depth + 1)
        if enc: full = T(enc)['full'] + '/' + tr['name']
        target = enc[0] if enc else None
    elif sc and sc[0] == 'AssemblyRef':
        target = A['asmrefs'][sc[1] - 1]
    else:
        target = an
    res = find_type(full, target)
    _tr_cache[ck] = res
    return res

def find_type(full, target, depth=0):
    if target in asms:
        for k in by_full.get(full, []):
            if k[0] == target: return k
        fw = asms[target]['exported'].get(full.split('/')[0])
        if fw and depth < 5 and fw != target:
            return find_type(full, fw, depth + 1)
    c = by_full.get(full)
    return c[0] if c else None

def resolve_tdor(an, ci, depth=0):
    if not ci or depth > 5: return None
    tab, row = ci
    if row == 0: return None
    if tab == 'TypeDef': return (an, row)
    if tab == 'TypeRef': return resolve_typeref(an, row)
    if tab == 'TypeSpec':
        g = asms[an]['typespecs'][row - 1] if row - 1 < len(asms[an]['typespecs']) else None
        return resolve_tdor(an, g, depth + 1) if g else None
    return None

_base_cache = {}
def base_of(tk):
    if tk in _base_cache: return _base_cache[tk]
    r = resolve_tdor(tk[0], T(tk)['extends'])
    _base_cache[tk] = r
    return r

_sup_cache = {}
def supers(tk):
    """all supertypes incl. self (base chain + interfaces, transitive)"""
    if tk in _sup_cache: return _sup_cache[tk]
    out = set(); st = [tk]
    while st:
        x = st.pop()
        if x in out or x is None: continue
        out.add(x)
        b = base_of(x)
        if b: st.append(b)
        for ic in T(x)['ifaces']:
            r = resolve_tdor(x[0], ic)
            if r: st.append(r)
    _sup_cache[tk] = out
    return out

def methods_of(tk):
    A = asms[tk[0]]
    return [(tk[0], mi) for mi in T(tk)['methods_idx'] if 0 < mi <= len(A['methods'])]

def M(mk): return asms[mk[0]]['methods'][mk[1] - 1]

def find_method(tk, name, np, walk=True):
    seen = 0
    while tk and seen < 30:
        cands = [m for m in methods_of(tk) if M(m)['name'] == name and (np < 0 or M(m)['nparams'] == np)]
        if cands: return cands
        if not walk: return []
        tk = base_of(tk); seen += 1
    return []

def find_override(tk, name, np):
    """virtual impl lookup in instantiated type chain (incl explicit impls 'Ns.IFoo.Name')"""
    seen = 0
    while tk and seen < 30:
        for m in methods_of(tk):
            mm = M(m)
            if (mm['name'] == name or mm['name'].endswith('.' + name)) and (np < 0 or mm['nparams'] == np) and not mm['abstract']:
                return m
        tk = base_of(tk); seen += 1
    return None

_mr_cache = {}
def resolve_memberref(an, idx):
    ck = (an, idx)
    if ck in _mr_cache: return _mr_cache[ck]
    A = asms[an]; mr = A['memberrefs'][idx - 1]
    sig = mr['sig']; res = ('none', None, None)
    cls = mr['class']
    if sig and sig[0] == 0x06:  # field
        tk = resolve_tdor(an, cls)
        res = ('field', tk, None)
    else:
        np = asmmodel.sig_paramcount(sig)
        if cls and cls[0] == 'MethodDef':
            res = ('method', None, [(an, cls[1])])
        else:
            tk = resolve_tdor(an, cls)
            if tk:
                ms = find_method(tk, mr['name'], np)
                res = ('method', tk, ms, mr['name'], np)
            else:
                res = ('unresolved', None, None, mr['name'])
    _mr_cache[ck] = res
    return res

def resolve_method_token(an, tok):
    tab = tok >> 24; row = tok & 0xFFFFFF
    if tab == 0x06: return [(an, row)], (an, M((an, row))['type']) if row <= len(asms[an]['methods']) else None
    if tab == 0x0A:
        r = resolve_memberref(an, row)
        if r[0] == 'method': return r[2] or [], r[1]
        return [], None
    if tab == 0x2B:
        ms = asms[an]['methodspecs'][row - 1] if row - 1 < len(asms[an]['methodspecs']) else None
        if ms: return resolve_method_token(an, ({'MethodDef': 0x06, 'MemberRef': 0x0A}[ms[0]] << 24) | ms[1])
    return [], None

def type_of_token(an, tok):
    tab = tok >> 24; row = tok & 0xFFFFFF
    return resolve_tdor(an, ({0x02: 'TypeDef', 0x01: 'TypeRef', 0x1B: 'TypeSpec'}.get(tab, 'x'), row))

def field_owner(an, tok):
    tab = tok >> 24; row = tok & 0xFFFFFF
    if tab == 0x04:
        fo = asms[an]['fields_owner']
        return (an, fo[row]) if row < len(fo) and fo[row] else None
    if tab == 0x0A:
        r = resolve_memberref(an, row)
        return r[1] if r[0] == 'field' else None
    return None

_us = {}
def user_string(an, tok):
    if an not in _us:
        import dnfile
        _us[an] = dnfile.dnPE(asms[an]['path'])
    try:
        v = _us[an].net.user_strings.get(tok & 0xFFFFFF)
        v = getattr(v, 'value', v)
        return v if isinstance(v, str) else (v.decode('utf-16le', 'replace') if isinstance(v, bytes) else None)
    except Exception:
        return None

# ---------- RTA worklist
class Graph:
    def __init__(self, exclude_asm_prefixes=()):
        self.reach = {}            # mk -> parent mk (for path)
        self.work = collections.deque()
        self.inst = set()
        self.inst_by_super = collections.defaultdict(set)
        self.vcalls = collections.defaultdict(set)   # super tk -> {(name,np)}
        self.touched = set()
        self.unresolved = collections.Counter()
        self.excl = exclude_asm_prefixes
        self.strseed = True; self.seen_str = set(); self.strseeded = []; self.allow_str = None
    def allowed(self, mk):
        return not any(mk[0].startswith(p) for p in self.excl)
    def add(self, mk, parent=None):
        if mk in self.reach or not self.allowed(mk): return
        self.reach[mk] = parent
        self.work.append(mk)
        self.touch((mk[0], M(mk)['type']), mk)
    def touch(self, tk, parent):
        if not tk or not tk[1] or tk in self.touched: return
        self.touched.add(tk)
        for m in find_method(tk, '.cctor', -1, walk=False): self.add(m, parent)
    def instantiate(self, tk, parent):
        if not tk or tk in self.inst: return
        self.inst.add(tk); self.touch(tk, parent)
        for s in supers(tk):
            self.inst_by_super[s].add(tk)
            for (n, np) in list(self.vcalls.get(s, ())):
                o = find_override(tk, n, np)
                if o: self.add(o, parent)
        # finalizer
        o = find_override(tk, 'Finalize', 0)
        if o: self.add(o, parent)
    def vcall(self, decl_tk, name, np, parent):
        if (name, np) in self.vcalls[decl_tk]: return
        self.vcalls[decl_tk].add((name, np))
        for t in list(self.inst_by_super.get(decl_tk, ())):
            o = find_override(t, name, np)
            if o: self.add(o, parent)
    def run(self):
        while self.work:
            mk = self.work.popleft(); m = M(mk); an = mk[0]
            for op, tok in (m['calls'] or []):
                if op in ('call', 'callvirt', 'newobj', 'jmp', 'ldftn', 'ldvirtftn'):
                    targets, decl = resolve_method_token(an, tok)
                    if not targets:
                        self.unresolved[(an, hex(tok))] += 1; continue
                    for t in targets:
                        tm = M(t)
                        if op == 'newobj':
                            self.instantiate((t[0], tm['type']), mk)
                        if op in ('callvirt', 'ldvirtftn') and (tm['virtual'] or tm['abstract']):
                            self.vcall((t[0], tm['type']), tm['name'], tm['nparams'], mk)
                            if not tm['abstract']: self.add(t, mk)
                        else:
                            self.add(t, mk)
                elif op in ('ldsfld', 'ldsflda', 'stsfld'):
                    self.touch(field_owner(an, tok), mk)
                elif op in ('stfld', 'ldflda'):
                    fo = field_owner(an, tok)
                    if fo: self.instantiate(fo, mk)   # value types / state machines (conservative)
                elif op in ('box', 'initobj', 'ldtoken'):
                    self.instantiate(type_of_token(an, tok), mk)
                elif op == 'ldstr' and self.strseed:
                    sv = user_string(an, tok)
                    if sv and sv not in self.seen_str:
                        self.seen_str.add(sv)
                        for cand in (sv, sv.rsplit('.', 1)[0] if '.' in sv else None):
                            if cand and (cand + '.AssemblyEntry') in by_full and (self.allow_str is None or cand in self.allow_str):
                                for m in find_methods_by_name(cand + '.AssemblyEntry', 'Start'):
                                    if m not in self.reach: self.strseeded.append((sv, mkey_name(m)))
                                    self.add(m, mk)
                elif op == 'constrained':
                    self.instantiate(type_of_token(an, tok), mk)

def mkey_name(mk):
    m = M(mk); t = asms[mk[0]]['types'][m['type'] - 1]['full'] if m['type'] else '?'
    return f'{t}::{m["name"]}'

def find_methods_by_name(full_type, mname=None):
    out = []
    for tk in by_full.get(full_type, []):
        for m in methods_of(tk):
            if mname is None or M(m)['name'] == mname: out.append(m)
    return out

def boot_registry_seeds(timings):
    """the RunItems from app.exe BootRegistry (decompiled) -> (assembly, interfaceName, timing)"""
    items = [("Sce.Vsh.ShellUI.AppSystem", "Sce.Vsh.ShellUI.AppSystem.BeforeRun", 0),
             ("Sce.Vsh.ShellUI.Legacy", "Sce.Vsh.ShellUI.Legacy.BeforeRun", 0),
             ("Sce.Vsh.ShellUI.ReactNativeShellApp", "Sce.Vsh.ShellUI.ReactNativeShellApp.AfterLaunch", 1)]
    for f in ["Initialize", "EvalBGLayer", "EvalBase", "EvalMainOnStandby", "EvalRegMgrInitOK", "EvalBackupRestore", "EvalSelectResolution",
              "EvalInitialSetup", "EvalAutoMounterBootStart", "EvalCrashReport", "EvalPowerOffWarning", "EvalCreateCronosUser",
              "EvalRemoteStorageManagement", "EvalDBRecovery", "EvalNotifyDBInitializeStarted", "EvalAutoStandbyAnnouncement", "EvalSystemUpdate",
              "EvalHealthWarning", "EvalSystemPasscodeInput", "EvalNotifyDbInitialized", "EvalFarsightUI", "EvalMediaEventListener", "EvalDaemons",
              "EvalReadyGameBoot", "EvalMediaEventListenerEnabled", "Terminate"]:
        items.append(("Sce.Vsh.ShellUI.Legacy", "Sce.Vsh.ShellUI.AppMain.BootManager." + f, 2))
    for f, tm in [("BackgroundInitVisionEngine", 3), ("BackgroundInitFrameworkDependent", 3), ("BackgroundInitNetwork", 4), ("BackgroundInitIme", 4),
                  ("BackgroundInitBgft", 4), ("BackgroundInitCamera", 5), ("BackgroundInitVcsIf", 5), ("BackgroundInitBeforeBGLayer", 5),
                  ("BackgroundInitBeforeBasePlugin", 5), ("BackgroundInitCrashReport", 5), ("BackgroundInitDbRecovery", 5),
                  ("BackgroundInitSystemPasscode", 5), ("BackgroundInitBeforeFarsightUI", 5), ("BackgroundInitBackgroundAutoLogin", 6),
                  ("BackgroundInitBeforeReadyGameBoot", 6), ("BackgroundInitWebKit", 6), ("BackgroundInitDaemons", 6), ("BackgroundInitResetSurfaceRect", 6)]:
        items.append(("Sce.Vsh.ShellUI.Legacy", "Sce.Vsh.ShellUI.AppMain.BootManager." + f, tm))
    return [i for i in items if i[2] in timings]

def seed_interface_name(g, asm, iname, parent=None):
    """RunManager resolves InterfaceName by reflection: a type with that name (instantiate, all methods) or Type.Method"""
    hit = 0
    for tk in by_full.get(iname, []):
        g.instantiate(tk, parent)
        for m in methods_of(tk): g.add(m, parent); hit += 1
    if not hit and '.' in iname:
        t, mn = iname.rsplit('.', 1)
        for m in find_methods_by_name(t, mn): g.add(m, parent); hit += 1
    return hit

def seed_implementors(g, iface_full, in_asm=None):
    n = 0
    for itk in by_full.get(iface_full, []):
        for an, A in asms.items():
            if in_asm and an != in_asm: continue
            for ti in range(1, len(A['types']) + 1):
                if itk in supers((an, ti)) and (an, ti) != itk:
                    g.instantiate((an, ti), None); n += 1
    return n

def build(scenario):
    g = Graph()
    log = []
    main = find_methods_by_name('Sce.Vsh.ShellUI.Program', 'Main')
    if scenario == 'full':
        for m in main: g.add(m)
    elif scenario in ('main_only', 'boot0', 'boot012', 'boot_all'):
        for m in main: g.add(m)
        n = seed_implementors(g, 'Sce.Vsh.ShellUI.AppMain.Launcher.IRun', 'Sce.Vsh.ShellUI.AppSystem')
        log.append(f'IRun implementors instantiated: {n}')
        timings = {'main_only': set(), 'boot0': {0}, 'boot012': {0, 1, 2}, 'boot_all': {0, 1, 2, 3, 4, 5, 6}}[scenario]
        for a, iname, tm in boot_registry_seeds(timings):
            h = seed_interface_name(g, a, iname)
            log.append(f'seed {iname} -> {h} methods')
    elif scenario.startswith('pui_core'):
        # framework-only: PUI Application.Create(config) -> ctor -> Run() (InitializeBeforeRun, Update/Render loop), no ShellUI plugins
        for tname in ['Sce.PlayStation.PUI.Application', 'Sce.PlayStation.PUIPlatform.PlatformApplication']:
            for tk in by_full.get(tname, []):
                g.instantiate(tk, None)
                for m in methods_of(tk):
                    if M(m)['name'] in ('.ctor', '.cctor', 'Create', 'Run'):
                        g.add(m); log.append('root ' + mkey_name(m))
    elif scenario == 'shell_boot':
        # Main + only the 3 assemblies named in app.exe BootRegistry (AppSystem incl. AppMain, Legacy, ReactNativeShellApp)
        for m in main: g.add(m)
        g.allow_str = {'Sce.Vsh.ShellUI.AppSystem', 'Sce.Vsh.ShellUI.Legacy', 'Sce.Vsh.ShellUI.ReactNativeShellApp'}
    if scenario.startswith('pui_core'): g.strseed = False
    g.run()
    log += [f'string-seeded {a} -> {b}' for a, b in g.strseeded]
    return g, log

def classify_native(mk):
    m = M(mk); A = asms[mk[0]]
    if m['icall']: return 'icall'
    if mk[1] in A['implmap']: return 'pinvoke'
    return None

if __name__ == '__main__':
    scen = sys.argv[1] if len(sys.argv) > 1 else 'boot012'
    t0 = time.time()
    g, log = build(scen)
    icall_attr = {r['key']: r['provider'] for r in json.load(open(bconfig.out(f'icalls_attributed_{TAG}.json')))}
    pin_attr = {(r['lib'], r['entry']): r['provider'] for r in json.load(open(bconfig.out(f'pinvokes_attributed_{TAG}.json')))}
    nat = []
    for mk in g.reach:
        c = classify_native(mk)
        if not c: continue
        name = mkey_name(mk)
        if c == 'icall':
            nat.append({'kind': 'icall', 'asm': mk[0], 'key': name, 'provider': icall_attr.get(name, '?')})
        else:
            lib, ent = asms[mk[0]]['implmap'][mk[1]]
            nat.append({'kind': 'pinvoke', 'asm': mk[0], 'key': name, 'lib': lib, 'entry': ent, 'provider': pin_attr.get((lib, ent), '?')})
    # dedupe
    seen = {}; 
    for n in nat: seen.setdefault(n['key'] if n['kind'] == 'icall' else (n['lib'], n['entry']), n)
    nat = list(seen.values())
    out = {'scenario': scen, 'tag': TAG, 'reachable_methods': len(g.reach), 'instantiated_types': len(g.inst),
           'reachable_by_asm': collections.Counter(mk[0] for mk in g.reach).most_common(),
           'unresolved_tokens': sum(g.unresolved.values()), 'log': log, 'native': nat,
           'secs': round(time.time() - t0, 1)}
    json.dump(out, open(bconfig.out(f'reach_{TAG}_{scen}.json'), 'w'), indent=1)
    print(f"[{scen}] reachable methods={out['reachable_methods']} inst types={out['instantiated_types']} unresolved call tokens={out['unresolved_tokens']} ({out['secs']}s)")
    ic = [n for n in nat if n['kind'] == 'icall']; pv = [n for n in nat if n['kind'] == 'pinvoke']
    print('  icalls reachable:', len(ic), dict(collections.Counter(n['provider'].split('+')[0] for n in ic)))
    print('  pinvokes reachable:', len(pv), dict(collections.Counter(n['provider'].split(':')[0] for n in pv)))
    print('  top asms:', out['reachable_by_asm'][:12])
