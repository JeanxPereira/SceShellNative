"""idalib script: dump N qword pointers at an address and the C strings they point to."""
import sys
import idapro
idapro.enable_console_messages(False)
if idapro.open_database(sys.argv[1], True) != 0:
    sys.exit("open failed")
import ida_bytes, idc
ea, n = int(sys.argv[2], 16), int(sys.argv[3])
for i in range(n):
    p = ida_bytes.get_qword(ea + 8 * i)
    s = idc.get_strlit_contents(p, -1, 0)
    print(i, hex(p), s.decode("utf-8", "replace") if s else "")
idapro.close_database(False)
