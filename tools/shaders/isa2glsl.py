"""RDNA (gfx10) ISA -> GLSL 4.50 transcription of an embedded eboot shader, one statement per
instruction.

The ShellUI eboot's particle shaders (launcher/shaders/bglayer/particle*) are long, mostly
straight-line programs (the compute shader alone is ~5000 instructions). Rather than rewriting
them by hand, this tool transcribes the disassembly literally:

  * every VGPR / SGPR is a `uint` holding the register's bits; float instructions reinterpret
    them (uintBitsToFloat / floatBitsToUint), so integer and float uses of one register stay exact;
  * one invocation is one lane: EXEC and every 64-bit lane mask (VCC, s[n:n+1]) are `bool`s;
    instructions that write a VGPR are skipped when EXEC is false, scalar instructions always run;
  * scalar control flow is kept: the program is a loop over a `switch (pc)` whose cases are the
    basic blocks (byte offsets in the shader). Branches on "no lane active" (s_cbranch_execz,
    s_cbranch_vccz, SCC of 64-bit mask ops) use subgroupAny(), so every invocation of a subgroup
    takes the path the hardware wave takes and scalar state stays uniform;
  * VALU float arithmetic goes through `precise` helpers: v_mad/v_mac/v_madmk/v_madak are an
    unfused multiply and add, v_fma_f32 is fma(), nothing else may be contracted.

Each emitted statement carries the byte offset and the instruction text it comes from.

Memory (configured per shader, see SHADERS below):
  * s_buffer_load from the user-data V# (the SRT constant buffer) -> uniform block `SRT`;
  * s_load through the pointer the SRT holds at +0 -> uniform block `P` (the CB the eboot fills);
  * buffer_load/store with a V# that was s_load'ed from P -> the storage buffer configured for
    that P offset, with the V# NUM_RECORDS / STRIDE bounds check (OOB_SELECT 0: loads return 0,
    stores are dropped);
  * tbuffer loads through s_getpc-relative descriptors -> const arrays copied from the blob's
    .shader_text (the tables the compiler appended after the code).

usage: python -I isa2glsl.py <shader-name> <blob.bin> <disasm.ann> <out.glsl>
       (disasm.ann: llvm-objdump --mcpu=gfx1013 output, "OFF mnemonic operands" per line,
        produced by tools/shaders/ags_disasm.py)
"""
import re
import struct
import sys

# ------------------------------------------------------------------------------------------------
# Per-shader configuration.
#   stage      : comp / vert / frag
#   srt_vsharp : the user-data SGPRs holding the SRT buffer's V# (s_buffer_load source)
#   p_buffers  : P byte offset of a V# -> (storage buffer block name, binding)
#   inputs     : register initialisation
#   skip       : [start, end) byte ranges not transcribed (NGG prologue: primitive export)
SHADERS = {
    "particle_c": dict(stage="comp", srt_vsharp="s0", srt_binding=0, p_binding=1,
                       p_buffers={0x00: ("PermBuf", 2), 0x10: ("ParticleBuf", 3)},
                       inputs={"s4": "gl_WorkGroupID.x", "v0": "gl_LocalInvocationID.x"}, skip=[]),
    "particle_vv": dict(stage="vert", srt_vsharp="s8", srt_binding=0, p_binding=1,
                        p_buffers={0x00: ("ParticleBuf", 3), 0x10: ("PermBuf", 2)},
                        inputs={"v5": "uint(gl_VertexIndex)"}, skip=[(0x000, 0x03C)]),
    "particle_p": dict(stage="frag", srt_vsharp="s0", srt_binding=0, p_binding=1,
                       p_buffers={0x00: ("ParticleBuf", 3), 0x10: ("PermBuf", 2)},
                       inputs={}, skip=[]),
    "large_particle_vv": dict(stage="vert", srt_vsharp="s8", srt_binding=0, p_binding=1,
                              p_buffers={0x00: ("ParticleBuf", 3), 0x10: ("PermBuf", 2)},
                              inputs={"v5": "uint(gl_VertexIndex)"}, skip=[(0x000, 0x03C)]),
    # Samples the two boot-animation textures (Sce.Vsh.ShellUI.BGLayer.Particle0/1.gnf), whose
    # T#s the eboot copies into the draw block at +0x20 / +0x40 (sub_94020 -> sub_877530). The S#
    # is built in s[16:19] from immediates: {0x92, 0, 37 << 20, 0} = clamp-to-last-texel on X/Y/Z,
    # bilinear mag and min, no mip filter. Both textures have one mip level (the GNFs' T# and the
    # 1x1 fallback of the job 0x96240), so the implicit LOD selects nothing: the sample is written
    # textureLod(.., 0.0), which is defined in the non-uniform control flow of the transcription.
    "large_particle_p": dict(stage="frag", srt_vsharp="s0", srt_binding=0, p_binding=1,
                             p_buffers={0x00: ("ParticleBuf", 3), 0x10: ("PermBuf", 2)},
                             p_textures={0x20: ("Particle0", 4), 0x40: ("Particle1", 5)},
                             ssharp_note="psmhost/eboot/bglayer_particle.cpp", sample_lod0=True,
                             inputs={}, skip=[]),
    # The eboot's background-image transition pixel shaders (Transition layer, Initialize 0xA3CB0:
    # #44 single_p, #5 cross_fade_p, #46 ripple_p, #48 slide_in_p, #49 game_splash_p) and the copy
    # shader #0 basic_p (XfGraphics default PS, old-image capture 0xA48D0). Resources are in user
    # data: T#s and S#s (bound by the host as combined image samplers), constant buffers b0 / b1
    # (c0 = the per-shader parameters, c1 = the basemat GradationParameter pair, 160 bytes); the
    # shaders also read T# words as values (texture width), so the user-data dwords are a uniform
    # block the host fills with the T#s it binds. s_memrealtime (the basemat dither seed) is a
    # push constant, as in light_p.
    "single_p": dict(stage="frag", attr_comps={0: 2}, no_srt=True, srt_vsharp="-", srt_binding=-1, p_binding=-1, p_buffers={},
                     ud_values=0, ud_dwords=20, ud_cbs={"s12": ("CB0", 1), "s16": ("CB1", 2)},
                     ud_textures={"s0": ("Texture0", 3)}, memrealtime=True, push="uint realtime;",
                     inputs={}, skip=[]),
    "cross_fade_p": dict(stage="frag", attr_comps={0: 2}, no_srt=True, srt_vsharp="-", srt_binding=-1, p_binding=-1, p_buffers={},
                         ud_values=0, ud_dwords=32, ud_cbs={"s24": ("CB0", 1), "s28": ("CB1", 2)},
                         ud_textures={"s0": ("Texture0", 3), "s8": ("Texture1", 4)}, memrealtime=True,
                         push="uint realtime;", inputs={}, skip=[]),
    "ripple_p": dict(stage="frag", attr_comps={0: 2}, no_srt=True, srt_vsharp="-", srt_binding=-1, p_binding=-1, p_buffers={},
                     ud_values=0, ud_dwords=32, ud_cbs={"s24": ("CB0", 1), "s28": ("CB1", 2)},
                     ud_textures={"s0": ("Texture0", 3), "s8": ("Texture1", 4)}, memrealtime=True,
                     push="uint realtime;", inputs={}, skip=[]),
    "slide_in_p": dict(stage="frag", attr_comps={0: 2}, no_srt=True, srt_vsharp="-", srt_binding=-1, p_binding=-1, p_buffers={},
                       ud_values=0, ud_dwords=32, ud_cbs={"s24": ("CB0", 1), "s28": ("CB1", 2)},
                       ud_textures={"s0": ("Texture0", 3), "s8": ("Texture1", 4)}, memrealtime=True,
                       push="uint realtime;", inputs={}, skip=[]),
    "game_splash_p": dict(stage="frag", attr_comps={0: 2}, no_srt=True, srt_vsharp="-", srt_binding=-1, p_binding=-1, p_buffers={},
                          ud_values=0, ud_dwords=32, ud_cbs={"s24": ("CB0", 1), "s28": ("CB1", 2)},
                          ud_textures={"s0": ("Texture0", 3), "s8": ("Texture1", 4)}, memrealtime=True,
                          push="uint realtime;", inputs={}, skip=[]),
    "basic_p": dict(stage="frag", attr_comps={0: 2}, no_srt=True, srt_vsharp="-", srt_binding=-1, p_binding=-1, p_buffers={},
                    ud_values=0, ud_dwords=16, ud_cbs={"s12": ("CB0", 1)},
                    ud_textures={"s0": ("Texture0", 3)}, inputs={}, skip=[]),
    # libScePsm GPU texture compressor (Image::ConvertNative with ImageConvertOption.DXTCompressByGPU,
    # sub_296B70): an NGG vertex shader, one vertex per 4x4 block (blockId = vertex index), that
    # reads srcTex (t0, user data s[8:15]) and stores the 128-bit block to the u0 image (s[16:23]).
    # The host runs it as a compute shader, one invocation per vertex; EXEC starts as the
    # prologue leaves it (the lanes below the vertex count).
    "gtc_bc7": dict(stage="comp", no_srt=True, srt_vsharp="-", srt_binding=-1, p_binding=-1, p_buffers={},
                    images={"s8": ("srcTex", "binding = 0, rgba8", "readonly image2D"),
                            "s16": ("dstTex", "binding = 1, rgba32ui", "writeonly uimage2D")},
                    push="uint vertex_count;",
                    inputs={"v5": "gl_GlobalInvocationID.x"},
                    exec_init="gl_GlobalInvocationID.x < pc_.vertex_count",
                    table_words=512,  # the s_getpc V# at 0x4D0 has NUM_RECORDS 0x800 bytes
                    skip=[(0x000, 0x03C)]),
}

