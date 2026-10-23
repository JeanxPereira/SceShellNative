"""PS4/PS5 symbol NID: first 8 bytes of SHA-1(name + suffix), byte-swapped, base64 ('/' -> '-'), 11 chars.
usage: python nid.py <symbol>..."""
import hashlib, base64, struct
SUFFIX = bytes.fromhex('518d64a635ded8c1e6b039b1c3e55230')
def nid(name):
    h = hashlib.sha1(name.encode() + SUFFIX).digest()[:8]
    v = struct.unpack('<Q', h)[0]
    return base64.b64encode(struct.pack('>Q', v)).decode().rstrip('=').replace('/', '-')[:11]
if __name__ == '__main__':
    import sys
    for n in sys.argv[1:]: print(n, nid(n))
