#version 450
// Transcription of libScePsm embedded shader #31 "mask_p" (work/shaders/31_mask_p.ags.bin),
// AGC binary type 1 (pixel), .shader_text 0xB20 bytes (code up to s_endpgm at 0x9A4). EmbeddedShader.TextureMask = (basic_vv, mask_p).
// Every statement maps to the RDNA instruction at the byte offset in its trailing comment.
//
// Math:
//   t  = s_Texture at attr1.xy with the same LOD-driven 2x2 / 4x4 box pre-filter as rgba_p (see rgba_p.frag)
//   mu = (M.x*u + O.x + v*M.y, u*M.z + O.y + v*M.w)   M = u_clipMaskTextureMat, O = u_clipMaskTextureOffset
//   a  = s_MaskTexture(mu).a with the same kind of LOD pre-filter on the alpha channel only
//        (2x2 stage: threshold 0.2, 4x4 stage: 1.2; coefficients transcribed as they appear)
//   t.a *= (1 - a);  if (u_Flag & 1) t.rgb *= (1 - a)
//   c = t * attr0
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
// Resources: s_Texture t0 -> T# s[0:7] (dword 0), s_MaskTexture t1 -> T# s[8:15] (dword 8),
//   Sampler0 -> S# s[16:19] (dword 16), Sampler1 -> S# s[20:23] (dword 20),
//   __GLOBAL_CB__ slot 0 -> s[24:27] (dword 24), globalConstantBuffer slot 7 -> s[28:31] (dword 28).
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

layout(set = 0, binding = 1, std140) uniform GLOBAL_CB {   // metadata "__GLOBAL_CB__", resource slot 0, 88 bytes
    layout(offset = 0)  vec4  u_FadeA;
    layout(offset = 16) vec4  u_FadeB;
    layout(offset = 32) vec4  u_TargetRect;
    layout(offset = 48) float u_Radius;
    layout(offset = 52) uint  u_Flag;       // metadata base type 9 (32-bit integer class)
    layout(offset = 64) vec4  u_clipMaskTextureMat;
    layout(offset = 80) vec2  u_clipMaskTextureOffset;
};
// globalConstantBuffer: metadata resource slot 7, 8 bytes. Created by PsmGraphicsContext::Create (icall 0x1e5d50,
// sub_1230 with initial data {1.0f, 1.0f}) and rewritten only by PsmGraphicsContext::UpdateGlobalConstantBuffer
// (icall 0x1f1c80, memcpy of the managed struct). Bound to PS and VS slot 7 at every draw (sub_1F32A0).
layout(set = 0, binding = 2, std140) uniform globalConstantBuffer {
    layout(offset = 0) float u_GlobalAlpha;
    layout(offset = 4) float u_GlobalIntensity;
};

// s_Texture (texture slot 0) with Sampler0 (sampler slot 0); s_MaskTexture (texture slot 1) with Sampler1 (slot 1).
layout(set = 0, binding = 3) uniform sampler2D s_Texture;
layout(set = 0, binding = 4) uniform sampler2D s_MaskTexture;

layout(location = 0) in vec4 in_attr0;   // PS attr0 = input semantic 0: input.Color (COLOR)
layout(location = 1) in vec4 in_attr1;   // PS attr1 = input semantic 1: .xy input.TexCoord (TEXCOORD0), .zw input.ClipPos (TEXCOORD1)
layout(location = 0) out vec4 out_mrt0;  // exp mrt0 (SPI_SHADER_COL_FORMAT = 4: packed fp16, v_cvt_pkrtz_f16_f32 = round toward zero)

