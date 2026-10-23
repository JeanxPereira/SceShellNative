#version 450
// Transcription of libScePsm embedded shader #14 "solid_fill_p" (work/shaders/14_solid_fill_p.ags.bin),
// AGC binary type 1 (pixel), .shader_text 0x330 bytes (code up to s_endpgm at 0x1C4). EmbeddedShader.SolidFill = (basic_vv, solid_fill_p).
// Every statement maps to the RDNA instruction at the byte offset in its trailing comment.
//
// Math (c = vertex color attr0, p = ClipPos = attr1.zw, F = u_Flag):
//   if (F & 6):
//     m = 1
//     if (F & 4): rounded-rect coverage against u_TargetRect/u_Radius, with y scaled by 0.5625:
//        qx = |p.x - R.x| - R.z ; qy = |0.5625*p.y - R.y| - R.w
//        d  = (min(qx,qy) < 0) ? (qx < 0 ? qy : qx) : sqrt(qx*qx + qy*qy)
//        dx = coarse ddx(p.x);  if (d > u_Radius - dx) m = saturate((u_Radius - d) * (1/dx) + 0.5)
//     if (F & 2): f = prod_i saturate(p[i]*FadeA[i] + FadeB[i]) over (p.x,p.x,p.y,p.y); m = f*f*m*(3 - 2f)
//     c.a *= m;  if (F & 1) c.rgb *= m
//   k = (F & 1) ? u_GlobalAlpha * u_GlobalIntensity : u_GlobalIntensity
//   out = (c.rgb * k, c.a * u_GlobalAlpha)
//
// Not expressible / differences:
//   * dFdxCoarse stands for the DPP pair (quad lane 1 minus quad lane 0 of ClipPos.x); that is what AMD
//     hardware computes, other GPUs' coarse derivative may pick a different lane pair.
//   * v_rcp_f32 (1 ulp) vs GLSL 1.0/x; v_mad_f32/v_mac_f32 are unfused (locals are `precise`).
//   * v_min_f32 NaN rules differ from GLSL min() (undefined for NaN).
//   * Output is packed to fp16 with round-toward-zero (v_cvt_pkrtz_f16_f32); the render target format decides here.

layout(set = 0, binding = 1, std140) uniform GLOBAL_CB {   // metadata "__GLOBAL_CB__", resource slot 0, 56 bytes
    layout(offset = 0)  vec4  u_FadeA;
    layout(offset = 16) vec4  u_FadeB;
    layout(offset = 32) vec4  u_TargetRect;
    layout(offset = 48) float u_Radius;
    layout(offset = 52) uint  u_Flag;       // metadata base type 9 (32-bit integer class)
};
// globalConstantBuffer: metadata resource slot 7, 8 bytes. Created by PsmGraphicsContext::Create (icall 0x1e5d50,
// sub_1230 with initial data {1.0f, 1.0f}) and rewritten only by PsmGraphicsContext::UpdateGlobalConstantBuffer
// (icall 0x1f1c80, memcpy of the managed struct). Bound to PS and VS slot 7 at every draw (sub_1F32A0).
layout(set = 0, binding = 2, std140) uniform globalConstantBuffer {
    layout(offset = 0) float u_GlobalAlpha;
    layout(offset = 4) float u_GlobalIntensity;
};

layout(location = 0) in vec4 in_attr0;   // PS attr0 = input semantic 0: input.Color (COLOR)
layout(location = 1) in vec4 in_attr1;   // PS attr1 = input semantic 1: .xy input.TexCoord (TEXCOORD0), .zw input.ClipPos (TEXCOORD1)
layout(location = 0) out vec4 out_mrt0;  // exp mrt0 (SPI_SHADER_COL_FORMAT = 4: packed fp16, v_cvt_pkrtz_f16_f32 = round toward zero)

