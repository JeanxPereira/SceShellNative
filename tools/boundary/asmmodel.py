"""Load .NET PE images (dnfile) into a plain-python model + IL decoder.
Spike code: throwaway.  Usage: from asmmodel import load_all
"""
import os, sys, glob, pickle, struct
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import dnfile
import bconfig

ROOT = bconfig.ROOT
TAG = bconfig.TAG
CACHE = bconfig.out("model_%s.pkl" % TAG)

def s(x):
    if x is None: return ""
    if hasattr(x, "value"):
        v = x.value
        return v.decode("utf-8", "replace") if isinstance(v, bytes) else str(v)
    return str(x)

def blob(x):
    if x is None: return b""
    v = getattr(x, "value", x)
    return bytes(v) if v is not None else b""

def cidx(ci):
    """coded index -> (table_name, row_index) or None"""
    if ci is None: return None
    t = getattr(ci, "table", None)
    if t is None: return None
    return (t.name, ci.row_index)

def decomp(b, i):
    a = b[i]
    if a & 0x80 == 0: return a, i + 1
    if a & 0xC0 == 0x80: return ((a & 0x3F) << 8) | b[i + 1], i + 2
    return ((a & 0x1F) << 24) | (b[i + 1] << 16) | (b[i + 2] << 8) | b[i + 3], i + 4

def sig_paramcount(b):
    try:
        if not b: return -1
        cc = b[0]; i = 1
        if cc & 0x10: _, i = decomp(b, i)
        n, i = decomp(b, i)
        return n
    except Exception:
        return -1

def typespec_generic_def(b):
    """TypeSpec blob: GENERICINST (0x15) CLASS/VALUETYPE TypeDefOrRef ... -> (table, row)"""
    try:
        if b and b[0] == 0x15 and b[1] in (0x12, 0x11):
            v, _ = decomp(b, 2)
            tag = v & 3; row = v >> 2
            return ({0: "TypeDef", 1: "TypeRef", 2: "TypeSpec"}[tag], row)
    except Exception:
        pass
    return None

def methodspec_inst_types(b):
    return None

# ---------------- IL opcode operand sizes ----------------
OP1 = {}
for o in range(0x00, 0xE1): OP1[o] = 0
for o in (0x0E, 0x0F, 0x10, 0x11, 0x12, 0x13, 0x1F, 0xDE): OP1[o] = 1
for o in range(0x2B, 0x38): OP1[o] = 1
for o in range(0x38, 0x45): OP1[o] = 4
for o in (0x20, 0x22, 0xDD): OP1[o] = 4
OP1[0x21] = 8; OP1[0x23] = 8
TOK1 = {0x27, 0x28, 0x29, 0x6F, 0x70, 0x71, 0x72, 0x73, 0x74, 0x75, 0x79, 0x7B, 0x7C, 0x7D, 0x7E, 0x7F,
        0x80, 0x81, 0x8C, 0x8D, 0x8F, 0xA3, 0xA4, 0xA5, 0xC2, 0xC6, 0xD0}
for o in TOK1: OP1[o] = 4
OP2 = {i: 0 for i in range(0x00, 0x23)}
for o in (0x06, 0x07, 0x15, 0x16, 0x1C): OP2[o] = 4
for o in (0x09, 0x0A, 0x0B, 0x0C, 0x0D, 0x0E): OP2[o] = 2
for o in (0x12, 0x19): OP2[o] = 1
NAMES1 = {0x28: "call", 0x6F: "callvirt", 0x73: "newobj", 0x27: "jmp", 0x8C: "box", 0x8D: "newarr",
          0x7E: "ldsfld", 0x7F: "ldsflda", 0x80: "stsfld", 0x7B: "ldfld", 0x7D: "stfld", 0x7C: "ldflda",
          0xD0: "ldtoken", 0x72: "ldstr", 0x74: "castclass", 0x75: "isinst", 0xA5: "unbox.any", 0x79: "unbox"}
NAMES2 = {0x06: "ldftn", 0x07: "ldvirtftn", 0x15: "initobj", 0x16: "constrained"}

def decode_il(code):
    """yield (opname, token) for token-bearing interesting ops"""
    i = 0; n = len(code); out = []
    while i < n:
        op = code[i]; i += 1
        if op == 0xFE:
            if i >= n: break
            op2 = code[i]; i += 1
            sz = OP2.get(op2, 0)
            if sz == 4 and op2 in NAMES2:
                out.append((NAMES2[op2], struct.unpack_from("<I", code, i)[0]))
            i += sz
        elif op == 0x45:
            cnt = struct.unpack_from("<I", code, i)[0]; i += 4 + 4 * cnt
        else:
            sz = OP1.get(op, 0)
            if sz == 4 and op in NAMES1:
                out.append((NAMES1[op], struct.unpack_from("<I", code, i)[0]))
            i += sz
    return out

def read_body(pe, rva):
    try:
        hdr = pe.get_data(rva, 12)
        if hdr[0] & 3 == 2:  # tiny
            size = hdr[0] >> 2
            return pe.get_data(rva + 1, size)
        flags = struct.unpack_from("<H", hdr, 0)[0]
        hsize = (flags >> 12) * 4
        codesize = struct.unpack_from("<I", hdr, 4)[0]
        return pe.get_data(rva + hsize, codesize)
    except Exception:
        return None

def rows(t,name):
    x=getattr(t,name,None)
    return (x.rows or []) if x is not None else []

