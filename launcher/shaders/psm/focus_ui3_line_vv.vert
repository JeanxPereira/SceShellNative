#version 450
// Transcription of libScePsm embedded shader #6 "focus_ui3_line_vv" (work/shaders/06_focus_ui3_line_vv.ags.bin),
// AGC binary type 2 (NGG/primitive-shader vertex stage), code up to s_endpgm at 0x184.
// EmbeddedShader.LineFocus = (focus_ui3_line_vv, focus_ui3_line_p).
// Every statement maps to the RDNA instruction at the byte offset in its trailing comment.
//
// Math (P = Position, M = u_WorldMatrix, W = u_WarpDistortionMatrix, F = u_FocusPosition, S = u_FocusSize,
//       k = u_InOutScale):
//   gl_Position = M * P                                       (column-major: dword k of the matrix = [k/4][k%4])
//   cp  = gl_Position.xy * (1/w)                              (main.ClipPos)
//   a   = (cp.x - F.x) * (1/S.x) ;  b = (0.5625*cp.y - F.y) * (1/S.y)
//   St  = (W.x*a + W.z*b, W.y*a + W.w*b)                      (main.St)
//   FrameSt = (St.x * (S.x * (1/(k*S.x))), St.y * (S.y * (1/(k*S.y))))   (main.FrameSt)
//   out_param0 = Color * u_Color                              (main.Color, output semantic 0)
//   out_param1 = (TexCoord.xy, cp)                            (main.TexCoord / main.ClipPos, output semantic 1)
//   out_param2 = (St.xy, FrameSt.xy)                          (main.St / main.FrameSt, output semantic 2)
//
// Resources: __GLOBAL_CB__ CB slot 0 -> s[8:11] (user-data dword 0), 116 bytes; vertex-buffer table pointer
//   (direct resource 8, user-data dword 4) -> s[12:13], entries 0/1/2 = POSITION/COLOR/TEXCOORD, as basic_vv.
//
// Not expressible / differences: as basic_vv.vert (NGG plumbing omitted; v_rcp_f32 1 ulp vs GLSL 1.0/x;
// v_mad/v_mac unfused -> `precise`; PS5 clip-space convention handled by the host viewport).

layout(set = 0, binding = 0, std140) uniform GLOBAL_CB {   // metadata "__GLOBAL_CB__", 116 bytes
    layout(offset = 0)   vec2  u_FocusPosition;
    layout(offset = 8)   vec2  u_FocusSize;
    layout(offset = 16)  vec4  u_WarpDistortionMatrix;
    layout(offset = 32)  mat4  u_WorldMatrix;
    layout(offset = 96)  vec4  u_Color;
    layout(offset = 112) float u_InOutScale;
};

layout(location = 0) in vec4 in_Position;   // input.Position  POSITION  (vertex-buffer table entry 0)
layout(location = 1) in vec4 in_Color;      // input.Color     COLOR     (entry 1)
layout(location = 2) in vec2 in_TexCoord;   // input.TexCoord  TEXCOORD  (entry 2, buffer_load_format_xy)

layout(location = 0) out vec4 out_param0;   // exp param0: main.Color (output semantic 0)
layout(location = 1) out vec4 out_param1;   // exp param1: .xy main.TexCoord, .zw main.ClipPos (output semantic 1)
layout(location = 2) out vec4 out_param2;   // exp param2: .xy main.St, .zw main.FrameSt (output semantic 2)

