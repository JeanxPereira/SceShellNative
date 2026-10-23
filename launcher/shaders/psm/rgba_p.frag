#version 450
// Transcription of libScePsm embedded shader #43 "rgba_p" (work/shaders/43_rgba_p.ags.bin),
// AGC binary type 1 (pixel), .shader_text 0x7D0 bytes (code up to s_endpgm at 0x650). EmbeddedShader.Texture = (basic_vv, rgba_p).
// Every statement maps to the RDNA instruction at the byte offset in its trailing comment.
//
// Math:
//   uv = attr1.xy; t = texture(s_Texture, uv); lod = textureQueryLod(s_Texture, uv).y
//   if (lod > 0.2):   2x2 box pre-filter on the even texel grid, blended in by saturate(lod):
//       x0 = 2*floor(W*u/2 - 0.5) + 1, y0 = 2*floor(H*v/2 - 0.5) + 1, fx = (W*u - x0)/2, fy = (H*v - y0)/2
//       A,B,C,D = bilinear taps at (x0/W, y0/H), ((x0+2)/W, y0/H), (x0/W, (y0+2)/H), ((x0+2)/W, (y0+2)/H)
//       t = t + saturate(lod) * (lerp(lerp(A,B,fx), lerp(C,D,fx), fy) - t)
//     if (lod > 1.2): 4x4 box (16 bilinear taps on a 2-texel grid, summed in 2x2 groups of 4) blended in by
//       saturate(lod - 1):  t = t + saturate(lod - 1) * (0.25 * bilerp(groups, fx2, fy2) - t),
//       x1 = 4*floor(W*u/4 - 0.5) + 2, fx2 = (W*u - x1)/4 (likewise y)
//   c = attr0 * t
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
//   * image_get_lod dmask 0x2 is written as textureQueryLod(...).y (2nd component = LOD before clamping, as
//     the AMD LLVM backend lowers textureQueryLod to image_get_lod dmask 0x3). TODO(verify): confirm on PS5
//     hardware that component 1 of IMAGE_GET_LOD is the unclamped LOD.
//   * The texture size comes from the T# (WIDTH/HEIGHT fields + 1, i.e. mip 0 of the resource); written as
//     textureSize(tex, 0), which is identical only while the view's base mip level is 0.
//   * The filter taps run under a narrowed EXEC mask (v_cmpx) with implicit-LOD image_sample: lanes of a quad
//     that failed the LOD test keep stale coordinates in hardware, so their derivatives are not reproducible;
//     GLSL texture() inside the same non-uniform `if` is likewise undefined there.

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
    precise float v0, v1, v2, v3, v4, v5, v6, v7, v8, v9, v10, v11, v12, v13, v14, v15, v16, v17, v18, v19, v20, v21, v22, v23, v24, v25, v26, v27, v28, v29, v30, v31, v32, v33, v34, v35, v36, v37, v38, v39, v40, v41, v42, v43;
    float s0, s1, s2, s3, s4, s5, s6, s7, s24;
    uint s8_u;
    float vcc_lo, vcc_hi;
    bool vcc;
    vcc_lo = uintBitsToFloat(0x3E4CCCCDu);   // 0.2                          // 0x8
    v6 = in_attr1.x;                                                         // 0x20
    v7 = in_attr1.y;                                                         // 0x24
    v32 = textureQueryLod(s_Texture, vec2(v6, v7)).y;  // dmask 0x2          // 0x28
    { vec4 t_ = texture(s_Texture, vec2(v6, v7)); v2 = t_.x; v3 = t_.y; v4 = t_.z; v5 = t_.w; } // 0x30
    if (vcc_lo < v32) {   // v_cmpx_lt_f32 + s_cbranch_execz -> 0x484        // 0x40
        v33 = clamp(max(0.0, v32), 0.0, 1.0);                                    // 0x48
        s24 = uintBitsToFloat(0x3F99999Au);   // 1.2                             // 0x54
        v9 = float(uint(textureSize(s_Texture, 0).x));   // T# WIDTH+1 (s_lshl/s_and/s_lshr/s_or/s_add 0x50..0x6C) // 0x70
        v27 = v9 * v6;                                                           // 0x80
        v8 = float(uint(textureSize(s_Texture, 0).y));   // T# HEIGHT+1 (s_bfe_u32 0x74, s_add 0x7C) // 0x84
        v28 = 1.0 / v9;                                                          // 0x88
        v10 = 0.5 * v27 + -0.5;                                                  // 0x8C
        v29 = v8 * v7;                                                           // 0x94
        v30 = 1.0 / v8;                                                          // 0x98
        v31 = 2.0 * v28;                                                         // 0x9C
        v10 = floor(v10);                                                        // 0xA0
        v11 = 0.5 * v29 + -0.5;                                                  // 0xA4
        v26 = 2.0 * v30;                                                         // 0xAC
        v10 = 2.0 * v10 + 1.0;                                                   // 0xB0
        v11 = floor(v11);                                                        // 0xB8
        v34 = (v9 * v6 + -v10) * 0.5;                                            // 0xBC
        v6 = v28 * v10;                                                          // 0xC4
        v35 = 2.0 * v11 + 1.0;                                                   // 0xC8
        v22 = 2.0 * v28 + v6;                                                    // 0xD0
        v23 = v30 * v35;                                                         // 0xD8
        v25 = v30 * v35 + v26;                                                   // 0xDC
        v35 = (v8 * v7 + -v35) * 0.5;                                            // 0xE4
        v24 = -2.0 * v28 + v22;                                                  // 0xEC
        { vec4 t_ = texture(s_Texture, vec2(v22, v23)); v18 = t_.x; v19 = t_.y; v20 = t_.z; v21 = t_.w; } // 0xF4
        { vec4 t_ = texture(s_Texture, vec2(v6, v23)); v14 = t_.x; v15 = t_.y; v16 = t_.z; v17 = t_.w; } // 0xFC
        { vec4 t_ = texture(s_Texture, vec2(v24, v25)); v6 = t_.x; v7 = t_.y; v8 = t_.z; v9 = t_.w; } // 0x108
        { vec4 t_ = texture(s_Texture, vec2(v22, v25)); v10 = t_.x; v11 = t_.y; v12 = t_.z; v13 = t_.w; } // 0x110
        v24 = v22;                                                               // 0x11C
        v18 = v18 - v14;                                                         // 0x124
        v22 = v19 - v15;                                                         // 0x128
        v20 = v20 - v16;                                                         // 0x12C
        v19 = v21 - v17;                                                         // 0x130
        v11 = v11 - v7;                                                          // 0x138
        v18 = v34 * v18 + v14;                                                   // 0x13C
        v15 = v22 * v34 + v15;                                                   // 0x144
        v14 = v34 * v20 + v16;                                                   // 0x148
        v16 = v10 - v6;                                                          // 0x150
        v10 = v34 * v19 + v17;                                                   // 0x154
        v19 = v6 - v18;                                                          // 0x15C
        v17 = v7 - v15;                                                          // 0x160
        v6 = v12 - v8;                                                           // 0x164
        v12 = v13 - v9;                                                          // 0x168
        v13 = v9 - v10;                                                          // 0x16C
        v9 = v18 - v2;                                                           // 0x170
        v15 = v15 - v3;                                                          // 0x174
        v7 = v14 - v4;                                                           // 0x178
        v19 = v16 * v34 + v19;                                                   // 0x17C
        v17 = v11 * v34 + v17;                                                   // 0x180
        v11 = v8 - v14;                                                          // 0x184
        v8 = v10 - v5;                                                           // 0x188
        v13 = v12 * v34 + v13;                                                   // 0x18C
        v9 = v19 * v35 + v9;                                                     // 0x190
        v15 = v17 * v35 + v15;                                                   // 0x194
        v11 = v6 * v34 + v11;                                                    // 0x198
        v8 = v13 * v35 + v8;                                                     // 0x19C
        v2 = v9 * v33 + v2;                                                      // 0x1A0
        v3 = v15 * v33 + v3;                                                     // 0x1A4
        v7 = v11 * v35 + v7;                                                     // 0x1A8
        v5 = v8 * v33 + v5;                                                      // 0x1AC
        v4 = v7 * v33 + v4;                                                      // 0x1B0
        if (s24 < v32) {   // v_cmpx_lt_f32 + s_cbranch_execz -> 0x484           // 0x1B4
            v6 = 0.25 * v27 + -0.5;                                                  // 0x1BC
            v7 = 0.25 * v29 + -0.5;                                                  // 0x1C8
            vcc_lo = uintBitsToFloat(0x3E800000u);   // 0.25                         // 0x1D4
            v32 = clamp(-1.0 + v32, 0.0, 1.0);                                       // 0x1DC
            v6 = floor(v6);                                                          // 0x1E4
            v7 = floor(v7);                                                          // 0x1E8
            v6 = 4.0 * v6 + 2.0;                                                     // 0x1EC
            v8 = 4.0 * v7 + 2.0;                                                     // 0x1F4
            v28 = v28 * v6 + -v28;                                                   // 0x1FC
            v7 = v30 * v8 + -v30;                                                    // 0x204
            v10 = v29 - v8;                                                          // 0x20C
            v9 = v27 - v6;                                                           // 0x210
            v6 = v28 + v31;                                                          // 0x214
            v30 = v26 + v7;                                                          // 0x218
            v33 = 0.25 * v10;                                                        // 0x21C
            v34 = 0.25 * v9;                                                         // 0x224
            { vec4 t_ = texture(s_Texture, vec2(v28, v7)); v14 = t_.x; v15 = t_.y; v16 = t_.z; v17 = t_.w; } // 0x22C
            { vec4 t_ = texture(s_Texture, vec2(v6, v7)); v18 = t_.x; v19 = t_.y; v20 = t_.z; v21 = t_.w; } // 0x238
            v6 = v6 + v31;                                                           // 0x240
            v27 = v26 + v30;                                                         // 0x244
            v29 = v6 + v31;                                                          // 0x248
            v28 = v26 + v27;                                                         // 0x24C
            v26 = v29 - v31;                                                         // 0x250
            v36 = v14 + v18;                                                         // 0x258
            v38 = v15 + v19;                                                         // 0x25C
            v39 = v16 + v20;                                                         // 0x260
            v40 = v17 + v21;                                                         // 0x264
            { vec4 t_ = texture(s_Texture, vec2(v6, v7)); v10 = t_.x; v11 = t_.y; v12 = t_.z; v13 = t_.w; } // 0x268
            { vec4 t_ = texture(s_Texture, vec2(v29, v7)); v22 = t_.x; v23 = t_.y; v24 = t_.z; v25 = t_.w; } // 0x274
            { vec4 t_ = texture(s_Texture, vec2(v26, v30)); v14 = t_.x; v15 = t_.y; v16 = t_.z; v17 = t_.w; } // 0x280
            { vec4 t_ = texture(s_Texture, vec2(v29, v30)); v6 = t_.x; v7 = t_.y; v8 = t_.z; v9 = t_.w; } // 0x28C
            v29 = v26 - v31;                                                         // 0x294
            { vec4 t_ = texture(s_Texture, vec2(v29, v30)); v18 = t_.x; v19 = t_.y; v20 = t_.z; v21 = t_.w; } // 0x298
            v29 = v29 - v31;                                                         // 0x2A0
            v26 = v29 + v31;                                                         // 0x2A4
            v35 = v26 + v31;                                                         // 0x2A8
            v10 = v10 + v22;                                                         // 0x2B0
            v11 = v11 + v23;                                                         // 0x2B4
            v12 = v12 + v24;                                                         // 0x2B8
            v13 = v13 + v25;                                                         // 0x2BC
            { vec4 t_ = texture(s_Texture, vec2(v29, v30)); v22 = t_.x; v23 = t_.y; v24 = t_.z; v25 = t_.w; } // 0x2C0
            v30 = v10 + v6;                                                          // 0x2CC
            v41 = v11 + v7;                                                          // 0x2D0
            v18 = v36 + v18;                                                         // 0x2D8
            v19 = v38 + v19;                                                         // 0x2DC
            v20 = v39 + v20;                                                         // 0x2E0
            v21 = v40 + v21;                                                         // 0x2E4
            v42 = v13 + v9;                                                          // 0x2E8
            v43 = v12 + v8;                                                          // 0x2EC
            { vec4 t_ = texture(s_Texture, vec2(v26, v27)); v10 = t_.x; v11 = t_.y; v12 = t_.z; v13 = t_.w; } // 0x2F0
            { vec4 t_ = texture(s_Texture, vec2(v29, v27)); v6 = t_.x; v7 = t_.y; v8 = t_.z; v9 = t_.w; } // 0x2F8
            v26 = v35 + v31;                                                         // 0x304
            v29 = v30 + v14;                                                         // 0x308
            v38 = v42 + v17;                                                         // 0x30C
            v30 = v41 + v15;                                                         // 0x310
            v36 = v43 + v16;                                                         // 0x314
            v37 = v26 - v31;                                                         // 0x318
            v25 = v21 + v25;                                                         // 0x320
            v41 = v18 + v22;                                                         // 0x324
            v42 = v19 + v23;                                                         // 0x328
            v43 = v20 + v24;                                                         // 0x32C
            { vec4 t_ = texture(s_Texture, vec2(v35, v27)); v14 = t_.x; v15 = t_.y; v16 = t_.z; v17 = t_.w; } // 0x330
            { vec4 t_ = texture(s_Texture, vec2(v26, v27)); v18 = t_.x; v19 = t_.y; v20 = t_.z; v21 = t_.w; } // 0x33C
            v27 = v37 - v31;                                                         // 0x344
            v29 = v29 - v41;                                                         // 0x348
            v30 = v30 - v42;                                                         // 0x34C
            v35 = v36 - v43;                                                         // 0x350
            v40 = v7 + v11;                                                          // 0x358
            v39 = v9 + v13;                                                          // 0x35C
            v36 = v38 - v25;                                                         // 0x360
            v38 = v8 + v12;                                                          // 0x364
            v41 = v29 * v34 + v41;                                                   // 0x368
            v42 = v30 * v34 + v42;                                                   // 0x36C
            v43 = v35 * v34 + v43;                                                   // 0x370
            v29 = v34 * v36 + v25;                                                   // 0x374
            v18 = v14 + v18;                                                         // 0x380
            v19 = v15 + v19;                                                         // 0x384
            v20 = v16 + v20;                                                         // 0x388
            v21 = v17 + v21;                                                         // 0x38C
            { vec4 t_ = texture(s_Texture, vec2(v27, v28)); v22 = t_.x; v23 = t_.y; v24 = t_.z; v25 = t_.w; } // 0x390
            { vec4 t_ = texture(s_Texture, vec2(v26, v28)); v14 = t_.x; v15 = t_.y; v16 = t_.z; v17 = t_.w; } // 0x398
            v27 = v27 - v31;                                                         // 0x3A4
            v31 = v6 + v10;                                                          // 0x3A8
            { vec4 t_ = texture(s_Texture, vec2(v27, v28)); v10 = t_.x; v11 = t_.y; v12 = t_.z; v13 = t_.w; } // 0x3AC
            { vec4 t_ = texture(s_Texture, vec2(v37, v28)); v6 = t_.x; v7 = t_.y; v8 = t_.z; v9 = t_.w; } // 0x3B4
            v27 = v26;                                                               // 0x3C0
            v22 = v31 + v22;                                                         // 0x3C8
            v14 = v18 + v14;                                                         // 0x3D0
            v23 = v40 + v23;                                                         // 0x3D4
            v15 = v19 + v15;                                                         // 0x3D8
            v24 = v38 + v24;                                                         // 0x3DC
            v16 = v20 + v16;                                                         // 0x3E0
            v25 = v39 + v25;                                                         // 0x3E4
            v17 = v21 + v17;                                                         // 0x3E8
            v22 = v22 + v10;                                                         // 0x3F0
            v6 = v14 + v6;                                                           // 0x3F8
            v23 = v23 + v11;                                                         // 0x3FC
            v7 = v15 + v7;                                                           // 0x400
            v24 = v24 + v12;                                                         // 0x404
            v8 = v16 + v8;                                                           // 0x408
            v25 = v25 + v13;                                                         // 0x40C
            v9 = v17 + v9;                                                           // 0x410
            v6 = v6 - v22;                                                           // 0x414
            v13 = v22 - v41;                                                         // 0x418
            v7 = v7 - v23;                                                           // 0x41C
            v12 = v23 - v42;                                                         // 0x420
            v8 = v8 - v24;                                                           // 0x424
            v11 = v24 - v43;                                                         // 0x428
            v9 = v9 - v25;                                                           // 0x42C
            v10 = v25 - v29;                                                         // 0x430
            v13 = v6 * v34 + v13;                                                    // 0x434
            v12 = v7 * v34 + v12;                                                    // 0x438
            v11 = v8 * v34 + v11;                                                    // 0x43C
            v10 = v9 * v34 + v10;                                                    // 0x440
            v41 = v13 * v33 + v41;                                                   // 0x444
            v42 = v12 * v33 + v42;                                                   // 0x448
            v43 = v11 * v33 + v43;                                                   // 0x44C
            v29 = v10 * v33 + v29;                                                   // 0x450
            v6 = vcc_lo * v41 + -v2;                                                 // 0x454
            v7 = vcc_lo * v42 + -v3;                                                 // 0x45C
            v8 = vcc_lo * v43 + -v4;                                                 // 0x464
            v9 = vcc_lo * v29 + -v5;                                                 // 0x46C
            v2 = v6 * v32 + v2;                                                      // 0x474
            v3 = v7 * v32 + v3;                                                      // 0x478
            v4 = v8 * v32 + v4;                                                      // 0x47C
            v5 = v9 * v32 + v5;                                                      // 0x480
        }                                                                        // 0x484
    }                                                                        // 0x484
    s8_u = u_Flag;                                                           // 0x488
    v9 = in_attr0.x;                                                         // 0x4A0
    v8 = in_attr0.y;                                                         // 0x4A4
    v7 = in_attr0.z;                                                         // 0x4A8
    v6 = in_attr0.w;                                                         // 0x4AC
    v2 = v9 * v2;                                                            // 0x4B4
    v3 = v8 * v3;                                                            // 0x4B8
    v4 = v7 * v4;                                                            // 0x4BC
    v5 = v6 * v5;                                                            // 0x4C0
    v7 = in_attr1.z;                                                         // 0x4D0
    if ((s8_u & 6u) != 0u) {
        v6 = 1.0;                                                                // 0x4D8
        v0 = in_attr1.w;                                                         // 0x4E4
        if ((s8_u & 4u) != 0u) {
            vcc_lo = uintBitsToFloat(0x3F100000u);   // 0.5625                       // 0x4F4
            s0 = u_TargetRect.x; s1 = u_TargetRect.y; s2 = u_TargetRect.z; s3 = u_TargetRect.w; // 0x4FC
            v9 = vcc_lo * v0 + -s1;                                                  // 0x508
            v10 = v7 - s0;                                                           // 0x510
            v9 = -s3 + abs(v9);                                                      // 0x51C
            v10 = -s2 + abs(v10);                                                    // 0x524
            v11 = min(v10, v9);                                                      // 0x52C
            if (0.0 > v11) {                                                         // 0x530
                vcc = 0.0 > v10;                                                         // 0x534
                v9 = vcc ? v9 : v10;                                                     // 0x538
            } else {                                                                 // 0x53C
                v10 = v10 * v10;                                                         // 0x540
                v10 = v9 * v9 + v10;                                                     // 0x544
                v9 = sqrt(v10);                                                          // 0x548
            }                                                                        // 0x54C
            vcc_lo = u_Radius;                                                       // 0x550
            v1 = dFdxCoarse(v7);   // v1 = dpp quad_perm[1,1,1,1](v7) (0x4EC) - dpp quad_perm[0,0,0,0](v7) (0x514) // 0x558
            v8 = vcc_lo - v1;                                                        // 0x560
            if (v9 > v8) {                                                           // 0x564
                v6 = vcc_lo - v9;                                                        // 0x568
                v1 = 1.0 / v1;                                                           // 0x56C
                v6 = clamp(v1 * v6 + 0.5, 0.0, 1.0);                                     // 0x570
            }                                                                        // 0x578
        }
        if ((s8_u & 2u) != 0u) {
            s0 = u_FadeA.x; s1 = u_FadeA.y; s2 = u_FadeA.z; s3 = u_FadeA.w; s4 = u_FadeB.x; s5 = u_FadeB.y; s6 = u_FadeB.z; s7 = u_FadeB.w; // 0x584
            v1 = clamp(s0 * v7 + s4, 0.0, 1.0);                                      // 0x590
            v8 = clamp(v7 * s1 + s5, 0.0, 1.0);                                      // 0x598
            v7 = clamp(v0 * s2 + s6, 0.0, 1.0);                                      // 0x5A0
            v0 = clamp(v0 * s3 + s7, 0.0, 1.0);                                      // 0x5A8
            v1 = v1 * v8;                                                            // 0x5B0
            v1 = v1 * v7;                                                            // 0x5B4
            v1 = v1 * v0;                                                            // 0x5B8
            v0 = v1 * v1;                                                            // 0x5BC
            v1 = -2.0 * v1 + 3.0;                                                    // 0x5C0
            v0 = v0 * v6;                                                            // 0x5C8
            v6 = v0 * v1;                                                            // 0x5CC
        }
        v0 = v6 * v2;                                                            // 0x5D0
        v1 = v6 * v3;                                                            // 0x5D4
        v7 = v6 * v4;                                                            // 0x5D8
        v5 = v6 * v5;                                                            // 0x5E0
        vcc = (s8_u & 1u) != 0u;                                                 // 0x5E4
        v2 = vcc ? v0 : v2;                                                      // 0x5E8
        v3 = vcc ? v1 : v3;                                                      // 0x5EC
        v4 = vcc ? v7 : v4;                                                      // 0x5F0
    }
    vcc_hi = u_GlobalIntensity;                                              // 0x5F4
    v1 = vcc_hi;                                                             // 0x600
    if ((s8_u & 1u) != 0u) {
        vcc_lo = u_GlobalAlpha;                                                  // 0x60C
        v1 = vcc_lo * vcc_hi;                                                    // 0x618
    }
    v0 = v1 * v2;                                                            // 0x620
    vcc_lo = u_GlobalAlpha;                                                  // 0x624
    v2 = v1 * v3;                                                            // 0x62C
    v1 = v1 * v4;                                                            // 0x630
    v3 = vcc_lo * v5;                                                        // 0x638
    out_mrt0 = vec4(v0, v2, v1, v3);   // v_cvt_pkrtz_f16_f32 v0,v0,v2 / v1,v1,v3 ; exp mrt0 compr (0x63C..0x648)
}
