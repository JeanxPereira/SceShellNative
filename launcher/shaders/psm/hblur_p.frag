#version 450
// Transcription of libScePsm embedded shader #9 "hblur_p" (work/shaders/09_hblur_p.ags.bin),
// AGC binary type 1 (pixel), code up to s_endpgm at 0x2C4. EmbeddedShader.BlurH = (blur_vv, hblur_p).
// Every statement maps to the RDNA instruction at the byte offset in its trailing comment; the four channel
// operations of one tap are written on one line (they are four independent instructions).
//
// Math (uv = TexCoord, b = BlurAmount, T(k) = texture(s_Texture, (uv.x + k*b, uv.y)), all four channels):
//   out = w0*T(-5) + w1*T(-4) + w2*T(-3) + w3*T(-2) + w4*T(-1) + w5*T(0) + w4*T(1) + w3*T(2) + w2*T(3) + w1*T(4)
//         + w0*T(5) + w6*(T(-6) + T(6)), accumulated in exactly that order
//   w0 = 0.048968751, w1 = 0.066379108, w2 = 0.084097967, w3 = 0.099582098, w4 = 0.11020942, w5 = 0.11399817,
//   w6 = 0.033763569. No u_Flag tail and no globalConstantBuffer.
//   Tap offsets: -5b = v_madmk(b, -5, u), -4b = v_mad(-4, b, u), -3b = v_madmk(b, -3, u), -2b = v_mad(-2, b, u),
//   -b = u - b, +b = u + b, 2b = v_mad(2, b, u), 3b = v_madmk(b, 3, u), 4b = v_mad(4, b, u), 5b/6b/-6b = v_madmk.
//
// Resources: s_Texture t0 -> s[0:7], Sampler0 -> s[8:11], __GLOBAL_CB__ slot 0 -> s[12:15] (4 bytes).
// Interpolants: ONE PS input, semantic 1 (SPI_PS_IN_CONTROL = 1): the ISA's attr0 is the vertex shader's param1
//   (blur_vv: TexCoord.xy / ClipPos.zw), declared at location 1.
//
// Not expressible / differences:
//   * v_mad/v_madmk are unfused (locals are `precise`).
//   * Output is packed to fp16 with round-toward-zero (v_cvt_pkrtz_f16_f32); the render target format decides here.

layout(set = 0, binding = 1, std140) uniform GLOBAL_CB {   // metadata "__GLOBAL_CB__", resource slot 0, 4 bytes
    layout(offset = 0) float BlurAmount;
};
// s_Texture (t0) with Sampler0 (s0).
layout(set = 0, binding = 3) uniform sampler2D s_Texture;

