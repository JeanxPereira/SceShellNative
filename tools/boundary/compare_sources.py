"""Compare key assemblies across sources: method counts, real bodies, version, and method-set overlap vs FW7.00 stripped set.
usage: python compare_sources.py [assembly file names...]
Sources (each optional, skipped when the directory does not exist):
  SPIKE_FW700_ROOT  stripped FW 7.00 managed set with common/ and Application/ (the reference method set)
  SPIKE_ROOT        managed IL set (default work/il403, flat or common/+Application/)
  SPIKE_EXTRACTED   another directory of plain PEs to compare
  SPIKE_KEK         kek dump: system_ex/common_ex/lib (*.sprx, PE extracted on the fly)"""
import sys, os, glob, struct
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import dnfile
import bconfig
from ilcheck import extract_pe

FW700 = os.environ.get('SPIKE_FW700_ROOT', '')
def _sub(root, name):
    p = os.path.join(root, name)
    return p if os.path.isdir(p) else root
SRC = {
  'fw700_stripped': _sub(FW700, 'common') if FW700 else '',
  'legacy':         _sub(bconfig.ROOT, 'common'),
  'extracted':      os.environ.get('SPIKE_EXTRACTED', ''),
  'kek':            bconfig.COMMON_EX_LIB,
}
APPSRC = {'fw700_stripped': _sub(FW700, 'Application') if FW700 else '', 'legacy': _sub(bconfig.ROOT, 'Application')}
SRC = {k: v for k, v in SRC.items() if v and os.path.isdir(v)}
KEY = sys.argv[1:] or ['mscorlib.dll', 'Sce.PlayStation.Core.dll', 'Sce.PlayStation.PUI.dll', 'Sce.PlayStation.PUIPlatform.dll',
       'ReactNative.PUI.dll', 'Sce.Vsh.ShellUI.ReactNativeShellApp.dll', 'Sce.Vsh.ShellUI.AppSystem.dll', 'Sce.Vsh.ShellUI.Legacy.dll', 'app.exe']

def load(path):
    for p in (path, path + '.sprx'):
        if os.path.exists(p):
            b = open(p, 'rb').read()
            if p.endswith('.sprx'): b = extract_pe(b)
            return dnfile.dnPE(data=b), p
    return None, None

def sigset(pe):
    t = pe.net.mdtables
    tds = t.TypeDef.rows; md = t.MethodDef.rows
    own = {}
    for td in tds:
        for m in (td.MethodList or []): own[m.row_index] = (str(td.TypeNamespace) + '.' + str(td.TypeName))
    out = set(); real = 0
    for i, r in enumerate(md, 1):
        out.add(own.get(i, '?') + '::' + str(r.Name))
    rvas = set(r.Rva for r in md if r.Rva)
    return out, len(md), len(rvas)

base = {}
for k in KEY:
    print('==', k)
    for name, d in list(SRC.items()) + []:
        dd = APPSRC.get(name, d) if k == 'app.exe' else d
        if k == 'app.exe' and name not in APPSRC: continue
        try:
            pe, p = load(os.path.join(dd, k))
        except Exception as e:
            print(f'  {name:16s} parse error {e}'); continue
        if not pe: print(f'  {name:16s} missing'); continue
        a = pe.net.mdtables.Assembly.rows[0]
        ss, n, distinct = sigset(pe)
        if name == 'fw700_stripped': base[k] = ss
        b = base.get(k, set())
        ov = len(ss & b) / len(b) * 100 if b else 0
        print(f'  {name:16s} v{a.MajorVersion}.{a.MinorVersion}.{a.BuildNumber}.{a.RevisionNumber} methods={n} distinctBodies={distinct} '
              f'coverOfFw700MethodSet={ov:.1f}% extra={len(ss-b)} mvid={pe.net.mdtables.Module.rows[0].Mvid}  [{os.path.basename(p)}]')
