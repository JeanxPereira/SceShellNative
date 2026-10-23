#version 450
// Transcription of libScePsm embedded shader #64 "focus_ui3_p" (work/shaders/64_focus_ui3_p.ags.bin),
// AGC binary type 1 (pixel), code up to s_endpgm at 0x1174. EmbeddedShader.FocusUI3 = (basic_vv, focus_ui3_p).
// Every statement maps to the RDNA instruction at the byte offset in its trailing comment.
//
// Math (p = ClipPos, the only interpolant; F = u_Flag; "ss(t)" = t*t*(3-2t); snoise = the 2D simplex noise with
//       mod-289 permutation (x*34+1)*x and 41-gradient ring (constants 0.36602540, 0.21132487, -0.57735026,
//       1/289, 34, 1/41, -0.85373473, 1.79284286), evaluated exactly as in the code below):
//   a = (p.x - FocusPosition.x)/FocusSize.x ; b = (0.5625*p.y - FocusPosition.y)/FocusSize.y
//   rotate (a,b) by u_Angle, divide the rotated y by (1 - min(0.2, u_Distance*u_Strain*u_Moving)), rotate back
//     -> q = (v21, v11)
//   u_IsLine > 0 (line, on the unrotated coordinates):
//     s = 1 + u_InOutScale*(1 - u_Showing); x = |p.x - F.x| / |S.x*s|; y = |0.5625*p.y - F.y| / |S.y*s|
//     d = (sqrt(max(x + R - 1, 0)^2 + max(Ratio*(y - 1) + R, 0)^2) - R) + min(R + max(x - 1, Ratio*(y - 1)), 0)
//     T = Thickness*(1-Moving)*(1-Trail); O = Offset*(1-Pressing)*(1-Trail); k = (1 - 4*Moving)*(1-Pressing)
//     o1 = T*(k - 1) + O; o2 = T*k + O; ai = saturate((d + 0.5*u_Pixel - oi) / ((0.5*u_Pixel + oi) + (0.5*u_Pixel - oi)))
//     c = 3*(a1^2 - a2^2) + (-2*a1^3 + 2*a2^3)   (= ss(a1) - ss(a2), as in focus_ui3_line_p)
//     if (c > 0 or NaN): n = snoise(q/u_NoiseScale + (sin, cos)(u_Time*u_NoiseMoveFrequency));
//        x = saturate(65n + 0.5); u = sqrt(x)*x; rgb = texture(s_Texture, (u, 0.5)).rgb
//        if (n > -1/130): u = |tone(u)*(1-MinOpacity) + MinOpacity| ^ u_AlphaGamma   (tone curve as focus_ui3_line_p)
//        u *= u_Showing * (1 - 4*u_Moving) * c;  rgba = ShowAlpha != 0 ? (u,u,u,1) : (rgb, u)
//     else rgba = 0
//   else (area): rounded-box SDF of q with u_FocusRadius/u_Ratio/u_AreaWarpFadePixel, morph by
//     u_MorphByRatioIntensity, g = -ss(saturate(d / (Moving > 0 ? AreaWarpFadePixel : u_Pixel)));
//     if (g > -1 or NaN): shimmer (two time-wrapped cosine bands), warp gradient (pow by
//        u_WarpGradientCurveByRatioIntensity, toe u_WarpGradientCurveToeValue), snoise(q/NoiseScale + (sin,cos)(Time*NMF)),
//        u_Pressing / u_AreaOpacityDecreaseRateBySize / u_Showing, radial mask (u_RadialMaskRadius/Intensity, when
//        Moving > 0); rgb = texture(s_Texture, (u, 0.5)).rgb unless ShowAlpha; alpha through MinOpacity/AlphaGamma
//     else rgba = 0
//   rgba = saturate(max(0, rgba)); mode (F >> 20) & 0xF conversions as focus_ui3_area_p.frag;
//   u_Flag tail as solid_fill_p.frag but with the rect y taken as (0.5625*p.y) - R.y and the min/select operands
//   in the order x, y (see 0x1028..0x1070).
//
// Resources: s_Texture t0 -> s[0:7], Sampler0 -> s[8:11], __GLOBAL_CB__ slot 0 -> s[12:15],
//   globalConstantBuffer slot 7 -> s[16:19].
// Interpolants: ONE PS input, semantic 1 (SPI_PS_IN_CONTROL = 1 interpolant): the ISA's attr0 is the vertex
//   shader's param1 (basic_vv: TexCoord.xy / ClipPos.zw), so it is declared at location 1 and only .zw is read.
//
// Not expressible / differences:
//   * v_sin_f32 / v_cos_f32 take revolutions; written sin/cos(x * 2pi) of the pre-scaled operand (extra rounding,
//     different implementation). v_log_f32 / v_exp_f32 are written log2 / exp2 (hardware log2(0) = -inf,
//     log2(x<0) = NaN; GLSL undefined). v_fract_f32 -> fract(), v_floor_f32 -> floor(), v_trunc_f32 -> trunc().
//   * v_med3_f32(1.0, x, 0) clamp (0xCCC) is written clamp(x, 0, 1); identical except for NaN.
//   * v_fma_f32 is written fma() (fused, as on hardware); v_mad/v_mac/v_madak/v_madmk are unfused (`precise`).
//     The omods mul:2 (0x254) and div:2 (0xA04) are written as `* 2.0` / `* 0.5` after the unfused operation.
//   * The image_samples run with implicit derivatives inside non-uniform branches (0x5AC, 0xD28): quads that are
//     only partly inside keep stale coordinates on hardware and have undefined derivatives in GLSL.
//   * dFdxCoarse stands for the DPP pair (quad lane 1 minus quad lane 0 of ClipPos.x), as in solid_fill_p.
//   * v_rcp_f32 (1 ulp) vs GLSL 1.0/x; v_min/v_max NaN rules differ from GLSL min()/max(); v_cmpx_nle (NaN -> true)
//     is written !(a <= b) to keep the NaN behaviour.
//   * Output is packed to fp16 with round-toward-zero (v_cvt_pkrtz_f16_f32); the render target format decides here.

layout(set = 0, binding = 1, std140) uniform GLOBAL_CB {   // metadata "__GLOBAL_CB__", resource slot 0, 252 bytes
    layout(offset = 0)   vec4  u_FadeA;
    layout(offset = 16)  vec4  u_FadeB;
    layout(offset = 32)  vec4  u_TargetRect;
    layout(offset = 48)  float u_Radius;
    layout(offset = 52)  uint  u_Flag;       // metadata base type 9 (32-bit integer class)
    layout(offset = 56)  vec2  u_FocusPosition;
    layout(offset = 64)  vec2  u_FocusSize;
    layout(offset = 72)  float u_FocusRadius;
    layout(offset = 76)  float u_Ratio;
    layout(offset = 80)  float u_Trail;
    layout(offset = 84)  float u_Angle;
    layout(offset = 88)  float u_Thickness;
    layout(offset = 92)  float u_Offset;
    layout(offset = 96)  float u_Pressing;
    layout(offset = 100) float u_Time;
    layout(offset = 104) float u_Pixel;
    layout(offset = 108) float u_Strain;
    layout(offset = 112) float u_Moving;
    layout(offset = 116) float u_Showing;
    layout(offset = 120) float u_InOutScale;
    layout(offset = 124) float u_Distance;
    layout(offset = 128) float u_IsLine;
    layout(offset = 132) float u_NoiseScale;
    layout(offset = 136) float u_MinOpacity;
    layout(offset = 140) float u_AlphaGamma;
    layout(offset = 144) float u_RadialMaskRadius;
    layout(offset = 148) float u_RadialMaskIntensity;
    layout(offset = 152) float u_MorphByRatioIntensity;
    layout(offset = 156) float u_WarpGradientCurveByRatioIntensity;
    layout(offset = 160) float u_WarpGradientCurveToeValue;
    layout(offset = 164) float u_AreaOpacityDecreaseRateBySize;
    layout(offset = 168) float u_AreaWarpFadePixel;
    layout(offset = 172) float u_NoiseMoveFrequency;
    layout(offset = 176) vec2  u_ShimmerGradientMaskParam;
    layout(offset = 192) vec4  u_ToneCurveToe;
    layout(offset = 208) vec4  u_ToneCurveMid;
    layout(offset = 224) vec4  u_ToneCurveShoulder;
    layout(offset = 240) vec2  u_ToneCurveMidpoint;
    layout(offset = 248) float u_ShowAlpha;
};
// globalConstantBuffer: metadata resource slot 7, 8 bytes (see solid_fill_p.frag).
layout(set = 0, binding = 2, std140) uniform globalConstantBuffer {
    layout(offset = 0) float u_GlobalAlpha;
    layout(offset = 4) float u_GlobalIntensity;
};
// s_Texture (t0) with Sampler0 (s0).
layout(set = 0, binding = 3) uniform sampler2D s_Texture;

