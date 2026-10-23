"""idalib script: list the functions that call/reference the given addresses."""
import sys
import idapro
idapro.enable_console_messages(False)
if idapro.open_database(sys.argv[1], True) != 0:
    sys.exit("open failed")
import ida_funcs, ida_name, idautils
for a in sys.argv[2:]:
    ea = int(a, 16)
    seen = set()
    for x in idautils.CodeRefsTo(ea, 0):
        f = ida_funcs.get_func(x)
        if f and f.start_ea not in seen:
            seen.add(f.start_ea)
            print(a, "<-", hex(f.start_ea), ida_name.get_name(f.start_ea), "at", hex(x))
idapro.close_database(False)
