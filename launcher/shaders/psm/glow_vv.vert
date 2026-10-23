#version 450
// Transcription of libScePsm embedded shader #2 "glow_vv" (work/shaders/02_glow_vv.ags.bin),
// AGC binary type 2 (NGG/primitive-shader vertex stage), code up to s_endpgm at 0x108.
// EmbeddedShader.GlowH = (glow_vv, hglow_p), GlowV = (glow_vv, vglow_p), GlowR8V = (glow_vv, vglow_r8_p).
// Every statement maps to the RDNA instruction at the byte offset in its trailing comment.
//
// Math:  gl_Position = u_World * Position                  (column-major: dword k of the CB = [k/4][k%4])
//        out_param0  = Color * u_FilterColor                (main.Color)
//        out_param1  = (TexCoord.xy, gl_Position.xy * (1/w)) (main.TexCoord, main.ClipPos)
//   (the same instruction sequence as basic_vv, with u_World / u_FilterColor in place of u_WorldMatrix / u_Color)
//
// Resources: __GLOBAL_CB__ CB slot 0 -> s[8:11] (user-data dword 0), 80 bytes; vertex-buffer table pointer
//   (direct resource 8) -> s[12:13], entries 0/1/2 = POSITION/COLOR/TEXCOORD, as basic_vv.
//
// Not expressible / differences: as basic_vv.vert (NGG plumbing omitted; v_rcp_f32 1 ulp vs GLSL 1.0/x;
// v_mad/v_mac unfused -> `precise`; PS5 clip-space convention handled by the host viewport).

layout(set = 0, binding = 0, std140) uniform GLOBAL_CB {   // metadata "__GLOBAL_CB__", 80 bytes
    layout(offset = 0)  mat4 u_World;
    layout(offset = 64) vec4 u_FilterColor;
};

layout(location = 0) in vec4 in_Position;   // input.Position  POSITION  (vertex-buffer table entry 0)
layout(location = 1) in vec4 in_Color;      // input.Color     COLOR     (entry 1)
layout(location = 2) in vec2 in_TexCoord;   // input.TexCoord  TEXCOORD  (entry 2, buffer_load_format_xy)

layout(location = 0) out vec4 out_param0;   // exp param0: main.Color (output semantic 0)
layout(location = 1) out vec4 out_param1;   // exp param1: .xy main.TexCoord, .zw main.ClipPos (output semantic 1)

void main()
{
    precise float v0, v1, v2, v3, v4, v5, v6, v7, v8, v9, v10, v11, v12, v13;
    float s12, s13, s14, s15, s16, s17, s18, s19, s20, s21, s22, s23, s24, s25, s26, s27, s32, s33, s34, s35;
    s12 = u_World[0][0]; s13 = u_World[0][1]; s14 = u_World[0][2]; s15 = u_World[0][3]; s16 = u_World[1][0]; s17 = u_World[1][1]; s18 = u_World[1][2]; s19 = u_World[1][3]; s20 = u_World[2][0]; s21 = u_World[2][1]; s22 = u_World[2][2]; s23 = u_World[2][3]; s24 = u_World[3][0]; s25 = u_World[3][1]; s26 = u_World[3][2]; s27 = u_World[3][3]; // 0x50
    s32 = u_FilterColor.x; s33 = u_FilterColor.y; s34 = u_FilterColor.z; s35 = u_FilterColor.w; // 0x58
    v6 = in_Position.x; v7 = in_Position.y; v8 = in_Position.z; v9 = in_Position.w; // 0x64
    v0 = in_Color.x; v1 = in_Color.y; v2 = in_Color.z; v3 = in_Color.w;      // 0x6C
    v4 = in_TexCoord.x; v5 = in_TexCoord.y;                                  // 0x74
    v11 = s12 * v6;                                                          // 0x80
    v12 = s15 * v6;                                                          // 0x84
    v10 = s14 * v6;                                                          // 0x88
    v6 = s13 * v6;                                                           // 0x8C
    v0 = s32 * v0;                                                           // 0x94
    v13 = s16 * v7 + v11;                                                    // 0x98
    v11 = v7 * s19 + v12;                                                    // 0xA0
    v10 = s18 * v7 + v10;                                                    // 0xA8
    v12 = v7 * s17 + v6;                                                     // 0xAC
    v3 = s35 * v3;                                                           // 0xB4
    v13 = s20 * v8 + v13;                                                    // 0xB8
    v11 = s23 * v8 + v11;                                                    // 0xBC
    v10 = s22 * v8 + v10;                                                    // 0xC0
    v12 = s21 * v8 + v12;                                                    // 0xC4
    v2 = s34 * v2;                                                           // 0xC8
    v13 = s24 * v9 + v13;                                                    // 0xCC
    v11 = s27 * v9 + v11;                                                    // 0xD0
    v10 = s26 * v9 + v10;                                                    // 0xD4
    v12 = s25 * v9 + v12;                                                    // 0xD8
    v1 = s33 * v1;                                                           // 0xDC
    v9 = 1.0 / v11;                                                          // 0xE0
    gl_Position = vec4(v13, v12, v10, v11);                                  // exp pos0 0xE4
    v6 = v9 * v13;                                                           // 0xEC
    v7 = v9 * v12;                                                           // 0xF0
    out_param1 = vec4(v4, v5, v6, v7);                                       // 0xF8
    out_param0 = vec4(v0, v1, v2, v3);                                       // 0x100
}