layout(location = 1) in vec4 in_attr0;   // ISA attr0 = PS input semantic 1 (VS param1): .zw input.ClipPos (TEXCOORD1)
layout(location = 0) out vec4 out_mrt0;  // exp mrt0 (SPI_SHADER_COL_FORMAT = 4: packed fp16, v_cvt_pkrtz_f16_f32 = round toward zero)

const float K_2PI = 6.28318530717958647692;

void main()
{
    precise float v0, v1, v2, v3, v4, v5, v6, v7, v8, v9, v10, v11, v12, v13, v14, v15, v16, v17, v18, v19, v20, v21, v22, v23;
    float s0f, s1f, s2f, s3f, s4f, s5f, s6f, s7f;
    float s6, s7, s20, s21, s22, s23, s26, s27, s28, s29, s30, s31, s32, s33, s36, s37;
    uint s0_u, s8_u;
    float vcc_lo, vcc_hi;
    bool vcc, s20m, s34m, s0m;
    vec4 t;

    // ---- prologue (both paths) ----
    s26 = u_Strain; s27 = u_Moving;                                          // 0x4
    vcc_hi = uintBitsToFloat(0x3F100000u);   // 0.5625                       // 0x10
    s30 = uintBitsToFloat(0x3E22F983u);      // 1/(2*pi)                     // 0x18
    s20 = u_FocusPosition.x; s21 = u_FocusPosition.y; s22 = u_FocusSize.x; s23 = u_FocusSize.y; // 0x20
    vcc_lo = u_Angle;                                                        // 0x28
    s28 = u_Distance; s29 = u_IsLine;                                        // 0x30
    s31 = uintBitsToFloat(0xBE22F983u);      // -1/(2*pi)                    // 0x38
    v4 = s26 * s27;                                                          // 0x4C
    v5 = in_attr0.w;                                                         // 0x54/0x58
    v2 = vcc_hi * v5 + -s21;                                                 // 0x5C
    v7 = 1.0 / s23;                                                          // 0x64
    v3 = s30 * vcc_lo;                                                       // 0x68
    v4 = s28 * v4;                                                           // 0x70
    v6 = in_attr0.z;                                                         // 0x74/0x78
    v1 = v7 * v2;                                                            // 0x7C
    v8 = cos(v3 * K_2PI);    // v_cos_f32 (revolutions)                      // 0x80
    v0 = v6 - s20;                                                           // 0x84
    v10 = 1.0 / s22;                                                         // 0x88
    v11 = min(uintBitsToFloat(0x3E4CCCCDu), v4);   // 0.2                    // 0x8C
    v9 = sin(v3 * K_2PI);    // v_sin_f32 (revolutions)                      // 0x94
    v4 = v8 * v1;                                                            // 0x98
    v7 = s31 * vcc_lo;                                                       // 0x9C
    v3 = v10 * v0;                                                           // 0xA4
    v10 = 1.0 - v11;                                                         // 0xA8
    v12 = v9 * v1;                                                           // 0xAC
    vcc = s29 > 0.0;                                                         // 0xB0
    v1 = sin(v7 * K_2PI);    // v_sin_f32 (revolutions)                      // 0xB8
    v4 = v3 * -v9 + v4;                                                      // v_mac_f32_e64 0xBC
    v10 = 1.0 / v10;                                                         // 0xC4
    v12 = v3 * v8 + v12;                                                     // v_mac 0xC8
    v7 = uintBitsToFloat(0x3F100000u) * v5;   // 0.5625                      // 0xCC
    v10 = v10 * v4;                                                          // 0xD4
    v21 = v1 * v10;                                                          // 0xD8
    v11 = v8 * v10;                                                          // 0xDC
    v21 = v12 * v8 + v21;                                                    // v_mac 0xE0
    v11 = v12 * -v1 + v11;                                                   // v_mac_f32_e64 0xE4

    if (vcc) {                                                               // s_cbranch_vccz 0xEC -> 0x70C
        // ---- line (u_IsLine > 0) ----
        v4 = 1.0 - s27;                                                          // 0xF0
        s20 = u_Showing; s21 = u_InOutScale;                                     // 0xF8
        v1 = 1.0 - s20;                                                          // 0x104
        vcc_lo = u_Pressing;                                                     // 0x10C
        v3 = 1.0 - vcc_lo;                                                       // 0x118
        s28 = u_FocusRadius; s29 = u_Ratio;                                      // 0x120
        vcc_lo = u_Trail;                                                        // 0x128
        v1 = s21 * v1 + 1.0;                                                     // 0x130
        v12 = 1.0 - vcc_lo;                                                      // 0x13C
        v8 = -4.0 * s27 + 1.0;                                                   // 0x144
        s26 = u_Thickness; s27 = u_Offset;                                       // 0x14C
        v4 = s26 * v4;                                                           // 0x158
        v9 = s22 * v1;                                                           // 0x15C
        v14 = s23 * v1;                                                          // 0x160
        v13 = s27 * v3;                                                          // 0x164
        v1 = v8 * v3 + -1.0;                                                     // 0x168
        v4 = v4 * v12;                                                           // 0x170
        v9 = 1.0 / v9;                                                           // 0x174
        v14 = 1.0 / v14;                                                         // 0x178
        v16 = v13 * v12;                                                         // 0x17C
        v3 = v8 * v3;                                                            // 0x180
        vcc_lo = u_Pixel;                                                        // 0x184
        v15 = abs(v9) * abs(v0) + s28;                                           // 0x18C
        v13 = abs(v14) * abs(v2) + -1.0;                                         // 0x194
        v2 = abs(v9) * abs(v0) + -1.0;                                           // 0x19C
        v0 = v4 * v1 + v16;                                                      // 0x1A4
        v16 = v4 * v3 + v16;                                                     // v_mac 0x1AC
        v14 = -1.0 + v15;                                                        // 0x1B0
        v4 = s29 * v13 + s28;                                                    // 0x1B4
        v9 = s29 * v13;                                                          // 0x1BC
        v3 = 0.5 * vcc_lo + -v0;                                                 // 0x1C4
        v0 = 0.5 * vcc_lo + v0;                                                  // 0x1CC
        v12 = max(0.0, v14);                                                     // 0x1D4
        v1 = max(0.0, v4);                                                       // 0x1D8
        v2 = max(v2, v9);                                                        // 0x1DC
        v4 = 0.5 * vcc_lo + -v16;                                                // 0x1E0
        v13 = v0 + v3;                                                           // 0x1E8
        v12 = v12 * v12;                                                         // 0x1EC
        v16 = 0.5 * vcc_lo + v16;                                                // 0x1F0
        v2 = s28 + v2;                                                           // 0x1F8
        v0 = 0.0;                                                                // 0x1FC
        v9 = 1.0 / v13;                                                          // 0x200
        v12 = v1 * v1 + v12;                                                     // v_mac 0x204
        v13 = v16 + v4;                                                          // 0x208
        v14 = min(0.0, v2);                                                      // 0x20C
        v1 = 0.0;                                                                // 0x210
        v2 = 0.0;                                                                // 0x214
        v15 = sqrt(v12);                                                         // 0x218
        v13 = 1.0 / v13;                                                         // 0x21C
        v12 = 0.0;                                                               // 0x220
        v15 = v15 - s28;                                                         // 0x228
        v14 = v15 + v14;                                                         // 0x22C
        v3 = v14 + v3;                                                           // 0x230
        v4 = v14 + v4;                                                           // 0x234
        v3 = clamp(v9 * v3, 0.0, 1.0);                                           // 0x238
        v4 = clamp(v13 * v4, 0.0, 1.0);                                          // 0x240
        v9 = v3 * v3;                                                            // 0x248
        v13 = -v4 * v4;                                                          // 0x24C
        v3 = (-v3 * v9) * 2.0;                                                   // v_mul_f32 mul:2 0x254
        v13 = v4 * v13;                                                          // 0x25C
        v9 = v4 * -v4 + v9;                                                      // v_mac_f32_e64 0x260
        v3 = -2.0 * v13 + v3;                                                    // v_mac 0x268
        v9 = v9 * uintBitsToFloat(0x40400000u) + v3;   // 3.0                    // v_madmk 0x26C
        if (!(v9 <= 0.0)) {                                                      // v_cmpx_nle 0x274, s_cbranch_execz 0x27C -> 0x704
            v0 = uintBitsToFloat(0xBF5A8E5Cu);   // -0.85373473                     // 0x280
            vcc_lo = u_Time;                                                         // 0x288
            vcc_hi = u_NoiseMoveFrequency;                                           // 0x290
            v1 = vcc_lo * vcc_hi;                                                    // 0x29C
            vcc_lo = u_NoiseScale;                                                   // 0x2A4
            s21 = uintBitsToFloat(0x42820000u);   // 65.0                           // 0x2AC
            v4 = 0.5;                                                                // 0x2B4
            s28 = uintBitsToFloat(0xBBFC0FC1u);   // -1/130                         // 0x2B8
            v3 = uintBitsToFloat(0x3E22F983u) * v1;   // 1/(2*pi)                   // 0x2C0
            v1 = 1.0 / vcc_lo;                                                       // 0x2CC
            v2 = sin(v3 * K_2PI);                                                    // 0x2D4
            v3 = cos(v3 * K_2PI);                                                    // 0x2D8
            v2 = v21 * v1 + v2;                                                      // v_mac 0x2DC
            v3 = v11 * v1 + v3;                                                      // v_mac 0x2E0
            v10 = v2 + v3;                                                           // 0x2E4
            v1 = v10 * uintBitsToFloat(0x3EBB67AFu) + v2;   // 0.36602542          // v_madmk 0x2E8
            v10 = v10 * uintBitsToFloat(0x3EBB67AFu) + v3;                           // v_madmk 0x2F0
            v1 = floor(v1);                                                          // 0x2F8
            v10 = floor(v10);                                                        // 0x2FC
            v13 = v2 - v1;                                                           // 0x300
            v2 = v1 + v10;                                                           // 0x304
            v14 = v3 - v10;                                                          // 0x308
            v12 = uintBitsToFloat(0x3B62C4A7u) * v10;   // 1/289                    // 0x30C
            v11 = uintBitsToFloat(0x3B62C4A7u) * v1;                                 // 0x314
            v3 = v2 * uintBitsToFloat(0x3E58658Cu) + v13;   // 0.21132487          // v_madmk 0x31C
            v2 = v2 * uintBitsToFloat(0x3E58658Cu) + v14;                            // v_madmk 0x324
            v13 = trunc(v12);                                                        // 0x32C
            v14 = trunc(v11);                                                        // 0x330
            v16 = v3 * v3;                                                           // 0x334
            vcc = v3 > v2;                                                           // 0x338
            v11 = v13 * uintBitsToFloat(0xC3908000u) + v10;   // -289              // v_madmk 0x33C
            v10 = v14 * uintBitsToFloat(0xC3908000u) + v1;                           // v_madmk 0x344
            v1 = uintBitsToFloat(0xBF13CD3Au) + v3;   // -0.57735026                // 0x34C
            v16 = v2 * v2 + v16;                                                     // v_mac 0x354
            v13 = vcc ? 0.0 : 1.0;                                                   // 0x358
            v14 = v11 * v11;                                                         // 0x360
            v12 = 1.0 + v11;                                                         // 0x364
            v15 = vcc ? 1.0 : 0.0;                                                   // 0x368
            v20 = vcc ? -1.0 : 0.0;                                                  // 0x370
            v13 = v13 + v11;                                                         // 0x378
            v14 = v14 * uintBitsToFloat(0x42080000u) + v11;   // 34.0              // v_madmk 0x37C
            v16 = 0.5 - v16;                                                         // 0x384
            v19 = v12 * v12;                                                         // 0x388
            v11 = v15 + v10;                                                         // 0x38C
            v17 = v13 * v13;                                                         // 0x390
            v18 = uintBitsToFloat(0x3B62C4A7u) * v14;                                // 0x394
            v20 = v20 + v3;                                                          // 0x39C
            v16 = max(0.0, v16);                                                     // 0x3A0
            v15 = v19 * uintBitsToFloat(0x42080000u) + v12;                          // v_madmk 0x3A4
            v17 = v17 * uintBitsToFloat(0x42080000u) + v13;                          // v_madmk 0x3AC
            v18 = trunc(v18);                                                        // 0x3B4
            v19 = v10 + v14;                                                         // 0x3B8
            v12 = vcc ? 0.0 : -1.0;                                                  // 0x3BC
            v22 = v1 * v1;                                                           // 0x3C4
            v21 = uintBitsToFloat(0x3B62C4A7u) * v17;                                // 0x3C8
            v14 = v11 + v17;                                                         // 0x3D0
            v17 = v10 + v15;                                                         // 0x3D4
            v11 = uintBitsToFloat(0x3E58658Cu) + v20;                                // 0x3D8
            v12 = v12 + v2;                                                          // 0x3E0
            v20 = trunc(v21);                                                        // 0x3E4
            v10 = uintBitsToFloat(0xBF13CD3Au) + v2;                                 // 0x3E8
            v13 = v16 * v16;                                                         // 0x3F0
            v16 = uintBitsToFloat(0x3B62C4A7u) * v15;                                // 0x3F4
            v15 = v18 * uintBitsToFloat(0xC3908000u) + v19;                          // v_madmk 0x3FC
            v18 = v11 * v11;                                                         // 0x404
            v12 = uintBitsToFloat(0x3E58658Cu) + v12;                                // 0x408
            v22 = v10 * v10 + v22;                                                   // v_mac 0x410
            v21 = trunc(v16);                                                        // 0x414
            v16 = v20 * uintBitsToFloat(0xC3908000u) + v14;                          // v_madmk 0x418
            v13 = v13 * v13;                                                         // 0x420
            v18 = v12 * v12 + v18;                                                   // v_mac 0x424
            v20 = v21 * uintBitsToFloat(0xC3908000u) + v17;                          // v_madmk 0x428
            v17 = v15 * v15;                                                         // 0x430
            v21 = v16 * v16;                                                         // 0x434
            v19 = 0.5 - v18;                                                         // 0x438
            v17 = v17 * uintBitsToFloat(0x42080000u) + v15;                          // v_madmk 0x43C
            v15 = 1.0 + v20;                                                         // 0x444
            v16 = v21 * uintBitsToFloat(0x42080000u) + v16;                          // v_madmk 0x448
            v19 = max(0.0, v19);                                                     // 0x450
            v21 = uintBitsToFloat(0x3B62C4A7u) * v17;                                // 0x454
            v20 = v15 * v15;                                                         // 0x45C
            v18 = uintBitsToFloat(0x3B62C4A7u) * v16;                                // 0x460
            v21 = trunc(v21);                                                        // 0x468
            v15 = v20 * uintBitsToFloat(0x42080000u) + v15;                          // v_madmk 0x46C
            v20 = v21 * uintBitsToFloat(0xC3908000u) + v17;                          // v_madmk 0x474
            v17 = uintBitsToFloat(0x3B62C4A7u) * v15;                                // 0x47C
            v21 = 0.5 - v22;                                                         // 0x484
            v22 = trunc(v18);                                                        // 0x488
            v18 = v19 * v19;                                                         // 0x48C
            v14 = uintBitsToFloat(0x3CC7CE0Cu) * v20;   // 1/41                     // 0x490
            v17 = trunc(v17);                                                        // 0x498
            v19 = max(0.0, v21);                                                     // 0x49C
            v20 = v22 * uintBitsToFloat(0xC3908000u) + v16;                          // v_madmk 0x4A0
            v16 = fract(v14);                                                        // 0x4A8
            v14 = v18 * v18;                                                         // 0x4AC
            v18 = v19 * v19;                                                         // 0x4B0
            v19 = uintBitsToFloat(0x3CC7CE0Cu) * v20;                                // 0x4B4
            v20 = v17 * uintBitsToFloat(0xC3908000u) + v15;                          // v_madmk 0x4BC
            v17 = 2.0 * v16 + -0.5;                                                  // 0x4C4
            v16 = 2.0 * v16 + -1.0;                                                  // 0x4CC
            v15 = v18 * v18;                                                         // 0x4D4
            v19 = fract(v19);                                                        // 0x4D8
            v18 = uintBitsToFloat(0x3CC7CE0Cu) * v20;                                // 0x4DC
            v21 = floor(v17);                                                        // 0x4E4
            v17 = -0.5 + abs(v16);                                                   // 0x4E8
            v20 = 2.0 * v19 + -0.5;                                                  // 0x4F0
            v18 = fract(v18);                                                        // 0x4F8
            v21 = v16 - v21;                                                         // 0x4FC
            v19 = 2.0 * v19 + -1.0;                                                  // 0x500
            v22 = floor(v20);                                                        // 0x508
            v20 = v21 * v21;                                                         // 0x50C
            v16 = -0.5 + abs(v19);                                                   // 0x510
            v3 = v3 * v21;                                                           // 0x518
            v21 = 2.0 * v18 + -0.5;                                                  // 0x51C
            v19 = v19 - v22;                                                         // 0x524
            v18 = 2.0 * v18 + -1.0;                                                  // 0x528
            v20 = v17 * v17 + v20;                                                   // v_mac 0x530
            v3 = v17 * v2 + v3;                                                      // v_mac 0x534
            v21 = floor(v21);                                                        // 0x538
            v17 = v19 * v19;                                                         // 0x53C
            v2 = -0.5 + abs(v18);                                                    // 0x540
            v20 = v0 * v20 + uintBitsToFloat(0x3FE57BE0u);   // 1.7928429          // v_madak 0x548
            v19 = v11 * v19;                                                         // 0x550
            v11 = v18 - v21;                                                         // 0x554
            v17 = v16 * v16 + v17;                                                   // v_mac 0x558
            v13 = v13 * v20;                                                         // 0x55C
            v19 = v16 * v12 + v19;                                                   // v_mac 0x560
            v12 = v11 * v11;                                                         // 0x564
            v17 = v0 * v17 + uintBitsToFloat(0x3FE57BE0u);                           // v_madak 0x568
            v1 = v1 * v11;                                                           // 0x570
            v3 = v13 * v3;                                                           // 0x574
            v12 = v2 * v2 + v12;                                                     // v_mac 0x578
            v13 = v14 * v17;                                                         // 0x57C
            v1 = v2 * v10 + v1;                                                      // v_mac 0x580
            v0 = v0 * v12 + uintBitsToFloat(0x3FE57BE0u);                            // v_madak 0x584
            v3 = v19 * v13 + v3;                                                     // v_mac 0x58C
            v0 = v15 * v0;                                                           // 0x590
            v10 = v0 * v1 + v3;                                                      // 0x594
            v0 = clamp(s21 * v10 + 0.5, 0.0, 1.0);                                   // 0x59C
            v1 = sqrt(v0);                                                           // 0x5A4
            v3 = v1 * v0;                                                            // 0x5A8
            t = texture(s_Texture, vec2(v3, v4));   // image_sample dmask 0x7         // 0x5AC
            v0 = t.x; v1 = t.y; v2 = t.z;
            if (s28 < v10) {                                                         // v_cmpx_lt 0x5B4, s_cbranch_execz 0x5B8 -> 0x6C8
                s6 = u_ToneCurveMidpoint.x; s7 = u_ToneCurveMidpoint.y;                  // 0x5BC
                if (s6 >= v3) {                                                          // v_cmpx_ge 0x5CC (+ no-op v_cmpx_eq_u32 0,0 0x5D0), execz 0x5D8
                    v10 = v3 * v3;                                                           // 0x5DC
                    s0f = u_ToneCurveToe.x; s1f = u_ToneCurveToe.y; s2f = u_ToneCurveToe.z; s3f = u_ToneCurveToe.w; // 0x5E0
                    v4 = s2f * v10 + s0f;                                                    // 0x5EC
                    v10 = v10 * s3f + s1f;                                                   // 0x5F4
                    v10 = v3 * v10 + v4;                                                     // 0x5FC
                } else {                                                                 // s_andn2_b64 exec 0x604, execz 0x608
                    s0m = s7 <= v3;                                                          // v_cmp_le s[0:1] 0x60C (& exec -> s[8:9] 0x624)
                    if (s7 >= v3 && s6 <= v3) {                                              // v_cmpx_ge 0x620, v_cmpx_le 0x628, execz 0x62C
                        v4 = v3 - s6;                                                            // 0x630
                        s0f = u_ToneCurveMid.x; s1f = u_ToneCurveMid.y; s2f = u_ToneCurveMid.z; s3f = u_ToneCurveMid.w; // 0x634
                        v11 = v4 * v4;                                                           // 0x63C
                        v10 = s2f * v11 + s0f;                                                   // 0x644
                        v11 = v11 * s3f + s1f;                                                   // 0x64C
                        v10 = v11 * v4 + v10;                                                    // v_mac 0x654
                    } else {                                                                 // s_andn2_b64 exec, vcc 0x658, execz 0x65C
                        v10 = 0.0;                                                               // 0x660
                        if (s0m) {                                                               // s_and_b64 exec, s[8:9] 0x664, execz 0x668
                            v3 = v3 - s7;                                                            // 0x66C
                            s0f = u_ToneCurveShoulder.x; s1f = u_ToneCurveShoulder.y; s2f = u_ToneCurveShoulder.z; s3f = u_ToneCurveShoulder.w; // 0x670
                            v10 = v3 * v3;                                                           // 0x678
                            v4 = s2f * v10 + s0f;                                                    // 0x680
                            v10 = v10 * s3f + s1f;                                                   // 0x688
                            v10 = v3 * v10 + v4;                                                     // 0x690
                        }
                    }
                }                                                                        // s_mov_b64 exec, s[4:5] 0x698
                vcc_lo = u_MinOpacity; vcc_hi = u_AlphaGamma;                            // 0x69C
                v3 = 1.0 - vcc_lo;                                                       // 0x6A8
                v3 = v10 * v3 + vcc_lo;                                                  // 0x6B0
                v3 = log2(abs(v3));                                                      // 0x6B8
                v3 = vcc_hi * v3;                                                        // 0x6C0
                v3 = exp2(v3);                                                           // 0x6C4
            }                                                                        // s_mov_b64 exec, s[26:27] 0x6C8
            vcc_lo = u_ShowAlpha;                                                    // 0x6CC
            v3 = s20 * v3;                                                           // 0x6D4
            vcc = 0.0 != vcc_lo;                                                     // 0x6DC
            v3 = v3 * v8;                                                            // 0x6E4
            v3 = v3 * v9;                                                            // 0x6E8
            v0 = vcc ? v3 : v0;                                                      // 0x6F0
            v1 = vcc ? v3 : v1;                                                      // 0x6F4
            v2 = vcc ? v3 : v2;                                                      // 0x6F8
            v12 = vcc ? 1.0 : v3;                                                    // 0x6FC
        }                                                                        // s_mov_b64 exec, s[30:31] 0x704; s_branch 0x708 -> 0xD68
    } else {
        // ---- area (u_IsLine <= 0) ----
        v4 = v21 * v21;                                                          // 0x70C
        s26 = u_AreaWarpFadePixel;                                               // 0x710
        s22 = u_FocusRadius; s23 = u_Ratio;                                      // 0x718
        v1 = s22 - s26;                                                          // 0x724
        v0 = -1.0 + s26;                                                         // 0x72C
        v4 = v11 * v11 + v4;                                                     // v_mac 0x734
        s20m = s27 > 0.0;                                                        // v_cmp_gt s[20:21] 0x738 (s27 = u_Moving)
        s28 = u_MorphByRatioIntensity;                                           // 0x740
        v12 = abs(v21) + v1;                                                     // 0x748
        v3 = abs(v11) + v0;                                                      // 0x750
        v2 = v0 + abs(v21);                                                      // 0x758
        v4 = sqrt(v4);                                                           // 0x760
        vcc = s20m;                                                              // 0x764
        v12 = v12 + v0;                                                          // 0x768
        v14 = s23 * v3 + v1;                                                     // 0x76C
        v13 = s23 * v3;                                                          // 0x774
        v12 = max(0.0, v12);                                                     // 0x778
        v3 = max(0.0, v14);                                                      // 0x77C
        v2 = max(v2, v13);                                                       // 0x780
        v12 = v12 * v12;                                                         // 0x784
        v2 = v1 + v2;                                                            // 0x788
        v12 = v3 * v3 + v12;                                                     // v_mac 0x78C
        v2 = min(0.0, v2);                                                       // 0x790
        v3 = sqrt(v12);                                                          // 0x794
        v1 = v3 - v1;                                                            // 0x798
        v3 = v1 + v2;                                                            // 0x79C
        v2 = v4 - v3;                                                            // 0x7A0
        v0 = v2 + v0;                                                            // 0x7A4
        v3 = s28 * v0 + v3;                                                      // v_mac 0x7AC
        if (!vcc) {                                                              // s_cbranch_vccnz 0x7B0
            s26 = u_Pixel;                                                           // 0x7B4
        }
        v13 = 1.0 / s26;                                                         // 0x7C0
        v0 = 0.0;                                                                // 0x7C4
        v1 = 0.0;                                                                // 0x7C8
        v2 = 0.0;                                                                // 0x7CC
        v12 = 0.0;                                                               // 0x7D0
        v13 = clamp(v13 * v3, 0.0, 1.0);                                         // 0x7D4
        v3 = -2.0 * v13 + uintBitsToFloat(0x40400000u);   // 3.0                // v_madak 0x7E0
        v13 = -v13 * v13;                                                        // 0x7E8
        v3 = v13 * v3;                                                           // 0x7F0
        if (!(v3 <= -1.0)) {                                                     // v_cmpx_nle 0x7F4, s_cbranch_execz 0x7FC -> 0xD64
            v15 = v9 * v11;                                                          // 0x800
            s28 = u_Pressing; s29 = u_Time;                                          // 0x804
            s30 = u_NoiseMoveFrequency; s31 = u_ShimmerGradientMaskParam.x;          // 0x80C
            v2 = s29 * s30;                                                          // 0x818
            vcc_hi = uintBitsToFloat(0x3DCCCCCDu);   // 0.1                         // 0x820
            s26 = uintBitsToFloat(0x3E4CCCCDu);      // 0.2                         // 0x828
            v14 = vcc_hi;                                                            // 0x830
            v0 = s26 * s29;                                                          // 0x834
            v12 = uintBitsToFloat(0x3E22F983u) * v2;   // 1/(2*pi)                  // 0x83C
            vcc_lo = u_NoiseScale;                                                   // 0x844
            v1 = 1.0 / vcc_lo;                                                       // 0x850
            v14 = fma(s29, uintBitsToFloat(0x3E4CCCCDu), v14);                       // v_fma_f32 0x854
            v13 = trunc(v0);                                                         // 0x860
            v0 = sin(v12 * K_2PI);                                                   // 0x864
            v17 = cos(v12 * K_2PI);                                                  // 0x868
            v2 = 0.5 + s29;                                                          // 0x86C
            v14 = trunc(v14);                                                        // 0x874
            v13 = uintBitsToFloat(0xC0A00000u) * v13 + s29;   // -5.0              // 0x878
            v0 = v21 * v1 + v0;                                                      // v_mac 0x884
            v17 = v11 * v1 + v17;                                                    // v_mac 0x888
            v15 = -v21 * v8 + v15;                                                   // v_mac_f32_e64 0x88C
            v14 = v14 * uintBitsToFloat(0xC0A00000u) + v2;                           // v_madmk 0x894
            s32 = u_WarpGradientCurveByRatioIntensity; s33 = u_WarpGradientCurveToeValue; // 0x89C
            v12 = -4.0 + v13;                                                        // 0x8A4
            v9 = abs(v8) + abs(v9);                                                  // 0x8A8
            v8 = v0 + v17;                                                           // 0x8B0
            v13 = -4.0 + v14;                                                        // 0x8B4
            vcc_lo = u_ShimmerGradientMaskParam.y;                                   // 0x8B8
            v12 = max(-1.0, v12);                                                    // 0x8C0
            v2 = 1.0 / v9;                                                           // 0x8C4
            v14 = v8 * uintBitsToFloat(0x3EBB67AFu) + v0;   // 0.36602542          // v_madmk 0x8C8
            v13 = max(-1.0, v13);                                                    // 0x8D0
            v9 = uintBitsToFloat(0xBF35082Du) * v11;   // -0.707156                 // 0x8D4
            v12 = uintBitsToFloat(0x3EFFFE10u) * v12;   // 0.49998522               // 0x8DC
            v2 = v2 * v15;                                                           // 0x8E4
            v15 = v8 * uintBitsToFloat(0x3EBB67AFu) + v17;                           // v_madmk 0x8E8
            v8 = floor(v14);                                                         // 0x8F0
            v16 = uintBitsToFloat(0x3EFFFE10u) * v13;                                // 0x8F4
            v22 = cos(v12 * K_2PI);                                                  // 0x8FC
            v9 = v21 * uintBitsToFloat(0x3F3501B9u) + v9;   // 0.70705754          // v_madmk 0x900
            v15 = floor(v15);                                                        // 0x908
            v18 = v0 - v8;                                                           // 0x90C
            v14 = cos(v16 * K_2PI);                                                  // 0x910
            v2 = 0.5 * v2 + 0.5;                                                     // 0x914
            v9 = uintBitsToFloat(0x3EB504F3u) * v9 + 0.5;   // 0.35355338          // 0x91C
            v0 = v8 + v15;                                                           // 0x928
            v19 = v17 - v15;                                                         // 0x92C
            v16 = uintBitsToFloat(0x3B62C4A7u) * v15;   // 1/289                    // 0x930
            v17 = uintBitsToFloat(0x3B62C4A7u) * v8;                                 // 0x938
            v14 = v14 - v22;                                                         // 0x940
            v1 = v0 * uintBitsToFloat(0x3E58658Cu) + v18;   // 0.21132487          // v_madmk 0x944
            v0 = v0 * uintBitsToFloat(0x3E58658Cu) + v19;                            // v_madmk 0x94C
            v18 = 1.0 - s33;                                                         // 0x958
            v16 = trunc(v16);                                                        // 0x960
            v19 = trunc(v17);                                                        // 0x964
            v17 = uintBitsToFloat(0x3F3503E0u) * v11;   // 0.70709038               // 0x968
            s34m = v1 > v0;                                                          // 0x970
            v20 = 1.0 / v18;                                                         // 0x978
            v9 = log2(abs(v9));                                                      // 0x97C
            v11 = v19 * uintBitsToFloat(0xC3908000u) + v8;   // -289               // v_madmk 0x984
            v12 = v2 - s33;                                                          // 0x98C
            v19 = s34m ? 1.0 : 0.0;                                                  // 0x990
            v18 = s34m ? 0.0 : 1.0;                                                  // 0x998
            v23 = v1 * v1;                                                           // 0x9A0
            v8 = uintBitsToFloat(0xBF13CD3Au) + v0;   // -0.57735026                // 0x9A4
            v12 = clamp(v20 * v12, 0.0, 1.0);                                        // 0x9AC
            v20 = v16 * uintBitsToFloat(0xC3908000u) + v15;                          // v_madmk 0x9B4
            v16 = v21 * uintBitsToFloat(0xBF350606u) + v17;   // -0.70712316       // v_madmk 0x9BC
            v10 = v19 + v11;                                                         // 0x9C4
            v23 = v0 * v0 + v23;                                                     // v_mac 0x9C8
            s36 = u_AreaOpacityDecreaseRateBySize;                                   // 0x9CC
            v19 = v20 * v20;                                                         // 0x9D4
            v17 = v18 + v20;                                                         // 0x9D8
            v15 = 1.0 + v20;                                                         // 0x9DC
            v16 = uintBitsToFloat(0x3EB504F3u) * v16 + 0.5;                          // 0x9E0
            s37 = u_Showing;                                                         // 0x9EC
            v18 = v19 * uintBitsToFloat(0x42080000u) + v20;   // 34.0              // v_madmk 0x9F4
            v21 = v17 * v17;                                                         // 0x9FC
            v19 = v15 * v15;                                                         // 0xA00
            v22 = (v14 * v16 + v22) * 0.5;                                           // v_mac_f32_e64 div:2 0xA04
            v14 = s34m ? -1.0 : 0.0;                                                 // 0xA0C
            v20 = uintBitsToFloat(0x3B62C4A7u) * v18;                                // 0xA14
            v17 = v21 * uintBitsToFloat(0x42080000u) + v17;                          // v_madmk 0xA1C
            v18 = v11 + v18;                                                         // 0xA24
            v15 = v19 * uintBitsToFloat(0x42080000u) + v15;                          // v_madmk 0xA28
            v19 = trunc(v20);                                                        // 0xA30
            v16 = uintBitsToFloat(0x3B62C4A7u) * v17;                                // 0xA34
            v13 = v10 + v17;                                                         // 0xA3C
            v11 = v11 + v15;                                                         // 0xA40
            v15 = uintBitsToFloat(0x3B62C4A7u) * v15;                                // 0xA44
            v17 = v19 * uintBitsToFloat(0xC3908000u) + v18;                          // v_madmk 0xA4C
            v16 = trunc(v16);                                                        // 0xA54
            v20 = 1.0 / vcc_lo;                                                      // 0xA58
            v10 = max(0.0, v22);                                                     // 0xA5C
            v19 = trunc(v15);                                                        // 0xA60
            v18 = v17 * v17;                                                         // 0xA64
            v16 = v16 * uintBitsToFloat(0xC3908000u) + v13;                          // v_madmk 0xA68
            v15 = s34m ? 0.0 : -1.0;                                                 // 0xA70
            v20 = v9 * v20;                                                          // 0xA78
            v11 = v19 * uintBitsToFloat(0xC3908000u) + v11;                          // v_madmk 0xA7C
            v13 = v18 * uintBitsToFloat(0x42080000u) + v17;                          // v_madmk 0xA84
            v19 = v16 * v16;                                                         // 0xA8C
            v18 = v15 + v0;                                                          // 0xA90
            vcc = s20m;                                                              // s_and_b64 vcc, exec, s[20:21] 0xA94
            v15 = 1.0 + v11;                                                         // 0xA98
            v17 = uintBitsToFloat(0x3B62C4A7u) * v13;                                // 0xA9C
            v11 = v14 + v1;                                                          // 0xAA4
            v14 = v19 * uintBitsToFloat(0x42080000u) + v16;                          // v_madmk 0xAA8
            v9 = uintBitsToFloat(0x3E58658Cu) + v18;                                 // 0xAB0
            v18 = v15 * v15;                                                         // 0xAB8
            v19 = trunc(v17);                                                        // 0xABC
            v17 = exp2(v20);                                                         // 0xAC0
            v16 = uintBitsToFloat(0x3B62C4A7u) * v14;                                // 0xAC4
            v11 = uintBitsToFloat(0x3E58658Cu) + v11;                                // 0xACC
            v19 = v19 * uintBitsToFloat(0xC3908000u) + v13;                          // v_madmk 0xAD4
            v13 = v18 * uintBitsToFloat(0x42080000u) + v15;                          // v_madmk 0xADC
            v20 = trunc(v16);                                                        // 0xAE4
            v15 = v12 * v12;                                                         // 0xAE8
            v17 = -1.0 + v17;                                                        // 0xAEC
            v19 = uintBitsToFloat(0x3CC7CE0Cu) * v19;   // 1/41                     // 0xAF0
            v16 = uintBitsToFloat(0x3B62C4A7u) * v13;                                // 0xAF8
            v18 = s31 * v10;                                                         // 0xB00
            v2 = v2 * v15;                                                           // 0xB04
            v15 = -2.0 * v12 + uintBitsToFloat(0x40400000u);   // 3.0               // v_madak 0xB08
            v16 = trunc(v16);                                                        // 0xB10
            v10 = v17 * v18 + v10;                                                   // v_mac 0xB14
            v17 = v20 * uintBitsToFloat(0xC3908000u) + v14;                          // v_madmk 0xB18
            v14 = fract(v19);                                                        // 0xB20
            v2 = v2 * v15;                                                           // 0xB24
            v13 = v16 * uintBitsToFloat(0xC3908000u) + v13;                          // v_madmk 0xB28
            v19 = 0.5 - v23;                                                         // 0xB30
            v18 = uintBitsToFloat(0x3CC7CE0Cu) * v17;                                // 0xB34
            v12 = 2.0 * v14 + -1.0;                                                  // 0xB3C
            v17 = 2.0 * v14 + -0.5;                                                  // 0xB44
            v16 = uintBitsToFloat(0x3CC7CE0Cu) * v13;                                // 0xB4C
            v15 = fract(v18);                                                        // 0xB54
            v18 = log2(abs(v2));                                                     // 0xB58
            v17 = floor(v17);                                                        // 0xB60
            v16 = fract(v16);                                                        // 0xB64
            v14 = -0.5 + abs(v12);                                                   // 0xB68
            v2 = uintBitsToFloat(0xBF5A8E5Cu);   // -0.85373473                     // 0xB70
            v20 = s32 * v18;                                                         // 0xB78
            v17 = v12 - v17;                                                         // 0xB7C
            v12 = uintBitsToFloat(0xBF13CD3Au) + v1;                                 // 0xB80
            v21 = 2.0 * v16 + -0.5;                                                  // 0xB88
            v16 = 2.0 * v16 + -1.0;                                                  // 0xB90
            v18 = v17 * v17;                                                         // 0xB98
            v22 = v12 * v12;                                                         // 0xB9C
            v23 = floor(v21);                                                        // 0xBA0
            v21 = 2.0 * v15 + -0.5;                                                  // 0xBA4
            v15 = 2.0 * v15 + -1.0;                                                  // 0xBAC
            v13 = -0.5 + abs(v16);                                                   // 0xBB4
            v22 = v8 * v8 + v22;                                                     // v_mac 0xBBC
            v16 = v16 - v23;                                                         // 0xBC0
            v23 = v11 * v11;                                                         // 0xBC4
            v18 = v14 * v14 + v18;                                                   // v_mac 0xBC8
            v17 = v1 * v17;                                                          // 0xBCC
            v22 = 0.5 - v22;                                                         // 0xBD0
            v1 = exp2(v20);                                                          // 0xBD4
            v20 = max(0.0, v19);                                                     // 0xBD8
            v19 = floor(v21);                                                        // 0xBDC
            v23 = v9 * v9 + v23;                                                     // v_mac 0xBE0
            v18 = v2 * v18 + uintBitsToFloat(0x3FE57BE0u);   // 1.7928429          // v_madak 0xBE4
            v17 = v14 * v0 + v17;                                                    // v_mac 0xBEC
            v14 = max(0.0, v22);                                                     // 0xBF0
            v0 = v20 * v20;                                                          // 0xBF4
            v14 = v14 * v14;                                                         // 0xBF8
            v21 = v0 * v0;                                                           // 0xBFC
            v0 = 0.5 - v23;                                                          // 0xC00
            v14 = v14 * v14;                                                         // 0xC04
            v20 = v21 * v18;                                                         // 0xC08
            v18 = v16 * v16;                                                         // 0xC0C
            v21 = v12 * v16;                                                         // 0xC10
            v16 = v15 - v19;                                                         // 0xC14
            v15 = -0.5 + abs(v15);                                                   // 0xC18
            v12 = v20 * v17;                                                         // 0xC20
            v18 = v13 * v13 + v18;                                                   // v_mac 0xC24
            v17 = -1.0 + v1;                                                         // 0xC28
            v21 = v13 * v8 + v21;                                                    // v_mac 0xC2C
            v13 = v16 * v16;                                                         // 0xC30
            v8 = v11 * v16;                                                          // 0xC34
            v11 = v2 * v18 + uintBitsToFloat(0x3FE57BE0u);                           // v_madak 0xC38
            v13 = v15 * v15 + v13;                                                   // v_mac 0xC40
            v8 = v15 * v9 + v8;                                                      // v_mac 0xC44
            v11 = v14 * v11;                                                         // 0xC48
            v14 = max(0.0, v0);                                                      // 0xC4C
            v15 = s27 * v17 + 1.0;                                                   // 0xC50
            v13 = v2 * v13 + uintBitsToFloat(0x3FE57BE0u);                           // v_madak 0xC58
            v0 = 1.0;                                                                // 0xC60
            v9 = v10 * v15;                                                          // 0xC64
            v10 = v14 * v14;                                                         // 0xC68
            v10 = v10 * v10;                                                         // 0xC6C
            v10 = v10 * v13;                                                         // 0xC70
            v12 = v8 * v10 + v12;                                                    // v_mac 0xC74
            v12 = v21 * v11 + v12;                                                   // v_mac 0xC78
            v8 = uintBitsToFloat(0x42820000u) * v12 + 0.5;   // 65.0               // 0xC7C
            v1 = v1 * v8 + -v9;                                                      // 0xC88
            v9 = s27 * v1 + v9;                                                      // v_mac 0xC90
            v2 = uintBitsToFloat(0x3E19999Au) - v9;   // 0.15                       // 0xC94
            v9 = s28 * v2 + v9;                                                      // v_mac 0xC9C
            v1 = s36 * v9;                                                           // 0xCA4
            v1 = v1 * v3 + v1;                                                       // v_mac 0xCA8
            v3 = s37 * v1;                                                           // 0xCAC
            if (vcc) {                                                               // s_cbranch_vccz 0xCB0 -> 0xCE0
                vcc_lo = u_RadialMaskRadius; vcc_hi = u_RadialMaskIntensity;             // 0xCB4
                v0 = 1.0 / vcc_lo;                                                       // 0xCC0
                v0 = -v4 * v0 + 1.0;                                                     // 0xCC4
                v0 = clamp(v0, 0.0, 1.0);   // v_med3_f32 1.0, v0, 0 clamp            // 0xCCC
                v0 = -1.0 + v0;                                                          // 0xCD4
                v0 = vcc_hi * v0 + 1.0;                                                  // 0xCD8
            }
            v0 = -1.0 + v0;                                                          // 0xCE0
            v1 = s27 * v3;                                                           // 0xCE4
            vcc = s27 > 0.0;                                                         // 0xCE8
            v12 = 1.0;                                                               // 0xCF0
            v0 = v1 * v0 + v3;                                                       // 0xCF4
            v0 = vcc ? v0 : v3;                                                      // 0xCFC
            vcc_lo = u_ShowAlpha;                                                    // 0xD00
            vcc = 0.0 != vcc_lo;                                                     // 0xD0C
            v1 = v0;                                                                 // 0xD14
            v2 = v0;                                                                 // 0xD18
            if (!vcc) {                                                              // s_cbranch_vccnz 0xD1C -> 0xD30
                v4 = 0.5;                                                                // 0xD20
                v12 = v0;                                                                // 0xD24
                t = texture(s_Texture, vec2(v3, v4));   // image_sample dmask 0x7         // 0xD28
                v0 = t.x; v1 = t.y; v2 = t.z;
            }
            if (0.0 < v12) {                                                         // v_cmpx_lt 0xD30, s_cbranch_execz 0xD34 -> 0xD64
                vcc_lo = u_MinOpacity; vcc_hi = u_AlphaGamma;                            // 0xD38
                v3 = 1.0 - vcc_lo;                                                       // 0xD44
                v3 = v12 * v3 + vcc_lo;                                                  // 0xD4C
                v3 = log2(abs(v3));                                                      // 0xD54
                v3 = vcc_hi * v3;                                                        // 0xD5C
                v12 = exp2(v3);                                                          // 0xD60
            }
        }                                                                        // s_mov_b64 exec, s[22:23] 0xD64
    }

    // ---- common: clamp, colour-space mode, u_Flag tail (0xD68..) ----
    v3 = clamp(max(0.0, v0), 0.0, 1.0);                                      // 0xD6C
    s8_u = u_Flag;                                                           // 0xD74
    s0_u = bitfieldExtract(s8_u, 20, 4);                                     // s_bfe_u32 0x40014 0xD80
    v4 = clamp(max(0.0, v1), 0.0, 1.0);                                      // 0xD88
    v1 = clamp(max(0.0, v2), 0.0, 1.0);                                      // 0xD90
    v0 = clamp(max(0.0, v12), 0.0, 1.0);                                     // 0xD98
    if (s0_u == 2u) {                                                        // 0xDA0..0xDB4 (s0 != 1 and s0 == 2)
        v2 = log2(v3);                                                           // 0xDB8
        v3 = log2(v4);                                                           // 0xDBC
        v1 = log2(v1);                                                           // 0xDC0
        v2 = uintBitsToFloat(0x40166666u) * v2;   // 2.35                       // 0xDC4
        v4 = uintBitsToFloat(0x40166666u) * v3;                                  // 0xDCC
        v1 = uintBitsToFloat(0x40166666u) * v1;                                  // 0xDD4
        v3 = exp2(v2);                                                           // 0xDDC
        v2 = exp2(v4);                                                           // 0xDE0
        v1 = exp2(v1);                                                           // 0xDE4
        v8 = uintBitsToFloat(0x3D84BA2Fu) * v3;   // 0.064808242                // 0xDE8
        v4 = uintBitsToFloat(0x3F23CF31u) * v3;   // 0.63988024                 // 0xDF0
        v3 = uintBitsToFloat(0x3C2C09BDu) * v3;   // 0.010500369                // 0xDF8
        v8 = v2 * uintBitsToFloat(0x3F6F74A4u) + v8;   // 0.93537354            // v_madmk 0xE00
        v4 = v2 * uintBitsToFloat(0x3EA79F91u) + v4;   // 0.32738927            // v_madmk 0xE08
        v3 = v2 * uintBitsToFloat(0x3DA17DE5u) + v3;   // 0.078853406           // v_madmk 0xE10
        v2 = v1 * uintBitsToFloat(0xB9169957u) + v8;   // -0.00014362238        // v_madmk 0xE18
        v4 = v1 * uintBitsToFloat(0x3D05FBE8u) + v4;   // 0.03271094            // v_madmk 0xE20
        v3 = v1 * uintBitsToFloat(0x3F691B7Bu) + v3;   // 0.91057557            // v_madmk 0xE28
        vcc = 0.0 <= v2;                                                         // 0xE30
        v1 = uintBitsToFloat(0x3CCCCCCCu) * v4;   // 0.025                      // 0xE34
        v3 = uintBitsToFloat(0x3CCCCCCCu) * v3;                                  // 0xE3C
        v2 = vcc ? v2 : -v2;                                                     // 0xE44
        v1 = log2(v1);                                                           // 0xE4C
        v3 = log2(v3);                                                           // 0xE50
        v2 = uintBitsToFloat(0x3CCCCCCCu) * v2;                                  // 0xE54
        v1 = uintBitsToFloat(0x3EE8BA2Eu) * v1;   // 1/2.2                      // 0xE5C
        v4 = uintBitsToFloat(0x3EE8BA2Eu) * v3;                                  // 0xE64
        v2 = log2(abs(v2));                                                      // 0xE6C
        v3 = exp2(v1);                                                           // 0xE74
        v1 = exp2(v4);                                                           // 0xE78
        v2 = uintBitsToFloat(0x3EE8BA2Eu) * v2;                                  // 0xE7C
        v2 = exp2(v2);                                                           // 0xE84
        v4 = vcc ? v2 : -v2;                                                     // 0xE88
    } else if (s0_u == 1u) {                                                 // s_cbranch_scc0 0xDAC -> 0xE94
        v2 = log2(v3);                                                           // 0xE94
        v3 = log2(v4);                                                           // 0xE98
        v4 = log2(v1);                                                           // 0xE9C
        v1 = uintBitsToFloat(0x4196D000u);   // 18.8515625                      // 0xEA0
        v2 = uintBitsToFloat(0x40166666u) * v2;   // 2.35                       // 0xEA8
        v3 = uintBitsToFloat(0x40166666u) * v3;                                  // 0xEB0
        v8 = uintBitsToFloat(0x40166666u) * v4;                                  // 0xEB8
        v4 = exp2(v2);                                                           // 0xEC0
        v3 = exp2(v3);                                                           // 0xEC4
        v2 = exp2(v8);                                                           // 0xEC8
        v9 = uintBitsToFloat(0x3D84BA2Fu) * v4;                                  // 0xECC
        v8 = uintBitsToFloat(0x3F23CF31u) * v4;                                  // 0xED4
        v4 = uintBitsToFloat(0x3C2C09BDu) * v4;                                  // 0xEDC
        v9 = v3 * uintBitsToFloat(0x3F6F74A4u) + v9;                             // v_madmk 0xEE4
        v8 = v3 * uintBitsToFloat(0x3EA79F91u) + v8;                             // v_madmk 0xEEC
        v4 = v3 * uintBitsToFloat(0x3DA17DE5u) + v4;                             // v_madmk 0xEF4
        v3 = v2 * uintBitsToFloat(0xB9169957u) + v9;                             // v_madmk 0xEFC
        v8 = v2 * uintBitsToFloat(0x3D05FBE8u) + v8;                             // v_madmk 0xF04
        v4 = v2 * uintBitsToFloat(0x3F691B7Bu) + v4;                             // v_madmk 0xF0C
        vcc = 0.0 <= v3;                                                         // 0xF14
        v2 = uintBitsToFloat(0x3CCCCCCCu) * v8;   // 0.025                      // 0xF18
        v4 = uintBitsToFloat(0x3CCCCCCCu) * v4;                                  // 0xF20
        v3 = vcc ? v3 : -v3;                                                     // 0xF28
        v2 = log2(v2);                                                           // 0xF30
        v4 = log2(v4);                                                           // 0xF34
        v3 = uintBitsToFloat(0x3CCCCCCCu) * v3;                                  // 0xF38
        v2 = uintBitsToFloat(0x3E232000u) * v2;   // m1 = 0.1593017578125       // 0xF40
        v4 = uintBitsToFloat(0x3E232000u) * v4;                                  // 0xF48
        v8 = log2(abs(v3));                                                      // 0xF50
        v2 = exp2(v2);                                                           // 0xF58
        v3 = exp2(v4);                                                           // 0xF5C
        v9 = uintBitsToFloat(0x3E232000u) * v8;                                  // 0xF60
        v8 = uintBitsToFloat(0x41958000u) * v2 + 1.0;   // c3 = 18.6875         // 0xF68
        v4 = uintBitsToFloat(0x41958000u) * v3 + 1.0;                            // 0xF74
        v2 = v1 * v2 + uintBitsToFloat(0x3F560000u);   // c2*Y + c1 (0.8359375) // v_madak 0xF80
        v3 = v1 * v3 + uintBitsToFloat(0x3F560000u);                             // v_madak 0xF88
        v9 = exp2(v9);                                                           // 0xF90
        v8 = 1.0 / v8;                                                           // 0xF94
        v10 = 1.0 / v4;                                                          // 0xF98
        v4 = uintBitsToFloat(0x41958000u) * v9 + 1.0;                            // 0xF9C
        v1 = v1 * v9 + uintBitsToFloat(0x3F560000u);                             // v_madak 0xFA8
        v2 = v8 * v2;                                                            // 0xFB0
        v3 = v10 * v3;                                                           // 0xFB4
        v4 = 1.0 / v4;                                                           // 0xFB8
        v2 = log2(v2);                                                           // 0xFBC
        v3 = log2(v3);                                                           // 0xFC0
        v1 = v4 * v1;                                                            // 0xFC4
        v2 = uintBitsToFloat(0x429DB000u) * v2;   // m2 = 78.84375              // 0xFC8
        v4 = uintBitsToFloat(0x429DB000u) * v3;                                  // 0xFD0
        v8 = log2(v1);                                                           // 0xFD8
        v3 = exp2(v2);                                                           // 0xFDC
        v1 = exp2(v4);                                                           // 0xFE0
        v2 = uintBitsToFloat(0x429DB000u) * v8;                                  // 0xFE4
        v2 = exp2(v2);                                                           // 0xFEC
        v4 = vcc ? v2 : -v2;                                                     // 0xFF0
    }
    if ((s8_u & 6u) != 0u) {                                                 // 0xFF8..0x100C
        v2 = 1.0;                                                                // 0x1010
        if ((s8_u & 4u) != 0u) {                                                 // s[0:1] 0x1004, s_cbranch_vccz 0x1018
            s0f = u_TargetRect.x; s1f = u_TargetRect.y; s2f = u_TargetRect.z; s3f = u_TargetRect.w; // 0x101C
            v8 = v6 - s0f;                                                           // 0x1028
            v9 = v7 - s1f;                                                           // 0x102C
            v7 = dFdxCoarse(v6);   // v_mov_b32_dpp quad_perm[0,0,0,0] (0x1030) + v_sub_f32_dpp quad_perm[1,1,1,1] (0x104C)
            v8 = -s2f + abs(v8);                                                     // 0x103C
            v9 = -s3f + abs(v9);                                                     // 0x1044
            v10 = min(v8, v9);                                                       // 0x1054
            if (0.0 > v10) {                                                         // v_cmpx_gt 0x1058
                vcc = 0.0 > v8;                                                          // 0x105C
                v8 = vcc ? v9 : v8;                                                      // 0x1060
            } else {                                                                 // s_andn2_b64 exec 0x1064
                v8 = v8 * v8;                                                            // 0x1068
                v8 = v9 * v9 + v8;                                                       // 0x106C
                v8 = sqrt(v8);                                                           // 0x1070
            }                                                                        // 0x1074
            vcc_lo = u_Radius;                                                       // 0x1078
            v9 = vcc_lo - v7;                                                        // 0x1084
            if (v8 > v9) {                                                           // v_cmpx_gt 0x1088
                v2 = vcc_lo - v8;                                                        // 0x108C
                v7 = 1.0 / v7;                                                           // 0x1090
                v2 = clamp(v7 * v2 + 0.5, 0.0, 1.0);                                     // 0x1094
            }                                                                        // 0x109C
        }
        if ((s8_u & 2u) != 0u) {                                                 // s_bitcmp1_b32 0x10A0, s_cbranch_scc0 0x10A4
            s0f = u_FadeA.x; s1f = u_FadeA.y; s2f = u_FadeA.z; s3f = u_FadeA.w; s4f = u_FadeB.x; s5f = u_FadeB.y; s6f = u_FadeB.z; s7f = u_FadeB.w; // 0x10A8
            v7 = clamp(s0f * v6 + s4f, 0.0, 1.0);                                    // 0x10B4
            v8 = clamp(v6 * s1f + s5f, 0.0, 1.0);                                    // 0x10BC
            v6 = clamp(v5 * s2f + s6f, 0.0, 1.0);                                    // 0x10C4
            v5 = clamp(v5 * s3f + s7f, 0.0, 1.0);                                    // 0x10CC
            v7 = v7 * v8;                                                            // 0x10D4
            v6 = v7 * v6;                                                            // 0x10D8
            v6 = v6 * v5;                                                            // 0x10DC
            v5 = v6 * v6;                                                            // 0x10E0
            v6 = -2.0 * v6 + uintBitsToFloat(0x40400000u);   // 3.0                  // v_madak 0x10E4
            v2 = v5 * v2;                                                            // 0x10EC
            v2 = v2 * v6;                                                            // 0x10F0
        }
        v5 = v2 * v3;                                                            // 0x10F4
        v6 = v2 * v4;                                                            // 0x10F8
        v7 = v2 * v1;                                                            // 0x10FC
        v0 = v2 * v0;                                                            // 0x1104
        vcc = (s8_u & 1u) != 0u;                                                 // s_bitcmp1_b32 0x1100, s_cselect_b64 0x1108
        v3 = vcc ? v5 : v3;                                                      // 0x110C
        v4 = vcc ? v6 : v4;                                                      // 0x1110
        v1 = vcc ? v7 : v1;                                                      // 0x1114
    }
    vcc_hi = u_GlobalIntensity;                                              // 0x1118
    v5 = vcc_hi;                                                             // 0x1124
    if ((s8_u & 1u) != 0u) {                                                 // s_bitcmp1_b32 0x1128, s_cbranch_scc0 0x112C
        vcc_lo = u_GlobalAlpha;                                                  // 0x1130
        v5 = vcc_lo * vcc_hi;                                                    // 0x113C
    }
    v2 = v5 * v3;                                                            // 0x1144
    vcc_lo = u_GlobalAlpha;                                                  // 0x1148
    v3 = v5 * v4;                                                            // 0x1150
    v1 = v5 * v1;                                                            // 0x1154
    v0 = vcc_lo * v0;                                                        // 0x115C
    out_mrt0 = vec4(v2, v3, v1, v0);   // v_cvt_pkrtz_f16_f32 v2,v2,v3 / v0,v1,v0 ; exp mrt0 compr (0x1160..0x116C)
}
