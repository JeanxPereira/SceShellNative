"""Shared idalib helper: find every Mono internal call a module registers.

Registrations come in two shapes: static {const char* name, void* fn} tables, and
inline code (lea name; lea fn; call mono_add_internal_call). Yields (fn_ea, name).
"""
import re
import ida_bytes, ida_funcs, ida_ua, idautils, idc

PAT = re.compile(r"^[A-Za-z_][\w.]*(/[\w]+)*::[\w.<>`]+(\(.*\))?$")


def _code_target(x):
    ea = x
    for _ in range(6):
        ea = idc.next_head(ea)
        insn = ida_ua.insn_t()
        if not ida_ua.decode_insn(insn, ea):
            break
        if insn.get_canon_mnem() == "lea" and insn.ops[1].type == ida_ua.o_mem:
            t = insn.ops[1].addr
            if ida_bytes.is_code(ida_bytes.get_flags(t)) or ida_funcs.get_func(t):
                return t
        if insn.get_canon_mnem() == "call":
            break
    return None


def registrations():
    for s in idautils.Strings():
        t = str(s)
        if "::" not in t or not PAT.match(t):
            continue
        for x in idautils.DataRefsTo(s.ea):
            if ida_bytes.is_code(ida_bytes.get_flags(x)):
                fn = _code_target(x)
            else:
                fn = ida_bytes.get_qword(x + 8)
            if fn:
                yield fn, t
