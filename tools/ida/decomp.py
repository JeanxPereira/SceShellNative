"""idalib script: print Hex-Rays pseudocode (or disassembly when the decompiler is not
available) for functions of an analysed database.

usage: python ida/decomp.py <module.elf|.i64> <name-regex|0xaddr> [...]
"""
import re, sys
import idapro

idapro.enable_console_messages(False)
if idapro.open_database(sys.argv[1], True) != 0:
    sys.exit("open failed")
import ida_funcs, ida_hexrays, ida_lines, ida_name, idautils, idc

have_hr = ida_hexrays.init_hexrays_plugin()
targets = []
for q in sys.argv[2:]:
    if q.startswith("0x"):
        targets.append(int(q, 16))
    else:
        rx = re.compile(q)
        targets += [f for f in idautils.Functions() if rx.search(ida_name.get_name(f) or "")]
for f in targets:
    print("=" * 8, hex(f), ida_name.get_name(f), idc.get_func_cmt(f, 1) or "")
    if have_hr:
        try:
            print(str(ida_hexrays.decompile(f)))
            continue
        except Exception as e:
            print("decompile failed:", e)
    fn = ida_funcs.get_func(f)
    for ea in idautils.Heads(fn.start_ea, fn.end_ea):
        print("%08x  %s" % (ea, idc.GetDisasm(ea)))
idapro.close_database(False)
