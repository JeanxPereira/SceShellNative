#version 450
#extension GL_EXT_demote_to_helper_invocation : require
// Transcription of libScePsm embedded shader #65 "focus_ui3_area_p" (work/shaders/65_focus_ui3_area_p.ags.bin),
// AGC binary type 1 (pixel), code up to s_endpgm at 0x6E8 (+ the all-killed exit at 0x6EC..0x6F8).
// EmbeddedShader.AreaFocus = (focus_ui3_area_vv, focus_ui3_area_p).
// Every statement maps to the RDNA instruction at the byte offset in its trailing comment.
//
// Math (st = St = attr2.xy, p = ClipPos = attr1.zw, ca = Color.a = attr0.w, F = u_Flag; "ss(t)" = t*t*(3-2t)):
//   w  = u_Moving > 0 ? u_AreaWarpFadePixel : u_Pixel
//   r0 = u_FocusRadius - u_AreaWarpFadePixel ;  e = u_AreaWarpFadePixel - 1
//   qx = max(|st.x| + r0 + e, 0) ; qy = max(u_Ratio*(|st.y| + e) + r0, 0)
//   d  = (sqrt(qx^2+qy^2) - r0) + min(r0 + max(e + |st.x|, u_Ratio*(|st.y| + e)), 0)       (rounded-box SDF)
//   d' = d + u_MorphByRatioIntensity * ((|st| - d) + e)
//   g  = -ss(saturate(d' / w))
//   if (g <= -1): demote (pixel killed, as HLSL clip/discard); rgba = 0
//   else:
//     a  = Angle/(2pi): cs = (cos, sin)(u_Angle)
//     nuv = 0.5 + 0.5 * (st / u_NoiseScale + u_NoiseChangeParam);   n = texture(s_TextureNoise, nuv).r
//     sh = 0.5 * (mix(ShimmerParam.x, ShimmerParam.y, 0.35355*(0.70709*st.y - 0.70712*st.x) + 0.5))
//     gr = 0.5 + 0.5 * (sin*st.y - cos*st.x) / (|cos| + |sin|)
//     wg = pow(|gr * ss(saturate((gr - Toe)/(1 - Toe)))|, u_WarpGradientCurveByRatioIntensity)
//     v  = max(0, sh) ; v += (wg - 1) * (u_Moving * v) ; v = v + u_Moving * (wg*n - v)
//     v  = v + u_Pressing * (0.15 - v) ; v *= u_AreaOpacityDecreaseRateBySize ; v += v * g
//     u  = ca * v ; rgb = texture(s_TextureColorTable, (u, 0.5)).rgb
//     if (u > 0) u = exp2(log2(u) * u_AlphaGamma)
//     rgba = u_ShowAlpha != 0 ? (u, u, u, 1) : (rgb, u)
//   rgba = saturate(max(0, rgba))
//   mode = (F >> 20) & 0xF:  1 -> PQ encode of the gamut-converted pow(rgb, 2.35);  2 -> pow(0.025 * gamut-converted
//         pow(rgb, 2.35), 1/2.2) (the G-row keeps its sign); others -> unchanged.  (Constants in the code.)
//   common u_Flag tail exactly as solid_fill_p.frag (bit 2 rounded rect, bit 1 fade, bit 0 premultiply + global alpha).
//
// Resources: s_TextureColorTable t0 -> s[0:7], s_TextureNoise t1 -> s[8:15], Sampler0 -> s[16:19], Sampler1 -> s[20:23],
//   __GLOBAL_CB__ slot 0 -> s[24:27], globalConstantBuffer slot 7 -> s[28:31].
//   The ISA samples t1 with Sampler1 (0x1EC) and t0 with Sampler0 (0x278), i.e. t-slot n with sampler slot n.
// Interpolants: PS input semantics 0,1,2 (attr0 = main.Color, attr1 = TexCoord/ClipPos, attr2 = St).
//
// Not expressible / differences:
//   * The kill (0x100: s_andn2_b64 s[32:33] = live mask without those lanes, exported with the final exec at 0x6DC)
//     leaves the killed lanes running in WQM as helpers; that is `demote` (GL_EXT_demote_to_helper_invocation,
//     SPIR-V DemoteToHelperInvocation), which needs the Vulkan 1.3 feature shaderDemoteToHelperInvocation enabled.
//     The all-lanes-killed early exit (0x104 -> 0x6EC, null export) has the same visible result.
//   * v_sin_f32 / v_cos_f32 take revolutions; written sin/cos(x * 2pi) of the pre-scaled operand (extra rounding,
//     different implementation). v_log_f32 / v_exp_f32 are written log2 / exp2 (hardware log2(0) = -inf,
//     log2(x<0) = NaN; GLSL leaves both undefined).
//   * v_fma_f32 is written fma() (fused, as on hardware); v_mad/v_mac/v_madak/v_madmk are unfused (`precise`).
//     The omod div:2 on 0x1E4 is written as `* 0.5` after the unfused mad.
//   * The two image_sample run with implicit derivatives inside the non-uniform else branch: quads that are
//     only partly inside keep stale coordinates on hardware and have undefined derivatives in GLSL.
//   * dFdxCoarse stands for the DPP pair (quad lane 1 minus quad lane 0 of ClipPos.x), as in solid_fill_p.
//   * v_rcp_f32 (1 ulp) vs GLSL 1.0/x; v_min/v_max NaN rules differ from GLSL min()/max().
//   * Output is packed to fp16 with round-toward-zero (v_cvt_pkrtz_f16_f32); the render target format decides here.

