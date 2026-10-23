#version 450
// Transcription of libScePsm embedded shader #41 "rgba_nomip_p" (work/shaders/41_rgba_nomip_p.ags.bin),
// AGC binary type 1 (pixel), code up to s_endpgm at 0x1F0. EmbeddedShader.TextureNoMip = (basic_vv, rgba_nomip_p).
// Every statement maps to the RDNA instruction at the byte offset in its trailing comment.
//
// Math: rgba_p without its pre-filter:
//   c = attr0 * texture(s_Texture, attr1.xy)          (one image_sample with implicit derivatives)
//   then the common u_Flag tail of solid_fill_p / rgba_p (see solid_fill_p.frag), p = ClipPos = attr1.zw:
//   if (F & 6): m = 1; (F & 4) rounded-rect coverage; (F & 2) fade ramp; c.a *= m; if (F & 1) c.rgb *= m
//   k = (F & 1) ? u_GlobalAlpha * u_GlobalIntensity : u_GlobalIntensity
//   out = (c.rgb * k, c.a * u_GlobalAlpha)
//
// Not expressible / differences (as in solid_fill_p.frag / rgba_p.frag):
//   * dFdxCoarse stands for the DPP pair (quad lane 1 minus quad lane 0 of ClipPos.x).
//   * v_rcp_f32 (1 ulp) vs GLSL 1.0/x; v_mad_f32/v_mac_f32 are unfused (locals are `precise`).
//   * v_min_f32 NaN rules differ from GLSL min().
//   * Output is packed to fp16 with round-toward-zero (v_cvt_pkrtz_f16_f32); the render target format decides here.

layout(set = 0, binding = 1, std140) uniform GLOBAL_CB {   // metadata "__GLOBAL_CB__", resource slot 0, 56 bytes
    layout(offset = 0)  vec4  u_FadeA;
    layout(offset = 16) vec4  u_FadeB;
    layout(offset = 32) vec4  u_TargetRect;
    layout(offset = 48) float u_Radius;
    layout(offset = 52) uint  u_Flag;       // metadata base type 9 (32-bit integer class)
};
// globalConstantBuffer: metadata resource slot 7, 8 bytes (see solid_fill_p.frag).
layout(set = 0, binding = 2, std140) uniform globalConstantBuffer {
    layout(offset = 0) float u_GlobalAlpha;
    layout(offset = 4) float u_GlobalIntensity;
};
// s_Texture / Sampler0: T# s[0:7], S# s[8:11] (t-slot 0, sampler slot 0).
layout(set = 0, binding = 3) uniform sampler2D s_Texture;

layout(location = 0) in vec4 in_attr0;   // PS attr0 = input semantic 0: input.Color (COLOR)
layout(location = 1) in vec4 in_attr1;   // PS attr1 = input semantic 1: .xy input.TexCoord (TEXCOORD0), .zw input.ClipPos (TEXCOORD1)
layout(location = 0) out vec4 out_mrt0;  // exp mrt0 (SPI_SHADER_COL_FORMAT = 4: packed fp16, v_cvt_pkrtz_f16_f32 = round toward zero)

