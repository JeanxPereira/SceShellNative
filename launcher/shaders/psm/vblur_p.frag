#version 450
// Transcription of libScePsm embedded shader #67 "vblur_p" (work/shaders/67_vblur_p.ags.bin),
// AGC binary type 1 (pixel), code up to s_endpgm at 0x438. EmbeddedShader.BlurV = (blur_vv, vblur_p).
// Every statement maps to the RDNA instruction at the byte offset in its trailing comment; the four channel
// operations of one tap are written on one line (they are four independent instructions).
//
// Math (uv = TexCoord = attr.xy, p = ClipPos = attr.zw, b = BlurAmount, T(k) = texture(s_Texture, (uv.x, uv.y + k*b))):
//   c = the 13-tap sum of hblur_p.frag along y (same weights, same accumulation order)
//   then the common u_Flag tail of solid_fill_p.frag on c (bit 2 rounded rect, bit 1 fade, bit 0 premultiply +
//   u_GlobalAlpha * u_GlobalIntensity), out = (c.rgb * k, c.a * u_GlobalAlpha).
//
// Resources: s_Texture t0 -> s[0:7], Sampler0 -> s[8:11], __GLOBAL_CB__ slot 0 -> s[12:15] (60 bytes),
//   globalConstantBuffer slot 7 -> s[16:19].
// Interpolants: ONE PS input, semantic 1 (SPI_PS_IN_CONTROL = 1): the ISA's attr0 is the vertex shader's param1
//   (blur_vv: TexCoord.xy / ClipPos.zw), declared at location 1.
//
// Not expressible / differences:
//   * dFdxCoarse stands for the DPP pair (quad lane 1 minus quad lane 0 of ClipPos.x), as in solid_fill_p.
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
    layout(offset = 4) float u_GlobalIntensity;
};
// s_Texture (t0) with Sampler0 (s0).
layout(set = 0, binding = 3) uniform sampler2D s_Texture;

layout(location = 1) in vec4 in_attr0;   // ISA attr0 = PS input semantic 1 (VS param1): .xy TexCoord, .zw ClipPos
layout(location = 0) out vec4 out_mrt0;  // exp mrt0 (SPI_SHADER_COL_FORMAT = 4: packed fp16, v_cvt_pkrtz_f16_f32 = round toward zero)

#define W0 uintBitsToFloat(0x3D489375u)
#define W1 uintBitsToFloat(0x3D87F1C5u)
#define W2 uintBitsToFloat(0x3DAC3B8Eu)
#define W3 uintBitsToFloat(0x3DCBF1B3u)
#define W4 uintBitsToFloat(0x3DE1B57Au)
#define W5 uintBitsToFloat(0x3DE977DFu)
#define W6 uintBitsToFloat(0x3D0A4BABu)

