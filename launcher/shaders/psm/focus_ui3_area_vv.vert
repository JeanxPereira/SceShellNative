#version 450
// Transcription of libScePsm embedded shader #5 "focus_ui3_area_vv" (work/shaders/05_focus_ui3_area_vv.ags.bin),
// AGC binary type 2 (NGG/primitive-shader vertex stage), code up to s_endpgm at 0x158.
// EmbeddedShader.AreaFocus = (focus_ui3_area_vv, focus_ui3_area_p).
// Every statement maps to the RDNA instruction at the byte offset in its trailing comment.
//
// Math (P = Position, M = u_WorldMatrix, W = u_WarpDistortionMatrix, F = u_FocusPosition, S = u_FocusSize):
//   gl_Position = M * P                                       (column-major: dword k of the matrix = [k/4][k%4])
//   cp  = gl_Position.xy * (1/w)                              (main.ClipPos)
//   a   = (cp.x - F.x) * (1/S.x) ;  b = (0.5625*cp.y - F.y) * (1/S.y)
//   St  = (W.x*a + W.z*b, W.y*a + W.w*b)                      (main.St)
//   out_param0 = Color * u_Color                              (main.Color, output semantic 0)
//   out_param1 = (TexCoord.xy, cp)                            (main.TexCoord / main.ClipPos, output semantic 1)
//   out_param2 = (St.xy, 0, 0)                                (main.St, output semantic 2)
//
// Resources: __GLOBAL_CB__ CB slot 0 -> s[8:11] (user-data dword 0), 112 bytes; vertex-buffer table pointer
//   (direct resource 8, user-data dword 4) -> s[12:13], entries 0/1/2 = POSITION/COLOR/TEXCOORD
//   (s_load_dwordx8 s[0:7] at +0, s_load_dwordx4 s[36:39] at +0x20), as basic_vv.
//
// Not expressible / differences: as basic_vv.vert (NGG plumbing omitted; v_rcp_f32 1 ulp vs GLSL 1.0/x;
// v_mad/v_mac unfused -> `precise`; PS5 clip-space convention handled by the host viewport).

layout(set = 0, binding = 0, std140) uniform GLOBAL_CB {   // metadata "__GLOBAL_CB__", 112 bytes
    layout(offset = 0)  vec2 u_FocusPosition;
    layout(offset = 8)  vec2 u_FocusSize;
    layout(offset = 16) vec4 u_WarpDistortionMatrix;
    layout(offset = 32) mat4 u_WorldMatrix;
    layout(offset = 96) vec4 u_Color;
};

layout(location = 0) in vec4 in_Position;   // input.Position  POSITION  (vertex-buffer table entry 0)
layout(location = 1) in vec4 in_Color;      // input.Color     COLOR     (entry 1)
layout(location = 2) in vec2 in_TexCoord;   // input.TexCoord  TEXCOORD  (entry 2, buffer_load_format_xy)

layout(location = 0) out vec4 out_param0;   // exp param0: main.Color (output semantic 0)
layout(location = 1) out vec4 out_param1;   // exp param1: .xy main.TexCoord, .zw main.ClipPos (output semantic 1)
layout(location = 2) out vec4 out_param2;   // exp param2: .xy main.St, .zw = 0 (output semantic 2)