void main()
{
    precise float v0, v1, v2, v3, v4, v5, v6, v7, v8, v9, v10;
    float s0, s1, s2, s3, s4, s5, s6, s7;
    uint s22_u;
    float vcc_lo, vcc_hi;
    bool vcc;
    s22_u = u_Flag;                                                          // 0x8
    v6 = in_attr1.x;                                                         // 0x20/0x28
    v7 = in_attr1.y;                                                         // 0x24/0x2C
    vec4 t = texture(s_Texture, vec2(v6, v7));                               // image_sample dmask 0xf // 0x30
    v2 = t.x; v3 = t.y; v4 = t.z; v5 = t.w;
    v10 = in_attr0.x;                                                        // 0x38/0x48
    v9 = in_attr0.y;                                                         // 0x3C/0x4C
    v8 = in_attr0.z;                                                         // 0x40/0x50
    v6 = in_attr0.w;                                                         // 0x44/0x54
    v7 = in_attr1.z;                                                         // 0x58/0x5C
    v2 = v2 * v10;                                                           // 0x64
    v3 = v9 * v3;                                                            // 0x68
    v4 = v8 * v4;                                                            // 0x6C
    v5 = v6 * v5;                                                            // 0x70
    if ((s22_u & 6u) != 0u) {                                                // s_and_b32 0x1C, s_cbranch_scc0 0x74
        v6 = 1.0;                                                                // 0x78
        v0 = in_attr1.w;                                                         // 0x80/0x84
        if ((s22_u & 4u) != 0u) {                                                // 0x7C / 0x88
            vcc_lo = uintBitsToFloat(0x3F100000u);   // 0.5625                       // 0x94
            s0 = u_TargetRect.x; s1 = u_TargetRect.y; s2 = u_TargetRect.z; s3 = u_TargetRect.w; // 0x9C
            v8 = vcc_lo * v0 + -s1;                                                  // 0xA8
            v9 = v7 - s0;                                                            // 0xB0
            v1 = dFdxCoarse(v7);   // v_mov_b32_dpp quad_perm[0,0,0,0] (0x8C) + v_sub_f32_dpp quad_perm[1,1,1,1] // 0xB4
            v8 = -s3 + abs(v8);                                                      // 0xC0
            v9 = -s2 + abs(v9);                                                      // 0xC8
            v10 = min(v9, v8);                                                       // 0xD0
            if (0.0 > v10) {                                                         // v_cmpx_gt 0xD4
                vcc = 0.0 > v9;                                                          // 0xD8
                v8 = vcc ? v8 : v9;                                                      // 0xDC
            } else {                                                                 // s_andn2_b64 exec 0xE0
                v9 = v9 * v9;                                                            // 0xE4
                v9 = v8 * v8 + v9;                                                       // 0xE8
                v8 = sqrt(v9);                                                           // 0xEC
            }                                                                        // 0xF0
            vcc_lo = u_Radius;                                                       // 0xF4
            v9 = vcc_lo - v1;                                                        // 0x100
            if (v8 > v9) {                                                           // v_cmpx_gt 0x104
                v6 = vcc_lo - v8;                                                        // 0x108
                v1 = 1.0 / v1;                                                           // 0x10C
                v6 = clamp(v1 * v6 + 0.5, 0.0, 1.0);                                     // 0x110
            }                                                                        // 0x118
        }
        if ((s22_u & 2u) != 0u) {                                                // 0x11C / 0x120
            s0 = u_FadeA.x; s1 = u_FadeA.y; s2 = u_FadeA.z; s3 = u_FadeA.w; s4 = u_FadeB.x; s5 = u_FadeB.y; s6 = u_FadeB.z; s7 = u_FadeB.w; // 0x124
            v1 = clamp(s0 * v7 + s4, 0.0, 1.0);                                      // 0x130
            v8 = clamp(v7 * s1 + s5, 0.0, 1.0);                                      // 0x138
            v7 = clamp(v0 * s2 + s6, 0.0, 1.0);                                      // 0x140
            v0 = clamp(v0 * s3 + s7, 0.0, 1.0);                                      // 0x148
            v1 = v1 * v8;                                                            // 0x150
            v1 = v1 * v7;                                                            // 0x154
            v1 = v1 * v0;                                                            // 0x158
            v0 = v1 * v1;                                                            // 0x15C
            v1 = -2.0 * v1 + 3.0;                                                    // 0x160
            v0 = v0 * v6;                                                            // 0x168
            v6 = v0 * v1;                                                            // 0x16C
        }
        v0 = v6 * v2;                                                            // 0x170
        v1 = v6 * v3;                                                            // 0x174
        v7 = v6 * v4;                                                            // 0x178
        v5 = v6 * v5;                                                            // 0x180
        vcc = (s22_u & 1u) != 0u;                                                // 0x17C / 0x184
        v2 = vcc ? v0 : v2;                                                      // 0x188
        v3 = vcc ? v1 : v3;                                                      // 0x18C
        v4 = vcc ? v7 : v4;                                                      // 0x190
    }
    vcc_hi = u_GlobalIntensity;                                              // 0x194
    v1 = vcc_hi;                                                             // 0x1A0
    if ((s22_u & 1u) != 0u) {                                                // 0x1A4 / 0x1A8
        vcc_lo = u_GlobalAlpha;                                                  // 0x1AC
        v1 = vcc_lo * vcc_hi;                                                    // 0x1B8
    }
    v0 = v1 * v2;                                                            // 0x1C0
    vcc_lo = u_GlobalAlpha;                                                  // 0x1C4
    v2 = v1 * v3;                                                            // 0x1CC
    v1 = v1 * v4;                                                            // 0x1D0
    v3 = vcc_lo * v5;                                                        // 0x1D8
    out_mrt0 = vec4(v0, v2, v1, v3);   // v_cvt_pkrtz_f16_f32 v0,v0,v2 / v1,v1,v3 ; exp mrt0 compr (0x1DC..0x1E8)
}
