#version 450
// Transcription of libScePsm embedded shader #68 "vglow_p" (work/shaders/68_vglow_p.ags.bin),
// AGC binary type 1 (pixel), code up to s_endpgm at 0x2A4. EmbeddedShader.GlowV = (glow_vv, vglow_p).
// Every statement maps to the RDNA instruction at the byte offset in its trailing comment.
//
// Math (c = Color = attr0, uv = TexCoord = attr1.xy, p = ClipPos = attr1.zw, F = u_Flag, b = BlurAmount):
//   m = 1;  F & 4: rounded-rect coverage (as solid_fill_p.frag);  F & 2: fade ramp (as solid_fill_p.frag)
//     (here the two bits are tested independently, without the F & 6 guard)
//   s = the 13-tap alpha sum of hglow_p.frag along y (same weights, same accumulation order)
//   out = (c.r, c.g, c.b, ((u_GlobalAlpha * m) * c.a) * s).  u_GlobalIntensity is not read (metadata: unused).
//
// Resources: s_Texture t0 -> s[0:7], Sampler0 -> s[8:11], __GLOBAL_CB__ slot 0 -> s[12:15] (60 bytes),
//   globalConstantBuffer slot 7 -> s[16:19].
// Interpolants: PS input semantics 0,1 (attr0 = main.Color, attr1 = TexCoord/ClipPos).
//
// Not expressible / differences:
//   * dFdxCoarse stands for the DPP pair (quad lane 1 at 0x38 minus quad lane 0 at 0x60 of ClipPos.x).
//   * image_sample dmask 0x8 returns the 4th component after the T# swizzle; written texture(...).a.
//   * v_rcp_f32 (1 ulp) vs GLSL 1.0/x; v_mad/v_mac/v_madak/v_madmk are unfused (locals are `precise`).
//   * v_min_f32 NaN rules differ from GLSL min().
//   * Output is packed to fp16 with round-toward-zero (v_cvt_pkrtz_f16_f32); the render target format decides here.

layout(set = 0, binding = 1, std140) uniform GLOBAL_CB {   // metadata "__GLOBAL_CB__", resource slot 0, 60 bytes
    layout(offset = 0)  vec4  u_FadeA;
    layout(offset = 16) vec4  u_FadeB;
    layout(offset = 32) vec4  u_TargetRect;
    layout(offset = 48) float u_Radius;
    layout(offset = 52) uint  u_Flag;       // metadata base type 9 (32-bit integer class)
    layout(offset = 56) float BlurAmount;
};
// globalConstantBuffer: metadata resource slot 7, 8 bytes (see solid_fill_p.frag).
layout(set = 0, binding = 2, std140) uniform globalConstantBuffer {
    layout(offset = 0) float u_GlobalAlpha;
    layout(offset = 4) float u_GlobalIntensity;   // not read by this shader
};
// s_Texture (t0) with Sampler0 (s0).
layout(set = 0, binding = 3) uniform sampler2D s_Texture;

layout(location = 0) in vec4 in_attr0;   // PS attr0 = input semantic 0: input.Color (COLOR)
layout(location = 1) in vec4 in_attr1;   // PS attr1 = input semantic 1: .xy input.TexCoord, .zw input.ClipPos
layout(location = 0) out vec4 out_mrt0;  // exp mrt0 (SPI_SHADER_COL_FORMAT = 4: packed fp16, v_cvt_pkrtz_f16_f32 = round toward zero)