void main()
{
    precise float v0, v1, v2, v3, v4, v5, v6, v7, v8, v9, v10, v11, v12, v13, v14, v15;
    float s12, s13, s14, s15, s16, s17, s18, s19;
    float s20, s21, s22, s23, s24, s25, s26, s27, s28, s29, s30, s31, s32, s33, s34, s35, s40, s41, s42, s43;
    float vcc_lo;
    v10 = 0.0;                                                               // 0x3C
    s12 = u_FocusPosition.x; s13 = u_FocusPosition.y; s14 = u_FocusSize.x; s15 = u_FocusSize.y;
    s16 = u_WarpDistortionMatrix.x; s17 = u_WarpDistortionMatrix.y; s18 = u_WarpDistortionMatrix.z; s19 = u_WarpDistortionMatrix.w;
    s20 = u_WorldMatrix[0][0]; s21 = u_WorldMatrix[0][1]; s22 = u_WorldMatrix[0][2]; s23 = u_WorldMatrix[0][3];
    s24 = u_WorldMatrix[1][0]; s25 = u_WorldMatrix[1][1]; s26 = u_WorldMatrix[1][2]; s27 = u_WorldMatrix[1][3]; // s_buffer_load_dwordx16 s[12:27] 0x54
    s28 = u_WorldMatrix[2][0]; s29 = u_WorldMatrix[2][1]; s30 = u_WorldMatrix[2][2]; s31 = u_WorldMatrix[2][3];
    s32 = u_WorldMatrix[3][0]; s33 = u_WorldMatrix[3][1]; s34 = u_WorldMatrix[3][2]; s35 = u_WorldMatrix[3][3]; // s_buffer_load_dwordx8 s[28:35] 0x5C
    vcc_lo = uintBitsToFloat(0x3F100000u);   // 0.5625                       // 0x64
    s40 = u_Color.x; s41 = u_Color.y; s42 = u_Color.z; s43 = u_Color.w;      // s_buffer_load_dwordx4 s[40:43] 0x6C
    v6 = in_Position.x; v7 = in_Position.y; v8 = in_Position.z; v9 = in_Position.w; // 0x78
    v0 = in_Color.x; v1 = in_Color.y; v2 = in_Color.z; v3 = in_Color.w;      // 0x80
    v4 = in_TexCoord.x; v5 = in_TexCoord.y;                                  // 0x88
    v11 = 1.0 / s15;                                                         // 0x90
    v12 = 1.0 / s14;                                                         // 0x94
    v14 = s23 * v6;                                                          // 0x9C
    v15 = s21 * v6;                                                          // 0xA0
    v13 = s20 * v6;                                                          // 0xA4
    v6 = s22 * v6;                                                           // 0xA8
    v0 = s40 * v0;                                                           // 0xB0
    v14 = s27 * v7 + v14;                                                    // 0xB4
    v15 = s25 * v7 + v15;                                                    // 0xB8
    v13 = s24 * v7 + v13;                                                    // 0xBC
    v6 = s26 * v7 + v6;                                                      // 0xC0
    v3 = s43 * v3;                                                           // 0xC4
    v7 = v8 * s31 + v14;                                                     // 0xC8
    v15 = s29 * v8 + v15;                                                    // 0xD0
    v13 = s28 * v8 + v13;                                                    // 0xD4
    v14 = v8 * s30 + v6;                                                     // 0xD8
    v2 = s42 * v2;                                                           // 0xE0
    v6 = v9 * s35 + v7;                                                      // 0xE4
    v15 = s33 * v9 + v15;                                                    // 0xEC
    v13 = s32 * v9 + v13;                                                    // 0xF0
    v14 = s34 * v9 + v14;                                                    // 0xF4
    v1 = s41 * v1;                                                           // 0xF8
    v8 = 1.0 / v6;                                                           // 0xFC
    gl_Position = vec4(v13, v15, v14, v6);                                   // exp pos0 0x100
    v6 = v8 * v15;                                                           // 0x10C
    v9 = v8 * v13 + -s12;                                                    // 0x110
    v7 = v8 * v13;                                                           // 0x118
    v13 = vcc_lo * v6 + -s13;                                                // 0x11C
    v8 = v12 * v9;                                                           // 0x124
    v11 = v11 * v13;                                                         // 0x128
    v9 = s19 * v11;                                                          // 0x12C
    v11 = s18 * v11;                                                         // 0x130
    v9 = s17 * v8 + v9;                                                      // 0x134
    v11 = s16 * v8 + v11;                                                    // 0x138
    out_param2 = vec4(v11, v9, v10, v10);                                    // 0x13C
    out_param1 = vec4(v4, v5, v7, v6);                                       // 0x148
    out_param0 = vec4(v0, v1, v2, v3);                                       // 0x150
}
