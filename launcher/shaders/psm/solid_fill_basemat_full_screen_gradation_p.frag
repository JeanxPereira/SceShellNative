#version 450
#extension GL_EXT_shader_realtime_clock : require
// Transcription of libScePsm embedded shader #17 "solid_fill_basemat_full_screen_gradation_p"
// (work/shaders/17_solid_fill_basemat_full_screen_gradation_p.ags.bin), AGC binary type 1 (pixel), code up to
// s_endpgm at 0x124. EmbeddedShader.SolidFillBasematFullScreenGradation (64) = (basic_vv, this shader).
// Every statement maps to the RDNA instruction at the byte offset in its trailing comment.
//
// Math (c = vertex color attr0, uv = TexCoord = attr1.xy, F = u_Flag, S = u_startAlpha):
//   n = fract(43758.547 * sin(uv.x*12.9898 + uv.y*78.233 + float(realtime & 0x7fffff) * 1.1920931e-5))   (dither)
//   a = saturate(c.a * uv.y^2 * (3 - 2*uv.y) + S + 0.01*n - 0.005)       (vertical smoothstep ramp from S)
//   k = (F & 1) ? u_GlobalAlpha * u_GlobalIntensity : u_GlobalIntensity
//   out = (c.rgb * k, a * u_GlobalAlpha)
// No rounded-rect / fade tail (F bits 1 and 2 are not read by this shader).
//
// Resources: __GLOBAL_CB__ slot 0 -> s[0:3] (user-data dword 0), globalConstantBuffer slot 7 -> s[4:7] (dword 4).
//
// Not expressible / differences (as in solid_fill_light_mat_p.frag):
//   * s_memrealtime (0x10) is written clockRealtime2x32EXT().x (needs VK_KHR_shader_clock / shaderDeviceClock);
//     the PS5 counter frequency and epoch differ from the host's, so the dither evolves differently over time.
//   * v_sin_f32 takes revolutions; the compiler pre-multiplied by 1/(2*pi) (0xB4), so it is written sin(x*2*pi).
//   * v_fract_f32 is written fract(); v_mad/v_madak/v_madmk are unfused (locals are `precise`).
//   * Output is packed to fp16 with round-toward-zero (v_cvt_pkrtz_f16_f32); the render target format decides here.

layout(set = 0, binding = 1, std140) uniform GLOBAL_CB {   // metadata "__GLOBAL_CB__", resource slot 0, 8 bytes
    layout(offset = 0) uint  u_Flag;        // metadata base type 9 (32-bit integer class)
    layout(offset = 4) float u_startAlpha;
};
// globalConstantBuffer: metadata resource slot 7, 8 bytes (see solid_fill_p.frag).
layout(set = 0, binding = 2, std140) uniform globalConstantBuffer {
    layout(offset = 0) float u_GlobalAlpha;
    layout(offset = 4) float u_GlobalIntensity;
};

layout(location = 0) in vec4 in_attr0;   // PS attr0 = input semantic 0: input.Color (COLOR)
layout(location = 1) in vec4 in_attr1;   // PS attr1 = input semantic 1: .xy input.TexCoord (TEXCOORD0)
layout(location = 0) out vec4 out_mrt0;  // exp mrt0 compr (SPI_SHADER_COL_FORMAT = 4: packed fp16, round toward zero)

void main()
{
    precise float v0, v1, v2, v3, v4, v5, v6, v7;
    float vcc_lo, vcc_hi;
    uint vcc_lo_u, s12_u;
    vcc_hi = u_GlobalIntensity;                                              // s_buffer_load_dword vcc_hi, s[4:7], 0x4 // 0x04
    s12_u = clockRealtime2x32EXT().x;                                        // s_memrealtime s[12:13] // 0x10
    v2 = vcc_hi;                                                             // 0x1C
    if ((u_Flag & 1u) != 0u) {                                               // s_buffer_load_dword 0x20, s_bitcmp1_b32 0x2C, s_cbranch_scc0 0x30
        vcc_lo = u_GlobalAlpha;                                                  // 0x34
        v2 = vcc_lo * vcc_hi;                                                    // v_mul_f32_sdwa 0x40
    }
    vcc_lo_u = s12_u & 0x7FFFFFu;                                            // 0x4C
    v3 = in_attr1.x;                                                         // v_interp_p1/p2 attr1.x 0x54/0x58
    v6 = uintBitsToFloat(0x414FD639u) * v3;   // 12.9898                     // 0x5C
    v5 = in_attr1.y;                                                         // 0x64/0x68
    v3 = float(vcc_lo_u);                                                    // v_cvt_f32_u32 0x6C
    v4 = v5 * v5;                                                            // 0x70
    v6 = v5 * uintBitsToFloat(0x429C774Cu) + v6;   // 78.233                 // v_madmk 0x74
    v7 = in_attr0.w;                                                         // 0x7C/0x80
    v5 = -2.0 * v5 + uintBitsToFloat(0x40400000u);   // 3.0                  // v_madak 0x84
    v7 = v7 * v4;                                                            // 0x8C
    v6 = v3 * uintBitsToFloat(0x37480002u) + v6;   // 1.1920931e-5           // v_madmk 0x90
    v4 = in_attr0.z;                                                         // 0x98/0x9C
    vcc_lo = u_startAlpha;                                                   // s_buffer_load_dword vcc_lo, s[0:3], 0x4 // 0xA0
    v3 = v7 * v5 + vcc_lo;                                                   // v_mad_f32 0xAC
    v7 = uintBitsToFloat(0x3E22F983u) * v6;   // 1/(2*pi)                    // 0xB4
    v5 = in_attr0.y;                                                         // 0xBC/0xC0
    vcc_hi = uintBitsToFloat(0xBBA3D70Au);   // -0.005                       // 0xC4
    v6 = in_attr0.x;                                                         // 0xCC/0xDC
    v7 = sin(v7 * 6.28318530717958647692);   // v_sin_f32 (operand in revolutions) // 0xD0
    vcc_lo = u_GlobalAlpha;                                                  // s_buffer_load_dword vcc_lo, s[4:7], 0x0 // 0xD4
    v0 = v2 * v4;                                                            // 0xE0
    v1 = v2 * v6;                                                            // 0xE4
    v4 = uintBitsToFloat(0x472AEE8Cu) * v7;   // 43758.547                   // 0xE8
    v2 = v2 * v5;                                                            // 0xF0
    v4 = fract(v4);                                                          // 0xF4
    vec2 rg = vec2(v1, v2);                                                  // v_cvt_pkrtz_f16_f32 v1, v1, v2 // 0xF8
    v2 = v4 * uintBitsToFloat(0x3C23D70Au) + v3;   // 0.01                   // v_madmk 0xFC
    v2 = clamp(vcc_hi + v2, 0.0, 1.0);                                       // v_add_f32_sdwa clamp 0x104
    v2 = vcc_lo * v2;                                                        // 0x110
    out_mrt0 = vec4(rg, v0, v2);   // v_cvt_pkrtz_f16_f32 v0, v0, v2 (0x114); exp mrt0 v1, v1, v0, v0 done compr (0x11C)
}
