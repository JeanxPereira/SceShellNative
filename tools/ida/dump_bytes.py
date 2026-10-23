"""idalib script: hex-dump bytes at an address: <db> <addr> <len>"""
import sys
import idapro
idapro.enable_console_messages(False)
if idapro.open_database(sys.argv[1], True) != 0:
    sys.exit("open failed")
import ida_bytes
ea, n = int(sys.argv[2], 16), int(sys.argv[3], 0)
b = ida_bytes.get_bytes(ea, n)
for i in range(0, n, 16):
    print("%08x  %s" % (ea + i, b[i:i + 16].hex(" ")))
idapro.close_database(False)
