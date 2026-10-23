#version 450
// Transcription of libScePsm embedded shader #15 "solid_fill_simple_p" (work/shaders/15_solid_fill_simple_p.ags.bin),
// AGC binary type 1 (pixel), code up to s_endpgm at 0x80. EmbeddedShader.SolidFillSimple = (solid_fill_simple_vv, solid_fill_simple_p).
// Every statement maps to the RDNA instruction at the byte offset in its trailing comment.
//
// Math (no interpolants; F = u_Flag):
//   k   = (F & 1) ? u_GlobalAlpha * u_GlobalIntensity : u_GlobalIntensity
//   out = (u_Color.rgb * k, u_GlobalAlpha * u_Color.a)
//
// Resources: __GLOBAL_CB__ slot 0 -> s[0:3] (user-data dword 0), globalConstantBuffer slot 7 -> s[4:7] (dword 4).
//
// Not expressible / differences:
//   * Output is packed to fp16 with round-toward-zero (v_cvt_pkrtz_f16_f32); the render target format decides here.

layout(set = 0, binding = 1, std140) uniform GLOBAL_CB {   // metadata "__GLOBAL_CB__", resource slot 0, 32 bytes
    layout(offset = 0)  uint u_Flag;        // metadata base type 9 (32-bit integer class)
    layout(offset = 16) vec4 u_Color;
};
// globalConstantBuffer: metadata resource slot 7, 8 bytes (see solid_fill_p.frag).
layout(set = 0, binding = 2, std140) uniform globalConstantBuffer {
    layout(offset = 0) float u_GlobalAlpha;
    layout(offset = 4) float u_GlobalIntensity;
};

layout(location = 0) out vec4 out_mrt0;  // exp mrt0 (SPI_SHADER_COL_FORMAT = 4: packed fp16, v_cvt_pkrtz_f16_f32 = round toward zero)

void main()
{
    precise float v0, v1, v2, v3;
    float s8, s9, s10, s11;
    uint vcc_lo_u;
    float vcc_lo, vcc_hi;
    vcc_hi = u_GlobalIntensity;                                              // 0x4
    v3 = vcc_hi;                                                             // 0x10
    vcc_lo_u = u_Flag;                                                       // 0x14
    s8 = u_Color.x; s9 = u_Color.y; s10 = u_Color.z; s11 = u_Color.w;        // 0x24
    if ((vcc_lo_u & 1u) != 0u) {                                             // s_bitcmp1_b32 0x20, s_cbranch_scc0 0x30
        vcc_lo = u_GlobalAlpha;                                                  // 0x34
        v3 = vcc_lo * vcc_hi;                                                    // 0x40
    }
    v1 = s8 * v3;                                                            // 0x4C
    vcc_lo = u_GlobalAlpha;                                                  // 0x50
    v2 = s9 * v3;                                                            // 0x58
    v0 = vcc_lo * s11;                                                       // 0x60
    v3 = s10 * v3;                                                           // 0x68
    out_mrt0 = vec4(v1, v2, v3, v0);   // v_cvt_pkrtz_f16_f32 v1,v1,v2 / v0,v3,v0 ; exp mrt0 compr (0x6C..0x78)
}
