"""idalib script: for each exact C string given, print where it is referenced and the
qwords next to each reference (P/Invoke name tables are {name, fn} or {fn, name})."""
import sys
import idapro
idapro.enable_console_messages(False)
if idapro.open_database(sys.argv[1], True) != 0:
    sys.exit("open failed")
import ida_bytes, ida_name, idautils
want = set(sys.argv[2:])
for s in idautils.Strings():
    t = str(s)
    if t in want:
        for x in idautils.DataRefsTo(s.ea):
            q = [ida_bytes.get_qword(x + o) for o in (-16, -8, 8, 16)]
            print(t, hex(s.ea), "ref", hex(x), " ".join("%x(%s)" % (v, ida_name.get_name(v) or "") for v in q))
idapro.close_database(False)