def load_asm(path):
    pe = dnfile.dnPE(path, fast_load=False)
    t = pe.net.mdtables
    A = {"path": path, "file": os.path.basename(path)}
    A["name"] = s(rows(t,"Assembly")[0].Name) if rows(t,"Assembly") else os.path.splitext(A["file"])[0]
    asmrefs = [s(r.Name) for r in rows(t,"AssemblyRef")]
    A["asmrefs"] = asmrefs
    modrefs = [s(r.Name) for r in rows(t,"ModuleRef")]
    A["modrefs"] = modrefs
    # typedefs
    tds = []
    trows = rows(t,"TypeDef")
    mrows = rows(t,"MethodDef")
    nmeth = len(mrows)
    starts = []
    for r in trows:
        ml = r.MethodList
        starts.append(ml[0].row_index if ml else None)
    for k, r in enumerate(trows):
        tds.append({"ns": s(r.TypeNamespace), "name": s(r.TypeName), "flags": 0,
                    "extends": cidx(r.Extends), "methods": [], "ifaces": [], "enclosing": None,
                    "interface": bool(getattr(r.Flags, "tdInterface", False)),
                    "methods_idx": [m.row_index for m in (r.MethodList or [])]})
    if True:
        for r in rows(t,"NestedClass"):
            tds[r.NestedClass.row_index - 1]["enclosing"] = r.EnclosingClass.row_index
    def full(k, depth=0):
        d = tds[k - 1]
        if d["enclosing"] and depth < 10:
            return full(d["enclosing"], depth + 1) + "/" + d["name"]
        return (d["ns"] + "." if d["ns"] else "") + d["name"]
    for k in range(1, len(tds) + 1):
        tds[k - 1]["full"] = full(k)
    if True:
        for r in rows(t,"InterfaceImpl"):
            tds[r.Class.row_index - 1]["ifaces"].append(cidx(r.Interface))
    # methods
    meths = []
    owner = [0] * (nmeth + 1)
    for k, d in enumerate(tds, 1):
        for mi in d["methods_idx"]:
            if 0 < mi <= nmeth: owner[mi] = k
    for i, r in enumerate(mrows, 1):
        f = r.Flags; imf = r.ImplFlags
        sig = blob(r.Signature)
        m = {"name": s(r.Name), "type": owner[i], "rva": r.Rva or 0,
             "icall": bool(getattr(imf, "miInternalCall", False)),
             "runtime": bool(getattr(imf, "miRuntime", False)),
             "pinvoke": bool(getattr(f, "mdPinvokeImpl", False)),
             "virtual": bool(getattr(f, "mdVirtual", False)),
             "static": bool(getattr(f, "mdStatic", False)),
             "abstract": bool(getattr(f, "mdAbstract", False)),
             "nparams": sig_paramcount(sig), "sig": sig, "calls": None}
        meths.append(m)
    # bodies
    for m in meths:
        if m["rva"]:
            code = read_body(pe, m["rva"])
            m["calls"] = decode_il(code) if code else []
            m["ilsize"] = len(code) if code else 0
    A["types"] = tds; A["methods"] = meths
    A["typerefs"] = []
    for r in rows(t,"TypeRef"):
        A["typerefs"].append({"scope": cidx(r.ResolutionScope), "ns": s(r.TypeNamespace), "name": s(r.TypeName)})
    A["memberrefs"] = [{"class": cidx(r.Class), "name": s(r.Name), "sig": blob(r.Signature)}
                       for r in rows(t,"MemberRef")]
    A["typespecs"] = [typespec_generic_def(blob(r.Signature)) for r in rows(t,"TypeSpec")]
    A["methodspecs"] = [cidx(r.Method) for r in rows(t,"MethodSpec")]
    A["fields_owner"] = None
    # field -> owner type (for ldsfld -> cctor)
    frows = rows(t,"Field")
    fown = [0] * (len(frows) + 1)
    for k, r in enumerate(trows, 1):
        for fi in (r.FieldList or []):
            if 0 < fi.row_index <= len(frows): fown[fi.row_index] = k
    A["fields_owner"] = fown
    A["implmap"] = {}
    for r in rows(t,"ImplMap"):
        mf = cidx(r.MemberForwarded)
        if mf and mf[0] == "MethodDef":
            A["implmap"][mf[1]] = (modrefs[r.ImportScope.row_index - 1] if r.ImportScope and r.ImportScope.row_index else "?",
                                   s(r.ImportName))
    A["exported"] = {}
    for r in rows(t,"ExportedType"):
        impl = cidx(r.Implementation)
        if impl and impl[0] == "AssemblyRef":
            A["exported"][(s(r.TypeNamespace) + "." if s(r.TypeNamespace) else "") + s(r.TypeName)] = asmrefs[impl[1] - 1]
    # custom attrs: count DllImport-less; skip
    pe.close()
    return A

def load_all(force=False):
    if os.path.exists(CACHE) and not force:
        with open(CACHE, "rb") as f: return pickle.load(f)
    files = sorted(glob.glob(os.path.join(ROOT, "common", "*.dll")) +
                   glob.glob(os.path.join(ROOT, "Application", "*.dll")) +
                   glob.glob(os.path.join(ROOT, "Application", "*.exe")))
    if not files:  # flat directory (tools/sprx_pe.py output)
        files = sorted(glob.glob(os.path.join(ROOT, "*.dll")) + glob.glob(os.path.join(ROOT, "*.exe")))
    asms = {}
    for p in files:
        try:
            A = load_asm(p)
            A["group"] = "Application" if "Application" in p else "common"
            asms[A["name"]] = A
        except Exception as e:
            print("FAIL", p, e)
    with open(CACHE, "wb") as f: pickle.dump(asms, f)
    return asms

if __name__ == "__main__":
    a = load_all(force=True)
    print(len(a), "assemblies")
    print(sum(len(x["methods"]) for x in a.values()), "methods")