void main()
{
    precise float v0, v1, v2, v3, v4, v5, v6, v7, v8, v9, v10, v11, v12, v13, v14, v15, v16, v17, v18;
    precise float c_r, c_g;
    float s20, s21, s22, s23, s24, s25, s26, s27;
    uint s24_u;
    float vcc_lo, vcc_hi;
    bool vcc;
    s24_u = u_Flag;                                                          // 0x8
    v4 = 1.0;                                                                // 0x20
    v3 = in_attr1.z;                                                         // 0x24/0x2C
    v2 = in_attr1.w;                                                         // 0x28/0x30
    if ((s24_u & 4u) != 0u) {                                                // s_bitcmp1_b32 0x1C, s_cbranch_scc0 0x34
        vcc_lo = uintBitsToFloat(0x3F100000u);   // 0.5625                       // 0x40
        s20 = u_TargetRect.x; s21 = u_TargetRect.y; s22 = u_TargetRect.z; s23 = u_TargetRect.w; // 0x48
        v7 = vcc_lo * v2 + -s21;                                                 // 0x54
        v8 = v3 - s20;                                                           // 0x5C
        v7 = -s23 + abs(v7);                                                     // 0x6C
        v8 = -s22 + abs(v8);                                                     // 0x74
        v9 = min(v8, v7);                                                        // 0x7C
        if (0.0 > v9) {                                                          // v_cmpx_gt 0x80
            vcc = 0.0 > v8;                                                          // 0x84
            v7 = vcc ? v7 : v8;                                                      // 0x88
        } else {                                                                 // s_andn2_b64 exec 0x8C
            v8 = v8 * v8;                                                            // 0x90
            v8 = v7 * v7 + v8;                                                       // 0x94
            v7 = sqrt(v8);                                                           // 0x98
        }                                                                        // 0x9C
        vcc_lo = u_Radius;                                                       // 0xA0
        v5 = dFdxCoarse(v3);   // v_mov_b32_dpp quad_perm[1,1,1,1] (0x38) - v_mov_b32_dpp quad_perm[0,0,0,0] (0x60) // v_sub 0xA8
        v6 = vcc_lo - v5;                                                        // 0xB0
        if (v7 > v6) {                                                           // v_cmpx_gt 0xB4
            v4 = vcc_lo - v7;                                                        // 0xB8
            v5 = 1.0 / v5;                                                           // 0xBC
            v4 = clamp(v5 * v4 + 0.5, 0.0, 1.0);                                     // 0xC0
        }                                                                        // 0xC8
    }
    if ((s24_u & 2u) != 0u) {                                                // s_bitcmp1_b32 0xCC, s_cbranch_scc0 0xD0
        s20 = u_FadeA.x; s21 = u_FadeA.y; s22 = u_FadeA.z; s23 = u_FadeA.w; s24 = u_FadeB.x; s25 = u_FadeB.y; s26 = u_FadeB.z; s27 = u_FadeB.w; // 0xD4
        v5 = clamp(s20 * v3 + s24, 0.0, 1.0);                                    // 0xE0
        v6 = clamp(v3 * s21 + s25, 0.0, 1.0);                                    // 0xE8
        v3 = clamp(v2 * s22 + s26, 0.0, 1.0);                                    // 0xF0
        v2 = clamp(v2 * s23 + s27, 0.0, 1.0);                                    // 0xF8
        v5 = v5 * v6;                                                            // 0x100
        v3 = v5 * v3;                                                            // 0x104
        v3 = v3 * v2;                                                            // 0x108
        v2 = v3 * v3;                                                            // 0x10C
        v3 = -2.0 * v3 + uintBitsToFloat(0x40400000u);   // 3.0                  // v_madak 0x110
        v2 = v2 * v4;                                                            // 0x118
        v4 = v2 * v3;                                                            // 0x11C
    }
    v7 = in_attr1.y;                                                         // 0x120/0x124
    vcc_hi = BlurAmount;                                                     // 0x128
    v3 = vcc_hi + v7;                                                        // 0x134
    v12 = 2.0 * vcc_hi + vcc_hi;                                             // 0x138
    v2 = in_attr1.x;                                                         // 0x140/0x144
    v15 = in_attr0.x;                                                        // 0x148/0x14C
    v13 = vcc_hi + v12;                                                      // 0x150
    vcc_lo = u_GlobalAlpha;                                                  // 0x154
    v5 = vcc_lo * v4;                                                        // 0x160
    v8 = texture(s_Texture, vec2(v2, v3)).a;                                 // 0x164
    v4 = texture(s_Texture, vec2(v2, v7)).a;                                 // 0x16C
    v3 = v7 - vcc_hi;                                                        // 0x178
    v10 = vcc_hi + v13;                                                      // 0x17C
    v6 = in_attr0.w;                                                         // 0x180/0x184
    v6 = v5 * v6;                                                            // 0x188
    v9 = texture(s_Texture, vec2(v2, v3)).a;                                 // 0x18C
    v3 = 2.0 * vcc_hi + v7;                                                  // 0x194
    v11 = vcc_hi + v10;                                                      // 0x19C
    v5 = in_attr0.z;                                                         // 0x1A0/0x1A8
    v0 = in_attr0.y;                                                         // 0x1A4/0x1BC
    v14 = texture(s_Texture, vec2(v2, v3)).a;                                // 0x1AC
    v3 = 2.0 * -vcc_hi + v7;                                                 // 0x1B4
    c_r = v15; c_g = v0;                                                     // v_cvt_pkrtz_f16_f32 v0, v15, v0 0x1C0
    v18 = texture(s_Texture, vec2(v2, v3)).a;                                // 0x1C4
    v3 = v7 + v12;                                                           // 0x1CC
    v15 = texture(s_Texture, vec2(v2, v3)).a;                                // 0x1D0
    v3 = v7 - v12;                                                           // 0x1D8
    v17 = texture(s_Texture, vec2(v2, v3)).a;                                // 0x1DC
    v3 = v7 + v13;                                                           // 0x1E4
    v12 = texture(s_Texture, vec2(v2, v3)).a;                                // 0x1E8
    v3 = v7 - v13;                                                           // 0x1F0
    v16 = texture(s_Texture, vec2(v2, v3)).a;                                // 0x1F4
    v3 = v7 + v10;                                                           // 0x1FC
    v1 = texture(s_Texture, vec2(v2, v3)).a;                                 // 0x200
    v3 = v7 - v10;                                                           // 0x208
    v13 = texture(s_Texture, vec2(v2, v3)).a;                                // 0x20C
    v3 = v7 + v11;                                                           // 0x214
    v10 = texture(s_Texture, vec2(v2, v3)).a;                                // 0x218
    v3 = v7 - v11;                                                           // 0x220
    v7 = texture(s_Texture, vec2(v2, v3)).a;                                 // 0x224
    v2 = v9 + v8;                                                            // 0x230
    v3 = v18 + v14;                                                          // 0x238
    v2 = uintBitsToFloat(0x3E18311Du) * v2;                                  // 0x23C
    v2 = v3 * uintBitsToFloat(0x3DEF6FCAu) + v2;                             // v_madmk 0x244
    v3 = v17 + v15;                                                          // 0x250
    v1 = v13 + v1;                                                           // 0x258
    v2 = v3 * uintBitsToFloat(0x3DA07FCDu) + v2;                             // v_madmk 0x25C
    v3 = v16 + v12;                                                          // 0x264
    v2 = v3 * uintBitsToFloat(0x3D375B71u) + v2;                             // v_madmk 0x268
    v3 = v7 + v10;                                                           // 0x274
    v1 = v1 * uintBitsToFloat(0x3CB27FC5u) + v2;                             // v_madmk 0x278
    v1 = v3 * uintBitsToFloat(0x3C1413A8u) + v1;                             // v_madmk 0x280
    v1 = v4 * uintBitsToFloat(0x3E24DE11u) + v1;                             // v_madmk 0x288
    v1 = v6 * v1;                                                            // 0x290
    out_mrt0 = vec4(c_r, c_g, v5, v1);   // v_cvt_pkrtz_f16_f32 v0,v15,v0 (0x1C0) / v1,v5,v1 (0x294) ; exp mrt0 compr 0x29C
}
