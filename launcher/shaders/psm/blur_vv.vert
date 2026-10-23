#version 450
// Transcription of libScePsm embedded shader #1 "blur_vv" (work/shaders/01_blur_vv.ags.bin),
// AGC binary type 2 (NGG/primitive-shader vertex stage), code up to s_endpgm at 0xE8.
// EmbeddedShader.BlurH = (blur_vv, hblur_p), BlurV = (blur_vv, vblur_p).
// Every statement maps to the RDNA instruction at the byte offset in its trailing comment.
//
// Math:  gl_Position = u_World * Position                  (column-major: dword k of the CB = [k/4][k%4])
//        out_param0  = (0, 0, 0, 0)                         (main.Color is exported as zero)
//        out_param1  = (TexCoord.xy, gl_Position.xy * (1/w)) (main.TexCoord, main.ClipPos)
//
// Resources: __GLOBAL_CB__ CB slot 0 -> s[8:11] (user-data dword 0), 64 bytes. Vertex-buffer table pointer
//   (direct resource 8) -> s[12:13]. The header has only two input semantics (0 = POSITION, 2 = TEXCOORD), but the
//   ISA loads the TEXCOORD V# from table entry 2 (s_load_dwordx4 s[4:7], s[12:13], 0x20), i.e. vertex stream 2:
//   location = semantic index, so COLOR (stream 1) is simply not fetched.
//
// Not expressible / differences: as basic_vv.vert (NGG plumbing omitted; v_rcp_f32 1 ulp vs GLSL 1.0/x;
// v_mad/v_mac unfused -> `precise`; PS5 clip-space convention handled by the host viewport).

layout(set = 0, binding = 0, std140) uniform GLOBAL_CB {   // metadata "__GLOBAL_CB__", 64 bytes
    layout(offset = 0) mat4 u_World;
};

layout(location = 0) in vec4 in_Position;   // input.Position  POSITION  (vertex-buffer table entry 0)
layout(location = 2) in vec2 in_TexCoord;   // input.TexCoord  TEXCOORD  (entry 2, buffer_load_format_xy)

layout(location = 0) out vec4 out_param0;   // exp param0: main.Color (output semantic 0) = 0
layout(location = 1) out vec4 out_param1;   // exp param1: .xy main.TexCoord, .zw main.ClipPos (output semantic 1)

void main()
{
    precise float v0, v1, v2, v3, v4, v5, v6, v7, v8, v9, v10;
    float s12, s13, s14, s15, s16, s17, s18, s19, s20, s21, s22, s23, s24, s25, s26, s27;
    v6 = 0.0;                                                                // 0x3C
    s12 = u_World[0][0]; s13 = u_World[0][1]; s14 = u_World[0][2]; s15 = u_World[0][3]; s16 = u_World[1][0]; s17 = u_World[1][1]; s18 = u_World[1][2]; s19 = u_World[1][3]; s20 = u_World[2][0]; s21 = u_World[2][1]; s22 = u_World[2][2]; s23 = u_World[2][3]; s24 = u_World[3][0]; s25 = u_World[3][1]; s26 = u_World[3][2]; s27 = u_World[3][3]; // s_buffer_load_dwordx16 0x54
    v0 = in_Position.x; v1 = in_Position.y; v2 = in_Position.z; v3 = in_Position.w; // 0x60
    v4 = in_TexCoord.x; v5 = in_TexCoord.y;                                  // 0x68
    v8 = s12 * v0;                                                           // 0x74
    v9 = s15 * v0;                                                           // 0x78
    v7 = s14 * v0;                                                           // 0x7C
    v0 = s13 * v0;                                                           // 0x80
    v10 = s16 * v1 + v8;                                                     // 0x84
    v8 = v1 * s19 + v9;                                                      // 0x8C
    v7 = s18 * v1 + v7;                                                      // 0x94
    v9 = v1 * s17 + v0;                                                      // 0x98
    v10 = s20 * v2 + v10;                                                    // 0xA0
    v8 = s23 * v2 + v8;                                                      // 0xA4
    v7 = s22 * v2 + v7;                                                      // 0xA8
    v9 = s21 * v2 + v9;                                                      // 0xAC
    v10 = s24 * v3 + v10;                                                    // 0xB0
    v8 = s27 * v3 + v8;                                                      // 0xB4
    v7 = s26 * v3 + v7;                                                      // 0xB8
    v9 = s25 * v3 + v9;                                                      // 0xBC
    v3 = 1.0 / v8;                                                           // 0xC0
    gl_Position = vec4(v10, v9, v7, v8);                                     // exp pos0 0xC4
    v0 = v3 * v10;                                                           // 0xCC
    v1 = v3 * v9;                                                            // 0xD0
    out_param1 = vec4(v4, v5, v0, v1);                                       // 0xD8
    out_param0 = vec4(v6, v6, v6, v6);                                       // 0xE0
}