void main()
{
    precise float v0, v1, v2, v3, v4, v5, v6, v7, v8, v9, v10;
    float s8, s9, s10, s11, s12, s13, s14, s15;
    uint s18_u;
    float vcc_lo, vcc_hi;
    bool vcc;
    s18_u = u_Flag;                                                          // 0x8
    v5 = in_attr0.x;                                                         // 0x34
    v4 = in_attr0.y;                                                         // 0x38
    v3 = in_attr0.z;                                                         // 0x3C
    v2 = in_attr0.w;                                                         // 0x40
    v7 = in_attr1.z;                                                         // 0x44
    if ((s18_u & 6u) != 0u) {
        v6 = 1.0;                                                                // 0x4C
        v0 = in_attr1.w;                                                         // 0x58
        if ((s18_u & 4u) != 0u) {
            vcc_lo = uintBitsToFloat(0x3F100000u);   // 0.5625                       // 0x68
            s8 = u_TargetRect.x; s9 = u_TargetRect.y; s10 = u_TargetRect.z; s11 = u_TargetRect.w; // 0x70
            v8 = vcc_lo * v0 + -s9;                                                  // 0x7C
            v9 = v7 - s8;                                                            // 0x84
            v1 = dFdxCoarse(v7);   // v_mov_b32_dpp quad_perm[0,0,0,0] + v_sub_f32_dpp quad_perm[1,1,1,1] // 0x88
            v8 = -s11 + abs(v8);                                                     // 0x94
            v9 = -s10 + abs(v9);                                                     // 0x9C
            v10 = min(v9, v8);                                                       // 0xA4
            if (0.0 > v10) {                                                         // 0xA8
                vcc = 0.0 > v9;                                                          // 0xAC
                v8 = vcc ? v8 : v9;                                                      // 0xB0
            } else {                                                                 // 0xB4
                v9 = v9 * v9;                                                            // 0xB8
                v9 = v8 * v8 + v9;                                                       // 0xBC
                v8 = sqrt(v9);                                                           // 0xC0
            }                                                                        // 0xC4
            vcc_lo = u_Radius;                                                       // 0xC8
            v9 = vcc_lo - v1;                                                        // 0xD4
            if (v8 > v9) {                                                           // 0xD8
                v6 = vcc_lo - v8;                                                        // 0xDC
                v1 = 1.0 / v1;                                                           // 0xE0
                v6 = clamp(v1 * v6 + 0.5, 0.0, 1.0);                                     // 0xE4
            }                                                                        // 0xEC
        }
        if ((s18_u & 2u) != 0u) {
            s8 = u_FadeA.x; s9 = u_FadeA.y; s10 = u_FadeA.z; s11 = u_FadeA.w; s12 = u_FadeB.x; s13 = u_FadeB.y; s14 = u_FadeB.z; s15 = u_FadeB.w; // 0xF8
            v1 = clamp(s8 * v7 + s12, 0.0, 1.0);                                     // 0x104
            v8 = clamp(v7 * s9 + s13, 0.0, 1.0);                                     // 0x10C
            v7 = clamp(v0 * s10 + s14, 0.0, 1.0);                                    // 0x114
            v0 = clamp(v0 * s11 + s15, 0.0, 1.0);                                    // 0x11C
            v1 = v1 * v8;                                                            // 0x124
            v1 = v1 * v7;                                                            // 0x128
            v1 = v1 * v0;                                                            // 0x12C
            v0 = v1 * v1;                                                            // 0x130
            v1 = -2.0 * v1 + 3.0;                                                    // 0x134
            v0 = v0 * v6;                                                            // 0x13C
            v6 = v0 * v1;                                                            // 0x140
        }
        v0 = v6 * v5;                                                            // 0x144
        v1 = v6 * v4;                                                            // 0x148
        v7 = v6 * v3;                                                            // 0x14C
        v2 = v6 * v2;                                                            // 0x154
        vcc = (s18_u & 1u) != 0u;                                                // 0x158
        v5 = vcc ? v0 : v5;                                                      // 0x15C
        v4 = vcc ? v1 : v4;                                                      // 0x160
        v3 = vcc ? v7 : v3;                                                      // 0x164
    }
    vcc_hi = u_GlobalIntensity;                                              // 0x168
    v1 = vcc_hi;                                                             // 0x174
    if ((s18_u & 1u) != 0u) {
        vcc_lo = u_GlobalAlpha;                                                  // 0x180
        v1 = vcc_lo * vcc_hi;                                                    // 0x18C
    }
    v0 = v1 * v5;                                                            // 0x194
    vcc_lo = u_GlobalAlpha;                                                  // 0x198
    v4 = v1 * v4;                                                            // 0x1A0
    v1 = v1 * v3;                                                            // 0x1A4
    v2 = vcc_lo * v2;                                                        // 0x1AC
    out_mrt0 = vec4(v0, v4, v1, v2);   // v_cvt_pkrtz_f16_f32 v0,v0,v4 / v1,v1,v2 ; exp mrt0 compr (0x1B0..0x1BC)
}