void main()
{
    precise float v0, v1, v2, v3, v4, v5, v6, v7, v8, v9, v10, v11, v12, v13, v14, v15, v16, v17, v18, v19, v20, v21, v22, v23, v24, v25, v26, v27, v28, v29, v30, v31, v32, v33, v34, v35, v36, v37, v38, v39, v40, v41, v42, v43;
    float s0, s1, s2, s3, s4, s5, s6, s7, s36;
    uint s4_u, s8_u;
    float vcc_lo, vcc_hi;
    bool vcc;
    vcc_lo = uintBitsToFloat(0x3E4CCCCDu);   // 0.2                          // 0x8
    v26 = in_attr1.x;                                                        // 0x20
    v27 = in_attr1.y;                                                        // 0x24
    v34 = textureQueryLod(s_Texture, vec2(v26, v27)).y;  // dmask 0x2        // 0x28
    { vec4 t_ = texture(s_Texture, vec2(v26, v27)); v2 = t_.x; v3 = t_.y; v4 = t_.z; v5 = t_.w; } // 0x30
    if (vcc_lo < v34) {   // v_cmpx_lt_f32 + s_cbranch_execz -> 0x48C        // 0x40
        v35 = clamp(max(0.0, v34), 0.0, 1.0);                                    // 0x48
        s36 = uintBitsToFloat(0x3F99999Au);   // 1.2                             // 0x54
        v6 = float(uint(textureSize(s_Texture, 0).x));   // T# WIDTH+1 (0x50..0x6C) // 0x70
        v29 = v6 * v26;                                                          // 0x80
        v7 = float(uint(textureSize(s_Texture, 0).y));   // T# HEIGHT+1 (0x74, 0x7C) // 0x84
        v30 = 1.0 / v6;                                                          // 0x88
        v8 = 0.5 * v29 + -0.5;                                                   // 0x8C
        v31 = v7 * v27;                                                          // 0x94
        v32 = 1.0 / v7;                                                          // 0x98
        v33 = 2.0 * v30;                                                         // 0x9C
        v8 = floor(v8);                                                          // 0xA0
        v9 = 0.5 * v31 + -0.5;                                                   // 0xA4
        v28 = 2.0 * v32;                                                         // 0xAC
        v8 = 2.0 * v8 + 1.0;                                                     // 0xB0
        v9 = floor(v9);                                                          // 0xB8
        v36 = (v6 * v26 + -v8) * 0.5;                                            // 0xBC
        v6 = v30 * v8;                                                           // 0xC4
        v37 = 2.0 * v9 + 1.0;                                                    // 0xC8
        v22 = 2.0 * v30 + v6;                                                    // 0xD0
        v23 = v32 * v37;                                                         // 0xD8
        v25 = v32 * v37 + v28;                                                   // 0xDC
        v37 = (v7 * v27 + -v37) * 0.5;                                           // 0xE4
        v24 = -2.0 * v30 + v22;                                                  // 0xEC
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
        v18 = v36 * v18 + v14;                                                   // 0x13C
        v15 = v22 * v36 + v15;                                                   // 0x144
        v14 = v36 * v20 + v16;                                                   // 0x148
        v16 = v10 - v6;                                                          // 0x150
        v10 = v36 * v19 + v17;                                                   // 0x154
        v19 = v6 - v18;                                                          // 0x15C
        v17 = v7 - v15;                                                          // 0x160
        v6 = v12 - v8;                                                           // 0x164
        v12 = v13 - v9;                                                          // 0x168
        v13 = v9 - v10;                                                          // 0x16C
        v9 = v18 - v2;                                                           // 0x170
        v15 = v15 - v3;                                                          // 0x174
        v7 = v14 - v4;                                                           // 0x178
        v19 = v16 * v36 + v19;                                                   // 0x17C
        v17 = v11 * v36 + v17;                                                   // 0x180
        v11 = v8 - v14;                                                          // 0x184
        v8 = v10 - v5;                                                           // 0x188
        v13 = v12 * v36 + v13;                                                   // 0x18C
        v9 = v19 * v37 + v9;                                                     // 0x190
        v15 = v17 * v37 + v15;                                                   // 0x194
        v11 = v6 * v36 + v11;                                                    // 0x198
        v8 = v13 * v37 + v8;                                                     // 0x19C
        v2 = v9 * v35 + v2;                                                      // 0x1A0
        v3 = v15 * v35 + v3;                                                     // 0x1A4
        v7 = v11 * v37 + v7;                                                     // 0x1A8
        v5 = v8 * v35 + v5;                                                      // 0x1AC
        v4 = v7 * v35 + v4;                                                      // 0x1B0
        if (s36 < v34) {   // v_cmpx_lt_f32 + s_cbranch_execz -> 0x48C           // 0x1B4
            v6 = 0.25 * v29 + -0.5;                                                  // 0x1BC
            v7 = 0.25 * v31 + -0.5;                                                  // 0x1C8
            vcc_lo = uintBitsToFloat(0x3E800000u);   // 0.25                         // 0x1D4
            v34 = clamp(-1.0 + v34, 0.0, 1.0);                                       // 0x1DC
            v6 = floor(v6);                                                          // 0x1E4
            v7 = floor(v7);                                                          // 0x1E8
            v6 = 4.0 * v6 + 2.0;                                                     // 0x1EC
            v8 = 4.0 * v7 + 2.0;                                                     // 0x1F4
            v30 = v30 * v6 + -v30;                                                   // 0x1FC
            v7 = v32 * v8 + -v32;                                                    // 0x204
            v10 = v31 - v8;                                                          // 0x20C
            v9 = v29 - v6;                                                           // 0x210
            v6 = v30 + v33;                                                          // 0x214
            v32 = v28 + v7;                                                          // 0x218
            v35 = 0.25 * v10;                                                        // 0x21C
            v36 = 0.25 * v9;                                                         // 0x224
            { vec4 t_ = texture(s_Texture, vec2(v30, v7)); v10 = t_.x; v11 = t_.y; v12 = t_.z; v13 = t_.w; } // 0x22C
            { vec4 t_ = texture(s_Texture, vec2(v6, v7)); v14 = t_.x; v15 = t_.y; v16 = t_.z; v17 = t_.w; } // 0x238
            v6 = v6 + v33;                                                           // 0x240
            v29 = v28 + v32;                                                         // 0x244
            v31 = v6 + v33;                                                          // 0x248
            v30 = v28 + v29;                                                         // 0x24C
            v28 = v31 - v33;                                                         // 0x250
            v39 = v11 + v15;                                                         // 0x258
            v41 = v13 + v17;                                                         // 0x25C
            v38 = v10 + v14;                                                         // 0x260
            v40 = v12 + v16;                                                         // 0x264
            { vec4 t_ = texture(s_Texture, vec2(v6, v7)); v18 = t_.x; v19 = t_.y; v20 = t_.z; v21 = t_.w; } // 0x268
            { vec4 t_ = texture(s_Texture, vec2(v31, v7)); v22 = t_.x; v23 = t_.y; v24 = t_.z; v25 = t_.w; } // 0x274
            { vec4 t_ = texture(s_Texture, vec2(v31, v32)); v6 = t_.x; v7 = t_.y; v8 = t_.z; v9 = t_.w; } // 0x280
            { vec4 t_ = texture(s_Texture, vec2(v28, v32)); v10 = t_.x; v11 = t_.y; v12 = t_.z; v13 = t_.w; } // 0x288
            v31 = v28 - v33;                                                         // 0x294
            { vec4 t_ = texture(s_Texture, vec2(v31, v32)); v14 = t_.x; v15 = t_.y; v16 = t_.z; v17 = t_.w; } // 0x298
            v31 = v31 - v33;                                                         // 0x2A0
            v28 = v31 + v33;                                                         // 0x2A4
            v37 = v28 + v33;                                                         // 0x2A8
            v22 = v18 + v22;                                                         // 0x2B0
            v42 = v19 + v23;                                                         // 0x2B4
            v24 = v20 + v24;                                                         // 0x2B8
            v25 = v21 + v25;                                                         // 0x2BC
            v23 = v22 + v6;                                                          // 0x2C4
            v24 = v24 + v8;                                                          // 0x2C8
            v22 = v25 + v9;                                                          // 0x2CC
            v25 = v42 + v7;                                                          // 0x2D0
            { vec4 t_ = texture(s_Texture, vec2(v31, v32)); v18 = t_.x; v19 = t_.y; v20 = t_.z; v21 = t_.w; } // 0x2D4
            { vec4 t_ = texture(s_Texture, vec2(v31, v29)); v6 = t_.x; v7 = t_.y; v8 = t_.z; v9 = t_.w; } // 0x2DC
            v32 = v39 + v15;                                                         // 0x2EC
            v39 = v41 + v17;                                                         // 0x2F0
            v17 = v22 + v13;                                                         // 0x2F4
            v15 = v25 + v11;                                                         // 0x2F8
            v31 = v38 + v14;                                                         // 0x2FC
            v14 = v23 + v10;                                                         // 0x300
            v38 = v40 + v16;                                                         // 0x304
            v16 = v24 + v12;                                                         // 0x308
            { vec4 t_ = texture(s_Texture, vec2(v37, v29)); v10 = t_.x; v11 = t_.y; v12 = t_.z; v13 = t_.w; } // 0x30C
            { vec4 t_ = texture(s_Texture, vec2(v28, v29)); v22 = t_.x; v23 = t_.y; v24 = t_.z; v25 = t_.w; } // 0x318
            v28 = v37 + v33;                                                         // 0x320
            v19 = v32 + v19;                                                         // 0x328
            v20 = v38 + v20;                                                         // 0x32C
            v21 = v39 + v21;                                                         // 0x330
            v39 = v28 - v33;                                                         // 0x334
            v43 = v31 + v18;                                                         // 0x338
            v32 = v15 - v19;                                                         // 0x33C
            v37 = v16 - v20;                                                         // 0x340
            v38 = v17 - v21;                                                         // 0x344
            v40 = v7 + v23;                                                          // 0x34C
            v41 = v8 + v24;                                                          // 0x350
            v42 = v9 + v25;                                                          // 0x354
            v31 = v14 - v43;                                                         // 0x358
            { vec4 t_ = texture(s_Texture, vec2(v28, v29)); v14 = t_.x; v15 = t_.y; v16 = t_.z; v17 = t_.w; } // 0x35C
            v29 = v39 - v33;                                                         // 0x364
            v43 = v31 * v36 + v43;                                                   // 0x368
            v31 = v36 * v32 + v19;                                                   // 0x36C
            v32 = v36 * v37 + v20;                                                   // 0x374
            v37 = v36 * v38 + v21;                                                   // 0x37C
            v14 = v10 + v14;                                                         // 0x388
            v15 = v11 + v15;                                                         // 0x38C
            v16 = v12 + v16;                                                         // 0x390
            v17 = v13 + v17;                                                         // 0x394
            { vec4 t_ = texture(s_Texture, vec2(v29, v30)); v18 = t_.x; v19 = t_.y; v20 = t_.z; v21 = t_.w; } // 0x398
            { vec4 t_ = texture(s_Texture, vec2(v28, v30)); v10 = t_.x; v11 = t_.y; v12 = t_.z; v13 = t_.w; } // 0x3A0
            v29 = v29 - v33;                                                         // 0x3AC
            v33 = v6 + v22;                                                          // 0x3B0
            { vec4 t_ = texture(s_Texture, vec2(v29, v30)); v22 = t_.x; v23 = t_.y; v24 = t_.z; v25 = t_.w; } // 0x3B4
            { vec4 t_ = texture(s_Texture, vec2(v39, v30)); v6 = t_.x; v7 = t_.y; v8 = t_.z; v9 = t_.w; } // 0x3BC
            v29 = v28;                                                               // 0x3C8
            v18 = v33 + v18;                                                         // 0x3D0
            v14 = v14 + v10;                                                         // 0x3D8
            v19 = v40 + v19;                                                         // 0x3DC
            v11 = v15 + v11;                                                         // 0x3E0
            v20 = v41 + v20;                                                         // 0x3E4
            v12 = v16 + v12;                                                         // 0x3E8
            v28 = v42 + v21;                                                         // 0x3EC
            v13 = v17 + v13;                                                         // 0x3F0
            v18 = v18 + v22;                                                         // 0x3F8
            v14 = v14 + v6;                                                          // 0x400
            v19 = v19 + v23;                                                         // 0x404
            v7 = v11 + v7;                                                           // 0x408
            v21 = v20 + v24;                                                         // 0x40C
            v8 = v12 + v8;                                                           // 0x410
            v23 = v28 + v25;                                                         // 0x414
            v9 = v13 + v9;                                                           // 0x418
            v11 = v14 - v18;                                                         // 0x41C
            v22 = v18 - v43;                                                         // 0x420
            v7 = v7 - v19;                                                           // 0x424
            v20 = v19 - v31;                                                         // 0x428
            v8 = v8 - v21;                                                           // 0x42C
            v10 = v21 - v32;                                                         // 0x430
            v9 = v9 - v23;                                                           // 0x434
            v6 = v23 - v37;                                                          // 0x438
            v22 = v11 * v36 + v22;                                                   // 0x43C
            v20 = v7 * v36 + v20;                                                    // 0x440
            v10 = v8 * v36 + v10;                                                    // 0x444
            v6 = v9 * v36 + v6;                                                      // 0x448
            v43 = v22 * v35 + v43;                                                   // 0x44C
            v31 = v20 * v35 + v31;                                                   // 0x450
            v32 = v10 * v35 + v32;                                                   // 0x454
            v37 = v6 * v35 + v37;                                                    // 0x458
            v9 = vcc_lo * v43 + -v2;                                                 // 0x45C
            v38 = vcc_lo * v31 + -v3;                                                // 0x464
            v8 = vcc_lo * v32 + -v4;                                                 // 0x46C
            v6 = vcc_lo * v37 + -v5;                                                 // 0x474
            v2 = v9 * v34 + v2;                                                      // 0x47C
            v3 = v38 * v34 + v3;                                                     // 0x480
            v4 = v8 * v34 + v4;                                                      // 0x484
            v5 = v6 * v34 + v5;                                                      // 0x488
        }                                                                        // 0x48C
    }                                                                        // 0x48C
    s0 = u_clipMaskTextureMat.x; s1 = u_clipMaskTextureMat.y; s2 = u_clipMaskTextureMat.z; s3 = u_clipMaskTextureMat.w; // 0x490
    vcc_lo = u_clipMaskTextureOffset.x; vcc_hi = u_clipMaskTextureOffset.y;  // 0x498
    v6 = v26 * s2 + vcc_hi;                                                  // 0x4A4
    v8 = s0 * v26 + vcc_lo;                                                  // 0x4AC
    s4 = uintBitsToFloat(0x3E4CCCCDu);   // 0.2                              // 0x4B4
    v7 = v27 * s3 + v6;                                                      // 0x4BC
    v6 = v27 * s1 + v8;                                                      // 0x4C4
    v15 = textureQueryLod(s_MaskTexture, vec2(v6, v7)).y;  // dmask 0x2      // 0x4CC
    v13 = texture(s_MaskTexture, vec2(v6, v7)).w;                            // 0x4D4
    if (s4 < v15) {   // v_cmpx_lt_f32 + s_cbranch_execz -> 0x7B0            // 0x4E0
        v19 = clamp(max(0.0, v15), 0.0, 1.0);                                    // 0x4E8
        s0 = uintBitsToFloat(0x3F99999Au);   // 1.2                              // 0x4F4
        v8 = float(uint(textureSize(s_MaskTexture, 0).x));   // T# s[8:15] WIDTH+1 (0x4F0..0x50C) // 0x518
        v20 = float(uint(textureSize(s_MaskTexture, 0).y));   // T# HEIGHT+1 (0x510, 0x51C) // 0x520
        v17 = v8 * v6;                                                           // 0x524
        v18 = 1.0 / v8;                                                          // 0x528
        v11 = v20 * v7;                                                          // 0x52C
        v12 = 1.0 / v20;                                                         // 0x530
        v10 = 0.5 * v17 + -0.5;                                                  // 0x534
        v14 = 2.0 * v18;                                                         // 0x53C
        v9 = 0.5 * v11 + -0.5;                                                   // 0x540
        v16 = 2.0 * v12;                                                         // 0x548
        v10 = floor(v10);                                                        // 0x54C
        v21 = floor(v9);                                                         // 0x550
        v23 = 2.0 * v10 + 1.0;                                                   // 0x554
        v24 = 2.0 * v21 + 1.0;                                                   // 0x55C
        v21 = v18 * v23;                                                         // 0x564
        v23 = (v8 * v6 + -v23) * 0.5;                                            // 0x568
        v9 = v12 * v24;                                                          // 0x570
        v10 = v12 * v24 + v16;                                                   // 0x574
        v8 = 2.0 * v18 + v21;                                                    // 0x57C
        v24 = v20 * v7 + -v24;                                                   // 0x584
        v6 = texture(s_MaskTexture, vec2(v21, v9)).w;                            // 0x58C
        v21 = texture(s_MaskTexture, vec2(v8, v9)).w;                            // 0x598
        v9 = -2.0 * v18 + v8;                                                    // 0x5A0
        v20 = texture(s_MaskTexture, vec2(v9, v10)).w;                           // 0x5A8
        v9 = texture(s_MaskTexture, vec2(v8, v10)).w;                            // 0x5B0
        v8 = v21 - v6;                                                           // 0x5C0
        v6 = v8 * v23 + v6;                                                      // 0x5C4
        v9 = v9 - v20;                                                           // 0x5CC
        v7 = v20 - v6;                                                           // 0x5D0
        v8 = v6 - v13;                                                           // 0x5D4
        v7 = v9 * v23 + v7;                                                      // 0x5D8
        v6 = v24 * v7;                                                           // 0x5DC
        v8 = 0.5 * v6 + v8;                                                      // 0x5E0
        v13 = v8 * v19 + v13;                                                    // 0x5E4
        if (s0 < v15) {   // v_cmpx_lt_f32 + s_cbranch_execz -> 0x7B0            // 0x5E8
            v7 = 0.25 * v17 + -0.5;                                                  // 0x5F0
            v6 = 0.25 * v11 + -0.5;                                                  // 0x5FC
            vcc_lo = uintBitsToFloat(0x3E800000u);   // 0.25                         // 0x608
            v15 = clamp(-1.0 + v15, 0.0, 1.0);                                       // 0x610
            v7 = floor(v7);                                                          // 0x618
            v6 = floor(v6);                                                          // 0x61C
            v7 = 4.0 * v7 + 2.0;                                                     // 0x620
            v8 = 4.0 * v6 + 2.0;                                                     // 0x628
            v18 = v18 * v7 + -v18;                                                   // 0x630
            v12 = v12 * v8 + -v12;                                                   // 0x638
            v7 = v17 - v7;                                                           // 0x640
            v17 = v11 - v8;                                                          // 0x644
            v11 = v18 + v14;                                                         // 0x648
            v10 = v16 + v12;                                                         // 0x64C
            v6 = v11 + v14;                                                          // 0x650
            v20 = texture(s_MaskTexture, vec2(v11, v12)).w;                          // 0x654
            v19 = texture(s_MaskTexture, vec2(v18, v12)).w;                          // 0x65C
            v21 = texture(s_MaskTexture, vec2(v6, v12)).w;                           // 0x668
            v11 = v6 + v14;                                                          // 0x674
            v18 = 0.25 * v7;                                                         // 0x678
            v7 = v16 + v10;                                                          // 0x680
            v9 = v11 - v14;                                                          // 0x684
            v8 = v16 + v7;                                                           // 0x688
            v22 = texture(s_MaskTexture, vec2(v11, v12)).w;                          // 0x68C
            v16 = texture(s_MaskTexture, vec2(v11, v10)).w;                          // 0x694
            v11 = texture(s_MaskTexture, vec2(v9, v10)).w;                           // 0x6A0
            v9 = v9 - v14;                                                           // 0x6A8
            v24 = v9 - v14;                                                          // 0x6AC
            v6 = v24 + v14;                                                          // 0x6B0
            v26 = texture(s_MaskTexture, vec2(v9, v10)).w;                           // 0x6B4
            v27 = texture(s_MaskTexture, vec2(v6, v7)).w;                            // 0x6BC
            v23 = texture(s_MaskTexture, vec2(v24, v7)).w;                           // 0x6C4
            v12 = texture(s_MaskTexture, vec2(v24, v10)).w;                          // 0x6D0
            v9 = v6 + v14;                                                           // 0x6DC
            v6 = v9 + v14;                                                           // 0x6E0
            v24 = texture(s_MaskTexture, vec2(v6, v7)).w;                            // 0x6E4
            v10 = texture(s_MaskTexture, vec2(v9, v7)).w;                            // 0x6EC
            v7 = v6 - v14;                                                           // 0x6F8
            v9 = texture(s_MaskTexture, vec2(v7, v8)).w;                             // 0x6FC
            v7 = v7 - v14;                                                           // 0x708
            v25 = texture(s_MaskTexture, vec2(v7, v8)).w;                            // 0x70C
            v7 = v7 - v14;                                                           // 0x714
            v14 = texture(s_MaskTexture, vec2(v7, v8)).w;                            // 0x718
            v7 = texture(s_MaskTexture, vec2(v6, v8)).w;                             // 0x720
            v8 = v19 + v20;                                                          // 0x730
            v6 = v21 + v22;                                                          // 0x738
            v16 = v6 + v16;                                                          // 0x740
            v11 = v16 + v11;                                                         // 0x748
            v8 = v8 + v26;                                                           // 0x750
            v6 = v23 + v27;                                                          // 0x758
            v8 = v8 + v12;                                                           // 0x760
            v11 = v11 - v8;                                                          // 0x764
            v10 = v10 + v24;                                                         // 0x76C
            v8 = v11 * v18 + v8;                                                     // 0x770
            v6 = v6 + v25;                                                           // 0x778
            v7 = v10 + v7;                                                           // 0x780
            v11 = v6 + v14;                                                          // 0x784
            v7 = v7 + v9;                                                            // 0x788
            v6 = v11 - v8;                                                           // 0x78C
            v7 = v7 - v11;                                                           // 0x790
            v6 = v7 * v18 + v6;                                                      // 0x794
            v6 = v17 * v6;                                                           // 0x798
            v6 = v6 * 0.25 + v8;                                                     // 0x79C
            v6 = vcc_lo * v6 + -v13;                                                 // 0x7A4
            v13 = v6 * v15 + v13;                                                    // 0x7AC
        }                                                                        // 0x7B0
    }                                                                        // 0x7B0
    s4_u = u_Flag;                                                           // 0x7B4
    v8 = 1.0 - v13;                                                          // 0x7C0
    s8_u = s4_u & 1u;                                                        // 0x7C8
    vcc = s8_u != 0u;   // s_cselect_b64 vcc, s[34:35], 0 (SCC from s_and 0x7C8) // 0x7D0
    v11 = v8 * v2;                                                           // 0x7D8
    v10 = v8 * v3;                                                           // 0x7DC
    v9 = v8 * v4;                                                            // 0x7E0
    v5 = v5 * v8;                                                            // 0x7E4
    v2 = vcc ? v11 : v2;                                                     // 0x7EC
    v3 = vcc ? v10 : v3;                                                     // 0x7F0
    v4 = vcc ? v9 : v4;                                                      // 0x7F4
    v7 = in_attr0.x;                                                         // 0x7FC
    v6 = in_attr0.y;                                                         // 0x800
    v8 = in_attr0.z;                                                         // 0x804
    v9 = in_attr0.w;                                                         // 0x808
    v2 = v7 * v2;                                                            // 0x80C
    v3 = v6 * v3;                                                            // 0x810
    v4 = v8 * v4;                                                            // 0x814
    v5 = v5 * v9;                                                            // 0x818
    v7 = in_attr1.z;                                                         // 0x824
    if ((s4_u & 6u) != 0u) {
        v6 = 1.0;                                                                // 0x82C
        v0 = in_attr1.w;                                                         // 0x838
        if ((s4_u & 4u) != 0u) {
            vcc_lo = uintBitsToFloat(0x3F100000u);   // 0.5625                       // 0x848
            s0 = u_TargetRect.x; s1 = u_TargetRect.y; s2 = u_TargetRect.z; s3 = u_TargetRect.w; // 0x850
            v9 = vcc_lo * v0 + -s1;                                                  // 0x85C
            v10 = v7 - s0;                                                           // 0x864
            v9 = -s3 + abs(v9);                                                      // 0x870
            v10 = -s2 + abs(v10);                                                    // 0x878
            v11 = min(v10, v9);                                                      // 0x880
            if (0.0 > v11) {                                                         // 0x884
                vcc = 0.0 > v10;                                                         // 0x888
                v9 = vcc ? v9 : v10;                                                     // 0x88C
            } else {                                                                 // 0x890
                v10 = v10 * v10;                                                         // 0x894
                v10 = v9 * v9 + v10;                                                     // 0x898
                v9 = sqrt(v10);                                                          // 0x89C
            }                                                                        // 0x8A0
            vcc_lo = u_Radius;                                                       // 0x8A4
            v1 = dFdxCoarse(v7);   // v1 = dpp quad_perm[1,1,1,1](v7) (0x840) - dpp quad_perm[0,0,0,0](v7) (0x868) // 0x8AC
            v8 = vcc_lo - v1;                                                        // 0x8B4
            if (v9 > v8) {                                                           // 0x8B8
                v6 = vcc_lo - v9;                                                        // 0x8BC
                v1 = 1.0 / v1;                                                           // 0x8C0
                v6 = clamp(v1 * v6 + 0.5, 0.0, 1.0);                                     // 0x8C4
            }                                                                        // 0x8CC
        }
        if ((s4_u & 2u) != 0u) {
            s0 = u_FadeA.x; s1 = u_FadeA.y; s2 = u_FadeA.z; s3 = u_FadeA.w; s4 = u_FadeB.x; s5 = u_FadeB.y; s6 = u_FadeB.z; s7 = u_FadeB.w; // 0x8D8
            v1 = clamp(s0 * v7 + s4, 0.0, 1.0);                                      // 0x8E4
            v8 = clamp(v7 * s1 + s5, 0.0, 1.0);                                      // 0x8EC
            v7 = clamp(v0 * s2 + s6, 0.0, 1.0);                                      // 0x8F4
            v0 = clamp(v0 * s3 + s7, 0.0, 1.0);                                      // 0x8FC
            v1 = v1 * v8;                                                            // 0x904
            v1 = v1 * v7;                                                            // 0x908
            v1 = v1 * v0;                                                            // 0x90C
            v0 = v1 * v1;                                                            // 0x910
            v1 = -2.0 * v1 + 3.0;                                                    // 0x914
            v0 = v0 * v6;                                                            // 0x91C
            v6 = v0 * v1;                                                            // 0x920
        }
        v0 = v6 * v2;                                                            // 0x924
        v1 = v6 * v3;                                                            // 0x928
        v7 = v6 * v4;                                                            // 0x92C
        v5 = v6 * v5;                                                            // 0x934
        vcc = 0u != s8_u;                                                        // 0x938
        v2 = vcc ? v0 : v2;                                                      // 0x93C
        v3 = vcc ? v1 : v3;                                                      // 0x940
        v4 = vcc ? v7 : v4;                                                      // 0x944
    }
    vcc_hi = u_GlobalIntensity;                                              // 0x948
    v1 = vcc_hi;                                                             // 0x954
    if (0u != s8_u) {
        vcc_lo = u_GlobalAlpha;                                                  // 0x960
        v1 = vcc_lo * vcc_hi;                                                    // 0x96C
    }
    v0 = v1 * v2;                                                            // 0x974
    vcc_lo = u_GlobalAlpha;                                                  // 0x978
    v2 = v1 * v3;                                                            // 0x980
    v1 = v1 * v4;                                                            // 0x984
    v3 = vcc_lo * v5;                                                        // 0x98C
    out_mrt0 = vec4(v0, v2, v1, v3);   // v_cvt_pkrtz_f16_f32 v0,v0,v2 / v1,v1,v3 ; exp mrt0 compr (0x990..0x99C)
}
