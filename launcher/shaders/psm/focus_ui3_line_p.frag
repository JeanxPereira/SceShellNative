#version 450
#extension GL_EXT_demote_to_helper_invocation : require
// Transcription of libScePsm embedded shader #66 "focus_ui3_line_p" (work/shaders/66_focus_ui3_line_p.ags.bin),
// AGC binary type 1 (pixel), code up to s_endpgm at 0x73C (+ the all-killed exit at 0x740).
// EmbeddedShader.LineFocus = (focus_ui3_line_vv, focus_ui3_line_p).
// Every statement maps to the RDNA instruction at the byte offset in its trailing comment.
//
// Math (st = St = attr2.xy, fs = FrameSt = attr2.zw, p = ClipPos = attr1.zw, ca = Color.a = attr0.w, F = u_Flag,
//       R = u_FocusRadius, h = u_Pixel, sp = u_StrokePosition):
//   e  = |fs| - 1
//   d  = (sqrt(max(R + |fs.x| - 1, 0)^2 + max(Ratio*e.y + R, 0)^2) - R) + min(R + max(e.x, Ratio*e.y), 0)
//   o1 = Thickness*(sp - 1) + Offset ;  o2 = sp*Thickness + Offset
//   a  = saturate((d + 0.5h - o1) / ((0.5h + o1) + (0.5h - o1)))
//   b  = saturate((d + 0.5h - o2) / ((0.5h + o2) + (0.5h - o2)))
//   c  = 3*(a^2 - b^2) + (-2*a^3 + 2*b^3)          (= ss(a) - ss(b), ss(t) = t*t*(3-2t))
//   if (c <= 0): rgba = (1, 0, 0, 0.7) if u_ShowAlpha != 0, else demote (pixel killed) with rgba = (0, 0, 0, 0.7)
//   else:
//     nuv = 0.5 + 0.5 * (st / u_NoiseScale + u_NoiseChangeParam);  n = texture(s_TextureNoise, nuv).r
//     x = saturate(max(0, n)) ;  u = sqrt(x) * x ;  rgb = texture(s_TextureColorTable, (u, 0.5)).rgb
//     if (n > 0):
//        tone = u <= Mid.x ? Toe.x + Toe.z*u^2 + u*(Toe.y + Toe.w*u^2)
//             : (Mid.x <= u <= Mid.y) ? Md.x + Md.z*t^2 + t*(Md.y + Md.w*t^2), t = u - Mid.x
//             : u >= Mid.y ? Sh.x + Sh.z*t^2 + t*(Sh.y + Sh.w*t^2), t = u - Mid.y : 0
//          (Toe/Md/Sh/Mid = u_ToneCurveToe/Mid/Shoulder/Midpoint)
//        u = tone * (1 - u_MinOpacity) + u_MinOpacity
//     u = ca * u * c ;  rgba = u_ShowAlpha != 0 ? (u, u, u, 1) : (rgb, u)
//   rgba = saturate(max(0, rgba));  mode (F >> 20) & 0xF conversions and the common u_Flag tail exactly as
//   focus_ui3_area_p.frag (1 = PQ, 2 = gamma 2.2 of 0.025*; tail as solid_fill_p.frag).
//
// Resources: s_TextureColorTable t0 -> s[0:7], s_TextureNoise t1 -> s[8:15], Sampler0 -> s[16:19], Sampler1 -> s[20:23],
//   __GLOBAL_CB__ slot 0 -> s[24:27], globalConstantBuffer slot 7 -> s[28:31].
//   The ISA samples t1 with Sampler1 (0x1BC) and t0 with Sampler0 (0x1D8).
// Interpolants: PS input semantics 0,1,2 (attr0 = main.Color, attr1 = TexCoord/ClipPos, attr2 = St/FrameSt).
//
// Not expressible / differences:
//   * The kill (0x154) leaves the killed lanes running in WQM as helpers: written `demote`
//     (GL_EXT_demote_to_helper_invocation), which needs the Vulkan 1.3 feature shaderDemoteToHelperInvocation.
//     The all-lanes-killed early exit (0x158 -> 0x740) has the same visible result.
//   * v_log_f32 / v_exp_f32 are written log2 / exp2 (hardware log2(0) = -inf, log2(x<0) = NaN; GLSL undefined).
//   * v_fma_f32 is written fma() (fused, as on hardware); v_mad/v_mac/v_madak/v_madmk are unfused (`precise`).
//     The omod mul:2 on 0x100 is written as `* 2.0` after the multiply.
//   * The two image_sample run with implicit derivatives inside the non-uniform else branch: quads that are
//     only partly inside keep stale coordinates on hardware and have undefined derivatives in GLSL.
//   * dFdxCoarse stands for the DPP pair (quad lane 1 minus quad lane 0 of ClipPos.x), as in solid_fill_p.
//   * v_rcp_f32 (1 ulp) vs GLSL 1.0/x; v_min/v_max NaN rules differ from GLSL min()/max(); the tone-curve masks
//     are kept exactly (including the NaN fall-through to 0).
//   * Output is packed to fp16 with round-toward-zero (v_cvt_pkrtz_f16_f32); the render target format decides here.

