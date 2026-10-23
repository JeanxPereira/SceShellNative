"""idalib script: open a PS5 module converted by ps5_to_elf.py, name every function
registered as a Mono internal call ("icall_<sanitized name>") and save the database.

usage: python ida/name_icalls.py <module.elf>
"""
import os, re, sys
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import idapro

idapro.enable_console_messages(False)
if idapro.open_database(sys.argv[1], True) != 0:
    sys.exit("open failed")
import ida_auto, ida_funcs, ida_name
import icalls

ida_auto.auto_wait()
named = 0
for fn, name in icalls.registrations():
    if not ida_funcs.get_func(fn):
        ida_funcs.add_func(fn)
    short = re.sub(r"\(.*\)$", "", name)
    ida_name.set_name(fn, "icall_" + re.sub(r"[^\w]", "_", short), ida_name.SN_NOWARN | ida_name.SN_FORCE)
    named += 1
print("named", named)
idapro.close_database(True)