layout(location = 1) in vec4 in_attr0;   // ISA attr0 = PS input semantic 1 (VS param1): .xy input.TexCoord
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
    precise float v20, v21, v22, v23, v24, v25, v26, v27;
    float s16;
    vec4 t;
    s16 = BlurAmount;                                                        // 0x8
    v22 = s16;                                                               // 0x1C
    v23 = in_attr0.x;                                                        // 0x20/0x24
    v21 = in_attr0.y;                                                        // 0x28/0x2C
    v20 = v22 * uintBitsToFloat(0xC0A00000u) + v23;   // -5                  // v_madmk 0x30
    t = texture(s_Texture, vec2(v20, v21)); v16 = t.x; v17 = t.y; v18 = t.z; v19 = t.w;  // 0x38
    t = texture(s_Texture, vec2(v23, v21)); v0 = t.x; v1 = t.y; v2 = t.z; v3 = t.w;      // 0x40
    v20 = -4.0 * s16 + v23;                                                  // 0x4C
    t = texture(s_Texture, vec2(v20, v21)); v4 = t.x; v5 = t.y; v6 = t.z; v7 = t.w;      // 0x54
    v20 = v22 * uintBitsToFloat(0xC0400000u) + v23;   // -3                  // v_madmk 0x5C
    t = texture(s_Texture, vec2(v20, v21)); v8 = t.x; v9 = t.y; v10 = t.z; v11 = t.w;    // 0x64
    v20 = -2.0 * s16 + v23;                                                  // 0x6C
    t = texture(s_Texture, vec2(v20, v21)); v12 = t.x; v13 = t.y; v14 = t.z; v15 = t.w;  // 0x74
    v20 = v23 - s16;                                                         // 0x7C
    v24 = W0 * v16; v25 = W0 * v17; v26 = W0 * v19; v27 = W0 * v18;         // 0x84..0x9C
    t = texture(s_Texture, vec2(v20, v21)); v16 = t.x; v17 = t.y; v18 = t.z; v19 = t.w;  // 0xA4
    v20 = s16 + v23;                                                         // 0xAC
    v24 = v4 * W1 + v24; v25 = v5 * W1 + v25; v26 = v7 * W1 + v26; v27 = v6 * W1 + v27; // 0xB4..0xCC
    t = texture(s_Texture, vec2(v20, v21)); v4 = t.x; v5 = t.y; v6 = t.z; v7 = t.w;      // 0xD4
    v20 = 2.0 * s16 + v23;                                                   // 0xDC
    v24 = v8 * W2 + v24; v25 = v9 * W2 + v25; v26 = v11 * W2 + v26; v27 = v10 * W2 + v27; // 0xE8..0x100
    t = texture(s_Texture, vec2(v20, v21)); v8 = t.x; v9 = t.y; v10 = t.z; v11 = t.w;    // 0x108
    v20 = v22 * uintBitsToFloat(0x40400000u) + v23;   // 3                   // v_madmk 0x110
    v24 = v12 * W3 + v24; v25 = v13 * W3 + v25; v26 = v15 * W3 + v26; v27 = v14 * W3 + v27; // 0x11C..0x134
    t = texture(s_Texture, vec2(v20, v21)); v12 = t.x; v13 = t.y; v14 = t.z; v15 = t.w;  // 0x13C
    v20 = 4.0 * s16 + v23;                                                   // 0x144
    v24 = v16 * W4 + v24; v25 = v17 * W4 + v25; v26 = v19 * W4 + v26; v27 = v18 * W4 + v27; // 0x150..0x168
    t = texture(s_Texture, vec2(v20, v21)); v16 = t.x; v17 = t.y; v18 = t.z; v19 = t.w;  // 0x170
    v0 = v0 * W5 + v24;                                                      // 0x178
    v20 = v22 * uintBitsToFloat(0x40A00000u) + v23;   // 5                   // v_madmk 0x180
    v1 = v1 * W5 + v25; v3 = v3 * W5 + v26; v2 = v2 * W5 + v27;              // 0x188..0x198
    v4 = v4 * W4 + v0; v5 = v5 * W4 + v1; v7 = v7 * W4 + v3; v6 = v6 * W4 + v2;         // 0x1A4..0x1BC
    t = texture(s_Texture, vec2(v20, v21)); v0 = t.x; v1 = t.y; v2 = t.z; v3 = t.w;      // 0x1C4
    v20 = v22 * uintBitsToFloat(0x40C00000u) + v23;   // 6                   // v_madmk 0x1CC
    v8 = v8 * W3 + v4; v9 = v9 * W3 + v5; v11 = v11 * W3 + v7; v10 = v10 * W3 + v6;     // 0x1D8..0x1F0
    t = texture(s_Texture, vec2(v20, v21)); v4 = t.x; v5 = t.y; v6 = t.z; v7 = t.w;      // 0x1F8
    v20 = v22 * uintBitsToFloat(0xC0C00000u) + v23;   // -6                  // v_madmk 0x200
    v12 = v12 * W2 + v8; v13 = v13 * W2 + v9; v15 = v15 * W2 + v11; v14 = v14 * W2 + v10; // 0x20C..0x224
    t = texture(s_Texture, vec2(v20, v21)); v8 = t.x; v9 = t.y; v10 = t.z; v11 = t.w;    // 0x22C
    v12 = v16 * W1 + v12; v13 = v17 * W1 + v13; v15 = v19 * W1 + v15; v14 = v18 * W1 + v14; // 0x238..0x250
    v0 = v0 * W0 + v12; v1 = v1 * W0 + v13; v3 = v3 * W0 + v15; v2 = v2 * W0 + v14;     // 0x25C..0x274
    v4 = v8 + v4; v5 = v9 + v5; v7 = v11 + v7; v6 = v10 + v6;                // 0x280..0x28C
    v0 = v4 * W6 + v0; v1 = v5 * W6 + v1; v3 = v7 * W6 + v3; v2 = v6 * W6 + v2;         // 0x290..0x2A8
    out_mrt0 = vec4(v0, v1, v2, v3);   // v_cvt_pkrtz_f16_f32 v0,v0,v1 / v1,v2,v3 ; exp mrt0 compr (0x2B0..0x2BC)
}