layout(set = 0, binding = 1, std140) uniform GLOBAL_CB {   // metadata "__GLOBAL_CB__", resource slot 0, 128 bytes
    layout(offset = 0)   vec4  u_FadeA;
    layout(offset = 16)  vec4  u_FadeB;
    layout(offset = 32)  vec4  u_TargetRect;
    layout(offset = 48)  float u_Radius;
    layout(offset = 52)  uint  u_Flag;       // metadata base type 9 (32-bit integer class)
    layout(offset = 56)  float u_FocusRadius;
    layout(offset = 60)  float u_Ratio;
    layout(offset = 64)  float u_Angle;
    layout(offset = 68)  float u_Pressing;
    layout(offset = 72)  float u_Pixel;
    layout(offset = 76)  float u_Moving;
    layout(offset = 80)  float u_NoiseScale;
    layout(offset = 84)  float u_AlphaGamma;
    layout(offset = 88)  vec2  u_NoiseChangeParam;
    layout(offset = 96)  float u_ShowAlpha;
    layout(offset = 100) float u_WarpGradientCurveByRatioIntensity;
    layout(offset = 104) float u_WarpGradientCurveToeValue;
    layout(offset = 108) float u_AreaOpacityDecreaseRateBySize;
    layout(offset = 112) float u_AreaWarpFadePixel;
    layout(offset = 116) float u_ShimmerParam_x;   // float2 u_ShimmerParam at +116 (not 8-aligned: std140 vec2 cannot sit here)
    layout(offset = 120) float u_ShimmerParam_y;
    layout(offset = 124) float u_MorphByRatioIntensity;
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
layout(location = 2) in vec4 in_attr2;   // PS attr2 = input semantic 2: .xy input.St (TEXCOORD2)
layout(location = 0) out vec4 out_mrt0;  // exp mrt0 (SPI_SHADER_COL_FORMAT = 4: packed fp16, v_cvt_pkrtz_f16_f32 = round toward zero)

void main()
{
    precise float v0, v1, v2, v3, v4, v5, v6, v7, v8, v9, v10, v11;
    float s0f, s1f, s2f, s3f, s4f, s5f, s6f, s7f;
    float s34, s35, s36, s37, s38, s39, s40, s41, s42;
    uint s0_u, s8_u;
    float vcc_lo, vcc_hi;
    bool vcc;
    s36 = u_AreaWarpFadePixel;                                               // 0x4
    s34 = u_FocusRadius; s35 = u_Ratio;                                      // 0xC
    s42 = u_Moving;                                                          // 0x18
    s37 = u_MorphByRatioIntensity;                                           // 0x20
    v5 = s34 - s36;                                                          // 0x34
    v2 = in_attr2.x;                                                         // 0x3C/0x40
    v4 = -1.0 + s36;                                                         // 0x44
    v3 = in_attr2.y;                                                         // 0x4C/0x58
    v9 = abs(v2) + v5;                                                       // 0x50
    v11 = v2 * v2;                                                           // 0x5C
    v6 = abs(v3) + v4;                                                       // 0x60
    v7 = v4 + abs(v2);                                                       // 0x68
    v10 = v9 + v4;                                                           // 0x70
    vcc = s42 > 0.0;                                                         // 0x74
    v11 = v3 * v3 + v11;                                                     // 0x7C
    v8 = s35 * v6 + v5;                                                      // 0x80
    v9 = s35 * v6;                                                           // 0x88
    v10 = max(0.0, v10);                                                     // 0x8C
    v6 = sqrt(v11);                                                          // 0x90
    v8 = max(0.0, v8);                                                       // 0x94
    v7 = max(v7, v9);                                                        // 0x98
    v9 = v10 * v10;                                                          // 0x9C
    v7 = v5 + v7;                                                            // 0xA0
    v9 = v8 * v8 + v9;                                                       // 0xA4
    v7 = min(0.0, v7);                                                       // 0xA8
    v8 = sqrt(v9);                                                           // 0xAC
    v5 = v8 - v5;                                                            // 0xB0
    v7 = v5 + v7;                                                            // 0xB4
    v6 = v6 - v7;                                                            // 0xB8
    v4 = v6 + v4;                                                            // 0xBC
    v7 = s37 * v4 + v7;                                                      // 0xC0
    if (!vcc) {                                                              // s_cbranch_vccnz 0xC4
        s36 = u_Pixel;                                                           // 0xC8
    }
    v5 = 1.0 / s36;                                                          // 0xD4
    v5 = clamp(v5 * v7, 0.0, 1.0);                                           // 0xDC
    v4 = -2.0 * v5 + uintBitsToFloat(0x40400000u);   // 3.0                  // v_madak 0xE4
    v5 = -v5 * v5;                                                           // 0xEC
    v4 = v5 * v4;                                                            // 0xF4
    if (-1.0 >= v4) {                                                        // v_cmpx_ge 0xF8, s_cbranch_execz 0xFC
        demote;                                                                  // s_andn2_b64 s[32:33] 0x100 (+ all-killed exit 0x104)
        v8 = 0.0;                                                                // 0x108
        v9 = 0.0;                                                                // 0x10C
        v10 = 0.0;                                                               // 0x110
    } else {                                                                 // s_andn2_b64 exec 0x114, s_cbranch_execz 0x118
        v5 = uintBitsToFloat(0x3F3503E0u) * v3;   // 0.70709038                 // 0x11C
        vcc_hi = uintBitsToFloat(0x3E22F983u);    // 1/(2*pi)                   // 0x124
        s36 = u_Angle; s37 = u_Pressing;                                         // 0x12C
        v8 = vcc_hi * s36;                                                       // 0x138
        vcc_lo = u_NoiseScale;                                                   // 0x140
        v6 = 1.0 / vcc_lo;                                                       // 0x14C
        vcc_lo = u_NoiseChangeParam.x; vcc_hi = u_NoiseChangeParam.y;            // 0x150
        s38 = u_WarpGradientCurveByRatioIntensity; s39 = u_WarpGradientCurveToeValue; // 0x158
        v7 = sin(v8 * 6.28318530717958647692);   // v_sin_f32 (revolutions)     // 0x160
        v9 = cos(v8 * 6.28318530717958647692);   // v_cos_f32 (revolutions)     // 0x164
        v10 = v2 * v6 + vcc_lo;                                                  // 0x16C
        v6 = v3 * v6 + vcc_hi;                                                   // 0x174
        s40 = u_ShimmerParam_x; s41 = u_ShimmerParam_y;                          // 0x17C
        v11 = v7 * v3;                                                           // 0x184
        v8 = abs(v9) + abs(v7);                                                  // 0x188
        v7 = 1.0 - s39;                                                          // 0x190
        v3 = fma(v6, 0.5, 0.5);                                                  // v_fma_f32 0x198
        v5 = v2 * uintBitsToFloat(0xBF350606u) + v5;   // -0.70712316           // v_madmk 0x1A0
        v11 = -v2 * v9 + v11;                                                    // v_mac_f32_e64 0x1A8
        v6 = 1.0 / v8;                                                           // 0x1B0
        v8 = 1.0 / v7;                                                           // 0x1B4
        v2 = fma(v10, 0.5, 0.5);                                                 // v_fma_f32 0x1B8
        v10 = s41 - s40;                                                         // 0x1C4
        v5 = uintBitsToFloat(0x3EB504F3u) * v5 + 0.5;   // 0.35355338           // 0x1CC
        vcc_lo = u_AreaOpacityDecreaseRateBySize;                                // 0x1D8
        v7 = (v5 * v10 + s40) * 0.5;                                             // v_mad_f32 div:2 0x1E4
        v5 = texture(s_TextureNoise, vec2(v2, v3)).x;   // image_sample dmask 0x1, t1/s1 // 0x1EC
        v3 = v6 * v11;                                                           // 0x1F4
        v2 = in_attr0.w;                                                         // 0x1F8/0x1FC
        v6 = 0.5;                                                                // 0x200
        v9 = 0.5 * v3 + 0.5;                                                     // 0x204
        v3 = max(0.0, v7);                                                       // 0x20C
        v10 = v9 - s39;                                                          // 0x210
        v7 = s42 * v3;                                                           // 0x214
        v10 = clamp(v8 * v10, 0.0, 1.0);                                         // 0x218
        v8 = v10 * v10;                                                          // 0x220
        v10 = -2.0 * v10 + uintBitsToFloat(0x40400000u);   // 3.0               // v_madak 0x224
        v8 = v9 * v8;                                                            // 0x22C
        v8 = v8 * v10;                                                           // 0x230
        v8 = log2(abs(v8));                                                      // 0x234
        v8 = s38 * v8;                                                           // 0x23C
        v8 = exp2(v8);                                                           // 0x240
        v9 = -1.0 + v8;                                                          // 0x244
        v3 = v9 * v7 + v3;                                                       // v_mac 0x248
        v5 = v8 * v5 + -v3;                                                      // 0x250
        v3 = s42 * v5 + v3;                                                      // v_mac 0x258
        v5 = uintBitsToFloat(0x3E19999Au) - v3;   // 0.15                       // 0x25C
        v3 = s37 * v5 + v3;                                                      // v_mac 0x264
        v3 = vcc_lo * v3;                                                        // 0x26C
        v3 = v3 * v4 + v3;                                                       // v_mac 0x270
        v5 = v2 * v3;                                                            // 0x274
        vec4 t0 = texture(s_TextureColorTable, vec2(v5, v6));   // image_sample dmask 0x7, t0/s0 // 0x278
        v2 = t0.x; v3 = t0.y; v4 = t0.z;
        if (0.0 < v5) {                                                          // v_cmpx_lt 0x280, s_cbranch_execz 0x284
            v5 = log2(v5);                                                           // 0x288
            vcc_lo = u_AlphaGamma;                                                   // 0x28C
            v5 = vcc_lo * v5;                                                        // 0x298
            v5 = exp2(v5);                                                           // 0x29C
        }                                                                        // s_mov_b64 exec 0x2A0
        vcc_lo = u_ShowAlpha;                                                    // 0x2A4
        vcc = 0.0 != vcc_lo;                                                     // 0x2B0
        v8 = vcc ? v5 : v2;                                                      // 0x2BC
        v9 = vcc ? v5 : v3;                                                      // 0x2C0
        v10 = vcc ? v5 : v4;                                                     // 0x2C4
        v7 = vcc ? 1.0 : v5;                                                     // 0x2C8
    }                                                                        // s_mov_b64 exec, s[34:35] 0x2D0
    s8_u = u_Flag;                                                           // 0x2D4
    s0_u = bitfieldExtract(s8_u, 20, 4);                                     // s_bfe_u32 0x40014 0x2E0
    v4 = clamp(max(0.0, v8), 0.0, 1.0);                                      // 0x2E8
    v5 = clamp(max(0.0, v9), 0.0, 1.0);                                      // 0x2F0
    v3 = clamp(max(0.0, v10), 0.0, 1.0);                                     // 0x2F8
    v2 = clamp(max(0.0, v7), 0.0, 1.0);                                      // 0x300
    if (s0_u == 2u) {                                                        // 0x308..0x31C (s0 != 1 and s0 == 2)
        v4 = log2(v4);                                                           // 0x320
        v5 = log2(v5);                                                           // 0x324
        v3 = log2(v3);                                                           // 0x328
        v4 = uintBitsToFloat(0x40166666u) * v4;   // 2.35                       // 0x32C
        v6 = uintBitsToFloat(0x40166666u) * v5;                                  // 0x334
        v3 = uintBitsToFloat(0x40166666u) * v3;                                  // 0x33C
        v5 = exp2(v4);                                                           // 0x344
        v4 = exp2(v6);                                                           // 0x348
        v3 = exp2(v3);                                                           // 0x34C
        v7 = uintBitsToFloat(0x3D84BA2Fu) * v5;   // 0.064808242                // 0x350
        v6 = uintBitsToFloat(0x3F23CF31u) * v5;   // 0.63988024                 // 0x358
        v5 = uintBitsToFloat(0x3C2C09BDu) * v5;   // 0.010500369                // 0x360
        v7 = v4 * uintBitsToFloat(0x3F6F74A4u) + v7;   // 0.93537354            // v_madmk 0x368
        v6 = v4 * uintBitsToFloat(0x3EA79F91u) + v6;   // 0.32738927            // v_madmk 0x370
        v5 = v4 * uintBitsToFloat(0x3DA17DE5u) + v5;   // 0.078853406           // v_madmk 0x378
        v4 = v3 * uintBitsToFloat(0xB9169957u) + v7;   // -0.00014362238        // v_madmk 0x380
        v6 = v3 * uintBitsToFloat(0x3D05FBE8u) + v6;   // 0.03271094            // v_madmk 0x388
        v5 = v3 * uintBitsToFloat(0x3F691B7Bu) + v5;   // 0.91057557            // v_madmk 0x390
        vcc = 0.0 <= v4;                                                         // 0x398
        v3 = uintBitsToFloat(0x3CCCCCCCu) * v6;   // 0.025                      // 0x39C
        v5 = uintBitsToFloat(0x3CCCCCCCu) * v5;                                  // 0x3A4
        v4 = vcc ? v4 : -v4;                                                     // 0x3AC
        v3 = log2(v3);                                                           // 0x3B4
        v5 = log2(v5);                                                           // 0x3B8
        v4 = uintBitsToFloat(0x3CCCCCCCu) * v4;                                  // 0x3BC
        v3 = uintBitsToFloat(0x3EE8BA2Eu) * v3;   // 1/2.2                      // 0x3C4
        v5 = uintBitsToFloat(0x3EE8BA2Eu) * v5;                                  // 0x3CC
        v6 = log2(abs(v4));                                                      // 0x3D4
        v4 = exp2(v3);                                                           // 0x3DC
        v3 = exp2(v5);                                                           // 0x3E0
        v5 = uintBitsToFloat(0x3EE8BA2Eu) * v6;                                  // 0x3E4
        v5 = exp2(v5);                                                           // 0x3EC
        v5 = vcc ? v5 : -v5;                                                     // 0x3F0
    } else if (s0_u == 1u) {                                                 // s_cbranch_scc0 0x314 -> 0x3FC
        v4 = log2(v4);                                                           // 0x3FC
        v5 = log2(v5);                                                           // 0x400
        v6 = log2(v3);                                                           // 0x404
        v3 = uintBitsToFloat(0x4196D000u);   // 18.8515625                      // 0x408
        v4 = uintBitsToFloat(0x40166666u) * v4;   // 2.35                       // 0x410
        v5 = uintBitsToFloat(0x40166666u) * v5;                                  // 0x418
        v7 = uintBitsToFloat(0x40166666u) * v6;                                  // 0x420
        v6 = exp2(v4);                                                           // 0x428
        v5 = exp2(v5);                                                           // 0x42C
        v4 = exp2(v7);                                                           // 0x430
        v8 = uintBitsToFloat(0x3D84BA2Fu) * v6;                                  // 0x434
        v7 = uintBitsToFloat(0x3F23CF31u) * v6;                                  // 0x43C
        v6 = uintBitsToFloat(0x3C2C09BDu) * v6;                                  // 0x444
        v8 = v5 * uintBitsToFloat(0x3F6F74A4u) + v8;                             // v_madmk 0x44C
        v7 = v5 * uintBitsToFloat(0x3EA79F91u) + v7;                             // v_madmk 0x454
        v6 = v5 * uintBitsToFloat(0x3DA17DE5u) + v6;                             // v_madmk 0x45C
        v5 = v4 * uintBitsToFloat(0xB9169957u) + v8;                             // v_madmk 0x464
        v7 = v4 * uintBitsToFloat(0x3D05FBE8u) + v7;                             // v_madmk 0x46C
        v6 = v4 * uintBitsToFloat(0x3F691B7Bu) + v6;                             // v_madmk 0x474
        vcc = 0.0 <= v5;                                                         // 0x47C
        v4 = uintBitsToFloat(0x3CCCCCCCu) * v7;   // 0.025                      // 0x480
        v6 = uintBitsToFloat(0x3CCCCCCCu) * v6;                                  // 0x488
        v5 = vcc ? v5 : -v5;                                                     // 0x490
        v4 = log2(v4);                                                           // 0x498
        v6 = log2(v6);                                                           // 0x49C
        v5 = uintBitsToFloat(0x3CCCCCCCu) * v5;                                  // 0x4A0
        v4 = uintBitsToFloat(0x3E232000u) * v4;   // m1 = 0.1593017578125       // 0x4A8
        v6 = uintBitsToFloat(0x3E232000u) * v6;                                  // 0x4B0
        v7 = log2(abs(v5));                                                      // 0x4B8
        v5 = exp2(v4);                                                           // 0x4C0
        v4 = exp2(v6);                                                           // 0x4C4
        v8 = uintBitsToFloat(0x3E232000u) * v7;                                  // 0x4C8
        v7 = uintBitsToFloat(0x41958000u) * v5 + 1.0;   // c3 = 18.6875         // 0x4D0
        v6 = uintBitsToFloat(0x41958000u) * v4 + 1.0;                            // 0x4DC
        v5 = v3 * v5 + uintBitsToFloat(0x3F560000u);   // c2*Y + c1 (0.8359375) // v_madak 0x4E8
        v4 = v3 * v4 + uintBitsToFloat(0x3F560000u);                             // v_madak 0x4F0
        v8 = exp2(v8);                                                           // 0x4F8
        v7 = 1.0 / v7;                                                           // 0x4FC
        v9 = 1.0 / v6;                                                           // 0x500
        v6 = uintBitsToFloat(0x41958000u) * v8 + 1.0;                            // 0x504
        v3 = v3 * v8 + uintBitsToFloat(0x3F560000u);                             // v_madak 0x510
        v5 = v7 * v5;                                                            // 0x518
        v4 = v9 * v4;                                                            // 0x51C
        v6 = 1.0 / v6;                                                           // 0x520
        v5 = log2(v5);                                                           // 0x524
        v4 = log2(v4);                                                           // 0x528
        v3 = v6 * v3;                                                            // 0x52C
        v5 = uintBitsToFloat(0x429DB000u) * v5;   // m2 = 78.84375              // 0x530
        v4 = uintBitsToFloat(0x429DB000u) * v4;                                  // 0x538
        v6 = log2(v3);                                                           // 0x540
        v3 = exp2(v4);                                                           // 0x544
        v4 = exp2(v5);                                                           // 0x548
        v5 = uintBitsToFloat(0x429DB000u) * v6;                                  // 0x54C
        v5 = exp2(v5);                                                           // 0x554
        v5 = vcc ? v5 : -v5;                                                     // 0x558
    }
    v7 = in_attr1.z;                                                         // 0x560/0x568
    if ((s8_u & 6u) != 0u) {                                                 // s_and_b32 0x564, s_cbranch_scc0 0x56C
        v6 = 1.0;                                                                // 0x570
        v0 = in_attr1.w;                                                         // 0x578/0x57C
        if ((s8_u & 4u) != 0u) {                                                 // s_bitcmp1_b32 0x574, s_cbranch_scc0 0x580
            vcc_lo = uintBitsToFloat(0x3F100000u);   // 0.5625                       // 0x58C
            s0f = u_TargetRect.x; s1f = u_TargetRect.y; s2f = u_TargetRect.z; s3f = u_TargetRect.w; // 0x594
            v8 = vcc_lo * v0 + -s1f;                                                 // 0x5A0
            v9 = v7 - s0f;                                                           // 0x5A8
            v1 = dFdxCoarse(v7);   // v_mov_b32_dpp quad_perm[0,0,0,0] (0x584) + v_sub_f32_dpp quad_perm[1,1,1,1] // 0x5AC
            v8 = -s3f + abs(v8);                                                     // 0x5B8
            v9 = -s2f + abs(v9);                                                     // 0x5C0
            v10 = min(v9, v8);                                                       // 0x5C8
            if (0.0 > v10) {                                                         // v_cmpx_gt 0x5CC
                vcc = 0.0 > v9;                                                          // 0x5D0
                v8 = vcc ? v8 : v9;                                                      // 0x5D4
            } else {                                                                 // s_andn2_b64 exec 0x5D8
                v9 = v9 * v9;                                                            // 0x5DC
                v9 = v8 * v8 + v9;                                                       // 0x5E0
                v8 = sqrt(v9);                                                           // 0x5E4
            }                                                                        // 0x5E8
            vcc_lo = u_Radius;                                                       // 0x5EC
            v9 = vcc_lo - v1;                                                        // 0x5F8
            if (v8 > v9) {                                                           // v_cmpx_gt 0x5FC
                v6 = vcc_lo - v8;                                                        // 0x600
                v1 = 1.0 / v1;                                                           // 0x604
                v6 = clamp(v1 * v6 + 0.5, 0.0, 1.0);                                     // 0x608
            }                                                                        // 0x610
        }
        if ((s8_u & 2u) != 0u) {                                                 // s_bitcmp1_b32 0x614, s_cbranch_scc0 0x618
            s0f = u_FadeA.x; s1f = u_FadeA.y; s2f = u_FadeA.z; s3f = u_FadeA.w; s4f = u_FadeB.x; s5f = u_FadeB.y; s6f = u_FadeB.z; s7f = u_FadeB.w; // 0x61C
            v1 = clamp(s0f * v7 + s4f, 0.0, 1.0);                                    // 0x628
            v8 = clamp(v7 * s1f + s5f, 0.0, 1.0);                                    // 0x630
            v7 = clamp(v0 * s2f + s6f, 0.0, 1.0);                                    // 0x638
            v0 = clamp(v0 * s3f + s7f, 0.0, 1.0);                                    // 0x640
            v1 = v1 * v8;                                                            // 0x648
            v1 = v1 * v7;                                                            // 0x64C
            v1 = v1 * v0;                                                            // 0x650
            v0 = v1 * v1;                                                            // 0x654
            v1 = -2.0 * v1 + uintBitsToFloat(0x40400000u);   // 3.0                  // v_madak 0x658
            v0 = v0 * v6;                                                            // 0x660
            v6 = v0 * v1;                                                            // 0x664
        }
        v0 = v6 * v4;                                                            // 0x668
        v1 = v6 * v5;                                                            // 0x66C
        v7 = v6 * v3;                                                            // 0x670
        v2 = v6 * v2;                                                            // 0x678
        vcc = (s8_u & 1u) != 0u;                                                 // s_bitcmp1_b32 0x674, s_cselect_b64 0x67C
        v4 = vcc ? v0 : v4;                                                      // 0x680
        v5 = vcc ? v1 : v5;                                                      // 0x684
        v3 = vcc ? v7 : v3;                                                      // 0x688
    }
    vcc_hi = u_GlobalIntensity;                                              // 0x68C
    v1 = vcc_hi;                                                             // 0x698
    if ((s8_u & 1u) != 0u) {                                                 // s_bitcmp1_b32 0x69C, s_cbranch_scc0 0x6A0
        vcc_lo = u_GlobalAlpha;                                                  // 0x6A4
        v1 = vcc_lo * vcc_hi;                                                    // 0x6B0
    }
    v0 = v1 * v4;                                                            // 0x6B8
    vcc_lo = u_GlobalAlpha;                                                  // 0x6BC
    v4 = v1 * v5;                                                            // 0x6C4
    v1 = v1 * v3;                                                            // 0x6C8
    v2 = vcc_lo * v2;                                                        // 0x6D0
    out_mrt0 = vec4(v0, v4, v1, v2);   // v_cvt_pkrtz_f16_f32 v0,v0,v4 / v1,v1,v2 ; exp mrt0 compr (0x6D4..0x6E0)
}
