#version 450
// Transcription of ShellUI eboot (NPXS40087 eboot.bin, FW 4.03) embedded shader #67 "light_p"
// (table entry 0xCC9C28, blob 0xD99F60, 4688 bytes; work/ida_bglayer/shaders/067_light_p.bin),
// AGC binary type 1 (pixel), .shader_text 0x9A0 bytes, code up to s_endpgm at 0x848.
// Drawn by the BackgroundLayer "Light" layer (render sub_9E7E0) as a full-screen triangle with
// basic_vv, primitive type 6, after the XF blend state {enable, Add, SrcAlpha, OneMinusSrcAlpha}
// for colour and alpha (sub_70DD0: sub_DF7A0/DF7B0/DF7C0 + sub_DE620).
//
// Every statement maps to the RDNA instruction at the byte offset in its trailing comment.
// Register names are kept (r = v23, g = v10/v9, b = v12/v8 accumulate the colour).
//
// Resources (metadata / user data): t0 texFloor -> s[0:7], t1 texVolume -> s[8:15],
// t2 texP -> s[16:23], b0 VolatileCB -> s[24:27], b1 ColorCb -> s[28:31]. There are no sampler
// resources: the shader builds its S# immediates itself:
//   texFloor  s[36:39] = {0x1B6, 0x00FFF000, 0x05500000, 0}: clamp-to-border (transparent black) on
//             x/y/z, bilinear min/mag, point mip, LOD 0..15.99
//   texVolume s[60:63] = {0x30, 0x00FFF000, 0x05500000, 0}: x wrap, y clamp-to-border, same filters
//   texP      s[0:3]   = {0x92, 0, 0x02500000, 0}: clamp-to-last-texel on x/y/z, bilinear, no mip
// The host creates the matching VkSamplers (psmhost/eboot/bglayer_gpu.cpp).
//
// Not expressible / differences:
//   * s_memrealtime (0x774) reads the GPU's 64-bit real-time counter; the low 23 bits seed the output
//     dither. GLSL has no portable equivalent without VK_KHR_shader_clock; the host passes the counter
//     it samples once per draw (push constant `realtime`), so all pixels of a frame share one value
//     where the hardware value advances while the waves run. The counter's frequency on the PS5 is
//     not traced (the host uses a 100 MHz scale).
//   * v_rcp_f32 / v_rsq_f32 / v_sqrt_f32 / v_log_f32 / v_exp_f32 are ~1 ulp approximations; written as
//     the exact GLSL operations. v_sin_f32 / v_cos_f32 take revolutions (x * 2pi radians).
//   * v_mad_f32 / v_mac_f32 / v_madak / v_madmk are unfused multiply-add (locals are `precise` so the
//     GLSL compiler does not fuse them); v_fma_f32 is written as fma().
//   * The output is packed to fp16 with round-toward-zero (v_cvt_pkrtz_f16_f32); the host render
//     target (R8G8B8A8_UNORM) decides the final precision.
//   * The two light blocks run under s_and_saveexec (u < 1.2 v): the image_sample inside them uses
//     implicit derivatives of non-uniform control flow, undefined in GLSL as on the hardware for
//     helper lanes; the IES textures have a single mip level, so the LOD does not matter.

layout(location = 0) in vec2 inUv;   // attr0, metadata "input.Uv" TEXCOORD0

layout(set = 0, binding = 0, std140) uniform VolatileCB {   // b0, 16 bytes
    float time;            // +0
    float opacity;         // +4
    float intensity;       // +8
    float particleAlpha;   // +12
};
layout(set = 0, binding = 1, std140) uniform ColorCb {      // b1, 108 bytes
    vec4  lightCol;          // +0
    vec4  lightColOnFloor;   // +16
    vec4  light2Col;         // +32 (unused by the code)
    vec4  light2ColOnFloor;  // +48
    vec4  pointLightCol;     // +64
    vec4  pointLightAmbCol;  // +80
    float gamma;             // +96
    float gintensity;        // +100
    float noise;             // +104
};
layout(set = 0, binding = 2) uniform sampler2D texFloor;    // t0
layout(set = 0, binding = 3) uniform sampler2D texVolume;   // t1
layout(set = 0, binding = 4) uniform sampler2D texP;        // t2

layout(push_constant) uniform Realtime { uint realtime; };  // s_memrealtime substitute (see header)