layout(set = 0, binding = 1, std140) uniform GLOBAL_CB {   // metadata "__GLOBAL_CB__", resource slot 0, 160 bytes
    layout(offset = 0)   vec4  u_FadeA;
    layout(offset = 16)  vec4  u_FadeB;
    layout(offset = 32)  vec4  u_TargetRect;
    layout(offset = 48)  float u_Radius;
    layout(offset = 52)  uint  u_Flag;       // metadata base type 9 (32-bit integer class)
    layout(offset = 56)  float u_FocusRadius;
    layout(offset = 60)  float u_Ratio;
    layout(offset = 64)  float u_Thickness;
    layout(offset = 68)  float u_Offset;
    layout(offset = 72)  float u_Pixel;
    layout(offset = 76)  float u_NoiseScale;
    layout(offset = 80)  float u_MinOpacity;
    layout(offset = 84)  float u_NoiseChangeParam_x;   // float2 u_NoiseChangeParam at +84 (not 8-aligned: std140 vec2 cannot sit here)
    layout(offset = 88)  float u_NoiseChangeParam_y;
    layout(offset = 96)  vec4  u_ToneCurveToe;
    layout(offset = 112) vec4  u_ToneCurveMid;
    layout(offset = 128) vec4  u_ToneCurveShoulder;
    layout(offset = 144) vec2  u_ToneCurveMidpoint;
    layout(offset = 152) float u_ShowAlpha;
    layout(offset = 156) float u_StrokePosition;
};
// globalConstantBuffer: metadata resource slot 7, 8 bytes (see solid_fill_p.frag).
layout(set = 0, binding = 2, std140) uniform globalConstantBuffer {
    layout(offset = 0) float u_GlobalAlpha;
    layout(offset = 4) float u_GlobalIntensity;
};
// s_TextureColorTable (t0) with Sampler0 (s0); s_TextureNoise (t1) with Sampler1 (s1).
layout(set = 0, binding = 3) uniform sampler2D s_TextureColorTable;
layout(set = 0, binding = 4) uniform sampler2D s_TextureNoise;

layout(location = 0) in vec4 in_attr0;   // PS attr0 = input semantic 0: input.Color (COLOR)
layout(location = 1) in vec4 in_attr1;   // PS attr1 = input semantic 1: .zw input.ClipPos (TEXCOORD1)
layout(location = 2) in vec4 in_attr2;   // PS attr2 = input semantic 2: .xy input.St (TEXCOORD2), .zw input.FrameSt (TEXCOORD3)
layout(location = 0) out vec4 out_mrt0;  // exp mrt0 (SPI_SHADER_COL_FORMAT = 4: packed fp16, v_cvt_pkrtz_f16_f32 = round toward zero)