void main()
{
    precise float v0, v1, v2, v3, v4, v5, v6, v7, v8, v9, v10, v11, v12, v13, v14, v15, v16, v17, v18, v19;
    precise float v20, v21, v22, v23, v24, v25, v26, v27, v28, v29, v30;
    float s0f, s1f, s2f, s3f, s4f, s5f, s6f, s7f, s23;
    uint s22_u;
    float vcc_lo, vcc_hi;
    bool vcc;
    vec4 t;
    s22_u = u_Flag; s23 = BlurAmount;                                        // s_buffer_load_dwordx2 0x4
    v25 = s23;                                                               // 0x20
    v26 = in_attr0.y;                                                        // 0x24/0x28
    v22 = in_attr0.x;                                                        // 0x2C/0x30
    v23 = v25 * uintBitsToFloat(0xC0A00000u) + v26;   // -5                  // v_madmk 0x34
    v24 = in_attr0.z;                                                        // 0x3C/0x40
    t = texture(s_Texture, vec2(v22, v23)); v18 = t.x; v19 = t.y; v20 = t.z; v21 = t.w;  // 0x44
    t = texture(s_Texture, vec2(v22, v26)); v2 = t.x; v3 = t.y; v4 = t.z; v5 = t.w;      // 0x4C
    v23 = -4.0 * s23 + v26;                                                  // 0x58
    t = texture(s_Texture, vec2(v22, v23)); v6 = t.x; v7 = t.y; v8 = t.z; v9 = t.w;      // 0x60
    v23 = v25 * uintBitsToFloat(0xC0400000u) + v26;   // -3                  // v_madmk 0x68
    t = texture(s_Texture, vec2(v22, v23)); v10 = t.x; v11 = t.y; v12 = t.z; v13 = t.w;  // 0x70
    v23 = -2.0 * s23 + v26;                                                  // 0x78
    t = texture(s_Texture, vec2(v22, v23)); v14 = t.x; v15 = t.y; v16 = t.z; v17 = t.w;  // 0x80
    v23 = v26 - s23;                                                         // 0x88
    v27 = W0 * v18; v28 = W0 * v19; v29 = W0 * v20; v30 = W0 * v21;         // 0x90..0xA8
    t = texture(s_Texture, vec2(v22, v23)); v18 = t.x; v19 = t.y; v20 = t.z; v21 = t.w;  // 0xB0
    v23 = s23 + v26;                                                         // 0xB8
    v27 = v6 * W1 + v27; v28 = v7 * W1 + v28; v29 = v8 * W1 + v29; v30 = v9 * W1 + v30; // 0xC0..0xD8
    t = texture(s_Texture, vec2(v22, v23)); v6 = t.x; v7 = t.y; v8 = t.z; v9 = t.w;      // 0xE0
    v23 = 2.0 * s23 + v26;                                                   // 0xE8
    v27 = v10 * W2 + v27; v28 = v11 * W2 + v28; v29 = v12 * W2 + v29; v30 = v13 * W2 + v30; // 0xF4..0x10C
    t = texture(s_Texture, vec2(v22, v23)); v10 = t.x; v11 = t.y; v12 = t.z; v13 = t.w;  // 0x114
    v23 = v25 * uintBitsToFloat(0x40400000u) + v26;   // 3                   // v_madmk 0x11C
    v27 = v14 * W3 + v27; v28 = v15 * W3 + v28; v29 = v16 * W3 + v29; v30 = v17 * W3 + v30; // 0x128..0x140
    t = texture(s_Texture, vec2(v22, v23)); v14 = t.x; v15 = t.y; v16 = t.z; v17 = t.w;  // 0x148
    v23 = 4.0 * s23 + v26;                                                   // 0x150
    v27 = v18 * W4 + v27; v28 = v19 * W4 + v28; v29 = v20 * W4 + v29; v30 = v21 * W4 + v30; // 0x15C..0x174
    t = texture(s_Texture, vec2(v22, v23)); v18 = t.x; v19 = t.y; v20 = t.z; v21 = t.w;  // 0x17C
    v2 = v2 * W5 + v27;                                                      // 0x184
    v23 = v25 * uintBitsToFloat(0x40A00000u) + v26;   // 5                   // v_madmk 0x18C
    v3 = v3 * W5 + v28; v4 = v4 * W5 + v29; v5 = v5 * W5 + v30;              // 0x194..0x1A4
    v6 = v6 * W4 + v2; v7 = v7 * W4 + v3; v8 = v8 * W4 + v4; v9 = v9 * W4 + v5;         // 0x1B0..0x1C8
    t = texture(s_Texture, vec2(v22, v23)); v2 = t.x; v3 = t.y; v4 = t.z; v5 = t.w;      // 0x1D0
    v23 = v25 * uintBitsToFloat(0x40C00000u) + v26;   // 6                   // v_madmk 0x1D8
    v10 = v10 * W3 + v6; v11 = v11 * W3 + v7; v12 = v12 * W3 + v8; v13 = v13 * W3 + v9; // 0x1E4..0x1FC
    t = texture(s_Texture, vec2(v22, v23)); v6 = t.x; v7 = t.y; v8 = t.z; v9 = t.w;      // 0x204
    v23 = v25 * uintBitsToFloat(0xC0C00000u) + v26;   // -6                  // v_madmk 0x20C
    v14 = v14 * W2 + v10; v15 = v15 * W2 + v11; v16 = v16 * W2 + v12; v17 = v17 * W2 + v13; // 0x218..0x230
    t = texture(s_Texture, vec2(v22, v23)); v10 = t.x; v11 = t.y; v12 = t.z; v13 = t.w;  // 0x238
    v14 = v18 * W1 + v14; v15 = v19 * W1 + v15; v16 = v20 * W1 + v16; v17 = v21 * W1 + v17; // 0x244..0x25C
    v2 = v2 * W0 + v14; v14 = v3 * W0 + v15; v3 = v4 * W0 + v16; v4 = v5 * W0 + v17;    // 0x268..0x280
    v6 = v10 + v6; v5 = v11 + v7; v7 = v12 + v8; v8 = v13 + v9;              // 0x28C..0x298
    v2 = v6 * W6 + v2; v5 = v5 * W6 + v14; v3 = v7 * W6 + v3; v4 = v8 * W6 + v4;       // 0x29C..0x2B4
    if ((s22_u & 6u) != 0u) {                                                // s_and_b32 0x1C, s_cbranch_scc0 0x2BC
        v6 = 1.0;                                                                // 0x2C0
        v0 = in_attr0.w;                                                         // 0x2C8/0x2CC
        if ((s22_u & 4u) != 0u) {                                                // s_bitcmp1_b32 0x2C4, s_cbranch_scc0 0x2D0
            vcc_lo = uintBitsToFloat(0x3F100000u);   // 0.5625                       // 0x2DC
            s0f = u_TargetRect.x; s1f = u_TargetRect.y; s2f = u_TargetRect.z; s3f = u_TargetRect.w; // 0x2E4
            v7 = vcc_lo * v0 + -s1f;                                                 // 0x2F0
            v8 = v24 - s0f;                                                          // 0x2F8
            v1 = dFdxCoarse(v24);  // v_mov_b32_dpp quad_perm[0,0,0,0] (0x2D4) + v_sub_f32_dpp quad_perm[1,1,1,1] // 0x2FC
            v7 = -s3f + abs(v7);                                                     // 0x308
            v8 = -s2f + abs(v8);                                                     // 0x310
            v9 = min(v8, v7);                                                        // 0x318
            if (0.0 > v9) {                                                          // v_cmpx_gt 0x31C
                vcc = 0.0 > v8;                                                          // 0x320
                v7 = vcc ? v7 : v8;                                                      // 0x324
            } else {                                                                 // s_andn2_b64 exec 0x328
                v8 = v8 * v8;                                                            // 0x32C
                v8 = v7 * v7 + v8;                                                       // 0x330
                v7 = sqrt(v8);                                                           // 0x334
            }                                                                        // 0x338
            vcc_lo = u_Radius;                                                       // 0x33C
            v8 = vcc_lo - v1;                                                        // 0x348
            if (v7 > v8) {                                                           // v_cmpx_gt 0x34C
                v6 = vcc_lo - v7;                                                        // 0x350
                v1 = 1.0 / v1;                                                           // 0x354
                v6 = clamp(v1 * v6 + 0.5, 0.0, 1.0);                                     // 0x358
            }                                                                        // 0x360
        }
        if ((s22_u & 2u) != 0u) {                                                // s_bitcmp1_b32 0x364, s_cbranch_scc0 0x368
            s0f = u_FadeA.x; s1f = u_FadeA.y; s2f = u_FadeA.z; s3f = u_FadeA.w; s4f = u_FadeB.x; s5f = u_FadeB.y; s6f = u_FadeB.z; s7f = u_FadeB.w; // 0x36C
            v1 = clamp(s0f * v24 + s4f, 0.0, 1.0);                                   // 0x378
            v8 = clamp(v24 * s1f + s5f, 0.0, 1.0);                                   // 0x380
            v7 = clamp(v0 * s2f + s6f, 0.0, 1.0);                                    // 0x388
            v0 = clamp(v0 * s3f + s7f, 0.0, 1.0);                                    // 0x390
            v1 = v1 * v8;                                                            // 0x398
            v1 = v1 * v7;                                                            // 0x39C
            v1 = v1 * v0;                                                            // 0x3A0
            v0 = v1 * v1;                                                            // 0x3A4
            v1 = -2.0 * v1 + uintBitsToFloat(0x40400000u);   // 3.0                  // v_madak 0x3A8
            v0 = v0 * v6;                                                            // 0x3B0
            v6 = v0 * v1;                                                            // 0x3B4
        }
        v0 = v6 * v2;                                                            // 0x3B8
        v1 = v6 * v5;                                                            // 0x3BC
        v7 = v6 * v3;                                                            // 0x3C0
        v4 = v6 * v4;                                                            // 0x3C8
        vcc = (s22_u & 1u) != 0u;                                                // s_bitcmp1_b32 0x3C4, s_cselect_b64 0x3CC
        v2 = vcc ? v0 : v2;                                                      // 0x3D0
        v5 = vcc ? v1 : v5;                                                      // 0x3D4
        v3 = vcc ? v7 : v3;                                                      // 0x3D8
    }
    vcc_hi = u_GlobalIntensity;                                              // 0x3DC
    v1 = vcc_hi;                                                             // 0x3E8
    if ((s22_u & 1u) != 0u) {                                                // s_bitcmp1_b32 0x3EC, s_cbranch_scc0 0x3F0
        vcc_lo = u_GlobalAlpha;                                                  // 0x3F4
        v1 = vcc_lo * vcc_hi;                                                    // 0x400
    }
    v0 = v1 * v2;                                                            // 0x408
    vcc_lo = u_GlobalAlpha;                                                  // 0x40C
    v2 = v1 * v5;                                                            // 0x414
    v1 = v1 * v3;                                                            // 0x418
    v3 = vcc_lo * v4;                                                        // 0x420
    out_mrt0 = vec4(v0, v2, v1, v3);   // v_cvt_pkrtz_f16_f32 v0,v0,v2 / v1,v1,v3 ; exp mrt0 compr (0x424..0x430)
}
