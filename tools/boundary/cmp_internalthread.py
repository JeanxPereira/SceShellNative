"""Compare the field layout (name:signature) of System.Threading.InternalThread / Thread between the PS5 mscorlib
and a desktop Mono 6.8 mscorlib (the runtime reads InternalThread fields by offset, so they must match).
usage: python cmp_internalthread.py [<ps5 mscorlib.dll> <desktop mono 4.5 mscorlib.dll>]
defaults: $SPIKE_ROOT/mscorlib.dll (or $SPIKE_ROOT/common/mscorlib.dll) and $SPIKE_DESKTOP_MSCORLIB"""
import dnfile, sys, os
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import bconfig
def fields(p, tname):
    pe = dnfile.dnPE(p); t = pe.net.mdtables
    for td in t.TypeDef.rows:
        if str(td.TypeNamespace) + '.' + str(td.TypeName) == tname:
            return [str(f.row.Name) + ":" + str(f.row.Signature.value.hex()) for f in (td.FieldList or [])]
if len(sys.argv) > 2:
    ps5, desktop = sys.argv[1], sys.argv[2]
else:
    ps5 = os.path.join(bconfig.ROOT, 'common', 'mscorlib.dll')
    if not os.path.exists(ps5): ps5 = os.path.join(bconfig.ROOT, 'mscorlib.dll')
    desktop = os.environ.get('SPIKE_DESKTOP_MSCORLIB', '')
    if not desktop: sys.exit('set SPIKE_DESKTOP_MSCORLIB to the Mono 6.8 lib/mono/4.5/mscorlib.dll or pass both paths')
for tn in ['System.Threading.InternalThread', 'System.Threading.Thread']:
    a = fields(ps5, tn)
    b = fields(desktop, tn)
    print(tn, 'ps5', len(a), 'desktop', len(b))
    print('  only ps5:', [x for x in a if x not in b]); print('  only desktop:', [x for x in b if x not in a])