void main()
{
    precise float v0, v1, v2, v3, v4, v5, v6, v7, v8, v9, v10, v11;
    float s0f, s1f, s2f, s3f, s4f, s5f, s6f, s7f;
    float s32, s33, s34, s35, s6, s7;
    uint s0_u, s8_u;
    float vcc_lo, vcc_hi;
    bool vcc, s0m;
    s32 = u_FocusRadius; s33 = u_Ratio; s34 = u_Thickness; s35 = u_Offset;   // 0x8
    vcc_lo = u_StrokePosition;                                               // 0x10
    vcc_hi = u_Pixel;                                                        // 0x18
    v2 = in_attr2.z;                                                         // 0x28/0x2C
    v4 = s32 + abs(v2);                                                      // 0x34
    v3 = in_attr2.w;                                                         // 0x3C/0x40
    v3 = -1.0 + abs(v3);                                                     // 0x44
    v2 = -1.0 + abs(v2);                                                     // 0x4C
    v7 = -1.0 + v4;                                                          // 0x54
    v8 = -1.0 + vcc_lo;                                                      // 0x58
    v4 = s35;                                                                // 0x60
    v5 = s33 * v3 + s32;                                                     // 0x64
    v6 = s33 * v3;                                                           // 0x6C
    v7 = max(0.0, v7);                                                       // 0x70
    v10 = s34 * v8 + s35;                                                    // 0x74
    v11 = vcc_lo * s34 + v4;                                                 // 0x7C
    v5 = max(0.0, v5);                                                       // 0x84
    v8 = max(v2, v6);                                                        // 0x88
    v9 = v7 * v7;                                                            // 0x8C
    v2 = 0.5 * vcc_hi + -v10;                                                // 0x90
    v10 = 0.5 * vcc_hi + v10;                                                // 0x98
    v3 = 0.5 * vcc_hi + -v11;                                                // 0xA0
    v7 = s32 + v8;                                                           // 0xA8
    v9 = v5 * v5 + v9;                                                       // v_mac 0xAC
    v11 = 0.5 * vcc_hi + v11;                                                // 0xB0
    v6 = v10 + v2;                                                           // 0xB8
    v4 = min(0.0, v7);                                                       // 0xC0
    v7 = sqrt(v9);                                                           // 0xC4
    v8 = v11 + v3;                                                           // 0xC8
    v5 = 1.0 / v6;                                                           // 0xCC
    v7 = v7 - s32;                                                           // 0xD0
    v6 = 1.0 / v8;                                                           // 0xD4
    v4 = v7 + v4;                                                            // 0xD8
    v2 = v4 + v2;                                                            // 0xDC
    v3 = v4 + v3;                                                            // 0xE0
    v2 = clamp(v5 * v2, 0.0, 1.0);                                           // 0xE4
    v3 = clamp(v6 * v3, 0.0, 1.0);                                           // 0xEC
    v4 = v2 * v2;                                                            // 0xF4
    v5 = -v3 * v3;                                                           // 0xF8
    v2 = (-v2 * v4) * 2.0;                                                   // v_mul_f32 mul:2 0x100
    v5 = v3 * v5;                                                            // 0x108
    v4 = v3 * -v3 + v4;                                                      // v_mac_f32_e64 0x10C
    v2 = -2.0 * v5 + v2;                                                     // v_mac 0x114
    v7 = v4 * uintBitsToFloat(0x40400000u) + v2;   // 3.0                    // v_madmk 0x118
    if (0.0 >= v7) {                                                         // v_cmpx_ge 0x120, s_cbranch_execz 0x124
        v7 = 1.0;                                                                // 0x128
        vcc_lo = u_ShowAlpha;                                                    // 0x12C
        vcc = 0.0 != vcc_lo;                                                     // 0x138
        v8 = 0.0;                                                                // 0x140
        v9 = 0.0;                                                                // 0x144
        v10 = uintBitsToFloat(0x3F333333u);   // 0.7                            // 0x148
        if (!vcc) {                                                              // s_cbranch_vccnz 0x150
            demote;                                                                  // s_andn2_b64 s[36:37] 0x154 (+ all-killed exit 0x158)
            v7 = 0.0;                                                                // 0x15C
        }
    } else {                                                                 // s_andn2_b64 exec 0x160, s_cbranch_execz 0x164
        v2 = in_attr2.x;                                                         // 0x168/0x180
        vcc_lo = u_NoiseScale;                                                   // 0x16C
        v3 = 1.0 / vcc_lo;                                                       // 0x178
        v4 = in_attr2.y;                                                         // 0x17C/0x18C
        vcc_lo = u_NoiseChangeParam_x; vcc_hi = u_NoiseChangeParam_y;            // 0x184
        v2 = v2 * v3 + vcc_lo;                                                   // 0x194
        v3 = v4 * v3 + vcc_hi;                                                   // 0x19C
        v6 = 0.5;                                                                // 0x1A4
        v2 = fma(v2, 0.5, 0.5);                                                  // v_fma_f32 0x1AC
        v3 = fma(v3, 0.5, 0.5);                                                  // v_fma_f32 0x1B4
        v8 = texture(s_TextureNoise, vec2(v2, v3)).x;   // image_sample dmask 0x1, t1/s1 // 0x1BC
        v2 = clamp(max(0.0, v8), 0.0, 1.0);                                      // 0x1C8
        v3 = sqrt(v2);                                                           // 0x1D0
        v5 = v3 * v2;                                                            // 0x1D4
        vec4 t0 = texture(s_TextureColorTable, vec2(v5, v6));   // image_sample dmask 0x7, t0/s0 // 0x1D8
        v2 = t0.x; v3 = t0.y; v4 = t0.z;
        if (0.0 < v8) {                                                          // v_cmpx_lt 0x1E0, s_cbranch_execz 0x1E4
            s6 = u_ToneCurveMidpoint.x; s7 = u_ToneCurveMidpoint.y;                  // 0x1E8
            if (s6 >= v5) {                                                          // v_cmpx_ge 0x1F8 (+ no-op v_cmpx_eq_u32 0,0 0x1FC), execz 0x204
                v8 = v5 * v5;                                                            // 0x208
                s0f = u_ToneCurveToe.x; s1f = u_ToneCurveToe.y; s2f = u_ToneCurveToe.z; s3f = u_ToneCurveToe.w; // 0x20C
                v6 = s2f * v8 + s0f;                                                     // 0x218
                v8 = v8 * s3f + s1f;                                                     // 0x220
                v8 = v5 * v8 + v6;                                                       // 0x228
            } else {                                                                 // s_andn2_b64 exec 0x230, execz 0x234
                s0m = s7 <= v5;                                                          // v_cmp_le s[0:1] 0x238 (& exec -> s[8:9] 0x248)
                if (s7 >= v5 && s6 <= v5) {                                              // v_cmpx_ge 0x250, v_cmpx_le 0x254, execz 0x258
                    v6 = v5 - s6;                                                            // 0x25C
                    s0f = u_ToneCurveMid.x; s1f = u_ToneCurveMid.y; s2f = u_ToneCurveMid.z; s3f = u_ToneCurveMid.w; // 0x260
                    v9 = v6 * v6;                                                            // 0x268
                    v8 = s2f * v9 + s0f;                                                     // 0x270
                    v9 = v9 * s3f + s1f;                                                     // 0x278
                    v8 = v9 * v6 + v8;                                                       // v_mac 0x280
                } else {                                                                 // s_andn2_b64 exec, vcc 0x284, execz 0x288
                    v8 = 0.0;                                                                // 0x28C
                    if (s0m) {                                                               // s_and_b64 exec, s[8:9] 0x290, execz 0x294
                        v5 = v5 - s7;                                                            // 0x298
                        s0f = u_ToneCurveShoulder.x; s1f = u_ToneCurveShoulder.y; s2f = u_ToneCurveShoulder.z; s3f = u_ToneCurveShoulder.w; // 0x29C
                        v8 = v5 * v5;                                                            // 0x2A4
                        v6 = s2f * v8 + s0f;                                                     // 0x2AC
                        v8 = v8 * s3f + s1f;                                                     // 0x2B4
                        v8 = v5 * v8 + v6;                                                       // 0x2BC
                    }
                }
            }                                                                        // s_mov_b64 exec, s[4:5] 0x2C4
            vcc_lo = u_MinOpacity;                                                   // 0x2C8
            v5 = 1.0 - vcc_lo;                                                       // 0x2D4
            v5 = v8 * v5 + vcc_lo;                                                   // 0x2DC
        }                                                                        // s_mov_b64 exec, s[32:33] 0x2E4
        vcc_lo = u_ShowAlpha;                                                    // 0x2E8
        v6 = in_attr0.w;                                                         // 0x2F0/0x2F4
        v5 = v6 * v5;                                                            // 0x2F8
        vcc = 0.0 != vcc_lo;                                                     // 0x300
        v5 = v5 * v7;                                                            // 0x308
        v7 = vcc ? v5 : v2;                                                      // 0x310
        v8 = vcc ? v5 : v3;                                                      // 0x314
        v9 = vcc ? v5 : v4;                                                      // 0x318
        v10 = vcc ? 1.0 : v5;                                                    // 0x31C
    }                                                                        // s_mov_b64 exec, s[38:39] 0x324
    s8_u = u_Flag;                                                           // 0x328
    s0_u = bitfieldExtract(s8_u, 20, 4);                                     // s_bfe_u32 0x40014 0x334
    v4 = clamp(max(0.0, v7), 0.0, 1.0);                                      // 0x33C
    v5 = clamp(max(0.0, v8), 0.0, 1.0);                                      // 0x344
    v3 = clamp(max(0.0, v9), 0.0, 1.0);                                      // 0x34C
    v2 = clamp(max(0.0, v10), 0.0, 1.0);                                     // 0x354
    if (s0_u == 2u) {                                                        // 0x35C..0x370 (s0 != 1 and s0 == 2)
        v4 = log2(v4);                                                           // 0x374
        v5 = log2(v5);                                                           // 0x378
        v3 = log2(v3);                                                           // 0x37C
        v4 = uintBitsToFloat(0x40166666u) * v4;   // 2.35                       // 0x380
        v6 = uintBitsToFloat(0x40166666u) * v5;                                  // 0x388
        v3 = uintBitsToFloat(0x40166666u) * v3;                                  // 0x390
        v5 = exp2(v4);                                                           // 0x398
        v4 = exp2(v6);                                                           // 0x39C
        v3 = exp2(v3);                                                           // 0x3A0
        v7 = uintBitsToFloat(0x3D84BA2Fu) * v5;   // 0.064808242                // 0x3A4
        v6 = uintBitsToFloat(0x3F23CF31u) * v5;   // 0.63988024                 // 0x3AC
        v5 = uintBitsToFloat(0x3C2C09BDu) * v5;   // 0.010500369                // 0x3B4
        v7 = v4 * uintBitsToFloat(0x3F6F74A4u) + v7;   // 0.93537354            // v_madmk 0x3BC
        v6 = v4 * uintBitsToFloat(0x3EA79F91u) + v6;   // 0.32738927            // v_madmk 0x3C4
        v5 = v4 * uintBitsToFloat(0x3DA17DE5u) + v5;   // 0.078853406           // v_madmk 0x3CC
        v4 = v3 * uintBitsToFloat(0xB9169957u) + v7;   // -0.00014362238        // v_madmk 0x3D4
        v6 = v3 * uintBitsToFloat(0x3D05FBE8u) + v6;   // 0.03271094            // v_madmk 0x3DC
        v5 = v3 * uintBitsToFloat(0x3F691B7Bu) + v5;   // 0.91057557            // v_madmk 0x3E4
        vcc = 0.0 <= v4;                                                         // 0x3EC
        v3 = uintBitsToFloat(0x3CCCCCCCu) * v6;   // 0.025                      // 0x3F0
        v5 = uintBitsToFloat(0x3CCCCCCCu) * v5;                                  // 0x3F8
        v4 = vcc ? v4 : -v4;                                                     // 0x400
        v3 = log2(v3);                                                           // 0x408
        v5 = log2(v5);                                                           // 0x40C
        v4 = uintBitsToFloat(0x3CCCCCCCu) * v4;                                  // 0x410
        v3 = uintBitsToFloat(0x3EE8BA2Eu) * v3;   // 1/2.2                      // 0x418
        v5 = uintBitsToFloat(0x3EE8BA2Eu) * v5;                                  // 0x420
        v6 = log2(abs(v4));                                                      // 0x428
        v4 = exp2(v3);                                                           // 0x430
        v3 = exp2(v5);                                                           // 0x434
        v5 = uintBitsToFloat(0x3EE8BA2Eu) * v6;                                  // 0x438
        v5 = exp2(v5);                                                           // 0x440
        v5 = vcc ? v5 : -v5;                                                     // 0x444
    } else if (s0_u == 1u) {                                                 // s_cbranch_scc0 0x368 -> 0x450
        v4 = log2(v4);                                                           // 0x450
        v5 = log2(v5);                                                           // 0x454
        v6 = log2(v3);                                                           // 0x458
        v3 = uintBitsToFloat(0x4196D000u);   // 18.8515625                      // 0x45C
        v4 = uintBitsToFloat(0x40166666u) * v4;   // 2.35                       // 0x464
        v5 = uintBitsToFloat(0x40166666u) * v5;                                  // 0x46C
        v7 = uintBitsToFloat(0x40166666u) * v6;                                  // 0x474
        v6 = exp2(v4);                                                           // 0x47C
        v5 = exp2(v5);                                                           // 0x480
        v4 = exp2(v7);                                                           // 0x484
        v8 = uintBitsToFloat(0x3D84BA2Fu) * v6;                                  // 0x488
        v7 = uintBitsToFloat(0x3F23CF31u) * v6;                                  // 0x490
        v6 = uintBitsToFloat(0x3C2C09BDu) * v6;                                  // 0x498
        v8 = v5 * uintBitsToFloat(0x3F6F74A4u) + v8;                             // v_madmk 0x4A0
        v7 = v5 * uintBitsToFloat(0x3EA79F91u) + v7;                             // v_madmk 0x4A8
        v6 = v5 * uintBitsToFloat(0x3DA17DE5u) + v6;                             // v_madmk 0x4B0
        v5 = v4 * uintBitsToFloat(0xB9169957u) + v8;                             // v_madmk 0x4B8
        v7 = v4 * uintBitsToFloat(0x3D05FBE8u) + v7;                             // v_madmk 0x4C0
        v6 = v4 * uintBitsToFloat(0x3F691B7Bu) + v6;                             // v_madmk 0x4C8
        vcc = 0.0 <= v5;                                                         // 0x4D0
        v4 = uintBitsToFloat(0x3CCCCCCCu) * v7;   // 0.025                      // 0x4D4
        v6 = uintBitsToFloat(0x3CCCCCCCu) * v6;                                  // 0x4DC
        v5 = vcc ? v5 : -v5;                                                     // 0x4E4
        v4 = log2(v4);                                                           // 0x4EC
        v6 = log2(v6);                                                           // 0x4F0
        v5 = uintBitsToFloat(0x3CCCCCCCu) * v5;                                  // 0x4F4
        v4 = uintBitsToFloat(0x3E232000u) * v4;   // m1 = 0.1593017578125       // 0x4FC
        v6 = uintBitsToFloat(0x3E232000u) * v6;                                  // 0x504
        v7 = log2(abs(v5));                                                      // 0x50C
        v5 = exp2(v4);                                                           // 0x514
        v4 = exp2(v6);                                                           // 0x518
        v8 = uintBitsToFloat(0x3E232000u) * v7;                                  // 0x51C
        v7 = uintBitsToFloat(0x41958000u) * v5 + 1.0;   // c3 = 18.6875         // 0x524
        v6 = uintBitsToFloat(0x41958000u) * v4 + 1.0;                            // 0x530
        v5 = v3 * v5 + uintBitsToFloat(0x3F560000u);   // c2*Y + c1 (0.8359375) // v_madak 0x53C
        v4 = v3 * v4 + uintBitsToFloat(0x3F560000u);                             // v_madak 0x544
        v8 = exp2(v8);                                                           // 0x54C
        v7 = 1.0 / v7;                                                           // 0x550
        v9 = 1.0 / v6;                                                           // 0x554
        v6 = uintBitsToFloat(0x41958000u) * v8 + 1.0;                            // 0x558
        v3 = v3 * v8 + uintBitsToFloat(0x3F560000u);                             // v_madak 0x564
        v5 = v7 * v5;                                                            // 0x56C
        v4 = v9 * v4;                                                            // 0x570
        v6 = 1.0 / v6;                                                           // 0x574
        v5 = log2(v5);                                                           // 0x578
        v4 = log2(v4);                                                           // 0x57C
        v3 = v6 * v3;                                                            // 0x580
        v5 = uintBitsToFloat(0x429DB000u) * v5;   // m2 = 78.84375              // 0x584
        v4 = uintBitsToFloat(0x429DB000u) * v4;                                  // 0x58C
        v6 = log2(v3);                                                           // 0x594
        v3 = exp2(v4);                                                           // 0x598
        v4 = exp2(v5);                                                           // 0x59C
        v5 = uintBitsToFloat(0x429DB000u) * v6;                                  // 0x5A0
        v5 = exp2(v5);                                                           // 0x5A8
        v5 = vcc ? v5 : -v5;                                                     // 0x5AC
    }
    v7 = in_attr1.z;                                                         // 0x5B4/0x5BC
    if ((s8_u & 6u) != 0u) {                                                 // s_and_b32 0x5B8, s_cbranch_scc0 0x5C0
        v6 = 1.0;                                                                // 0x5C4
        v0 = in_attr1.w;                                                         // 0x5CC/0x5D0
        if ((s8_u & 4u) != 0u) {                                                 // s_bitcmp1_b32 0x5C8, s_cbranch_scc0 0x5D4
            vcc_lo = uintBitsToFloat(0x3F100000u);   // 0.5625                       // 0x5E0
            s0f = u_TargetRect.x; s1f = u_TargetRect.y; s2f = u_TargetRect.z; s3f = u_TargetRect.w; // 0x5E8
            v8 = vcc_lo * v0 + -s1f;                                                 // 0x5F4
            v9 = v7 - s0f;                                                           // 0x5FC
            v1 = dFdxCoarse(v7);   // v_mov_b32_dpp quad_perm[0,0,0,0] (0x5D8) + v_sub_f32_dpp quad_perm[1,1,1,1] // 0x600
            v8 = -s3f + abs(v8);                                                     // 0x60C
            v9 = -s2f + abs(v9);                                                     // 0x614
            v10 = min(v9, v8);                                                       // 0x61C
            if (0.0 > v10) {                                                         // v_cmpx_gt 0x620
                vcc = 0.0 > v9;                                                          // 0x624
                v8 = vcc ? v8 : v9;                                                      // 0x628
            } else {                                                                 // s_andn2_b64 exec 0x62C
                v9 = v9 * v9;                                                            // 0x630
                v9 = v8 * v8 + v9;                                                       // 0x634
                v8 = sqrt(v9);                                                           // 0x638
            }                                                                        // 0x63C
            vcc_lo = u_Radius;                                                       // 0x640
            v9 = vcc_lo - v1;                                                        // 0x64C
            if (v8 > v9) {                                                           // v_cmpx_gt 0x650
                v6 = vcc_lo - v8;                                                        // 0x654
                v1 = 1.0 / v1;                                                           // 0x658
                v6 = clamp(v1 * v6 + 0.5, 0.0, 1.0);                                     // 0x65C
            }                                                                        // 0x664
        }
        if ((s8_u & 2u) != 0u) {                                                 // s_bitcmp1_b32 0x668, s_cbranch_scc0 0x66C
            s0f = u_FadeA.x; s1f = u_FadeA.y; s2f = u_FadeA.z; s3f = u_FadeA.w; s4f = u_FadeB.x; s5f = u_FadeB.y; s6f = u_FadeB.z; s7f = u_FadeB.w; // 0x670
            v1 = clamp(s0f * v7 + s4f, 0.0, 1.0);                                    // 0x67C
            v8 = clamp(v7 * s1f + s5f, 0.0, 1.0);                                    // 0x684
            v7 = clamp(v0 * s2f + s6f, 0.0, 1.0);                                    // 0x68C
            v0 = clamp(v0 * s3f + s7f, 0.0, 1.0);                                    // 0x694
            v1 = v1 * v8;                                                            // 0x69C
            v1 = v1 * v7;                                                            // 0x6A0
            v1 = v1 * v0;                                                            // 0x6A4
            v0 = v1 * v1;                                                            // 0x6A8
            v1 = -2.0 * v1 + uintBitsToFloat(0x40400000u);   // 3.0                  // v_madak 0x6AC
            v0 = v0 * v6;                                                            // 0x6B4
            v6 = v0 * v1;                                                            // 0x6B8
        }
        v0 = v6 * v4;                                                            // 0x6BC
        v1 = v6 * v5;                                                            // 0x6C0
        v7 = v6 * v3;                                                            // 0x6C4
        v2 = v6 * v2;                                                            // 0x6CC
        vcc = (s8_u & 1u) != 0u;                                                 // s_bitcmp1_b32 0x6C8, s_cselect_b64 0x6D0
        v4 = vcc ? v0 : v4;                                                      // 0x6D4
        v5 = vcc ? v1 : v5;                                                      // 0x6D8
        v3 = vcc ? v7 : v3;                                                      // 0x6DC
    }
    vcc_hi = u_GlobalIntensity;                                              // 0x6E0
    v1 = vcc_hi;                                                             // 0x6EC
    if ((s8_u & 1u) != 0u) {                                                 // s_bitcmp1_b32 0x6F0, s_cbranch_scc0 0x6F4
        vcc_lo = u_GlobalAlpha;                                                  // 0x6F8
        v1 = vcc_lo * vcc_hi;                                                    // 0x704
    }
    v0 = v1 * v4;                                                            // 0x70C
    vcc_lo = u_GlobalAlpha;                                                  // 0x710
    v4 = v1 * v5;                                                            // 0x718
    v1 = v1 * v3;                                                            // 0x71C
    v2 = vcc_lo * v2;                                                        // 0x724
    out_mrt0 = vec4(v0, v4, v1, v2);   // v_cvt_pkrtz_f16_f32 v0,v0,v4 / v1,v1,v2 ; exp mrt0 compr (0x728..0x734)
}
