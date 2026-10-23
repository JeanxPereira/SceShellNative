#version 450
// Transcription of libScePsm embedded shader #69 "vglow_r8_p" (work/shaders/69_vglow_r8_p.ags.bin),
// AGC binary type 1 (pixel), code up to s_endpgm at 0x278. EmbeddedShader.GlowR8V = (glow_vv, vglow_r8_p).
// Every statement maps to the RDNA instruction at the byte offset in its trailing comment.
//
// Math (uv = TexCoord = attr.xy, p = ClipPos = attr.zw, F = u_Flag, b = BlurAmount):
//   s = the 13-tap alpha sum of hglow_p.frag along y (same weights, same accumulation order)
//   m = 1;  F & 4: rounded-rect coverage (as solid_fill_p.frag);  F & 2: fade ramp (as solid_fill_p.frag)
//     (the two bits are tested independently, without the F & 6 guard)
//   out = ((u_GlobalAlpha * m) * s, 1, 1, 1)   (G from v_cvt_pkrtz_f16_f32 v0, v0, 1.0; B, A = constant 0x3C003C00)
//   Vertex colour is not read; u_GlobalIntensity is not read (metadata: unused).
//
// Resources: s_Texture t0 -> s[0:7], Sampler0 -> s[8:11], __GLOBAL_CB__ slot 0 -> s[12:15] (60 bytes),
//   globalConstantBuffer slot 7 -> s[16:19].
// Interpolants: ONE PS input, semantic 1 (SPI_PS_IN_CONTROL = 1): the ISA's attr0 is the vertex shader's param1
//   (glow_vv: TexCoord.xy / ClipPos.zw), declared at location 1.
//
// Not expressible / differences:
//   * image_sample dmask 0x8 returns the 4th component AFTER the T# dst_sel swizzle. For the R8 textures this
//     program is meant for, the value depends on the swizzle libScePsm puts in the T#; texture(...).a here returns
//     whatever the host image view's component mapping gives for A (identity R8 view: 1.0). TODO(verify) the
//     PS5 T# swizzle for PixelFormat R8 and mirror it in the host's VkImageView components.
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
    layout(offset = 4) float u_GlobalIntensity;   // not read by this shader
};
// s_Texture (t0) with Sampler0 (s0).
layout(set = 0, binding = 3) uniform sampler2D s_Texture;

layout(location = 1) in vec4 in_attr0;   // ISA attr0 = PS input semantic 1 (VS param1): .xy TexCoord, .zw ClipPos
layout(location = 0) out vec4 out_mrt0;  // exp mrt0 (SPI_SHADER_COL_FORMAT = 4: packed fp16, v_cvt_pkrtz_f16_f32 = round toward zero)

