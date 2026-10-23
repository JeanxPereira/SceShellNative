"""idalib script: list libScePsm's embedded shader table (72 x {name, data, size} at
the address given) and optionally extract every blob into a directory.

usage: python ida/shader_table.py <db> <table addr> [out dir]
"""
import os, sys
import idapro
idapro.enable_console_messages(False)
if idapro.open_database(sys.argv[1], True) != 0:
    sys.exit("open failed")
import ida_bytes, idc
ea = int(sys.argv[2], 16)
out = sys.argv[3] if len(sys.argv) > 3 else None
if out:
    os.makedirs(out, exist_ok=True)
for i in range(72):
    name_p = ida_bytes.get_qword(ea + 24 * i)
    data_p = ida_bytes.get_qword(ea + 24 * i + 8)
    size = ida_bytes.get_dword(ea + 24 * i + 16)
    name = (idc.get_strlit_contents(name_p, -1, 0) or b"").decode()
    print("%2d %-40s data=%08x size=%d" % (i, name, data_p, size))
    if out and size:
        open(os.path.join(out, "%02d_%s.bin" % (i, name)), "wb").write(ida_bytes.get_bytes(data_p, size))
idapro.close_database(False)