layout(location = 0) out vec4 outColor;   // mrt0

const float TWO_PI = 6.28318530717958647692;

void main() {
    precise float v2 = inUv.x;                                  // 0x4C/0x54 v_interp attr0.x
    precise float v3 = inUv.y;                                  // 0x50/0x58 v_interp attr0.y
    float s56 = time;                                     // 0x30 s_buffer_load_dword s56, b0+0
    // 0x38 s[40:43] = ColorCb+0x50 (pointLightAmbCol)
    precise float v0 = fma(v2, 0.0433565751, -0.0216782875);   // 0x5C
    precise float v5 = fma(v3, -0.465352088, 0.232676044);     // 0x68
    precise float v7 = fma(v2, 0.827292621, -0.413646311);     // 0x74
    precise float v6 = 8.75;                                    // 0x80
    precise float v1 = fma(v3, 0.0243880711, -0.0121940356);   // 0x88
    precise float v4 = 1.20000005 * v3;                         // 0x94
    v0 = v0 + v5;                                               // 0x9C
    v5 = -0.5 + v3;                                             // 0xA0
    precise float v9 = 0.239999995 * s56;                       // 0xA8
    v1 = v7 + v1;                                               // 0xB0
    bool s50 = v2 < v4;                                   // 0xB4 v_cmp_lt_f32 s50, v2, v4
    v7 = v6 * v0 + -1.25;                                       // 0xBC v_madak
    v6 = 1.0 / (-v0);                                           // 0xC4 v_rcp_f32 v6, -v0
    v4 = -0.5 + v2;                                             // 0xCC
    precise float r = pointLightAmbCol.x;                       // 0xD0 v23 = s40
    precise float g = pointLightAmbCol.y;                       // 0xD4 v10 = s41
    precise float b = pointLightAmbCol.z;                       // 0xDC v12 = s42
    precise float v8 = v7 * v7;                                 // 0xD8
    v8 = v0 * 42.75 + v8;                                       // 0xF4 v_madmk
    v8 = sqrt(v8);                                              // 0x104
    v7 = -v7 + -v8;                                             // 0x108
    v7 = v6 * v7;                                               // 0x110
    v6 = v1 * v7;                                               // 0x114
    v8 = v0 * v7;                                               // 0x118
    // v6 = x, v8 = y, v7 = t of the view ray; used by both light blocks and the tail.

    if (s50) {   // 0x11C s_and_saveexec_b64 s[52:53], s[50:51] / 0x120 s_cbranch_execz
        precise float a1 = 2.64199996 + v6;                     // 0x124
        precise float a10 = fma(v8, -0.816496611, 3.15004396);  // 0x13C
        precise float a11 = fma(v6, -0.408248305, -1.07859206); // 0x148
        precise float a13 = a1 * a1;                            // 0x164
        precise float a0 = fma(-v7, 0.408248305, 4.74952793);   // 0x168
        a11 = a11 + a10;                                        // 0x170
        a10 = -3.85800004 + v8;                                 // 0x174
        precise float a12 = 11.6339197 - v7;                    // 0x18C
        a0 = a11 + a0;                                          // 0x194
        precise float a14 = a10 * a10 + a13;                    // 0x198 v_mad_f32
        a11 = 0.0208349992;                                     // 0x1A0
        // 0x1B0 s[44:47] = ColorCb+0x10 (lightColOnFloor)
        a1 = -0.408248305 * a0 - a1;                            // 0x1B8 v_mad_f32 v1, vcc_lo, v0, -v1
        a10 = -0.816496611 * a0 - a10;                          // 0x1C0 v_mad_f32 v10, vcc_hi, v0, -v10
        a14 = a12 * a12 + a14;                                  // 0x1C8 v_mac
        precise float a15 = -0.89442718 * a1;                   // 0x1CC
        a12 = -0.89442718 * a10;                                // 0x1D4
        a13 = inversesqrt(a14);                                 // 0x1DC
        a12 = a1 * -0.44721359 + a12;                           // 0x1E0 v_madmk
        a1 = a10 * 0.44721359 + a15;                            // 0x1E8 v_madmk
        a15 = -abs(a13) * abs(a0) + 1.0;                        // 0x1F0
        a0 = a13 * a0;                                          // 0x1F8
        a10 = 1.0 / a14;                                        // 0x1FC
        a14 = max(abs(a12), abs(a1));                           // 0x200
        a13 = min(abs(a12), abs(a1));                           // 0x208
        precise float a18 = min(a12, a1);                       // 0x210
        precise float a17 = a15 * a15;                          // 0x214
        bool vcc = abs(a1) < abs(a12);                          // 0x218
        precise float a19 = 1.0 / a14;                          // 0x220
        a14 = -0.117851131 * a15;                               // 0x224
        a12 = max(a12, a1);                                     // 0x22C
        precise float a16 = -0.000988719054 * a17;              // 0x230
        bool s54 = a18 < -a18;                            // 0x238
        a13 = a19 * a13;                                        // 0x240
        a18 = a17 * a17;                                        // 0x244
        a19 = a17 * -0.0265165046 + a14;                        // 0x248 v_madmk
        a17 = a15 * a17;                                        // 0x250
        a14 = sqrt(a15);                                        // 0x254
        a15 = a13 * a13;                                        // 0x258
        a19 = a18 * -0.00268540974 + a19;                       // 0x25C
        a16 = a17 * -0.000383445527 + a16;                      // 0x264
        precise float a20 = a11 * a15 + -0.0851330012;          // 0x26C v_madak
        a19 = -1.41421354 + a19;                                // 0x274
        a16 = a18 * -0.000154291178 + a16;                      // 0x27C
        a18 = a20 * a15 + 0.180141002;                          // 0x284
        a16 = -0.00789181702 + a16;                             // 0x28C
        a18 = a18 * a15 + -0.33029899;                          // 0x294
        a19 = a16 * a17 + a19;                                  // 0x29C v_mac
        a15 = a18 * a15 + 0.999866009;                          // 0x2A0
        a11 = a14 * a19 + 3.14159274;                           // 0x2A8
        a14 = a14 * a19;                                        // 0x2B0
        a17 = a13 * a15;                                        // 0x2B4
        a16 = -2.0 * a17 + 1.57079637;                          // 0x2B8
        a16 = vcc ? a16 : 0.0;                                  // 0x2C0 v_cndmask_b32 v16, 0, v16, vcc
        vcc = 0.0 > a1;                                         // 0x2C4
        a1 = vcc ? -3.14159274 : 0.0;                           // 0x2C8 v_cndmask v1, 0, s43(-pi), vcc
        vcc = a12 >= -a12;                                      // 0x2D0
        a1 = a16 + a1;                                          // 0x2D8
        vcc = s54 && vcc;                                       // 0x2DC s_and_b64 vcc, s[54:55], vcc
        a1 = a15 * a13 + a1;                                    // 0x2E0 v_mac
        a1 = vcc ? -a1 : a1;                                    // 0x2E4
        vcc = 0.0 >= a0;                                        // 0x2EC
        a0 = a1 + v9;                                           // 0x2F0
        a11 = vcc ? a11 : -a14;                                 // 0x2F4 v_cndmask v11, -v14, v11, vcc
        a1 = 0.159154937 * a0;                                  // 0x2FC
        a0 = 0.318309873 * a11;                                 // 0x304
        a11 = cos(TWO_PI * a1);                                 // 0x30C v_cos_f32
        a1 = sin(TWO_PI * a1);                                  // 0x310 v_sin_f32
        a1 = a1 * a0 + 0.5;                                     // 0x314
        a0 = a11 * a0 + 0.5;                                    // 0x31C
        a11 = texture(texFloor, vec2(a0, a1)).x;                // 0x324 image_sample dmask 0x1
        a0 = a11 * a11;                                         // 0x330
        a0 = (a0 * a10) * 2.0;                                  // 0x334 v_mul_f32 mul:2
        r = lightColOnFloor.x * a0 + r;                         // 0x340 v_mad_f32 v23, s44, v0, s40
        g = lightColOnFloor.y * a0 + g;                         // 0x348
        b = lightColOnFloor.z * a0 + b;                         // 0x350
    }                                                           // 0x358 exec restored

    if (s50) {   // 0x35C s_and_saveexec_b64 s[44:45], s[50:51] / 0x360 s_cbranch_execz
        precise float c0 = 1.36453998 + v6;                     // 0x364
        precise float c13 = fma(v8, -0.79975009, 3.4022007);    // 0x37C
        precise float c14 = fma(v6, -0.599812567, -0.818468213);// 0x388
        precise float c16 = -4.25407982 + v8;                   // 0x3A4
        precise float c1 = fma(-v7, 0.0249921903, 0.0837828144);// 0x3AC
        precise float c17 = c14 + c13;                          // 0x3B4
        precise float c11 = c0 * c0;                            // 0x3B8
        c13 = 3.35235977 - v7;                                  // 0x3BC
        c14 = 0.0208349992;                                     // 0x3C4
        c1 = c17 + c1;                                          // 0x3D4
        c11 = c16 * c16 + c11;                                  // 0x3D8 v_mac
        // 0x3DC s[40:43] = ColorCb+0x30 (light2ColOnFloor)
        precise float c15 = c1 * 0.599812567 + c0;              // 0x3E4 v_madmk
        c0 = c1 * 0.79975009 + c16;                             // 0x3EC v_madmk
        c11 = c13 * c13 + c11;                                  // 0x3F4 v_mac
        precise float c18 = -0.799999952 * c15;                 // 0x3F8
        c17 = -0.799999952 * c0;                                // 0x400
        c16 = inversesqrt(c11);                                 // 0x408
        c13 = 1.0 / c11;                                        // 0x40C
        c0 = c0 * 0.599999964 + c18;                            // 0x410 v_madmk
        c15 = c15 * -0.599999964 + c17;                         // 0x418 v_madmk
        c18 = -abs(c16) * abs(c1) + 1.0;                        // 0x420
        c1 = c16 * c1;                                          // 0x428
        precise float c19 = max(abs(c15), abs(c0));             // 0x42C
        c16 = min(abs(c15), abs(c0));                           // 0x434
        precise float c20 = min(c15, c0);                       // 0x43C
        bool vcc = abs(c0) < abs(c15);                          // 0x440
        c17 = -0.117851131 * c18;                               // 0x448
        precise float c21 = 1.0 / c19;                          // 0x450
        c19 = c18 * c18;                                        // 0x454
        c15 = max(c15, c0);                                     // 0x458
        bool s46 = c20 < -c20;                            // 0x45C
        c16 = c21 * c16;                                        // 0x464
        c21 = -0.000988719054 * c19;                            // 0x468
        c20 = c19 * c19;                                        // 0x470
        precise float c22 = c19 * -0.0265165046 + c17;          // 0x474
        c19 = c18 * c19;                                        // 0x47C
        c17 = sqrt(c18);                                        // 0x480
        c18 = c16 * c16;                                        // 0x484
        c22 = c20 * -0.00268540974 + c22;                       // 0x488
        c21 = c19 * -0.000383445527 + c21;                      // 0x490
        c14 = c14 * c18 + -0.0851330012;                        // 0x498 v_madak
        c11 = -1.41421354 + c22;                                // 0x4A0
        c20 = c20 * -0.000154291178 + c21;                      // 0x4A8
        c14 = c14 * c18 + 0.180141002;                          // 0x4B0
        c20 = -0.00789181702 + c20;                             // 0x4B8
        c14 = c14 * c18 + -0.33029899;                          // 0x4C0
        c11 = c20 * c19 + c11;                                  // 0x4C8 v_mac
        c18 = c14 * c18 + 0.999866009;                          // 0x4CC
        c14 = c17 * c11 + 3.14159274;                           // 0x4D4
        c17 = c17 * c11;                                        // 0x4DC
        c20 = c16 * c18;                                        // 0x4E0
        c19 = -2.0 * c20 + 1.57079637;                          // 0x4E4
        c19 = vcc ? c19 : 0.0;                                  // 0x4EC
        vcc = 0.0 > c0;                                         // 0x4F0
        c0 = vcc ? -3.14159274 : 0.0;                           // 0x4F4 v_cndmask v0, 0, s50(-pi), vcc
        vcc = c15 >= -c15;                                      // 0x4FC
        c0 = c19 + c0;                                          // 0x504
        vcc = s46 && vcc;                                       // 0x508
        c0 = c18 * c16 + c0;                                    // 0x50C v_mac
        c0 = vcc ? -c0 : c0;                                    // 0x510
        vcc = 0.0 >= c1;                                        // 0x518
        c0 = v9 * -0.699999988 + c0;                            // 0x51C v_madmk v0, v9, K, v0
        precise float c9 = vcc ? c14 : -c17;                    // 0x524 v_cndmask v9, -v17, v14, vcc
        c1 = 0.159154937 * c0;                                  // 0x52C
        c0 = 0.636619747 * c9;                                  // 0x534
        c9 = sin(TWO_PI * c1);                                  // 0x53C v_sin_f32
        c1 = cos(TWO_PI * c1);                                  // 0x540 v_cos_f32
        c9 = c9 * c0;                                           // 0x544
        c0 = c1 * c0;                                           // 0x548
        c1 = 0.5 * c9 + 0.5;                                    // 0x54C
        c0 = 0.5 * c0 + 0.5;                                    // 0x554
        c9 = texture(texFloor, vec2(c0, c1)).x;                 // 0x55C image_sample dmask 0x1
        c0 = c9 * c9;                                           // 0x568
        c0 = c0 * c13;                                          // 0x56C
        b = light2ColOnFloor.z * c0 + b;                        // 0x574 v_mac v12, s42, v0
        g = light2ColOnFloor.y * c0 + g;                        // 0x578 v_mac v10, s41, v0
        r = light2ColOnFloor.x * c0 + r;                        // 0x57C v_mac v23, s40, v0
    }                                                           // 0x580 exec restored

    precise float d0 = 3.55555558;                              // 0x58C
    precise float d13 = 2.0 * v3 + 0.449999988;                 // 0x594 v_madak
    v6 = 3.0 + v6;                                              // 0x59C
    precise float d1 = 0.150000006 + v8;                        // 0x5A4
    precise float d8 = d0 * v2 + -0.427778006;                  // 0x5B4 v_madak
    precise float d11 = 0.318309873;                            // 0x5BC
    d0 = 0.0381971821 * s56;                                    // 0x5C4
    bool vcc = -0.224999994 > v3;                               // 0x5CC
    precise float d16 = v6 * v6;                                // 0x5D4
    precise float d15 = max(abs(d8), abs(d13));                 // 0x5D8
    precise float d6 = min(abs(d8), abs(d13));                  // 0x5E0
    bool s38 = abs(d13) < abs(d8);                        // 0x5E8
    precise float d9 = d8 * d8;                                 // 0x5F0
    precise float d14 = 12.5 - v7;                              // 0x5F4
    d15 = 1.0 / d15;                                            // 0x5FC
    precise float d7 = 0.0208349992;                            // 0x600
    d16 = d1 * d1 + d16;                                        // 0x608 v_mac
    d1 = vcc ? -3.14159274 : 0.0;                               // 0x60C
    d9 = d13 * d13 + d9;                                        // 0x614 v_mac
    d6 = d15 * d6;                                              // 0x618
    d15 = min(d8, d13);                                         // 0x61C
    d8 = max(d8, d13);                                          // 0x620
    d16 = d14 * d14 + d16;                                      // 0x624 v_mac
    // 0x628 s[0:3] = ColorCb+0x40 (pointLightCol)
    d13 = d6 * d6;                                              // 0x630
    bool s40 = d15 < -d15;                                // 0x634
    vcc = d8 >= -d8;                                            // 0x63C
    d15 = 0.00999999978 + d9;                                   // 0x644
    d14 = 1.0 / d16;                                            // 0x64C
    // 0x650 s[4:7] = ColorCb+0x00 (lightCol); 0x658 s[36:37] = VolatileCB+8 (intensity, particleAlpha)
    bool s32 = s40 && vcc;                                // 0x660
    precise float G = pointLightCol.y * d14 + g;                // 0x670 v_mad_f32 v9, s1, v14, v10
    precise float d10 = 1.0 / d15;                              // 0x678
    precise float B = pointLightCol.z * d14 + b;                // 0x67C v_mad_f32 v8, s2, v14, v12
    precise float d12 = d7 * d13 + -0.0851330012;               // 0x684 v_madak
    r = pointLightCol.x * d14 + r;                              // 0x68C v_mac v23, s0, v14
    bool usesP = particleAlpha > 0.00999999978;           // 0x690 v_cmp_gt (uniform: both scalar)
    d12 = d12 * d13 + 0.180141002;                              // 0x698
    d12 = d12 * d13 + -0.33029899;                              // 0x6A0
    d12 = d12 * d13 + 0.999866009;                              // 0x6A8
    d13 = d6 * d12;                                             // 0x6B0
    d13 = -2.0 * d13 + 1.57079637;                              // 0x6B4
    d13 = s38 ? d13 : 0.0;                                      // 0x6BC v_cndmask_b32_e64 v13, 0, v13, s38
    d1 = d13 + d1;                                              // 0x6C4
    d1 = d12 * d6 + d1;                                         // 0x6C8 v_mac
    d1 = s32 ? -d1 : d1;                                        // 0x6CC
    d1 = d11 * d1 + 0.449999988;                                // 0x6D4 v_madak
    d6 = texture(texVolume, vec2(d0, d1)).x;                    // 0x6DC image_sample dmask 0x1
    d0 = d6 * d6;                                               // 0x6E8
    d1 = d0 * d10;                                              // 0x6EC
    r = lightCol.x * d1 + r;                                    // 0x6F0 v_mac v23, s4, v1
    G = lightCol.y * d1 + G;                                    // 0x6F4 v_mac v9, s5, v1
    B = lightCol.z * d1 + B;                                    // 0x6F8 v_mac v8, s6, v1
    precise float oR = intensity * r;                           // 0x6FC v6 = s36 * v23
    precise float oB = intensity * B;                           // 0x700 v7 = s36 * v8
    precise float oG = intensity * G;                           // 0x704 v8 = s36 * v9

    if (usesP) {   // 0x708 s_cbranch_vccz
        precise float p0 = v4 * 0.00150000001 + v2;             // 0x70C v_madmk
        precise float p1 = v5 * 0.00150000001 + v3;             // 0x71C v_madmk
        precise float p4 = 0.00150000001 * -v4 + v2;            // 0x724 v_mad_f32 v4, vcc_lo, -v4, v2
        precise float p5 = 0.00150000001 * -v5 + v3;            // 0x72C
        precise float tr = texture(texP, vec2(p0, p1)).x;       // 0x744 dmask 0x1
        precise float tb = texture(texP, vec2(p4, p5)).z;       // 0x74C dmask 0x4
        precise float tg = texture(texP, vec2(v2, v3)).y;       // 0x754 dmask 0x2
        oR = particleAlpha * tr + oR;                           // 0x760 v_mac v6, s37, v9
        oB = particleAlpha * tb + oB;                           // 0x768 v_mac v7, s37, v0
        oG = particleAlpha * tg + oG;                           // 0x770 v_mac v8, s37, v1
    }

    precise float e2 = 12.9898005 * v2;                         // 0x77C
    uint rt = realtime & 0x7fffffu;                       // 0x774 s_memrealtime / 0x788 s_and_b32
    precise float e4 = log2(abs(oB));                           // 0x790 v_log_f32 v4, |v7|
    precise float e1 = float(rt);                               // 0x798 v_cvt_f32_u32
    precise float e0 = log2(abs(oG));                           // 0x79C v_log_f32 v0, |v8|
    e2 = v3 * 78.2330017 + e2;                                  // 0x7A4 v_madmk
    precise float e3 = log2(abs(oR));                           // 0x7AC v_log_f32 v3, |v6|
    // 0x7B4 vcc = ColorCb+0x60 {gamma, gintensity}; 0x7BC s0 = noise; 0x7C4 s1 = opacity
    e1 = e1 * 1.19209308e-05 + e2;                              // 0x7CC v_madmk
    precise float e5 = gamma * e3;                              // 0x7D8
    e2 = gamma * e0;                                            // 0x7DC
    e3 = gamma * e4;                                            // 0x7E0
    e0 = gintensity * opacity;                                  // 0x7E4
    e4 = 0.159154937 * e1;                                      // 0x7EC
    e1 = exp2(e5);                                              // 0x7F4
    e2 = exp2(e2);                                              // 0x7F8
    e3 = exp2(e3);                                              // 0x7FC
    e4 = sin(TWO_PI * e4);                                      // 0x800 v_sin_f32
    e4 = 43758.5469 * e4;                                       // 0x804
    e4 = fract(e4);                                             // 0x80C
    e4 = 2.0 * e4 + -1.0;                                       // 0x810
    e4 = noise * e4;                                            // 0x818
    e1 = e0 * e1 + e4;                                          // 0x81C
    e2 = e0 * e2 + e4;                                          // 0x824
    e4 = e3 * e0 + e4;                                          // 0x82C v_mac
    outColor = vec4(e1, e2, e4, opacity);                       // 0x830/0x834 pkrtz, 0x840 exp mrt0 compr
}
