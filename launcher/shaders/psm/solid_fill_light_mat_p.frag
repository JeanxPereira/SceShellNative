#version 450
#extension GL_EXT_shader_realtime_clock : require
// Transcription of libScePsm embedded shader #18 "solid_fill_light_mat_p" (work/shaders/18_solid_fill_light_mat_p.ags.bin),
// AGC binary type 1 (pixel), code up to s_endpgm at 0x30C. EmbeddedShader.SolidFillLightMat = (basic_vv, solid_fill_light_mat_p).
// Every statement maps to the RDNA instruction at the byte offset in its trailing comment.
//
// Math (c = vertex color attr0, uv = TexCoord = attr1.xy, p = ClipPos = attr1.zw, F = u_Flag, T = u_lightTransform,
//       A = u_lightAlpha, L = u_lightColor):
//   d = sqrt((-0.7 * (T.x*uv.x))^2 + (T.y*(0.7 - 0.7*uv.y))^2)
//   a = d < 0.7 ? A.x + (A.y - A.x) * (d^2 * (6.122449 - 5.830904*d))
//               : A.y + (A.z - A.y) * (((-74.07407*d + 188.88887)*d - 155.55554)*d + 41.740738)
//   n = fract(43758.547 * sin((uv.x*12.9898 + uv.y*78.233) + float(realtime & 0x7fffff) * 1.1920931e-5))   (dither)
//   rgb = saturate(L*a + (1-a)*c.rgb + (0.004*n - 0.002))
//   if (F & 6): m = 1; (F & 4) rounded-rect coverage, (F & 2) fade ramp exactly as solid_fill_p.frag; c.a *= m
//               (unlike solid_fill_p, m is NOT applied to RGB when F & 1)
//   k = (F & 1) ? u_GlobalAlpha * u_GlobalIntensity : u_GlobalIntensity
//   out = (rgb * k, c.a * u_GlobalAlpha)
//
// Resources: __GLOBAL_CB__ slot 0 -> s[0:3] (user-data dword 0), globalConstantBuffer slot 7 -> s[4:7] (dword 4).
//
// Not expressible / differences:
//   * s_memrealtime (0xF4) reads the GPU 64-bit real-time counter; only bits 0..22 are used. It is written
//     clockRealtime2x32EXT().x (GL_EXT_shader_realtime_clock -> SPIR-V ShaderClockKHR, Device scope), which needs
//     VK_KHR_shader_clock with shaderDeviceClock enabled on the host device. The PS5 counter frequency is not known
//     here and the host clock frequency/epoch differ, so the time evolution of the dither is not identical.
//   * v_sin_f32 takes its operand in revolutions (sin(2*pi*x)); the compiler pre-multiplied by 1/(2*pi) (0x158).
//     GLSL sin() takes radians, so it is written sin(v10 * 2*pi): one extra rounding and a different sin
//     implementation/precision than the hardware one.
//   * v_fract_f32 is written fract() (x - floor(x)); the hardware clamps the result below 1.0.
//   * dFdxCoarse stands for the DPP pair (quad lane 1 minus quad lane 0 of ClipPos.x), as in solid_fill_p.
//   * v_rcp_f32 (1 ulp) vs GLSL 1.0/x; v_mad/v_mac/v_madak/v_madmk are unfused (locals are `precise`).
//   * v_min_f32 NaN rules differ from GLSL min().
//   * Output is packed to fp16 with round-toward-zero (v_cvt_pkrtz_f16_f32); the render target format decides here.

layout(set = 0, binding = 1, std140) uniform GLOBAL_CB {   // metadata "__GLOBAL_CB__", resource slot 0, 92 bytes
    layout(offset = 0)  vec4  u_FadeA;
    layout(offset = 16) vec4  u_FadeB;
    layout(offset = 32) vec4  u_TargetRect;
    layout(offset = 48) float u_Radius;
    layout(offset = 52) uint  u_Flag;       // metadata base type 9 (32-bit integer class)
    layout(offset = 56) vec2  u_lightTransform;
    layout(offset = 64) vec3  u_lightColor;
    layout(offset = 80) vec3  u_lightAlpha;
};
// globalConstantBuffer: metadata resource slot 7, 8 bytes (see solid_fill_p.frag).
layout(set = 0, binding = 2, std140) uniform globalConstantBuffer {
    layout(offset = 0) float u_GlobalAlpha;
    layout(offset = 4) float u_GlobalIntensity;
};

layout(location = 0) in vec4 in_attr0;   // PS attr0 = input semantic 0: input.Color (COLOR)
layout(location = 1) in vec4 in_attr1;   // PS attr1 = input semantic 1: .xy input.TexCoord (TEXCOORD0), .zw input.ClipPos (TEXCOORD1)
layout(location = 0) out vec4 out_mrt0;  // exp mrt0 (SPI_SHADER_COL_FORMAT = 4: packed fp16, v_cvt_pkrtz_f16_f32 = round toward zero)