REL_WINDOW = 64  # v_movrels/v_movreld: registers reachable from the base register (16 RGBA texels)

FLOAT_INLINE = {"0.5": 0x3F000000, "-0.5": 0xBF000000, "1.0": 0x3F800000, "-1.0": 0xBF800000,
                "2.0": 0x40000000, "-2.0": 0xC0000000, "4.0": 0x40800000, "-4.0": 0xC0800000,
                "0.15915494": 0x3E22F983}


class Ctx:
    def __init__(s, name, cfg, blob):
        s.name, s.cfg, s.blob = name, cfg, blob
        s.vregs, s.sregs, s.masks = set(), set(), set()
        s.ptr_regs = {}      # sreg -> "P" (holds the pointer the SRT stores at +0)
        s.vsharp = {}        # sreg (first of 4) -> P offset it was loaded from
        s.tsharp = {}        # sreg (first of 8) -> P offset of a T# it was loaded from
        s.sampled = set()    # (texture name, first S# register) pairs used by image_sample
        s.pc_regs = {}       # sreg -> code offset (s_getpc / s_add_u32 tracking)
        s.const_tables = {}  # code offset -> name
        s.outputs = set()
        s.uses_any = False
        s.uses_ballot = False
        s.uses_cvt_i32 = False
        s.uses_med3 = False


def die(msg):
    raise SystemExit("isa2glsl: " + msg)


# ---------------------------------------------------------------------------------------------
# operands
def split_ops(text):
    out, depth, cur = [], 0, ""
    for ch in text:
        if ch == "[":
            depth += 1
        elif ch == "]":
            depth -= 1
        if ch == "," and depth == 0:
            out.append(cur.strip())
            cur = ""
        else:
            cur += ch
    if cur.strip():
        out.append(cur.strip())
    return out


def reg_range(op):
    m = re.fullmatch(r"([sv])\[(\d+):(\d+)\]", op)
    if m:
        return m.group(1), int(m.group(2)), int(m.group(3))
    m = re.fullmatch(r"([sv])(\d+)", op)
    if m:
        return m.group(1), int(m.group(2)), int(m.group(2))
    return None


def mask_name(op):
    if op in ("vcc", "vcc_lo"):
        return "M_vcc"
    if op in ("exec", "exec_lo"):
        return "EXEC"
    r = reg_range(op)
    if r and r[0] == "s":
        return "M_s%d" % r[1]
    die("not a mask operand: " + op)


def lit(op):
    """Integer / float inline constants and literals -> uint expression, or None."""
    m = re.fullmatch(r"(0x[0-9a-fA-F]+)(\{[^}]*\})?", op)
    if m:
        return "0x%Xu" % int(m.group(1), 16)
    if op in FLOAT_INLINE:
        return "0x%08Xu" % FLOAT_INLINE[op]
    if re.fullmatch(r"-?\d+", op):
        v = int(op)
        return "%du" % v if v >= 0 else "uint(%d)" % v
    return None


class Src:
    def __init__(s, ctx, op):
        s.neg = s.abs = False
        o = op
        if o.startswith("-"):
            if lit(o) is None:
                s.neg = True
                o = o[1:]
        if o.startswith("|") and o.endswith("|"):
            s.abs = True
            o = o[1:-1]
        s.text = o
        s.ctx = ctx

    def u(s):
        """uint expression of the operand bits (no modifiers allowed)."""
        o = s.text
        l = lit(o)
        if l is not None:
            return l
        if o == "src_scc":
            return "(SCC ? 1u : 0u)"
        if o == "null":
            return "0u"
        if o in ("vcc_lo", "vcc_hi", "m0", "exec_lo", "exec_hi"):
            s.ctx.sregs.add(o)
            return o
        r = reg_range(o)
        if r and r[1] == r[2]:
            (s.ctx.vregs if r[0] == "v" else s.ctx.sregs).add("%s%d" % (r[0], r[1]))
            return "%s%d" % (r[0], r[1])
        die("unsupported source " + o)

    def f(s):
        e = "uf(%s)" % s.u()
        if s.abs:
            e = "abs(%s)" % e
        if s.neg:
            e = "(-%s)" % e
        return e


def vdst(ctx, op, i=0):
    r = reg_range(op)
    if not r or r[0] != "v":
        die("not a vdst: " + op)
    n = "v%d" % (r[1] + i)
    ctx.vregs.add(n)
    return n


def sdst(ctx, op, i=0):
    if op in ("vcc_lo", "vcc_hi", "m0", "exec_lo", "exec_hi") and i == 0:
        ctx.sregs.add(op)
        return op
    if op == "vcc":
        n = ["vcc_lo", "vcc_hi"][i]
        ctx.sregs.add(n)
        return n
    r = reg_range(op)
    if not r or r[0] != "s":
        die("not an sdst: " + op)
    n = "s%d" % (r[1] + i)
    ctx.sregs.add(n)
    return n


def fres(expr, mods):
    """float result expression with output modifiers -> uint."""
    if "mul:2" in mods:
        expr = "fmul_(%s, 2.0)" % expr
    if "mul:4" in mods:
        expr = "fmul_(%s, 4.0)" % expr
    if "div:2" in mods:
        expr = "fmul_(%s, 0.5)" % expr
    if "clamp" in mods:
        expr = "clamp(%s, 0.0, 1.0)" % expr
    return "fu(%s)" % expr


# ---------------------------------------------------------------------------------------------
FCMP = {"lt": "%s < %s", "le": "%s <= %s", "gt": "%s > %s", "ge": "%s >= %s", "eq": "%s == %s",
        "lg": "(%s < %s || %%s > %%s)", "neq": "!(%s == %s)", "nlt": "!(%s < %s)", "nle": "!(%s <= %s)",
        "ngt": "!(%s > %s)", "nge": "!(%s >= %s)", "ne": "%s != %s"}
ICMP = {"lt": "<", "le": "<=", "gt": ">", "ge": ">=", "eq": "==", "ne": "!=", "lg": "!="}


def cmp_expr(ctx, kind, typ, a, b):
    if typ == "f32":
        A, B = Src(ctx, a).f(), Src(ctx, b).f()
        if kind == "lg":
            return "(%s < %s || %s > %s)" % (A, B, A, B)
        return "(" + (FCMP[kind] % (A, B)) + ")"
    A, B = Src(ctx, a).u(), Src(ctx, b).u()
    if typ == "i32":
        A, B = "int(%s)" % A, "int(%s)" % B
    return "(%s %s %s)" % (A, ICMP[kind], B)


