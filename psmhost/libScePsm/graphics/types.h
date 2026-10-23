// Managed graphics types passed across the libScePsm boundary, mirrored bit-for-bit
// from Sce.PlayStation.Core.dll (Sce.PlayStation.Core.Graphics namespace).
#pragma once
#include <cstdint>

namespace ssn::psm::gfx {

// GraphicsUpdate (flags telling the native side which parts of GraphicsState and of
// the handle array changed since the last Update/DrawArrays).
enum GraphicsUpdate : uint32_t {
  kUpdEnable = 1u, kUpdScissor = 2u, kUpdViewport = 4u, kUpdDepthRange = 8u, kUpdClearColor = 16u, kUpdClearDepth = 32u,
  kUpdClearStencil = 64u, kUpdCullFace = 128u, kUpdBlendFunc = 256u, kUpdDepthFunc = 512u, kUpdPolygonOffset = 1024u,
  kUpdStencilFunc = 2048u, kUpdStencilOp = 4096u, kUpdColorMask = 8192u, kUpdLineWidth = 16384u,
  kUpdShaderProgram = 1u << 24, kUpdFrameBuffer = 1u << 25, kUpdConstantBuffer = 1u << 26,
  kUpdVertexBuffer0 = 1u << 28, kUpdVertexBufferN = 1u << 29, kUpdTexture0 = 1u << 30, kUpdTextureN = 1u << 31,
};

enum EnableMode : uint32_t {
  kEnScissorTest = 1, kEnCullFace = 2, kEnBlend = 4, kEnDepthTest = 8, kEnPolygonOffsetFill = 16, kEnStencilTest = 32, kEnDither = 64,
};

enum ClearMask : uint32_t { kClearColor = 1, kClearDepth = 2, kClearStencil = 4 };

enum DrawMode : uint32_t { kPoints, kLines, kLineStrip, kTriangles, kTriangleStrip, kTriangleFan };

struct ImageRect { int32_t x, y, width, height; };
struct Vector2 { float x, y; };
struct Vector4 { float x, y, z, w; };

// struct GraphicsState, [StructLayout(Sequential, Pack = 4)], 128 bytes. The
// packed sub-structs keep their managed "bits" encoding:
//   CullFace     = mode | direction << 8
//   BlendFunc    = mode | src << 8 | dst << 16
//   DepthFunc    = mode | (writeMask ? 0x100 : 0)
//   StencilFunc  = mode | ref << 8 | readMask << 16 | writeMask << 24
//   StencilOp    = fail | zFail << 8 | zPass << 16
#pragma pack(push, 4)
struct GraphicsState {
  uint32_t enable;
  ImageRect scissor;
  ImageRect viewport;
  Vector2 depth_range;
  Vector4 clear_color;
  float clear_depth;
  int32_t clear_stencil;
  uint32_t cull_face;
  uint32_t blend_rgb;
  uint32_t blend_alpha;
  uint32_t depth_func;
  float polygon_offset_factor;
  float polygon_offset_units;
  uint32_t stencil_func_front;
  uint32_t stencil_op_front;
  uint32_t stencil_func_back;
  uint32_t stencil_op_back;
  uint8_t color_mask;
  float line_width;
  int32_t dirty_buffer_slot_count;
  int32_t dirty_texture_slot_count;
  uint32_t dirty_constant_buffer_slots;
};
#pragma pack(pop)
static_assert(sizeof(GraphicsState) == 128);

// Indices into GraphicsContext.handles (int[24]) — see GraphicsContext.SetVertexBuffer /
// SetTexture / SetConstantBuffer / SetFrameBuffer in Core.dll.
constexpr int kHandleShader = 0, kHandleFrameBuffer = 1, kHandleVertexBuffer0 = 4, kHandleTexture0 = 8, kHandleConstantBuffer0 = 16;

enum PixelFormat : uint32_t {
  kPfNone, kPfRgba, kPfRgbaH, kPfRgba4444, kPfRgba5551, kPfRgb565, kPfLuminanceAlpha, kPfLuminanceAlphaH, kPfLuminance, kPfLuminanceH,
  kPfAlpha, kPfAlphaH, kPfDepth16, kPfDepth24, kPfDepth16Stencil8, kPfDepth24Stencil8, kPfDxt1, kPfDxt3, kPfDxt5, kPfBgra, kPfRed,
  kPfBc4U, kPfBc5U, kPfBc6U, kPfBc7U, kPfStencil8, kPfRgba1010102, kPfAbgr2101010,
};

}  // namespace ssn::psm::gfx
