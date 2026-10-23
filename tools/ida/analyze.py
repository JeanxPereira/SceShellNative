"""idalib script: open a module converted by ps5_to_elf.py, run auto-analysis, name the
Mono icalls it registers (icalls.py) and the P/Invoke entry points it publishes in a
{name, fn} table, then save the database.

usage: python ida/analyze.py <module.elf>
"""
import os, re, sys
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import idapro
idapro.enable_console_messages(False)
if idapro.open_database(sys.argv[1], True) != 0:
    sys.exit("open failed")
import ida_auto, ida_funcs, ida_name, ida_bytes, idautils
import icalls
ida_auto.auto_wait()
n = 0
for fn, name in icalls.registrations():
    if not ida_funcs.get_func(fn):
        ida_funcs.add_func(fn)
    ida_name.set_name(fn, "icall_" + re.sub(r"[^\w]", "_", re.sub(r"\(.*\)$", "", name)), ida_name.SN_NOWARN | ida_name.SN_FORCE)
    n += 1
print("icalls named", n)
idapro.close_database(True)