def translate(ctx, ins, nxt):
    """One instruction -> list of GLSL statements (vector ones are marked with a leading '@')."""
    m = re.match(r"(\S+)\s*(.*)", ins)
    mn, rest = m.group(1), m.group(2)
    # modifiers following the operands
    mods = set()
    for k in ("clamp", "mul:2", "mul:4", "div:2", "idxen", "offen", "glc", "slc", "dlc", "done", "compr", "vm"):
        if re.search(r"(^|\s)" + re.escape(k) + r"(\s|$)", rest):
            mods.add(k)
    offm = re.search(r"\soffset:(0x[0-9a-fA-F]+|\d+)", rest)
    offset = int(offm.group(1), 0) if offm else 0
    rest_ops = re.sub(r"\s(dst_sel|dst_unused|src0_sel|src1_sel|format|offset):\S+(\s\S+\])?", "", rest)
    rest_ops = re.sub(r"\s(clamp|idxen|offen|glc|slc|dlc|done|compr|vm|mul:\d|div:\d)\b", "", rest_ops)
    ops = split_ops(rest_ops)
    base = re.sub(r"_(e32|e64|sdwa|dpp)$", "", mn)
    out = []

    def V(stmt):
        out.append("@" + stmt)

    def S(stmt):
        out.append(stmt)

    # ---- program / sync -----------------------------------------------------------------------
    if base in ("s_inst_prefetch", "s_waitcnt", "s_nop", "s_code_end", "s_sendmsg", "s_dcache_inv"):
        return ["/* no effect on the transcription */"]
    if base == "s_endpgm":
        return ["@@END"]
    # ---- SMEM -----------------------------------------------------------------------------------
    m = re.fullmatch(r"s_(buffer_)?load_dword(x(\d+))?", base)
    if m:
        n = int(m.group(3) or 1)
        dst, src = ops[0], ops[1]
        offop = ops[2] if len(ops) > 2 else "null"
        if offop == "null":
            off = "%du" % offset
        elif lit(offop) is not None:
            off = "(%s + %du)" % (lit(offop), offset)
        else:
            off = "(%s + %du)" % (Src(ctx, offop).u(), offset)
        rr = reg_range(src)
        srcname = "s%d" % rr[1]
        if m.group(1) and srcname in ctx.cfg.get("ud_cbs", {}):  # constant buffer V# in user data
            mem = "%s_u" % ctx.cfg["ud_cbs"][srcname][0]
        elif m.group(1):  # s_buffer_load from the SRT V#
            if srcname != ctx.cfg["srt_vsharp"]:
                die("s_buffer_load from unexpected V# " + src)
            mem = "srt_u"
        else:
            if ctx.ptr_regs.get(srcname) != "P":
                die("s_load through untracked pointer " + src)
            mem = "p_u"
        names = [sdst(ctx, dst, i) for i in range(n)]
        for i, nm in enumerate(names):
            S("%s = %s(%s + %du);" % (nm, mem, off, 4 * i))
        # tracking
        if mem == "srt_u" and n == 2 and offop == "null" and offset == 0:
            ctx.ptr_regs[names[0]] = "P"
        if mem == "p_u" and n >= 4 and (offop == "null" or lit(offop) is not None):
            pbase = (0 if offop == "null" else int(lit(offop).rstrip("u"), 0)) + offset
            for k in range(0, n, 4):
                if pbase + 4 * k in ctx.cfg["p_buffers"]:
                    ctx.vsharp["s%d" % (reg_range(dst)[1] + k)] = pbase + 4 * k
            for k in range(0, n - 7, 8):  # 8-dword T#s stored in P (large_particle_p: P+0x20, P+0x40)
                if pbase + 4 * k in ctx.cfg.get("p_textures", {}):
                    ctx.tsharp["s%d" % (reg_range(dst)[1] + k)] = pbase + 4 * k
        return out
    # ---- SALU -----------------------------------------------------------------------------------
    if base == "s_getpc_b64":
        d = reg_range(ops[0])[1]
        ctx.pc_regs["s%d" % d] = nxt
        S("s%d = 0x%Xu; s%d = 0u; /* PC of the next instruction (code offset) */" % (d, nxt, d + 1))
        ctx.sregs.update(["s%d" % d, "s%d" % (d + 1)])
        return out
    if base == "s_add_u32":
        d, a, b = ops
        sa, sb = Src(ctx, a), Src(ctx, b)
        for x, y in ((sb, sa), (sa, sb)):
            if x.text in ctx.pc_regs and lit(y.text) is not None:
                ctx.pc_regs[sdst(ctx, d)] = ctx.pc_regs[x.text] + int(lit(y.text)[:-1], 0)
        dd = sdst(ctx, d)
        S("{ uint a_ = %s, b_ = %s; %s = a_ + b_; SCC = %s < a_; }" % (sa.u(), sb.u(), dd, dd))
        return out
    if base == "s_addc_u32":
        d, a, b = ops
        dd = sdst(ctx, d)
        S("{ uint c1_, c2_; uint t_ = uaddCarry(%s, %s, c1_); %s = uaddCarry(t_, SCC ? 1u : 0u, c2_); SCC = (c1_ | c2_) != 0u; }"
          % (Src(ctx, a).u(), Src(ctx, b).u(), dd))
        return out
    simple32 = {"s_and_b32": "&", "s_or_b32": "|", "s_xor_b32": "^"}
    if base in simple32:
        d, a, b = ops
        dd = sdst(ctx, d)
        S("%s = %s %s %s; SCC = %s != 0u;" % (dd, Src(ctx, a).u(), simple32[base], Src(ctx, b).u(), dd))
        return out
    if base in ("s_lshl_b32", "s_lshr_b32"):
        d, a, b = ops
        dd = sdst(ctx, d)
        opx = "<<" if base == "s_lshl_b32" else ">>"
        S("%s = %s %s (%s & 31u); SCC = %s != 0u;" % (dd, Src(ctx, a).u(), opx, Src(ctx, b).u(), dd))
        return out
    if base == "s_bfe_u32":
        d, a, b = ops
        dd = sdst(ctx, d)
        S("{ uint o_ = %s & 31u, w_ = (%s >> 16) & 127u; %s = w_ == 0u ? 0u : bitfieldExtract(%s, int(o_), int(min(w_, 32u - o_))); SCC = %s != 0u; }"
          % (Src(ctx, b).u(), Src(ctx, b).u(), dd, Src(ctx, a).u(), dd))
        return out
    if base in ("s_add_i32", "s_sub_i32"):
        d, a, b = ops
        dd = sdst(ctx, d)
        sign = "+" if base == "s_add_i32" else "-"
        S("{ int a_ = int(%s), b_ = int(%s); int r_ = a_ %s b_; %s = uint(r_); SCC = %s; }"
          % (Src(ctx, a).u(), Src(ctx, b).u(), sign, dd,
             "((a_ >= 0) == (b_ >= 0)) && ((r_ >= 0) != (a_ >= 0))" if sign == "+" else
             "((a_ >= 0) != (b_ >= 0)) && ((r_ >= 0) != (a_ >= 0))"))
        return out
    if base == "s_addk_i32":
        d, k = ops
        dd = sdst(ctx, d)
        kv = int(k, 0)
        kv = kv - 0x10000 if kv >= 0x8000 else kv
        S("{ int a_ = int(%s), b_ = %d; int r_ = a_ + b_; %s = uint(r_); SCC = ((a_ >= 0) == (b_ >= 0)) && ((r_ >= 0) != (a_ >= 0)); }" % (dd, kv, dd))
        return out
    if base == "s_mul_i32":
        d, a, b = ops
        S("%s = %s * %s;" % (sdst(ctx, d), Src(ctx, a).u(), Src(ctx, b).u()))
        return out
    if base == "s_mov_b32":
        d, a = ops
        if Src(ctx, a).text in ctx.pc_regs:
            ctx.pc_regs[sdst(ctx, d)] = ctx.pc_regs[Src(ctx, a).text]
        S("%s = %s;" % (sdst(ctx, d), Src(ctx, a).u()))
        return out
    if base == "s_cselect_b32":
        d, a, b = ops
        S("%s = SCC ? %s : %s;" % (sdst(ctx, d), Src(ctx, a).u(), Src(ctx, b).u()))
        return out
    m = re.fullmatch(r"s_cmpk?_(eq|lg|lt|le|gt|ge)_(u32|i32)", base)
    if m:
        a, b = ops
        A = Src(ctx, a).u()
        if base.startswith("s_cmpk"):
            B = "%du" % int(b, 0)
        else:
            B = Src(ctx, b).u()
        if m.group(2) == "i32":
            A, B = "int(%s)" % A, "int(%s)" % B
        S("SCC = %s %s %s;" % (A, {"eq": "==", "lg": "!=", "lt": "<", "le": "<=", "gt": ">", "ge": ">="}[m.group(1)], B))
        return out
    if base == "s_bitcmp1_b32":
        a, b = ops
        S("SCC = ((%s >> (%s & 31u)) & 1u) != 0u;" % (Src(ctx, a).u(), Src(ctx, b).u()))
        return out
    # 64-bit mask operations
    if base == "s_mov_b64":
        d, a = ops
        l = lit(a)
        if l is not None and a not in ("0", "-1"):
            # a 64-bit scalar value (zero-extended 32-bit literal), not a lane mask
            S("%s = %s; %s = 0u;" % (sdst(ctx, d, 0), l, sdst(ctx, d, 1)))
            return out
        dm = mask_name(d)
        if dm != "EXEC":
            ctx.masks.add(dm)
        sm = "false" if a == "0" else "true" if a == "-1" else mask_name(a)
        if sm not in ("true", "false", "EXEC"):
            ctx.masks.add(sm)
        S("%s = %s;" % (dm, sm))
        return out
    if base == "s_wqm_b64":
        S("/* s_wqm_b64: helper invocations already run in Vulkan fragment quads */ EXEC = true;")
        return out
    bin64 = {"s_and_b64": "%s && %s", "s_or_b64": "%s || %s", "s_xor_b64": "%s != %s", "s_andn2_b64": "%s && !%s",
             "s_orn2_b64": "%s || !%s", "s_nand_b64": "!(%s && %s)", "s_nor_b64": "!(%s || %s)", "s_xnor_b64": "%s == %s"}
    if base in bin64:
        d, a, b = ops
        def mexp(o):
            if o == "0":
                return "false"
            if o == "-1":
                return "true"
            nm = mask_name(o)
            if nm != "EXEC":
                ctx.masks.add(nm)
            return nm
        dm = mexp(d)
        S("%s = %s; SCC = subgroupAny(%s);" % (dm, bin64[base] % (mexp(a), mexp(b)), dm))
        ctx.uses_any = True
        return out
    m = re.fullmatch(r"s_(and|or|xor|andn1|andn2|orn1|orn2|nand|nor|xnor)_saveexec_b64", base)
    if m:
        d, a = ops
        dm = mask_name(d)
        sm = mask_name(a)
        for x in (dm, sm):
            if x != "EXEC":
                ctx.masks.add(x)
        f = {"and": "%s && EXEC", "or": "%s || EXEC", "xor": "%s != EXEC", "andn1": "!%s && EXEC",
             "andn2": "%s && !EXEC", "orn1": "!%s || EXEC", "orn2": "%s || !EXEC", "nand": "!(%s && EXEC)",
             "nor": "!(%s || EXEC)", "xnor": "%s == EXEC"}[m.group(1)] % sm
        S("{ bool s_ = %s; %s = EXEC; EXEC = s_; SCC = subgroupAny(EXEC); }" % (f, dm))
        ctx.uses_any = True
        return out
    if base == "s_cselect_b64":
        d, a, b = ops
        def mexp(o):
            if o == "0":
                return "false"
            if o == "-1":
                return "true"
            nm = mask_name(o)
            if nm != "EXEC":
                ctx.masks.add(nm)
            return nm
        dm = mexp(d)
        S("%s = SCC ? %s : %s;" % (dm, mexp(a), mexp(b)))
        return out
    # ---- branches -------------------------------------------------------------------------------
    m = re.fullmatch(r"s_(cbranch_(scc0|scc1|vccz|vccnz|execz|execnz)|branch)", base)
    if m:
        simm = int(ops[0])
        if simm >= 0x8000:
            simm -= 0x10000
        tgt = nxt + 4 * simm
        cond = {None: "true", "scc0": "!SCC", "scc1": "SCC", "vccz": "!subgroupAny(M_vcc)",
                "vccnz": "subgroupAny(M_vcc)", "execz": "!subgroupAny(EXEC)", "execnz": "subgroupAny(EXEC)"}[m.group(2)]
        if "M_vcc" in cond:
            ctx.masks.add("M_vcc")
        if "subgroupAny" in cond:
            ctx.uses_any = True
        return ["@@BRANCH %s %d" % (cond, tgt)]
    # ---- VOPC ------------------------------------------------------------------------------------
    m = re.fullmatch(r"v_cmp(x?)_(\w+?)_(f32|u32|i32)", base)
    if m:
        if m.group(1):  # v_cmpx: gfx10 writes EXEC only
            if mn.endswith("_e32"):
                a, b = ops
            else:
                a, b = ops[-2], ops[-1]
            S("EXEC = EXEC && %s;" % cmp_expr(ctx, m.group(2), m.group(3), a, b))
            return out
        if len(ops) == 2:
            d, (a, b) = "vcc", ops
        else:
            d, a, b = ops
        dm = mask_name(d)
        ctx.masks.add(dm)
        # lanes outside EXEC read 0 in the result mask
        S("%s = EXEC && %s;" % (dm, cmp_expr(ctx, m.group(2), m.group(3), a, b)))
        return out
    # ---- VALU ------------------------------------------------------------------------------------
    if base == "v_cndmask_b32":
        if len(ops) == 3:
            d, a, b = ops[0], ops[1], ops[2]
            mk = "M_vcc"
        else:
            d, a, b, mk = ops
            mk = mask_name(mk)
        ctx.masks.add(mk)
        sa, sb = Src(ctx, a), Src(ctx, b)
        ea = sa.u() if not (sa.neg or sa.abs) else "fu(%s)" % sa.f()
        eb = sb.u() if not (sb.neg or sb.abs) else "fu(%s)" % sb.f()
        V("%s = %s ? %s : %s;" % (vdst(ctx, d), mk, eb, ea))
        return out
    if base == "v_mov_b32":
        d, a = ops
        sa = Src(ctx, a)
        V("%s = %s;" % (vdst(ctx, d), sa.u() if not (sa.neg or sa.abs) else "fu(%s)" % sa.f()))
        return out
    f2 = {"v_add_f32": "fadd_(%s, %s)", "v_sub_f32": "fsub_(%s, %s)", "v_mul_f32": "fmul_(%s, %s)",
          "v_max_f32": "fmax_(%s, %s)", "v_min_f32": "fmin_(%s, %s)"}
    if base in f2:
        d, a, b = ops
        V("%s = %s;" % (vdst(ctx, d), fres(f2[base] % (Src(ctx, a).f(), Src(ctx, b).f()), mods)))
        return out
    if base == "v_subrev_f32":
        d, a, b = ops
        V("%s = %s;" % (vdst(ctx, d), fres("fsub_(%s, %s)" % (Src(ctx, b).f(), Src(ctx, a).f()), mods)))
        return out
    if base == "v_mac_f32":
        d, a, b = ops
        dd = vdst(ctx, d)
        V("%s = %s;" % (dd, fres("fmad_(%s, %s, uf(%s))" % (Src(ctx, a).f(), Src(ctx, b).f(), dd), mods)))
        return out
    if base == "v_mad_f32":
        d, a, b, c = ops
        V("%s = %s;" % (vdst(ctx, d), fres("fmad_(%s, %s, %s)" % (Src(ctx, a).f(), Src(ctx, b).f(), Src(ctx, c).f()), mods)))
        return out
    if base == "v_fma_f32":
        d, a, b, c = ops
        V("%s = %s;" % (vdst(ctx, d), fres("fma(%s, %s, %s)" % (Src(ctx, a).f(), Src(ctx, b).f(), Src(ctx, c).f()), mods)))
        return out
    if base == "v_madmk_f32":  # D = S0 * K + S1
        d, a, k, b = ops
        V("%s = %s;" % (vdst(ctx, d), fres("fmad_(%s, %s, %s)" % (Src(ctx, a).f(), Src(ctx, k).f(), Src(ctx, b).f()), mods)))
        return out
    if base == "v_madak_f32":  # D = S0 * S1 + K
        d, a, b, k = ops
        V("%s = %s;" % (vdst(ctx, d), fres("fmad_(%s, %s, %s)" % (Src(ctx, a).f(), Src(ctx, b).f(), Src(ctx, k).f()), mods)))
        return out
    if base == "v_med3_f32":
        d, a, b, c = ops
        ctx.uses_med3 = True
        V("%s = %s;" % (vdst(ctx, d), fres("fmed3_(%s, %s, %s)" % (Src(ctx, a).f(), Src(ctx, b).f(), Src(ctx, c).f()), mods)))
        return out
    if base in ("v_max3_f32", "v_min3_f32"):
        d, a, b, c = ops
        fn = "fmax_" if "max" in base else "fmin_"
        V("%s = %s;" % (vdst(ctx, d), fres("%s(%s(%s, %s), %s)" % (fn, fn, Src(ctx, a).f(), Src(ctx, b).f(), Src(ctx, c).f()), mods)))
        return out
    f1 = {"v_floor_f32": "floor(%s)", "v_fract_f32": "ffract_(%s)", "v_rcp_f32": "frcp_(%s)",
          "v_rsq_f32": "inversesqrt(%s)", "v_sqrt_f32": "sqrt(%s)", "v_exp_f32": "exp2(%s)",
          "v_log_f32": "log2(%s)", "v_rndne_f32": "roundEven(%s)", "v_sin_f32": "fsin_(%s)",
          "v_cos_f32": "fcos_(%s)", "v_trunc_f32": "trunc(%s)", "v_ceil_f32": "ceil(%s)"}
    if base in f1:
        d, a = ops
        V("%s = %s;" % (vdst(ctx, d), fres(f1[base] % Src(ctx, a).f(), mods)))
        return out
    if base == "v_ldexp_f32":
        d, a, b = ops
        V("%s = %s;" % (vdst(ctx, d), fres("ldexp(%s, int(%s))" % (Src(ctx, a).f(), Src(ctx, b).u()), mods)))
        return out
    if base == "v_cvt_f32_u32":
        d, a = ops
        V("%s = %s;" % (vdst(ctx, d), fres("float(%s)" % Src(ctx, a).u(), mods)))
        return out
    if base == "v_cvt_f32_i32":
        d, a = ops
        V("%s = %s;" % (vdst(ctx, d), fres("float(int(%s))" % Src(ctx, a).u(), mods)))
        return out
    if base == "v_cvt_u32_f32":
        d, a = ops
        V("%s = cvt_u32_f32_(%s);" % (vdst(ctx, d), Src(ctx, a).f()))
        return out
    if base == "v_cvt_pkrtz_f16_f32":
        d, a, b = ops
        V("%s = pkrtz_(%s, %s);" % (vdst(ctx, d), Src(ctx, a).f(), Src(ctx, b).f()))
        return out
    i2 = {"v_add_nc_u32": "%s + %s", "v_sub_nc_u32": "%s - %s", "v_mul_lo_u32": "%s * %s",
          "v_and_b32": "%s & %s", "v_or_b32": "%s | %s", "v_xor_b32": "%s ^ %s"}
    if base in i2:
        d, a, b = ops
        V("%s = %s;" % (vdst(ctx, d), i2[base] % (Src(ctx, a).u(), Src(ctx, b).u())))
        return out
    if base == "v_mul_hi_u32":
        d, a, b = ops
        V("{ uint hi_, lo_; umulExtended(%s, %s, hi_, lo_); %s = hi_; }" % (Src(ctx, a).u(), Src(ctx, b).u(), vdst(ctx, d)))
        return out
    if base == "v_mul_hi_i32":
        d, a, b = ops
        V("{ int hi_, lo_; imulExtended(int(%s), int(%s), hi_, lo_); %s = uint(hi_); }" % (Src(ctx, a).u(), Src(ctx, b).u(), vdst(ctx, d)))
        return out
    if base == "v_mad_u64_u32":
        d, cy, a, b, c = ops
        r = reg_range(d)
        cr = reg_range(c)
        if cr and cr[0] in "sv":
            clo, chi = "%s%d" % (cr[0], cr[1]), "%s%d" % (cr[0], cr[1] + 1)
            (ctx.sregs if cr[0] == "s" else ctx.vregs).update([clo, chi])
        elif c == "vcc":  # the 64-bit scalar pair vcc_hi:vcc_lo as a value (large_particle_vv 0x158)
            clo, chi = "vcc_lo", "vcc_hi"
            ctx.sregs.update([clo, chi])
        else:
            clo, chi = lit(c), "0u"
            if clo is None:
                die("v_mad_u64_u32 addend not supported: " + c)
        lo, hi = "v%d" % r[1], "v%d" % (r[1] + 1)
        ctx.vregs.update([lo, hi])
        V("{ uint ph_, pl_, cy_; umulExtended(%s, %s, ph_, pl_); %s = uaddCarry(pl_, %s, cy_); %s = ph_ + %s + cy_; } /* 64-bit carry-out (%s) unused */"
          % (Src(ctx, a).u(), Src(ctx, b).u(), lo, clo, hi, chi, cy))
        return out
    if base == "v_lshrrev_b32":
        d, a, b = ops
        V("%s = %s >> (%s & 31u);" % (vdst(ctx, d), Src(ctx, b).u(), Src(ctx, a).u()))
        return out
    if base == "v_lshlrev_b32":
        d, a, b = ops
        V("%s = %s << (%s & 31u);" % (vdst(ctx, d), Src(ctx, b).u(), Src(ctx, a).u()))
        return out
    if base == "v_ashrrev_i32":
        d, a, b = ops
        V("%s = uint(int(%s) >> int(%s & 31u));" % (vdst(ctx, d), Src(ctx, b).u(), Src(ctx, a).u()))
        return out
    if base == "v_lshl_add_u32":
        d, a, b, c = ops
        V("%s = (%s << (%s & 31u)) + %s;" % (vdst(ctx, d), Src(ctx, a).u(), Src(ctx, b).u(), Src(ctx, c).u()))
        return out
    if base == "v_and_or_b32":
        d, a, b, c = ops
        V("%s = (%s & %s) | %s;" % (vdst(ctx, d), Src(ctx, a).u(), Src(ctx, b).u(), Src(ctx, c).u()))
        return out
    if base == "v_bfe_u32":
        d, a, b, c = ops
        V("{ uint o_ = %s & 31u, w_ = %s & 31u; %s = w_ == 0u ? 0u : bitfieldExtract(%s, int(o_), int(min(w_, 32u - o_))); }"
          % (Src(ctx, b).u(), Src(ctx, c).u(), vdst(ctx, d), Src(ctx, a).u()))
        return out
    # ---- interpolation (pixel) ----------------------------------------------------------------------
    m = re.fullmatch(r"v_interp_(p1|p2|mov)_f32", base)
    if m:
        d = ops[0]
        am = re.fullmatch(r"attr(\d+)\.([xyzw])", ops[2])
        n, c = int(am.group(1)), am.group(2)
        ctx.outputs.add(("attr", n))
        V("%s = fu(attr%d.%s); /* %s */" % (vdst(ctx, d), n, c,
                                            "provoking-vertex value (flat)" if m.group(1) == "mov" else
                                            "interpolated value (p1/p2 pair)"))
        return out
    # ---- buffers -----------------------------------------------------------------------------------
    m = re.fullmatch(r"(t?)buffer_(load|store)_(dword|format_)(x(\d+)|xyzw|xyz|xy|x)?", base)
    if m:
        is_t, kind = m.group(1), m.group(2)
        cnt = {"xyzw": 4, "xyz": 3, "xy": 2, "x": 1}.get(m.group(4), int(m.group(5) or 1)) if m.group(4) else 1
        data, vaddr, vsh, soff = ops[0], ops[1], ops[2], ops[3]
        vs0 = "s%d" % reg_range(vsh)[1]
        if lit(soff) != "0u":
            die("buffer soffset not 0")
        r = reg_range(data)
        regs = ["v%d" % (r[1] + i) for i in range(cnt)]
        ctx.vregs.update(regs)
        ctx.vregs.add(Src(ctx, vaddr).u())
        if vs0 in ctx.pc_regs:  # code-relative constant table (raw, offen)
            if "offen" not in mods or kind != "load":
                die("unexpected constant-table access")
            code_off = ctx.pc_regs[vs0]
            ctx.const_tables[code_off] = "kTable%X" % code_off
            V("{ uint o_ = %s; %s }" % (Src(ctx, vaddr).u(), " ".join(
                "%s = ctab_%X(o_ + %du, s%d);" % (rg, code_off, offset + 4 * i, reg_range(vsh)[1] + 2) for i, rg in enumerate(regs))))
            return out
        if vs0 not in ctx.vsharp:
            die("buffer access through untracked V# " + vsh)
        blk = ctx.cfg["p_buffers"][ctx.vsharp[vs0]][0]
        if "idxen" not in mods:
            die("buffer access without idxen")
        b = reg_range(vsh)[1]
        ctx.sregs.update(["s%d" % (b + i) for i in range(4)])
        idx = Src(ctx, vaddr).u()
        if kind == "load":
            V("{ uint i_ = %s; bool ok_ = i_ < s%d; uint a_ = (i_ * ((s%d >> 16) & 0x3FFFu) + %du) >> 2; %s }"
              % (idx, b + 2, b + 1, offset, " ".join("%s = ok_ ? %s.d[a_ + %du] : 0u;" % (rg, blk, i) for i, rg in enumerate(regs))))
        else:
            V("{ uint i_ = %s; if (i_ < s%d) { uint a_ = (i_ * ((s%d >> 16) & 0x3FFFu) + %du) >> 2; %s } }"
              % (idx, b + 2, b + 1, offset, " ".join("%s.d[a_ + %du] = %s;" % (blk, i, rg) for i, rg in enumerate(regs))))
        return out
    # ---- exports --------------------------------------------------------------------------------------
    if base == "exp":
        tgt = ops[0]
        srcs = ops[1:5]
        if tgt == "prim":
            return ["/* exp prim: NGG primitive export (the host draws plain triangle lists) */"]
        if ctx.cfg["stage"] == "comp":
            return ["/* %s: a vertex shader run as compute has no position/parameter export */" % tgt]
        if ctx.cfg["stage"] == "frag":
            if all(x == "off" for x in srcs):
                return ["@@KILL"]
            if "compr" not in mods:
                die("uncompressed colour export")
            return ["@@COLOR %s %s" % (Src(ctx, srcs[0]).u(), Src(ctx, srcs[2]).u())]
        vals = ["uf(%s)" % (Src(ctx, x).u()) if x != "off" else "0.0" for x in srcs]
        if tgt == "pos0":
            V("gl_Position = vec4(%s);" % ", ".join(vals))
        else:
            pm = re.fullmatch(r"param(\d+)", tgt)
            ctx.outputs.add(("param", int(pm.group(1))))
            V("param%s = vec4(%s);" % (pm.group(1), ", ".join(vals)))
        return out
    # ---- instructions of the libScePsm GPU texture compressor (gtc_*) --------------------------
    if base == "s_movk_i32":
        d, k = ops
        kv = int(k, 0)
        kv = kv - 0x10000 if kv >= 0x8000 else kv
        S("%s = uint(%d);" % (sdst(ctx, d), kv))
        return out
    if base == "s_brev_b32":
        d, a = ops
        S("%s = bitfieldReverse(%s);" % (sdst(ctx, d), Src(ctx, a).u()))
        return out
    if base == "s_ashr_i32":
        d, a, b = ops
        dd = sdst(ctx, d)
        S("%s = uint(int(%s) >> int(%s & 31u)); SCC = %s != 0u;" % (dd, Src(ctx, a).u(), Src(ctx, b).u(), dd))
        return out
    if base == "v_add3_u32":
        d, a, b, c = ops
        V("%s = %s + %s + %s;" % (vdst(ctx, d), Src(ctx, a).u(), Src(ctx, b).u(), Src(ctx, c).u()))
        return out
    if base == "v_lshl_or_b32":
        d, a, b, c = ops
        V("%s = (%s << (%s & 31u)) | %s;" % (vdst(ctx, d), Src(ctx, a).u(), Src(ctx, b).u(), Src(ctx, c).u()))
        return out
    if base == "v_not_b32":
        d, a = ops
        V("%s = ~%s;" % (vdst(ctx, d), Src(ctx, a).u()))
        return out
    if base in ("v_min_u32", "v_max_u32"):
        d, a, b = ops
        V("%s = %s(%s, %s);" % (vdst(ctx, d), base[2:5], Src(ctx, a).u(), Src(ctx, b).u()))
        return out
    if base in ("v_min_i32", "v_max_i32"):
        d, a, b = ops
        V("%s = uint(%s(int(%s), int(%s)));" % (vdst(ctx, d), base[2:5], Src(ctx, a).u(), Src(ctx, b).u()))
        return out
    if base == "v_med3_i32":
        d, a, b, c = ops
        V("{ int a_ = int(%s), b_ = int(%s), c_ = int(%s); %s = uint(max(min(a_, b_), min(max(a_, b_), c_))); }"
          % (Src(ctx, a).u(), Src(ctx, b).u(), Src(ctx, c).u(), vdst(ctx, d)))
        return out
    if base in ("v_mul_u32_u24", "v_mul_i32_i24"):
        d, a, b = ops
        if base == "v_mul_u32_u24":
            V("%s = (%s & 0xFFFFFFu) * (%s & 0xFFFFFFu);" % (vdst(ctx, d), Src(ctx, a).u(), Src(ctx, b).u()))
        else:
            V("%s = uint(bitfieldExtract(int(%s), 0, 24) * bitfieldExtract(int(%s), 0, 24));" % (vdst(ctx, d), Src(ctx, a).u(), Src(ctx, b).u()))
        return out
    if base in ("v_add_co_ci_u32", "v_sub_co_ci_u32", "v_subrev_co_ci_u32"):
        d, co, a, b, ci = ops
        cim, com = mask_name(ci), mask_name(co)
        ctx.masks.update([cim, com])
        x, y = Src(ctx, a).u(), Src(ctx, b).u()
        if base == "v_subrev_co_ci_u32":
            x, y = y, x
        dd = vdst(ctx, d)
        if base == "v_add_co_ci_u32":
            V("{ uint c1_, c2_; uint t_ = uaddCarry(%s, %s, c1_); %s = uaddCarry(t_, %s ? 1u : 0u, c2_); %s = (c1_ | c2_) != 0u; }"
              % (x, y, dd, cim, com))
        else:
            V("{ uint b1_, b2_; uint t_ = usubBorrow(%s, %s, b1_); %s = usubBorrow(t_, %s ? 1u : 0u, b2_); %s = (b1_ | b2_) != 0u; }"
              % (x, y, dd, cim, com))
        # lanes outside EXEC read 0 in the carry-out mask
        out.append("if (!EXEC) %s = false;" % com)
        return out
    if base == "v_cvt_f32_ubyte0":
        d, a = ops
        V("%s = fu(float(%s & 0xFFu));" % (vdst(ctx, d), Src(ctx, a).u()))
        return out
    if base == "v_cvt_i32_f32":  # round toward zero, saturated, NaN -> 0
        d, a = ops
        ctx.uses_cvt_i32 = True
        V("%s = cvt_i32_f32_(%s);" % (vdst(ctx, d), Src(ctx, a).f()))
        return out
    if base == "v_cvt_rpi_i32_f32":  # floor(x + 0.5), saturated, NaN -> 0
        d, a = ops
        ctx.uses_cvt_i32 = True
        V("%s = cvt_i32_f32_(floor(fadd_(%s, 0.5)));" % (vdst(ctx, d), Src(ctx, a).f()))
        return out
    if base == "v_rcp_iflag_f32":
        d, a = ops
        V("%s = %s;" % (vdst(ctx, d), fres("frcp_(%s)" % Src(ctx, a).f(), mods)))
        return out
    if base == "v_fmac_f32":
        d, a, b = ops
        dd = vdst(ctx, d)
        V("%s = %s;" % (dd, fres("fma(%s, %s, uf(%s))" % (Src(ctx, a).f(), Src(ctx, b).f(), dd), mods)))
        return out
    if base == "v_readfirstlane_b32":
        # the value of the lowest lane in EXEC (lane 0 when EXEC is empty), uniform in a scalar register
        d, a = ops
        ctx.uses_ballot = True
        S("{ uvec4 b_ = subgroupBallot(EXEC); uint l_ = subgroupBallotFindLSB(b_); %s = subgroupShuffle(%s, subgroupAny(EXEC) ? l_ : 0u); }"
          % (sdst(ctx, d), Src(ctx, a).u()))
        return out
    if base in ("v_movrels_b32", "v_movreld_b32"):
        # M0-relative VGPR indexing: movrels reads v[src + M0], movreld writes v[dst + M0]
        d, a = ops
        ctx.sregs.add("m0")
        # The window is REL_WINDOW registers from the base (M0 stays below it in these shaders).
        if base == "v_movrels_b32":
            b0 = reg_range(a)[1]
            ctx.vregs.update("v%d" % (b0 + k) for k in range(REL_WINDOW))
            V("%s = %s;" % (vdst(ctx, d), " ".join("m0 == %du ? v%d :" % (k, b0 + k) for k in range(REL_WINDOW)) + " 0u"))
        else:
            b0 = reg_range(d)[1]
            ctx.vregs.update("v%d" % (b0 + k) for k in range(REL_WINDOW))
            V("{ uint x_ = %s; switch (m0) { %s } }" % (Src(ctx, a).u(), " ".join(
                "case %du: v%d = x_; break;" % (k, b0 + k) for k in range(REL_WINDOW))))
        return out
    if base == "s_memrealtime":
        # 64-bit real-time counter. HOST VALUE (as light_p): the host passes a counter value in a
        # push constant (cfg memrealtime), low dword; the high dword reads 0.
        if not ctx.cfg.get("memrealtime"):
            die("s_memrealtime without a memrealtime push constant")
        S("%s = pc_.realtime; %s = 0u;  /* s_memrealtime: host counter (push constant) */"
          % (sdst(ctx, ops[0], 0), sdst(ctx, ops[0], 1)))
        return out
    if base == "image_sample":
        # image_sample with a T# loaded from P (cfg p_textures) and an S# from s_mov immediates: a
        # combined image sampler whose VkSampler the host builds from the same S# dwords.
        data, vaddr, rsrc, samp = ops[0], ops[1], ops[2], ops[3].split()[0]
        t0 = "s%d" % reg_range(rsrc)[1]
        if t0 in ctx.cfg.get("ud_textures", {}):  # T# in user data
            tname = ctx.cfg["ud_textures"][t0][0]
        elif t0 not in ctx.tsharp:
            die("image_sample through untracked T# " + rsrc)
        else:
            tname = ctx.cfg["p_textures"][ctx.tsharp[t0]][0]
        if "dim:SQ_RSRC_IMG_2D" not in rest:
            die("image_sample not 2D: " + ins)
        dm = int(re.search(r"dmask:(0x[0-9a-fA-F]+)", rest).group(1), 16)
        comps = [c for i, c in enumerate("xyzw") if dm >> i & 1]
        r = reg_range(data)
        regs = ["v%d" % (r[1] + i) for i in range(r[2] - r[1] + 1)]
        if len(regs) != len(comps):
            die("image_sample dmask / data size mismatch")
        ar = reg_range(vaddr)
        if ar[2] - ar[1] != 1:
            die("image_sample address is not 2 VGPRs")
        ctx.vregs.update(regs + ["v%d" % ar[1], "v%d" % ar[2]])
        ctx.sampled.add((tname, samp))
        lod = ctx.cfg.get("sample_lod0")
        fn = "textureLod(%s, %%s, 0.0)" % tname if lod else "texture(%s, %%s)" % tname
        V("{ vec4 t_ = %s; %s }" % (fn % ("vec2(uf(v%d), uf(v%d))" % (ar[1], ar[2])),
                                   " ".join("%s = fu(t_.%s);" % (rg, c) for rg, c in zip(regs, comps))))
        return out
    m = re.fullmatch(r"image_(load|store|get_resinfo)", base)
    if m:
        data, vaddr, rsrc = ops[0], ops[1], ops[2].split()[0]
        img = ctx.cfg["images"].get("s%d" % reg_range(rsrc)[1])
        if img is None:
            die("image access through unknown descriptor " + rsrc)
        dm = int(re.search(r"dmask:(0x[0-9a-fA-F]+)", rest).group(1), 16)
        if "dim:SQ_RSRC_IMG_2D" not in rest:
            die("image access not 2D: " + ins)
        r = reg_range(data)
        regs = ["v%d" % (r[1] + i) for i in range(r[2] - r[1] + 1)]
        ctx.vregs.update(regs)
        if vaddr.startswith("["):
            coords = [Src(ctx, x.strip()).u() for x in vaddr[1:-1].split(",")]
        else:
            ar = reg_range(vaddr)
            coords = ["v%d" % k for k in range(ar[1], ar[2] + 1)]
            ctx.vregs.update(coords)
        name = img[0]
        if m.group(1) == "get_resinfo":
            if dm != 0x3:
                die("resinfo dmask")
            V("{ ivec2 z_ = imageSize(%s); %s = uint(z_.x); %s = uint(z_.y); }" % (name, regs[0], regs[1]))
            return out
        if dm != 0xF:
            die("image dmask")
        xy = "ivec2(int(%s), int(%s))" % (coords[0], coords[1])
        if m.group(1) == "load":
            # out-of-range coordinates read 0 (the hardware's behaviour for an image_load outside the resource)
            V("{ ivec2 c_ = %s; ivec2 z_ = imageSize(%s); vec4 t_ = (all(greaterThanEqual(c_, ivec2(0))) && all(lessThan(c_, z_))) ? imageLoad(%s, c_) : vec4(0.0); %s }"
              % (xy, name, name, " ".join("%s = fu(t_.%s);" % (rg, "xyzw"[i]) for i, rg in enumerate(regs))))
        else:
            V("imageStore(%s, %s, uvec4(%s));" % (name, xy, ", ".join(regs)))
        return out
    die("unhandled instruction: " + ins)


