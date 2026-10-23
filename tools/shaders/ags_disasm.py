"""Disassemble an eboot AGS shader blob (.shader_text) with llvm-objdump (gfx1013) and annotate
32-bit literals with their float value. The blobs mark .shader_text with an OBJECT symbol, which
makes objdump print it as data: the symbols are retyped to FUNC in a temporary copy.
usage: python -I tools/shaders/ags_disasm.py <blob.bin> <out.ann>
"""
import os, re, struct, subprocess, sys, tempfile

# llvm-objdump with AMDGPU support: SSN_LLVM_OBJDUMP, else the RGA download of
# tools/fetch_deps.ps1, else an installed ROCm / HIP SDK (newest version first).
_ROCM = r"C:\Program Files\AMD\ROCm"
_CANDIDATES = [
    os.environ.get("SSN_LLVM_OBJDUMP", ""),
    os.path.join(os.path.dirname(os.path.abspath(__file__)), "..", "..", "deps", "dl", "rga", "x", "utils", "lc", "opencl", "bin", "llvm-objdump.exe"),
] + [os.path.join(_ROCM, v, "bin", "llvm-objdump.exe") for v in sorted(os.listdir(_ROCM) if os.path.isdir(_ROCM) else [], reverse=True)]
OD = next((c for c in _CANDIDATES if c and os.path.isfile(c)), None)
if OD is None:
    sys.exit("ags_disasm: no llvm-objdump with AMDGPU support found (set SSN_LLVM_OBJDUMP)")
src, dst = sys.argv[1], sys.argv[2]
d = bytearray(open(src, "rb").read())
shoff, = struct.unpack_from("<Q", d, 0x28)
shentsize, shnum = struct.unpack_from("<HH", d, 0x3A)
for i in range(shnum):
    o = shoff + i * shentsize
    sh_type, = struct.unpack_from("<I", d, o + 4)
    if sh_type == 2:  # SHT_SYMTAB
        off, size = struct.unpack_from("<QQ", d, o + 24)
        entsize, = struct.unpack_from("<Q", d, o + 56)
        for s in range(off, off + size, entsize):
            info = d[s + 4]
            if info & 0xF == 1:
                d[s + 4] = (info & 0xF0) | 2
tmp = os.path.join(tempfile.gettempdir(), "ssn_dis_" + os.path.basename(src))
open(tmp, "wb").write(d)
out = subprocess.run([OD, "-d", "--triple=amdgcn-amd-amdhsa", "--mcpu=gfx1013", tmp], capture_output=True, text=True).stdout
os.remove(tmp)
lines = []
for ln in out.splitlines():
    m = re.match(r"\s+(.*?)\s*//\s*([0-9A-Fa-f]+):", ln)
    if not m:
        continue
    ins, addr = m.group(1), int(m.group(2), 16)
    def ann(mm):
        v = int(mm.group(0), 16)
        f = struct.unpack("<f", struct.pack("<I", v))[0]
        return "%s{%.9g}" % (mm.group(0), f)
    ins = re.sub(r"0x[0-9a-fA-F]{8}\b", ann, ins)
    # Newer llvm-objdump prints "exp pos0 v1, ..." where the RGA build printed "exp pos0, v1, ...";
    # keep the RGA form, which isa2glsl.py parses and the committed transcriptions quote.
    ins = re.sub(r"^exp (\w+) ", r"exp \1, ", ins)
    lines.append("%03X %s" % (addr, ins))
    if ins.startswith("s_code_end"):
        break
open(dst, "w").write("\n".join(lines) + "\n")
print(dst, len(lines))