void main()
{
    precise float v0, v1, v2, v3, v4, v5, v6, v7, v8, v9, v10, v11, v12, v13, v14, v15, v16, v17, v18;
    float s12, s13, s14, s15, s16, s17, s18, s19;
    float s20, s21, s22, s23, s24, s25, s26, s27, s28, s29, s30, s31, s32, s33, s34, s35, s40, s41, s42, s43;
    float vcc_lo;
    s12 = u_FocusPosition.x; s13 = u_FocusPosition.y; s14 = u_FocusSize.x; s15 = u_FocusSize.y;
    s16 = u_WarpDistortionMatrix.x; s17 = u_WarpDistortionMatrix.y; s18 = u_WarpDistortionMatrix.z; s19 = u_WarpDistortionMatrix.w;
    s20 = u_WorldMatrix[0][0]; s21 = u_WorldMatrix[0][1]; s22 = u_WorldMatrix[0][2]; s23 = u_WorldMatrix[0][3];
    s24 = u_WorldMatrix[1][0]; s25 = u_WorldMatrix[1][1]; s26 = u_WorldMatrix[1][2]; s27 = u_WorldMatrix[1][3]; // s_buffer_load_dwordx16 s[12:27] 0x50
    s28 = u_WorldMatrix[2][0]; s29 = u_WorldMatrix[2][1]; s30 = u_WorldMatrix[2][2]; s31 = u_WorldMatrix[2][3];
    s32 = u_WorldMatrix[3][0]; s33 = u_WorldMatrix[3][1]; s34 = u_WorldMatrix[3][2]; s35 = u_WorldMatrix[3][3]; // s_buffer_load_dwordx8 s[28:35] 0x58
    vcc_lo = u_InOutScale;                                                   // 0x60
    s40 = u_Color.x; s41 = u_Color.y; s42 = u_Color.z; s43 = u_Color.w;      // s_buffer_load_dwordx4 s[40:43] 0x68
    v6 = in_Position.x; v7 = in_Position.y; v8 = in_Position.z; v9 = in_Position.w; // 0x74
    v0 = in_Color.x; v1 = in_Color.y; v2 = in_Color.z; v3 = in_Color.w;      // 0x7C
    v4 = in_TexCoord.x; v5 = in_TexCoord.y;                                  // 0x84
    v12 = 1.0 / s15;                                                         // 0x8C
    v13 = 1.0 / s14;                                                         // 0x90
    v10 = vcc_lo * s15;                                                      // 0x94
    v11 = vcc_lo * s14;                                                      // 0x9C
    vcc_lo = uintBitsToFloat(0x3F100000u);   // 0.5625                       // 0xA4
    v10 = 1.0 / v10;                                                         // 0xAC
    v11 = 1.0 / v11;                                                         // 0xB0
    v10 = s15 * v10;                                                         // 0xB4
    v11 = s14 * v11;                                                         // 0xB8
    v15 = s23 * v6;                                                          // 0xC0
    v16 = s21 * v6;                                                          // 0xC4
    v14 = s20 * v6;                                                          // 0xC8
    v6 = s22 * v6;                                                           // 0xCC
    v0 = s40 * v0;                                                           // 0xD4
    v17 = v7 * s27 + v15;                                                    // 0xD8
    v15 = v7 * s25 + v16;                                                    // 0xE0
    v14 = s24 * v7 + v14;                                                    // 0xE8
    v16 = v7 * s26 + v6;                                                     // 0xEC
    v3 = s43 * v3;                                                           // 0xF4
    v17 = s31 * v8 + v17;                                                    // 0xF8
    v15 = s29 * v8 + v15;                                                    // 0xFC
    v7 = s28 * v8 + v14;                                                     // 0x100
    v16 = s30 * v8 + v16;                                                    // 0x108
    v2 = s42 * v2;                                                           // 0x10C
    v17 = s35 * v9 + v17;                                                    // 0x110
    v15 = s33 * v9 + v15;                                                    // 0x114
    v7 = s32 * v9 + v7;                                                      // 0x118
    v16 = s34 * v9 + v16;                                                    // 0x11C
    v1 = s41 * v1;                                                           // 0x120
    v8 = 1.0 / v17;                                                          // 0x124
    gl_Position = vec4(v7, v15, v16, v17);                                   // exp pos0 0x128
    v6 = v8 * v15;                                                           // 0x130
    v9 = v8 * v7 + -s12;                                                     // 0x134
    v18 = v8 * v7;                                                           // 0x13C
    v14 = vcc_lo * v6 + -s13;                                                // 0x140
    v8 = v13 * v9;                                                           // 0x148
    v12 = v12 * v14;                                                         // 0x14C
    v9 = s19 * v12;                                                          // 0x150
    v12 = s18 * v12;                                                         // 0x154
    v9 = s17 * v8 + v9;                                                      // 0x158
    v12 = s16 * v8 + v12;                                                    // 0x15C
    v10 = v10 * v9;                                                          // 0x160
    v11 = v11 * v12;                                                         // 0x164
    out_param2 = vec4(v12, v9, v11, v10);                                    // 0x168
    out_param1 = vec4(v4, v5, v18, v6);                                      // 0x174
    out_param0 = vec4(v0, v1, v2, v3);                                       // 0x17C
}