# ---------------------------------------------------------------------------------------------
HELPERS = r"""
float uf(uint x) { return uintBitsToFloat(x); }
uint fu(float x) { return floatBitsToUint(x); }
// VALU arithmetic: every rounding step separate (NoContraction).
float fadd_(float a, float b) { precise float r = a + b; return r; }
float fsub_(float a, float b) { precise float r = a - b; return r; }
float fmul_(float a, float b) { precise float r = a * b; return r; }
float fmad_(float a, float b, float c) { precise float p = a * b; precise float r = p + c; return r; }
// v_max_f32 / v_min_f32 (IEEE mode): a NaN operand yields the other operand.
float fmax_(float a, float b) { return isnan(a) ? b : isnan(b) ? a : max(a, b); }
float fmin_(float a, float b) { return isnan(a) ? b : isnan(b) ? a : min(a, b); }
// v_fract_f32: x - floor(x), at most the largest float below 1.0.
float ffract_(float x) { return min(x - floor(x), uintBitsToFloat(0x3F7FFFFFu)); }
// v_rcp_f32 (1 ulp on hardware).
float frcp_(float x) { return 1.0 / x; }
// v_sin_f32 / v_cos_f32 take the angle in revolutions.
float fsin_(float x) { return sin(x * 6.28318530717958647692); }
float fcos_(float x) { return cos(x * 6.28318530717958647692); }
// v_cvt_u32_f32: round toward zero, clamped to [0, 2^32 - 1], NaN -> 0.
uint cvt_u32_f32_(float x) { return isnan(x) ? 0u : x <= 0.0 ? 0u : x >= 4294967295.0 ? 0xFFFFFFFFu : uint(x); }
// v_cvt_pkrtz_f16_f32: two halves rounded toward zero.
uint f16rtz_(float x) {
    uint h = packHalf2x16(vec2(x, 0.0)) & 0xFFFFu;
    float back = unpackHalf2x16(h).x;
    if (!isinf(x) && !isnan(x) && abs(back) > abs(x)) h -= 1u;  // packHalf2x16 rounds to nearest
    return h;
}
uint pkrtz_(float a, float b) { return f16rtz_(a) | (f16rtz_(b) << 16); }
"""


