#version 450
// Transcription of libScePsm embedded shader #4 "solid_fill_simple_vv" (work/shaders/04_solid_fill_simple_vv.ags.bin),
// AGC binary type 2 (NGG/primitive-shader vertex stage), code up to s_endpgm at 0xB0.
// EmbeddedShader.SolidFillSimple = (solid_fill_simple_vv, solid_fill_simple_p).
// Every statement maps to the RDNA instruction at the byte offset in its trailing comment.
//
// Math:  gl_Position = u_WorldMatrix * Position   (column-major: dword k of the CB = [k/4][k%4])
//        No parameter exports (output semantic count 0; the only output is main = SV_Position).
//
// Resources: __GLOBAL_CB__ CB slot 0 -> s[8:11] (user-data dword 0), 64 bytes.
//   Vertex-buffer table pointer (direct resource 8) at user-data dword 4 -> s[12:13]; only entry 0 (POSITION,
//   s_load_dwordx4 s[0:3], s[12:13], 0) is fetched (input semantic count 1), so only vertex stream 0 is read.
//
// Not expressible / differences: as basic_vv.vert (NGG plumbing omitted, v_mad/v_mac unfused -> `precise`,
// PS5 clip-space convention handled by the host viewport).

layout(set = 0, binding = 0, std140) uniform GLOBAL_CB {   // metadata "__GLOBAL_CB__", 64 bytes
    layout(offset = 0) mat4 u_WorldMatrix;
};

layout(location = 0) in vec4 in_Position;   // input.Position POSITION (vertex-buffer table entry 0, buffer_load_format_xyzw)

void main()
{
    precise float v0, v1, v2, v3, v4, v5, v6, v7;
    float s12, s13, s14, s15, s16, s17, s18, s19, s20, s21, s22, s23, s24, s25, s26, s27;
    s12 = u_WorldMatrix[0][0]; s13 = u_WorldMatrix[0][1]; s14 = u_WorldMatrix[0][2]; s15 = u_WorldMatrix[0][3]; s16 = u_WorldMatrix[1][0]; s17 = u_WorldMatrix[1][1]; s18 = u_WorldMatrix[1][2]; s19 = u_WorldMatrix[1][3]; s20 = u_WorldMatrix[2][0]; s21 = u_WorldMatrix[2][1]; s22 = u_WorldMatrix[2][2]; s23 = u_WorldMatrix[2][3]; s24 = u_WorldMatrix[3][0]; s25 = u_WorldMatrix[3][1]; s26 = u_WorldMatrix[3][2]; s27 = u_WorldMatrix[3][3]; // s_buffer_load_dwordx16 0x48
    v0 = in_Position.x; v1 = in_Position.y; v2 = in_Position.z; v3 = in_Position.w; // 0x54
    v5 = s12 * v0;                                                           // 0x60
    v6 = s15 * v0;                                                           // 0x64
    v4 = s14 * v0;                                                           // 0x68
    v0 = s13 * v0;                                                           // 0x6C
    v7 = s16 * v1 + v5;                                                      // 0x70
    v5 = v1 * s19 + v6;                                                      // 0x78
    v4 = s18 * v1 + v4;                                                      // 0x80
    v0 = s17 * v1 + v0;                                                      // 0x84
    v7 = s20 * v2 + v7;                                                      // 0x88
    v5 = s23 * v2 + v5;                                                      // 0x8C
    v4 = s22 * v2 + v4;                                                      // 0x90
    v0 = s21 * v2 + v0;                                                      // 0x94
    v7 = s24 * v3 + v7;                                                      // 0x98
    v5 = s27 * v3 + v5;                                                      // 0x9C
    v4 = s26 * v3 + v4;                                                      // 0xA0
    v0 = s25 * v3 + v0;                                                      // 0xA4
    gl_Position = vec4(v7, v0, v4, v5);                                      // exp pos0 0xA8
}