void main()
{
    precise float v0, v1, v2, v3, v4, v5, v6, v7, v8, v9, v10, v11, v12, v13, v14, v15, v16, v17, v18;
    float s0f, s1f, s2f, s3f, s4f, s5f, s6f, s7f, s23;
    uint s22_u;
    float vcc_lo;
    bool vcc;
    s22_u = u_Flag; s23 = BlurAmount;                                        // s_buffer_load_dwordx2 0x8
    v12 = in_attr0.y;                                                        // 0x20/0x24
    v3 = s23 + v12;                                                          // 0x28
    v8 = 2.0 * s23 + s23;                                                    // 0x2C
    v2 = in_attr0.x;                                                         // 0x34/0x38
    v5 = in_attr0.z;                                                         // 0x3C/0x64
    v13 = texture(s_Texture, vec2(v2, v3)).a;                                // 0x40
    v6 = texture(s_Texture, vec2(v2, v12)).a;                                // 0x48
    v3 = v12 - s23;                                                          // 0x54
    v7 = s23 + v8;                                                           // 0x58
    v4 = in_attr0.w;                                                         // 0x5C/0x7C
    v0 = 1.0;                                                                // 0x60
    v15 = texture(s_Texture, vec2(v2, v3)).a;                                // 0x68
    v3 = 2.0 * s23 + v12;                                                    // 0x70
    v9 = s23 + v7;                                                           // 0x78
    v16 = texture(s_Texture, vec2(v2, v3)).a;                                // 0x80
    v3 = 2.0 * -s23 + v12;                                                   // 0x88
    v18 = s23 + v9;                                                          // 0x90
    v17 = texture(s_Texture, vec2(v2, v3)).a;                                // 0x94
    v3 = v12 + v8;                                                           // 0x9C
    v10 = texture(s_Texture, vec2(v2, v3)).a;                                // 0xA0
    v3 = v12 - v8;                                                           // 0xA8
    v14 = texture(s_Texture, vec2(v2, v3)).a;                                // 0xAC
    v3 = v12 + v7;                                                           // 0xB4
    v8 = texture(s_Texture, vec2(v2, v3)).a;                                 // 0xB8
    v3 = v12 - v7;                                                           // 0xC0
    v11 = texture(s_Texture, vec2(v2, v3)).a;                                // 0xC4
    v3 = v12 + v9;                                                           // 0xCC
    v1 = texture(s_Texture, vec2(v2, v3)).a;                                 // 0xD0
    v3 = v12 - v9;                                                           // 0xD8
    v9 = texture(s_Texture, vec2(v2, v3)).a;                                 // 0xDC
    v3 = v12 + v18;                                                          // 0xE4
    v7 = texture(s_Texture, vec2(v2, v3)).a;                                 // 0xE8
    v3 = v12 - v18;                                                          // 0xF0
    v12 = texture(s_Texture, vec2(v2, v3)).a;                                // 0xF4
    v2 = v15 + v13;                                                          // 0x100
    v3 = v17 + v16;                                                          // 0x108
    v2 = uintBitsToFloat(0x3E18311Du) * v2;                                  // 0x10C
    v2 = v3 * uintBitsToFloat(0x3DEF6FCAu) + v2;                             // v_madmk 0x114
    v3 = v14 + v10;                                                          // 0x120
    v1 = v9 + v1;                                                            // 0x128
    v2 = v3 * uintBitsToFloat(0x3DA07FCDu) + v2;                             // v_madmk 0x12C
    v3 = v11 + v8;                                                           // 0x134
    v2 = v3 * uintBitsToFloat(0x3D375B71u) + v2;                             // v_madmk 0x138
    v3 = v12 + v7;                                                           // 0x144
    v1 = v1 * uintBitsToFloat(0x3CB27FC5u) + v2;                             // v_madmk 0x148
    v1 = v3 * uintBitsToFloat(0x3C1413A8u) + v1;                             // v_madmk 0x150
    v1 = v6 * uintBitsToFloat(0x3E24DE11u) + v1;                             // v_madmk 0x158
    if ((s22_u & 4u) != 0u) {                                                // s_bitcmp1_b32 0x1C, s_cbranch_scc0 0x160
        vcc_lo = uintBitsToFloat(0x3F100000u);   // 0.5625                       // 0x16C
        s0f = u_TargetRect.x; s1f = u_TargetRect.y; s2f = u_TargetRect.z; s3f = u_TargetRect.w; // 0x174
        v3 = vcc_lo * v4 + -s1f;                                                 // 0x180
        v6 = v5 - s0f;                                                           // 0x188
        v2 = dFdxCoarse(v5);   // v_mov_b32_dpp quad_perm[0,0,0,0] (0x164) + v_sub_f32_dpp quad_perm[1,1,1,1] // 0x18C
        v3 = -s3f + abs(v3);                                                     // 0x198
        v6 = -s2f + abs(v6);                                                     // 0x1A0
        v7 = min(v6, v3);                                                        // 0x1A8
        if (0.0 > v7) {                                                          // v_cmpx_gt 0x1AC
            vcc = 0.0 > v6;                                                          // 0x1B0
            v3 = vcc ? v3 : v6;                                                      // 0x1B4
        } else {                                                                 // s_andn2_b64 exec 0x1B8
            v6 = v6 * v6;                                                            // 0x1BC
            v6 = v3 * v3 + v6;                                                       // 0x1C0
            v3 = sqrt(v6);                                                           // 0x1C4
        }                                                                        // 0x1C8
        vcc_lo = u_Radius;                                                       // 0x1CC
        v6 = vcc_lo - v2;                                                        // 0x1D8
        if (v3 > v6) {                                                           // v_cmpx_gt 0x1DC
            v0 = vcc_lo - v3;                                                        // 0x1E0
            v2 = 1.0 / v2;                                                           // 0x1E4
            v0 = clamp(v2 * v0 + 0.5, 0.0, 1.0);                                     // 0x1E8
        }                                                                        // 0x1F0
    }
    if ((s22_u & 2u) != 0u) {                                                // s_bitcmp1_b32 0x1F4, s_cbranch_scc0 0x1F8
        s0f = u_FadeA.x; s1f = u_FadeA.y; s2f = u_FadeA.z; s3f = u_FadeA.w; s4f = u_FadeB.x; s5f = u_FadeB.y; s6f = u_FadeB.z; s7f = u_FadeB.w; // 0x1FC
        v2 = clamp(s0f * v5 + s4f, 0.0, 1.0);                                    // 0x208
        v5 = clamp(v5 * s1f + s5f, 0.0, 1.0);                                    // 0x210
        v3 = clamp(v4 * s2f + s6f, 0.0, 1.0);                                    // 0x218
        v4 = clamp(v4 * s3f + s7f, 0.0, 1.0);                                    // 0x220
        v2 = v2 * v5;                                                            // 0x228
        v2 = v2 * v3;                                                            // 0x22C
        v3 = v2 * v4;                                                            // 0x230
        v2 = v3 * v3;                                                            // 0x234
        v3 = -2.0 * v3 + uintBitsToFloat(0x40400000u);   // 3.0                  // v_madak 0x238
        v0 = v2 * v0;                                                            // 0x240
        v0 = v0 * v3;                                                            // 0x244
    }
    vcc_lo = u_GlobalAlpha;                                                  // 0x248
    v0 = vcc_lo * v0;                                                        // 0x254
    v0 = v0 * v1;                                                            // 0x258
    out_mrt0 = vec4(v0, 1.0, 1.0, 1.0);   // v_cvt_pkrtz_f16_f32_e64 v0, v0, 1.0 (0x25C); v1 = 0x3C003C00 (0x268); exp mrt0 compr 0x270
}