def main():
    name, blob_path, ann_path, out_path = sys.argv[1:5]
    cfg = SHADERS[name]
    blob = open(blob_path, "rb").read()
    # .shader_text of the blob
    shoff, = struct.unpack_from("<Q", blob, 0x28)
    shentsize, shnum, shstrndx = struct.unpack_from("<HHH", blob, 0x3A)
    sh = [struct.unpack_from("<IIQQQQIIQQ", blob, shoff + i * shentsize) for i in range(shnum)]
    strtab = sh[shstrndx]
    def secname(h):
        o = strtab[4] + h[0]
        return blob[o:blob.index(b"\0", o)].decode()
    text = next(h for h in sh if secname(h) == ".shader_text")
    code = blob[text[4]:text[4] + text[5]]

    lines = []
    for ln in open(ann_path):
        m = re.match(r"([0-9A-F]+) (.*)", ln.rstrip("\n"))
        if m:
            lines.append((int(m.group(1), 16), m.group(2)))
    # stop at the last s_endpgm that is reachable as code (after it: constant tables)
    insts = []
    for i, (a, t) in enumerate(lines):
        if t.startswith("s_code_end"):
            break
        insts.append((a, t))
    # keep the instructions reachable from offset 0 (the code is followed by constant tables that
    # the disassembler decodes as garbage)
    idx_of = {a: k for k, (a, _) in enumerate(insts)}
    reach, work = set(), [insts[0][0]]
    while work:
        a = work.pop()
        while a in idx_of and a not in reach:
            reach.add(a)
            k = idx_of[a]
            t = insts[k][1]
            nx = insts[k + 1][0] if k + 1 < len(insts) else a + 4
            if t.startswith(".long"):  # SDWA word pair (see below): the second word is not an instruction
                nx = insts[k + 2][0]
            m = re.match(r"s_(cbranch_\w+|branch) (\d+)", t)
            if m:
                simm = int(m.group(2))
                simm = simm - 0x10000 if simm >= 0x8000 else simm
                work.append(nx + 4 * simm)
                if m.group(1) == "branch":
                    break
            if t.startswith("s_endpgm"):
                break
            a = nx
    keep = []
    for k, (a, t) in enumerate(insts):
        if a in reach:
            keep.append((a, t))
            if t.startswith(".long"):
                keep.append(insts[k + 1])
    insts = keep
    ctx = Ctx(name, cfg, blob)
    # .long words the disassembler could not decode (SDWA v_mov with a source modifier): decode
    fixed = []
    i = 0
    while i < len(insts):
        a, t = insts[i]
        if t.startswith(".long"):
            w0 = int(re.search(r"0x([0-9a-fA-F]+)", t).group(1), 16)
            w1 = int(re.search(r"0x([0-9a-fA-F]+)", insts[i + 1][1]).group(1), 16)
            if (w0 >> 25) == 0x3F and ((w0 >> 9) & 0xFF) == 0x01 and (w0 & 0x1FF) == 0xF9:
                vd = (w0 >> 17) & 0xFF
                src = w1 & 0xFF
                neg, ab = (w1 >> 20) & 1, (w1 >> 21) & 1
                s = "v%d" % src
                if ab:
                    s = "|%s|" % s
                if neg:
                    s = "-" + s
                fixed.append((a, "v_mov_b32_sdwa v%d, %s dst_sel:DWORD dst_unused:UNUSED_PAD src0_sel:DWORD"
                              " /* decoded from %08X %08X */" % (vd, s, w0, w1)))
                i += 2
                continue
            die("undecodable word at %X" % a)
        fixed.append((a, t))
        i += 1
    insts = fixed
    addrs = [a for a, _ in insts]
    nxt_of = {addrs[k]: (addrs[k + 1] if k + 1 < len(addrs) else addrs[k] + 4) for k in range(len(addrs))}
    for a, b in cfg["skip"]:
        pass
    # translate
    trans = []
    for a, t in insts:
        if any(lo <= a < hi for lo, hi in cfg["skip"]):
            trans.append((a, t, ["/* skipped: NGG primitive allocation / export prologue */"]))
            continue
        tt = re.sub(r"\s*/\*.*\*/", "", t)
        trans.append((a, t, translate(ctx, tt, nxt_of[a])))
    # basic blocks
    leaders = {insts[0][0]}
    for a, t, st in trans:
        for s in st:
            if s.startswith("@@BRANCH"):
                leaders.add(int(s.split()[-1]))
                leaders.add(nxt_of[a])
            if s in ("@@END", "@@KILL") or s.startswith("@@COLOR"):
                leaders.add(nxt_of[a])
    leaders = sorted(x for x in leaders if x in nxt_of or x == insts[0][0])
    # ---- emit -----------------------------------------------------------------------------------
    o = []
    st = cfg["stage"]
    o.append("#version 450")
    o.append("#extension GL_KHR_shader_subgroup_vote : require")
    if ctx.uses_ballot:
        o.append("#extension GL_KHR_shader_subgroup_ballot : require")
        o.append("#extension GL_KHR_shader_subgroup_shuffle : require")
    o.append("// GENERATED by tools/shaders/isa2glsl.py from the ShellUI eboot (NPXS40087 eboot.bin, FW 4.03)")
    o.append("// embedded shader \"%s\" -- one statement per RDNA instruction; see the tool's header for" % name)
    o.append("// the register / EXEC / control-flow model. Do not edit by hand: regenerate.")
    o.append("")
    if st == "comp":
        o.append("layout(local_size_x = 64) in;  // .shader_header sh 0x207..0x209 COMPUTE_NUM_THREAD_X/Y/Z = 64, 1, 1")
    if not cfg.get("no_srt"):
        o.append("layout(set = 0, binding = %d, std140) uniform SRT { uvec4 d[2]; } srt;  // user-data V# %s (SRT, 32 bytes)"
                 % (cfg["srt_binding"], cfg["srt_vsharp"]))
        o.append("layout(set = 0, binding = %d, std140) uniform PCB { uvec4 d[20]; } pcb;  // the CB the SRT points to (+0), 320 bytes"
                 % cfg["p_binding"])
    for sreg, (cname, bind) in sorted(cfg.get("ud_cbs", {}).items(), key=lambda x: x[1][1]):
        o.append("layout(set = 0, binding = %d, std140) uniform %s_ { uvec4 d[16]; } %s_cb;  // constant buffer, V# in user data %s"
                 % (bind, cname, cname, sreg))
    for sreg, (tname, bind) in sorted(cfg.get("ud_textures", {}).items(), key=lambda x: x[1][1]):
        samp = ", ".join(sorted(s for t, s in ctx.sampled if t == tname)) or "unused"
        o.append("layout(set = 0, binding = %d) uniform sampler2D %s;  // T# in user data %s, S# in %s (user data)"
                 % (bind, tname, sreg, samp))
    if cfg.get("ud_values") is not None:
        o.append("layout(set = 0, binding = %d, std140) uniform UD_ { uvec4 d[16]; } ud;  // user-data dwords read as values (T# words)"
                 % cfg["ud_values"])
    for poff, (tname, bind) in sorted(cfg.get("p_textures", {}).items()):
        samp = ", ".join(sorted(s for t, s in ctx.sampled if t == tname)) or "unused"
        o.append("layout(set = 0, binding = %d) uniform sampler2D %s;  // T# at P+0x%X, S# in %s (s_mov immediates; see %s)"
                 % (bind, tname, poff, samp, cfg.get("ssharp_note", "the host")))
    for sreg, (iname, lay, typ) in sorted(cfg.get("images", {}).items()):
        o.append("layout(set = 0, %s) uniform %s %s;  // image descriptor %s" % (lay, typ, iname, sreg))
    if cfg.get("push"):
        o.append("layout(push_constant) uniform PC_ { %s } pc_;" % cfg["push"])
    blocks = {}
    for off, (bn, bind) in sorted(cfg["p_buffers"].items()):
        blocks[bn] = bind
    for bn, bind in sorted(blocks.items(), key=lambda x: x[1]):
        o.append("layout(set = 0, binding = %d, std430) %sbuffer %s_ { uint d[]; } %s;" %
                 (bind, "" if st == "comp" or (st == "vert") else "readonly ", bn, bn))
    if st == "frag":
        for k, n in sorted(x for x in ctx.outputs if x[0] == "attr"):
            flat = "flat " if n == 4 else ""
            comps = cfg.get("attr_comps", {}).get(n, 4)  # match a narrower vertex output (eboot basic_vv: vec2)
            o.append("layout(location = %d) %sin %s attr%d;" % (n, flat, "vec%d" % comps if comps > 1 else "float", n))
        o.append("layout(location = 0) out vec4 outColor;  // mrt0 (32_ABGR fp16 export)")
    if st == "vert":
        for k, n in sorted(x for x in ctx.outputs if x[0] == "param"):
            flat = "flat " if n == 4 else ""
            o.append("layout(location = %d) %sout vec4 param%d;" % (n, flat, n))
    o.append(HELPERS)
    if not cfg.get("no_srt"):
        o.append("uint srt_u(uint off) { return off < 32u ? srt.d[off >> 4][(off >> 2) & 3u] : 0u; }")
        o.append("uint p_u(uint off) { return off < 320u ? pcb.d[off >> 4][(off >> 2) & 3u] : 0u; }")
    for sreg, (cname, bind) in sorted(cfg.get("ud_cbs", {}).items(), key=lambda x: x[1][1]):
        o.append("uint %s_u(uint off) { return off < 256u ? %s_cb.d[off >> 4][(off >> 2) & 3u] : 0u; }" % (cname, cname))
    if ctx.uses_med3:
        o.append("// v_med3_f32: median of three; with a NaN operand the result is min3 (fmin_ semantics).")
        o.append("float fmed3_(float a, float b, float c) { if (isnan(a) || isnan(b) || isnan(c)) return fmin_(fmin_(a, b), c);"
                 " return max(min(a, b), min(max(a, b), c)); }")
    if ctx.uses_cvt_i32:  # only where used, so the earlier transcriptions regenerate unchanged
        o.append("// v_cvt_i32_f32: round toward zero, saturated to int, NaN -> 0.")
        o.append("uint cvt_i32_f32_(float x) { return isnan(x) ? 0u : x <= -2147483648.0 ? 0x80000000u : x >= 2147483647.0 ? 0x7FFFFFFFu : uint(int(x)); }")
    for code_off, tn in sorted(ctx.const_tables.items()):
        words = []
        k = code_off
        while k + 4 <= len(code):
            words.append(struct.unpack_from("<I", code, k)[0])
            k += 4
            if len(words) >= cfg.get("table_words", 64):
                break
        o.append("// constant table at .shader_text+0x%X (s_getpc-relative), read as a raw buffer" % code_off)
        o.append("const uint kTable%X[%d] = uint[](%s);" % (code_off, len(words), ", ".join("0x%08Xu" % w for w in words)))
        o.append("uint ctab_%X(uint off, uint num_records) { return (off + 4u <= num_records && (off >> 2) < %du) ? kTable%X[off >> 2] : 0u; }"
                 % (code_off, len(words), code_off))
    o.append("")
    o.append("void main() {")
    regs = sorted(ctx.vregs, key=lambda r: int(r[1:])) + sorted((r for r in ctx.sregs if re.fullmatch(r"s\d+", r)), key=lambda r: int(r[1:]))
    regs += sorted(r for r in ctx.sregs if not re.fullmatch(r"s\d+", r))
    for k in range(0, len(regs), 16):
        o.append("    uint " + ", ".join("%s = 0u" % r for r in regs[k:k + 16]) + ";")
    ms = sorted(ctx.masks - {"EXEC"})
    if ms:
        o.append("    bool " + ", ".join("%s = false" % m for m in ms) + ";")
    o.append("    bool EXEC = %s, SCC = false;" % ("(" + cfg["exec_init"] + ")" if cfg.get("exec_init") else "true"))
    if st == "frag":
        o.append("    // s[0:3] = SRT V#; v0/v1 = barycentrics (only consumed by v_interp)")
    for r, e in cfg["inputs"].items():
        o.append("    %s = %s;" % (r, e))
    if cfg.get("ud_values") is not None:  # user SGPRs hold the user-data dwords at wave start
        for r in sorted((r for r in ctx.sregs if re.fullmatch(r"s\d+", r)), key=lambda r: int(r[1:])):
            n = int(r[1:])
            if n < cfg["ud_dwords"]:
                o.append("    %s = ud.d[%d][%d];  // user-data dword %d" % (r, n >> 2, n & 3, n))
    o.append("    int pc = 0x%X;" % insts[0][0])
    o.append("    for (;;) {")
    o.append("        switch (pc) {")
    cur = None
    for a, t, st_list in trans:
        if a in leaders:
            if cur is not None:
                o.append("            pc = 0x%X; break;" % a)
            o.append("        case 0x%X:" % a)
            cur = a
        for s in st_list:
            if s.startswith("@@BRANCH"):
                _, cond, tgt = s.split(" ", 1)[0], s.split(" ", 1)[1].rsplit(" ", 1)[0], int(s.rsplit(" ", 1)[1])
                if cond == "true":
                    o.append("            pc = 0x%X; break;  // 0x%03X %s" % (tgt, a, t))
                else:
                    o.append("            if (%s) { pc = 0x%X; break; }  // 0x%03X %s" % (cond, tgt, a, t))
            elif s == "@@END":
                o.append("            pc = -1; break;  // 0x%03X %s" % (a, t))
            elif s == "@@KILL":
                o.append("            discard;  // 0x%03X %s (no lane exports)" % (a, t))
            elif s.startswith("@@COLOR"):
                _, lo, hi = s.split()
                o.append("            if (!EXEC) discard;  // lanes outside EXEC do not export")
                o.append("            outColor = vec4(unpackHalf2x16(%s), unpackHalf2x16(%s));  // 0x%03X %s" % (lo, hi, a, t))
            elif s.startswith("@"):
                o.append("            if (EXEC) %s  // 0x%03X %s" % (s[1:], a, t))
            elif s.startswith("/*"):
                o.append("            %s  // 0x%03X %s" % (s, a, t))
            else:
                o.append("            %s  // 0x%03X %s" % (s, a, t))
    o.append("            pc = -1; break;")
    o.append("        default: pc = -1; break;")
    o.append("        }")
    o.append("        if (pc < 0) break;")
    o.append("    }")
    o.append("}")
    open(out_path, "w", newline="\n").write("\n".join(o) + "\n")
    print(out_path, len(trans), "instructions,", len(leaders), "blocks")


if __name__ == "__main__":
    main()
