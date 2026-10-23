"""Make a decrypted PS5 module (.sprx/.prx/eboot ELF, e_type 0xFE1x) loadable by IDA's
stock ELF loader: e_type -> ET_DYN and EI_OSABI -> SYSV. Program headers, dynamic
table and relocations are already standard on PS5 and are left untouched.

usage: python -I ps5_to_elf.py <in.sprx> <out.elf>
"""
import sys

d = bytearray(open(sys.argv[1], "rb").read())
assert d[:4] == b"\x7fELF", "not a decrypted ELF"
d[7] = 0
d[0x10:0x12] = (3).to_bytes(2, "little")
open(sys.argv[2], "wb").write(d)
