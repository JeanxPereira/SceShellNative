#version 450
// Transcription of libScePsm embedded shader #11 "hglow_p" (work/shaders/11_hglow_p.ags.bin),
// AGC binary type 1 (pixel), code up to s_endpgm at 0x180. EmbeddedShader.GlowH = (glow_vv, hglow_p).
// Every statement maps to the RDNA instruction at the byte offset in its trailing comment.
//
// Math (c = Color = attr0, uv = TexCoord = attr1.xy, b = BlurAmount, A(k) = texture(s_Texture, (uv.x + o_k, uv.y)).a,
//       o_k built incrementally: 3b = 2*b + b, 4b = b + 3b, 5b = b + 4b, 6b = b + 5b, -2b = 2*(-b) + u):
//   s = g1*(A(-1)+A(1)) ; s = (A(-2)+A(2))*g2 + s ; s = (A(-3)+A(3))*g3 + s ; s = (A(-4)+A(4))*g4 + s
//   s = (A(-5)+A(5))*g5 + s ; s = (A(-6)+A(6))*g6 + s ; s = A(0)*g0 + s
//   g0 = 0.16100337, g1 = 0.14862485, g2 = 0.11691244, g3 = 0.078368761, g4 = 0.04476494, g5 = 0.021789441,
//   g6 = 0.0090378895
//   out = (c.r, c.g, c.b, c.a * s).  No u_Flag tail, no globalConstantBuffer.
//
// Resources: s_Texture t0 -> s[0:7], Sampler0 -> s[8:11], __GLOBAL_CB__ slot 0 -> s[12:15] (4 bytes).
// Interpolants: PS input semantics 0,1 (attr0 = main.Color, attr1 = TexCoord/ClipPos).
//
// Not expressible / differences:
//   * image_sample dmask 0x8 returns the 4th component after the T# swizzle; written texture(...).a.
//   * v_mad/v_madmk are unfused (locals are `precise`).
//   * Output is packed to fp16 with round-toward-zero (v_cvt_pkrtz_f16_f32); the render target format decides here.

layout(set = 0, binding = 1, std140) uniform GLOBAL_CB {   // metadata "__GLOBAL_CB__", resource slot 0, 4 bytes
    layout(offset = 0) float BlurAmount;
};
// s_Texture (t0) with Sampler0 (s0).
layout(set = 0, binding = 3) uniform sampler2D s_Texture;

layout(location = 0) in vec4 in_attr0;   // PS attr0 = input semantic 0: input.Color (COLOR)
layout(location = 1) in vec4 in_attr1;   // PS attr1 = input semantic 1: .xy input.TexCoord
layout(location = 0) out vec4 out_mrt0;  // exp mrt0 (SPI_SHADER_COL_FORMAT = 4: packed fp16, v_cvt_pkrtz_f16_f32 = round toward zero)

void main()
{
    precise float v0, v1, v2, v3, v4, v5, v6, v7, v8, v9, v10, v11, v12, v13, v14, v15, v16, v17, v18;
    precise float c_r, c_g;
    float s16;
    s16 = BlurAmount;                                                        // 0x8
    v11 = in_attr1.x;                                                        // 0x18/0x1C
    v2 = s16 + v11;                                                          // 0x24
    v9 = 2.0 * s16 + s16;                                                    // 0x28
    v3 = in_attr1.y;                                                         // 0x30/0x34
    v8 = texture(s_Texture, vec2(v2, v3)).a;                                 // 0x3C
    v5 = texture(s_Texture, vec2(v11, v3)).a;                                // 0x44
    v2 = v11 - s16;                                                          // 0x50
    v10 = s16 + v9;                                                          // 0x54
    v14 = in_attr0.x;                                                        // 0x38/0x58
    v6 = in_attr0.w;                                                         // 0x5C/0x60
    v12 = texture(s_Texture, vec2(v2, v3)).a;                                // 0x64
    v2 = 2.0 * s16 + v11;                                                    // 0x6C
    v7 = s16 + v10;                                                          // 0x74
    v4 = in_attr0.z;                                                         // 0x78/0x80
    v0 = in_attr0.y;                                                         // 0x7C/0x98
    v13 = texture(s_Texture, vec2(v2, v3)).a;                                // 0x84
    v2 = 2.0 * -s16 + v11;                                                   // 0x8C
    v17 = s16 + v7;                                                          // 0x94
    c_r = v14; c_g = v0;                                                     // v_cvt_pkrtz_f16_f32 v0, v14, v0 0x9C
    v18 = texture(s_Texture, vec2(v2, v3)).a;                                // 0xA0
    v2 = v11 + v9;                                                           // 0xA8
    v14 = texture(s_Texture, vec2(v2, v3)).a;                                // 0xAC
    v2 = v11 - v9;                                                           // 0xB4
    v16 = texture(s_Texture, vec2(v2, v3)).a;                                // 0xB8
    v2 = v11 + v10;                                                          // 0xC0
    v9 = texture(s_Texture, vec2(v2, v3)).a;                                 // 0xC4
    v2 = v11 - v10;                                                          // 0xCC
    v15 = texture(s_Texture, vec2(v2, v3)).a;                                // 0xD0
    v2 = v11 + v7;                                                           // 0xD8
    v1 = texture(s_Texture, vec2(v2, v3)).a;                                 // 0xDC
    v2 = v11 - v7;                                                           // 0xE4
    v10 = texture(s_Texture, vec2(v2, v3)).a;                                // 0xE8
    v2 = v11 + v17;                                                          // 0xF0
    v7 = texture(s_Texture, vec2(v2, v3)).a;                                 // 0xF4
    v2 = v11 - v17;                                                          // 0xFC
    v11 = texture(s_Texture, vec2(v2, v3)).a;                                // 0x100
    v2 = v12 + v8;                                                           // 0x10C
    v3 = v18 + v13;                                                          // 0x114
    v2 = uintBitsToFloat(0x3E18311Du) * v2;                                  // 0x118
    v2 = v3 * uintBitsToFloat(0x3DEF6FCAu) + v2;                             // v_madmk 0x120
    v3 = v16 + v14;                                                          // 0x12C
    v1 = v10 + v1;                                                           // 0x134
    v2 = v3 * uintBitsToFloat(0x3DA07FCDu) + v2;                             // v_madmk 0x138
    v3 = v15 + v9;                                                           // 0x140
    v2 = v3 * uintBitsToFloat(0x3D375B71u) + v2;                             // v_madmk 0x144
    v3 = v11 + v7;                                                           // 0x150
    v1 = v1 * uintBitsToFloat(0x3CB27FC5u) + v2;                             // v_madmk 0x154
    v1 = v3 * uintBitsToFloat(0x3C1413A8u) + v1;                             // v_madmk 0x15C
    v1 = v5 * uintBitsToFloat(0x3E24DE11u) + v1;                             // v_madmk 0x164
    v1 = v6 * v1;                                                            // 0x16C
    out_mrt0 = vec4(c_r, c_g, v4, v1);   // v_cvt_pkrtz_f16_f32 v0,v14,v0 (0x9C) / v1,v4,v1 (0x170) ; exp mrt0 compr 0x178
}
