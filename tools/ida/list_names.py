"""idalib script: list 'address name comment' for functions whose name matches a regex."""
import re, sys
import idapro
idapro.enable_console_messages(False)
if idapro.open_database(sys.argv[1], True) != 0:
    sys.exit("open failed")
import ida_name, idautils, idc
rx = re.compile(sys.argv[2] if len(sys.argv) > 2 else ".")
for f in idautils.Functions():
    n = ida_name.get_name(f) or ""
    if rx.search(n):
        print("%08x\t%s\t%s" % (f, n, idc.get_func_cmt(f, 1) or ""))
idapro.close_database(False)
