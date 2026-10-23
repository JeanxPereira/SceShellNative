"""Decode registry.db0 from the decrypted 7.00 manufacturing kernel (700_t_manu_kernel.elf).

The kernel keeps the file XOR-obfuscated: 111056 bytes at VA 0xffffffff816D9F50, XORed with
the 16-byte key at VA 0xffffffff81308C50 (cyclic). Opened by name in 0xffffffff8077e0b0
("registry.db0" -> slot 0), data pointer/size set in 0xffffffff8077df90, de-XORed while
read in 0xffffffff80ca3b50. Addresses are specific to that kernel image.

usage: python -I extract_db0.py <kernel.elf> <out registry.db0>
"""
import struct, sys

def va_to_off(d, va):
    phoff, = struct.unpack_from('<Q', d, 0x20)
    n, = struct.unpack_from('<H', d, 0x38)
    for i in range(n):
        t, fl, off, v, pa, fs, ms, al = struct.unpack_from('<IIQQQQQQ', d, phoff + i * 56)
        if t == 1 and v <= va < v + fs:
            return off + (va - v)
    raise SystemExit('VA not in a loaded segment: %#x' % va)

d = open(sys.argv[1], 'rb').read()
k = va_to_off(d, 0xffffffff81308C50)
b = va_to_off(d, 0xffffffff816D9F50)
key = d[k:k + 16]
blob = d[b:b + 111056]
out = bytes(x ^ key[i % 16] for i, x in enumerate(blob))
if not out.startswith(b'[VERSION='):
    raise SystemExit('not a registry.db0 at the expected address (different kernel build?)')
open(sys.argv[2], 'wb').write(out)
print('wrote', len(out), 'bytes;', out.split(b'\n', 2)[:2])