void main()
{
    precise float v0, v1, v2, v3, v4, v5, v6, v7, v8, v9, v10, v11;
    float s8, s9, s10, s11, s12, s13, s14, s15;
    uint s18_u, vcc_lo_u;
    float vcc_lo, vcc_hi;
    bool vcc;
    vcc_lo = u_lightTransform.x; vcc_hi = u_lightTransform.y;               // s_buffer_load_dwordx2 vcc, 0x38 // 0x8
    s8 = uintBitsToFloat(0x3F333333u);   // 0.7                              // 0x10
    v5 = in_attr1.x;                                                         // 0x20/0x24
    v3 = vcc_lo * v5;                                                        // 0x2C
    v2 = uintBitsToFloat(0xBF333333u);   // -0.7                             // 0x30
    v4 = in_attr1.y;                                                         // 0x38/0x3C
    v3 = uintBitsToFloat(0xBF333333u) * v3;                                  // 0x44
    v2 = v2 * v4 + uintBitsToFloat(0x3F333333u);                             // v_madak 0x4C
    v3 = v3 * v3;                                                            // 0x54
    v2 = vcc_hi * v2;                                                        // 0x58
    v3 = v2 * v2 + v3;                                                       // v_mac 0x5C
    v2 = sqrt(v3);                                                           // 0x60
    if (s8 > v2) {                                                           // v_cmpx_gt 0x64, s_cbranch_execz 0x68
        v6 = uintBitsToFloat(0xC0BA96C4u);   // -5.830904                        // 0x6C
        vcc_lo = u_lightAlpha.x; vcc_hi = u_lightAlpha.y;                        // s_buffer_load_dwordx2 vcc, 0x50 // 0x74
        v6 = v6 * v2 + uintBitsToFloat(0x40C3EB1Au);   // + 6.122449            // v_madak 0x7C
        v2 = vcc_hi - vcc_lo;                                                    // 0x88
        v3 = v3 * v6;                                                            // 0x90
        v6 = v3 * v2 + vcc_lo;                                                   // 0x94
    } else {                                                                 // s_andn2_b64 exec 0x9C, s_cbranch_execz 0xA0
        v3 = uintBitsToFloat(0xC29425EDu);   // -74.07407                        // 0xA4
        vcc_lo = u_lightAlpha.y; vcc_hi = u_lightAlpha.z;                        // s_buffer_load_dwordx2 vcc, 0x54 // 0xAC
        v6 = v3 * v2 + uintBitsToFloat(0x433CE38Du);   // + 188.88887           // v_madak 0xB4
        v3 = vcc_hi - vcc_lo;                                                    // 0xC0
        v6 = v6 * v2 + uintBitsToFloat(0xC31B8E38u);   // - 155.55554           // v_madak 0xC8
        v2 = v6 * v2 + uintBitsToFloat(0x4226F684u);   // + 41.740738           // v_madak 0xD0
        v6 = v3 * v2 + vcc_lo;                                                   // 0xD8
    }                                                                        // s_mov_b64 exec 0xE0
    v3 = in_attr1.z;                                                         // 0xE4/0xEC
    v2 = in_attr0.w;                                                         // 0xE8/0xF0
    vcc_lo_u = clockRealtime2x32EXT().x;                                     // s_memrealtime vcc 0xF4
    v7 = uintBitsToFloat(0x414FD639u) * v5;   // 12.9898                     // 0xFC
    vcc_lo_u = vcc_lo_u & 0x7FFFFFu;                                         // 0x108
    v5 = 1.0 - v6;                                                           // 0x110
    v9 = float(vcc_lo_u);                                                    // v_cvt_f32_u32 0x114
    s8 = u_lightColor.x; s9 = u_lightColor.y; s10 = u_lightColor.z;         // s_buffer_load_dwordx4 s[8:11], 0x40 // 0x118
    v10 = v4 * uintBitsToFloat(0x429C774Cu) + v7;   // 78.233                // v_madmk 0x120
    v8 = s8 * v6;                                                            // 0x12C
    v7 = s9 * v6;                                                            // 0x130
    v6 = s10 * v6;                                                           // 0x134
    v4 = uintBitsToFloat(0x3B83126Fu);   // 0.004                            // 0x138
    v10 = v9 * uintBitsToFloat(0x37480002u) + v10;   // 1.1920931e-5         // v_madmk 0x140
    v9 = in_attr0.y;                                                         // 0x148/0x14C
    v11 = in_attr0.x;                                                        // 0x150/0x154
    v10 = uintBitsToFloat(0x3E22F983u) * v10;   // 1/(2*pi)                  // 0x158
    v8 = v5 * v11 + v8;                                                      // v_mac 0x160
    v11 = in_attr0.z;                                                        // 0x164/0x168
    v7 = v5 * v9 + v7;                                                       // v_mac 0x16C
    v9 = sin(v10 * 6.28318530717958647692);   // v_sin_f32 (operand in revolutions) // 0x170
    v6 = v5 * v11 + v6;                                                      // v_mac 0x174
    s18_u = u_Flag;                                                          // 0x178
    v5 = uintBitsToFloat(0x472AEE8Cu) * v9;   // 43758.547                   // 0x188
    v5 = fract(v5);                                                          // 0x190
    v9 = v4 * v5 + uintBitsToFloat(0xBB03126Fu);   // - 0.002                // v_madak 0x194
    v4 = clamp(v8 + v9, 0.0, 1.0);                                           // 0x19C
    v5 = clamp(v7 + v9, 0.0, 1.0);                                           // 0x1A4
    v6 = clamp(v6 + v9, 0.0, 1.0);                                           // 0x1AC
    if ((s18_u & 6u) != 0u) {                                                // s_and_b32 0x184, s_cbranch_scc0 0x1B4
        v7 = 1.0;                                                                // 0x1B8
        v0 = in_attr1.w;                                                         // 0x1C0/0x1C4
        if ((s18_u & 4u) != 0u) {                                                // s_bitcmp1_b32 0x1BC, s_cbranch_scc0 0x1C8
            vcc_lo = uintBitsToFloat(0x3F100000u);   // 0.5625                       // 0x1D4
            s8 = u_TargetRect.x; s9 = u_TargetRect.y; s10 = u_TargetRect.z; s11 = u_TargetRect.w; // 0x1DC
            v8 = vcc_lo * v0 + -s9;                                                  // 0x1E8
            v9 = v3 - s8;                                                            // 0x1F0
            v1 = dFdxCoarse(v3);   // v_mov_b32_dpp quad_perm[0,0,0,0] (0x1CC) + v_sub_f32_dpp quad_perm[1,1,1,1] // 0x1F4
            v8 = -s11 + abs(v8);                                                     // 0x1FC
            v9 = -s10 + abs(v9);                                                     // 0x204
            v10 = min(v9, v8);                                                       // 0x20C
            if (0.0 > v10) {                                                         // v_cmpx_gt 0x210
                vcc = 0.0 > v9;                                                          // 0x214
                v8 = vcc ? v8 : v9;                                                      // 0x218
            } else {                                                                 // s_andn2_b64 exec 0x21C
                v9 = v9 * v9;                                                            // 0x220
                v9 = v8 * v8 + v9;                                                       // 0x224
                v8 = sqrt(v9);                                                           // 0x228
            }                                                                        // 0x22C
            vcc_lo = u_Radius;                                                       // 0x230
            v9 = vcc_lo - v1;                                                        // 0x23C
            if (v8 > v9) {                                                           // v_cmpx_gt 0x240
                v7 = vcc_lo - v8;                                                        // 0x244
                v1 = 1.0 / v1;                                                           // 0x248
                v7 = clamp(v1 * v7 + 0.5, 0.0, 1.0);                                     // 0x24C
            }                                                                        // 0x254
        }
        if ((s18_u & 2u) != 0u) {                                                // s_bitcmp1_b32 0x258, s_cbranch_scc0 0x25C
            s8 = u_FadeA.x; s9 = u_FadeA.y; s10 = u_FadeA.z; s11 = u_FadeA.w; s12 = u_FadeB.x; s13 = u_FadeB.y; s14 = u_FadeB.z; s15 = u_FadeB.w; // 0x260
            v1 = clamp(s8 * v3 + s12, 0.0, 1.0);                                     // 0x26C
            v8 = clamp(v3 * s9 + s13, 0.0, 1.0);                                     // 0x274
            v3 = clamp(v0 * s10 + s14, 0.0, 1.0);                                    // 0x27C
            v0 = clamp(v0 * s11 + s15, 0.0, 1.0);                                    // 0x284
            v1 = v1 * v8;                                                            // 0x28C
            v1 = v1 * v3;                                                            // 0x290
            v1 = v1 * v0;                                                            // 0x294
            v0 = v1 * v1;                                                            // 0x298
            v1 = -2.0 * v1 + uintBitsToFloat(0x40400000u);   // 3.0                  // v_madak 0x29C
            v0 = v0 * v7;                                                            // 0x2A4
            v7 = v0 * v1;                                                            // 0x2A8
        }
        v2 = v7 * v2;                                                            // 0x2AC
    }
    vcc_hi = u_GlobalIntensity;                                              // 0x2B0
    v1 = vcc_hi;                                                             // 0x2BC
    if ((s18_u & 1u) != 0u) {                                                // s_bitcmp1_b32 0x2C0, s_cbranch_scc0 0x2C4
        vcc_lo = u_GlobalAlpha;                                                  // 0x2C8
        v1 = vcc_lo * vcc_hi;                                                    // 0x2D4
    }
    v0 = v1 * v4;                                                            // 0x2DC
    vcc_lo = u_GlobalAlpha;                                                  // 0x2E0
    v3 = v1 * v5;                                                            // 0x2E8
    v1 = v1 * v6;                                                            // 0x2EC
    v2 = vcc_lo * v2;                                                        // 0x2F4
    out_mrt0 = vec4(v0, v3, v1, v2);   // v_cvt_pkrtz_f16_f32 v0,v0,v3 / v1,v1,v2 ; exp mrt0 compr (0x2F8..0x304)
}
