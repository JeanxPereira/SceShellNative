"""idalib script: print '<fn address>\t<registered icall name>' for every Mono internal
call the module registers (see icalls.py)."""
import os, sys
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import idapro
idapro.enable_console_messages(False)
if idapro.open_database(sys.argv[1], True) != 0:
    sys.exit("open failed")
import icalls
for fn, name in icalls.registrations():
    print("%08x\t%s" % (fn, name))
idapro.close_database(False)
