"""idalib script: list functions referencing (code or data) each given address."""
import sys
import idapro
idapro.enable_console_messages(False)
if idapro.open_database(sys.argv[1], True) != 0:
    sys.exit("open failed")
import ida_funcs, ida_name, idautils
for a in sys.argv[2:]:
    ea = int(a, 16)
    seen = set()
    for x in idautils.XrefsTo(ea):
        f = ida_funcs.get_func(x.frm)
        k = f.start_ea if f else x.frm
        if k not in seen:
            seen.add(k)
            print(a, "<-", hex(k), ida_name.get_name(k) or "", "at", hex(x.frm))
idapro.close_database(False)
