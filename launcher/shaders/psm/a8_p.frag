#version 450
// Transcription of libScePsm embedded shader #19 "a8_p" (work/shaders/19_a8_p.ags.bin),
// AGC binary type 1 (pixel), .shader_text 0x340 bytes (code up to s_endpgm at 0x1E4). EmbeddedShader.A8Texture = (basic_vv, a8_p).
// Every statement maps to the RDNA instruction at the byte offset in its trailing comment.
//
// Math: c = (attr0.rgb, attr0.a * texture(s_Texture, attr1.xy).a)   (image_sample dmask 0x8 = alpha only)
//   Common u_Flag tail (identical math in solid_fill_p / a8_p / rgba_p / mask_p; p = ClipPos = attr1.zw):
//   if (F & 6):
//     m = 1
//     if (F & 4): rounded-rect coverage against u_TargetRect / u_Radius, y scaled by 0.5625:
//        qx = |p.x - R.x| - R.z ; qy = |0.5625*p.y - R.y| - R.w
//        d  = (min(qx,qy) < 0) ? (qx < 0 ? qy : qx) : sqrt(qx*qx + qy*qy)
//        dx = coarse ddx(p.x);  if (d > u_Radius - dx) m = saturate((u_Radius - d) * (1/dx) + 0.5)
//     if (F & 2): f = saturate(p.x*FadeA.x+FadeB.x) * saturate(p.x*FadeA.y+FadeB.y)
//                   * saturate(p.y*FadeA.z+FadeB.z) * saturate(p.y*FadeA.w+FadeB.w);  m = f*f*m*(3 - 2f)
//     c.a *= m;  if (F & 1) c.rgb *= m
//   k = (F & 1) ? u_GlobalAlpha * u_GlobalIntensity : u_GlobalIntensity
//   out = (c.rgb * k, c.a * u_GlobalAlpha)
//
// Resources: s_Texture t0 -> T# s[0:7] (user-data dword 0), Sampler0 s0 -> S# s[8:11] (dword 8),
//   __GLOBAL_CB__ slot 0 -> s[12:15] (dword 12), globalConstantBuffer slot 7 -> s[16:19] (dword 16).
//
// Not expressible / differences:
//   * dFdxCoarse stands for the DPP pair (quad lane 1 minus quad lane 0 of ClipPos.x); that is what AMD
//     hardware computes, other GPUs' coarse derivative may use a different lane pair.
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

// s_Texture (metadata texture slot 0, Texture2D<float4>) combined with Sampler0 (sampler slot 0).
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
    v2 = in_attr1.x;                                                         // 0x28
    v3 = in_attr1.y;                                                         // 0x2C
    v4 = texture(s_Texture, vec2(v2, v3)).w;                                 // 0x30
    v6 = in_attr0.w;                                                         // 0x3C
    v5 = in_attr0.x;                                                         // 0x50
    v3 = in_attr0.y;                                                         // 0x54
    v2 = in_attr0.z;                                                         // 0x58
    v7 = in_attr1.z;                                                         // 0x5C
    v4 = v4 * v6;                                                            // 0x64
    if ((s22_u & 6u) != 0u) {
        v6 = 1.0;                                                                // 0x6C
        v0 = in_attr1.w;                                                         // 0x78
        if ((s22_u & 4u) != 0u) {
            vcc_lo = uintBitsToFloat(0x3F100000u);   // 0.5625                       // 0x88
            s0 = u_TargetRect.x; s1 = u_TargetRect.y; s2 = u_TargetRect.z; s3 = u_TargetRect.w; // 0x90
            v8 = vcc_lo * v0 + -s1;                                                  // 0x9C
            v9 = v7 - s0;                                                            // 0xA4
            v1 = dFdxCoarse(v7);   // v_mov_b32_dpp quad_perm[0,0,0,0] + v_sub_f32_dpp quad_perm[1,1,1,1] // 0xA8
            v8 = -s3 + abs(v8);                                                      // 0xB4
            v9 = -s2 + abs(v9);                                                      // 0xBC
            v10 = min(v9, v8);                                                       // 0xC4
            if (0.0 > v10) {                                                         // 0xC8
                vcc = 0.0 > v9;                                                          // 0xCC
                v8 = vcc ? v8 : v9;                                                      // 0xD0
            } else {                                                                 // 0xD4
                v9 = v9 * v9;                                                            // 0xD8
                v9 = v8 * v8 + v9;                                                       // 0xDC
                v8 = sqrt(v9);                                                           // 0xE0
            }                                                                        // 0xE4
            vcc_lo = u_Radius;                                                       // 0xE8
            v9 = vcc_lo - v1;                                                        // 0xF4
            if (v8 > v9) {                                                           // 0xF8
                v6 = vcc_lo - v8;                                                        // 0xFC
                v1 = 1.0 / v1;                                                           // 0x100
                v6 = clamp(v1 * v6 + 0.5, 0.0, 1.0);                                     // 0x104
            }                                                                        // 0x10C
        }
        if ((s22_u & 2u) != 0u) {
            s0 = u_FadeA.x; s1 = u_FadeA.y; s2 = u_FadeA.z; s3 = u_FadeA.w; s4 = u_FadeB.x; s5 = u_FadeB.y; s6 = u_FadeB.z; s7 = u_FadeB.w; // 0x118
            v1 = clamp(s0 * v7 + s4, 0.0, 1.0);                                      // 0x124
            v8 = clamp(v7 * s1 + s5, 0.0, 1.0);                                      // 0x12C
            v7 = clamp(v0 * s2 + s6, 0.0, 1.0);                                      // 0x134
            v0 = clamp(v0 * s3 + s7, 0.0, 1.0);                                      // 0x13C
            v1 = v1 * v8;                                                            // 0x144
            v1 = v1 * v7;                                                            // 0x148
            v1 = v1 * v0;                                                            // 0x14C
            v0 = v1 * v1;                                                            // 0x150
            v1 = -2.0 * v1 + 3.0;                                                    // 0x154
            v0 = v0 * v6;                                                            // 0x15C
            v6 = v0 * v1;                                                            // 0x160
        }
        v0 = v6 * v5;                                                            // 0x164
        v1 = v6 * v3;                                                            // 0x168
        v7 = v6 * v2;                                                            // 0x16C
        v4 = v6 * v4;                                                            // 0x174
        vcc = (s22_u & 1u) != 0u;                                                // 0x178
        v5 = vcc ? v0 : v5;                                                      // 0x17C
        v3 = vcc ? v1 : v3;                                                      // 0x180
        v2 = vcc ? v7 : v2;                                                      // 0x184
    }
    vcc_hi = u_GlobalIntensity;                                              // 0x188
    v1 = vcc_hi;                                                             // 0x194
    if ((s22_u & 1u) != 0u) {
        vcc_lo = u_GlobalAlpha;                                                  // 0x1A0
        v1 = vcc_lo * vcc_hi;                                                    // 0x1AC
    }
    v0 = v1 * v5;                                                            // 0x1B4
    vcc_lo = u_GlobalAlpha;                                                  // 0x1B8
    v3 = v1 * v3;                                                            // 0x1C0
    v1 = v1 * v2;                                                            // 0x1C4
    v2 = vcc_lo * v4;                                                        // 0x1CC
    out_mrt0 = vec4(v0, v3, v1, v2);   // v_cvt_pkrtz_f16_f32 v0,v0,v3 / v1,v1,v2 ; exp mrt0 compr (0x1D0..0x1DC)
}
