#version 450
// Transcription of ShellUI eboot (NPXS40087 eboot.bin, FW 4.03) embedded shader #1 "basic_vv"
// (table {name, data, size} at 0xCC95F8, blob 0xD61C00, 2096 bytes; extracted to
// work/ida_bglayer/shaders/001_basic_vv.bin). AGC binary type 2 (vertex stage built as NGG/GS),
// .shader_text 0x1A0 bytes. XfGraphics creates it as its default vertex shader
// (sub_DDD90: sub_DFB30(gfx+96, 1)); the BG layers take it from there (Light: sub_9DFB0,
// light+104 = *(layer+32)+24).
//
// No inputs besides the vertex index (v5 in the NGG stage); a 3-vertex full-screen triangle:
//   id 0: pos (-1,-1) uv (0, 1)    id 1: pos (3,-1) uv (2, 1)    id 2: pos (-1, 3) uv (0,-1)
// PS5 clip space has +Y at the top of the target; the host pipeline flips the Vulkan viewport
// (negative height) the same way the libScePsm path does (psmhost/libScePsm/graphics/draw.cpp).
//
// Every statement maps to the instruction at the byte offset in its trailing comment
// (the NGG prologue at 0x00..0x2C only allocates/export the primitive).

layout(location = 0) out vec2 m_texture;   // param0, metadata "main.m_texture" TEXCOORD0

void main() {
    int v5 = gl_VertexIndex;                 // NGG vertex id in v5
    int v3 = v5 << 2;                        // 0x30 v_lshlrev_b32 v3, 2, v5
    int b1 = (v5 >> 1) & 1;                  // 0x34 v_bfe_u32 v5, v5, 1, 1
    int v4 = v3 & 4;                         // 0x48 v_and_b32 v4, 4, v3
    int v6 = (b1 << 2) + -1;                 // 0x4C v_lshl_add_u32 v6, v5, 2, -1
    int v5b = -1 + v4;                       // 0x54 v_add_nc_u32 v5, -1, v4
    float py = float(v6);                    // 0x58 v_cvt_f32_i32 v3, v6
    int v6b = 1 - v6;                        // 0x5C v_sub_nc_u32 v6, 1, v6
    float tx = float(v4) * 0.5;              // 0x60 v_cvt_f32_i32 v4, v4 div:2
    float px = float(v5b);                   // 0x68 v_cvt_f32_i32 v5, v5
    float ty = float(v6b) * 0.5;             // 0x6C v_cvt_f32_i32 v6, v6 div:2
    gl_Position = vec4(px, py, 0.0, 1.0);    // 0x74 exp pos0, v5, v3, v0(0), v1(1.0)
    m_texture = vec2(tx, ty);                // 0x7C exp param0, v4, v6
}
