// Clear and DrawArrays for the PSM graphics context, executed on the Vulkan backend.
//
// The fixed-function state comes from the managed GraphicsState as libScePsm applies
// it in 0x1f32a0: scissor (clamped to 0..4096), viewport + depth range (0xc730),
// colour mask, cull face (mode table 0x34ea30, direction = front face), blend (enable
// bit, op table 0x34ee10, factor table 0x3ef850), depth and stencil. Coordinates are
// top-left origin like the PS5 GPU; the Vulkan viewport is flipped so clip-space +Y
// maps to the top of the target as it does there.
#include "libScePsm/graphics/draw.h"

#include <windows.h>

#include <algorithm>
#include <array>
#include <cstdio>
#include <cstring>
#include <string>
#include <map>
#include <tuple>

#include "core/log.h"
#include "gfx/vk_device.h"
#include "host/boundary.h"
#include "libScePsm/error.h"
#include "libScePsm/graphics/core.h"
#include "libScePsm/graphics/objects.h"
#include "libScePsm/graphics/shader_program.h"

namespace ssn::psm::gfx {
namespace {

VkPipelineLayout g_layout = VK_NULL_HANDLE;
VkDescriptorSetLayout g_set_layout = VK_NULL_HANDLE;
PFN_vkCmdPushDescriptorSetKHR g_push = nullptr;

void ensure_layout() {
  if (g_layout) return;
  VkDevice d = ssn::gfx::device().device();
  std::array<VkDescriptorSetLayoutBinding, 3 + kTextureSlots> b{};
  for (uint32_t i = 0; i < 3; ++i) {
    b[i].binding = i;
    b[i].descriptorType = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER;
    b[i].descriptorCount = 1;
    b[i].stageFlags = VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT;
  }
  for (uint32_t i = 0; i < kTextureSlots; ++i) {
    b[3 + i].binding = kBindingTexture0 + i;
    b[3 + i].descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
    b[3 + i].descriptorCount = 1;
    b[3 + i].stageFlags = VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT;
  }
  VkDescriptorSetLayoutCreateInfo si{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO};
  si.flags = VK_DESCRIPTOR_SET_LAYOUT_CREATE_PUSH_DESCRIPTOR_BIT_KHR;
  si.bindingCount = static_cast<uint32_t>(b.size());
  si.pBindings = b.data();
  vkCreateDescriptorSetLayout(d, &si, nullptr, &g_set_layout);
  VkPipelineLayoutCreateInfo li{VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO};
  li.setLayoutCount = 1;
  li.pSetLayouts = &g_set_layout;
  vkCreatePipelineLayout(d, &li, nullptr, &g_layout);
  g_push = reinterpret_cast<PFN_vkCmdPushDescriptorSetKHR>(vkGetDeviceProcAddr(d, "vkCmdPushDescriptorSetKHR"));
}

// PSM enum -> Vulkan tables (same order as the managed enums).
constexpr VkBlendOp kBlendOp[3] = {VK_BLEND_OP_ADD, VK_BLEND_OP_SUBTRACT, VK_BLEND_OP_REVERSE_SUBTRACT};
constexpr VkBlendFactor kBlendFactor[11] = {
    VK_BLEND_FACTOR_ZERO,       VK_BLEND_FACTOR_ONE,       VK_BLEND_FACTOR_SRC_COLOR, VK_BLEND_FACTOR_ONE_MINUS_SRC_COLOR,
    VK_BLEND_FACTOR_SRC_ALPHA,  VK_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA, VK_BLEND_FACTOR_DST_COLOR, VK_BLEND_FACTOR_ONE_MINUS_DST_COLOR,
    VK_BLEND_FACTOR_DST_ALPHA,  VK_BLEND_FACTOR_ONE_MINUS_DST_ALPHA, VK_BLEND_FACTOR_SRC_ALPHA_SATURATE};
constexpr VkCompareOp kCompare[8] = {VK_COMPARE_OP_ALWAYS, VK_COMPARE_OP_NEVER,   VK_COMPARE_OP_EQUAL,          VK_COMPARE_OP_NOT_EQUAL,
                                     VK_COMPARE_OP_LESS,   VK_COMPARE_OP_GREATER, VK_COMPARE_OP_LESS_OR_EQUAL, VK_COMPARE_OP_GREATER_OR_EQUAL};
constexpr VkStencilOp kStencilOp[8] = {VK_STENCIL_OP_KEEP,      VK_STENCIL_OP_ZERO,      VK_STENCIL_OP_REPLACE,
                                       VK_STENCIL_OP_INVERT,    VK_STENCIL_OP_INCREMENT_AND_CLAMP, VK_STENCIL_OP_DECREMENT_AND_CLAMP,
                                       VK_STENCIL_OP_INCREMENT_AND_WRAP, VK_STENCIL_OP_DECREMENT_AND_WRAP};
constexpr VkPrimitiveTopology kTopology[6] = {VK_PRIMITIVE_TOPOLOGY_POINT_LIST,     VK_PRIMITIVE_TOPOLOGY_LINE_LIST,
                                              VK_PRIMITIVE_TOPOLOGY_LINE_STRIP,     VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST,
                                              VK_PRIMITIVE_TOPOLOGY_TRIANGLE_STRIP, VK_PRIMITIVE_TOPOLOGY_TRIANGLE_FAN};

struct PipelineKey {
  const ShaderProgram* program;
  std::array<uint32_t, 8> formats;
  uint32_t topology, enable, blend_rgb, blend_alpha, cull, depth, stencil_ff, stencil_of, stencil_fb, stencil_ob, color_mask;
  VkFormat color_format, depth_format;
  auto tie() const {
    return std::tie(program, formats, topology, enable, blend_rgb, blend_alpha, cull, depth, stencil_ff, stencil_of, stencil_fb, stencil_ob,
                    color_mask, color_format, depth_format);
  }
  bool operator<(const PipelineKey& o) const { return tie() < o.tie(); }
};

std::map<PipelineKey, VkPipeline> g_pipelines;

VkPipeline pipeline_for(const PipelineKey& k, const VertexBuffer& vb) {
  auto it = g_pipelines.find(k);
  if (it != g_pipelines.end()) return it->second;
  VkDevice d = ssn::gfx::device().device();

  VkPipelineShaderStageCreateInfo stages[2]{};
  stages[0] = {VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO, nullptr, 0, VK_SHADER_STAGE_VERTEX_BIT, k.program->vs, "main"};
  stages[1] = {VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO, nullptr, 0, VK_SHADER_STAGE_FRAGMENT_BIT, k.program->fs, "main"};

  // Attribute i is fetched from vertex-buffer stream i (0x1f32a0 binds the streams of
  // vertex buffer 0 from slot 0 with 0xae80).
  std::vector<VkVertexInputBindingDescription> binds;
  std::vector<VkVertexInputAttributeDescription> attrs;
  for (uint32_t s = 0; s < vb.formats.size(); ++s) {
    VkFormat vf;
    if (!vb.formats[s] || !vk_vertex_format(vb.formats[s], vf)) continue;
    const uint32_t f = vb.formats[s];
    const uint32_t raw = (f ? vertex_format_size(f) : 0);
    (void)raw;
    const uint32_t comp_bytes = std::array<uint32_t, 16>{0, 4, 2, 1, 0, 0, 2, 2, 1, 1}[(f >> 8) & 0xf];
    const uint32_t elem = comp_bytes * ((f & 3) + 1) * (((f >> 4) & 3) + 1);
    binds.push_back({s, elem, VK_VERTEX_INPUT_RATE_VERTEX});
    attrs.push_back({s, s, vf, 0});
  }
  VkPipelineVertexInputStateCreateInfo vi{VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO};
  vi.vertexBindingDescriptionCount = static_cast<uint32_t>(binds.size());
  vi.pVertexBindingDescriptions = binds.data();
  vi.vertexAttributeDescriptionCount = static_cast<uint32_t>(attrs.size());
  vi.pVertexAttributeDescriptions = attrs.data();

  VkPipelineInputAssemblyStateCreateInfo ia{VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO};
  ia.topology = kTopology[k.topology];
  VkPipelineViewportStateCreateInfo vp{VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO};
  vp.viewportCount = 1;
  vp.scissorCount = 1;

  VkPipelineRasterizationStateCreateInfo rs{VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO};
  rs.polygonMode = VK_POLYGON_MODE_FILL;
  rs.lineWidth = 1.0f;
  if (k.enable & kEnCullFace) {
    const uint32_t mode = k.cull & 0xff;
    rs.cullMode = mode == 1 ? VK_CULL_MODE_FRONT_BIT : mode == 2 ? VK_CULL_MODE_BACK_BIT : mode == 3 ? VK_CULL_MODE_FRONT_AND_BACK : VK_CULL_MODE_NONE;
  }
  rs.frontFace = ((k.cull >> 8) & 1) ? VK_FRONT_FACE_COUNTER_CLOCKWISE : VK_FRONT_FACE_CLOCKWISE;
  rs.depthBiasEnable = (k.enable & kEnPolygonOffsetFill) != 0;

  VkPipelineMultisampleStateCreateInfo ms{VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO};
  ms.rasterizationSamples = VK_SAMPLE_COUNT_1_BIT;

  VkPipelineDepthStencilStateCreateInfo ds{VK_STRUCTURE_TYPE_PIPELINE_DEPTH_STENCIL_STATE_CREATE_INFO};
  if (k.depth_format != VK_FORMAT_UNDEFINED) {
    ds.depthTestEnable = (k.enable & kEnDepthTest) != 0;
    ds.depthWriteEnable = ds.depthTestEnable && ((k.depth >> 8) & 1);
    ds.depthCompareOp = kCompare[k.depth & 7];
    ds.stencilTestEnable = (k.enable & kEnStencilTest) != 0;
    auto face = [](uint32_t func, uint32_t op) {
      VkStencilOpState s{};
      s.compareOp = kCompare[func & 7];
      s.reference = (func >> 8) & 0xff;
      s.compareMask = (func >> 16) & 0xff;
      s.writeMask = (func >> 24) & 0xff;
      s.failOp = kStencilOp[op & 7];
      s.depthFailOp = kStencilOp[(op >> 8) & 7];
      s.passOp = kStencilOp[(op >> 16) & 7];
      return s;
    };
    ds.front = face(k.stencil_ff, k.stencil_of);
    ds.back = face(k.stencil_fb, k.stencil_ob);
  }

  VkPipelineColorBlendAttachmentState cb{};
  cb.blendEnable = (k.enable & kEnBlend) != 0;
  cb.colorBlendOp = kBlendOp[std::min<uint32_t>(k.blend_rgb & 0xff, 2)];
  cb.srcColorBlendFactor = kBlendFactor[std::min<uint32_t>((k.blend_rgb >> 8) & 0xff, 10)];
  cb.dstColorBlendFactor = kBlendFactor[std::min<uint32_t>((k.blend_rgb >> 16) & 0xff, 10)];
  cb.alphaBlendOp = kBlendOp[std::min<uint32_t>(k.blend_alpha & 0xff, 2)];
  cb.srcAlphaBlendFactor = kBlendFactor[std::min<uint32_t>((k.blend_alpha >> 8) & 0xff, 10)];
  cb.dstAlphaBlendFactor = kBlendFactor[std::min<uint32_t>((k.blend_alpha >> 16) & 0xff, 10)];
  cb.colorWriteMask = k.color_mask & 0xf;  // ColorMask R=1 G=2 B=4 A=8 == VkColorComponentFlagBits
  VkPipelineColorBlendStateCreateInfo cbs{VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO};
  cbs.attachmentCount = 1;
  cbs.pAttachments = &cb;

  VkDynamicState dyn[] = {VK_DYNAMIC_STATE_VIEWPORT, VK_DYNAMIC_STATE_SCISSOR, VK_DYNAMIC_STATE_DEPTH_BIAS};
  VkPipelineDynamicStateCreateInfo dy{VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO};
  dy.dynamicStateCount = 3;
  dy.pDynamicStates = dyn;

  VkPipelineRenderingCreateInfo rinfo{VK_STRUCTURE_TYPE_PIPELINE_RENDERING_CREATE_INFO};
  rinfo.colorAttachmentCount = 1;
  rinfo.pColorAttachmentFormats = &k.color_format;
  rinfo.depthAttachmentFormat = k.depth_format;
  if (k.depth_format == VK_FORMAT_D24_UNORM_S8_UINT) rinfo.stencilAttachmentFormat = k.depth_format;

  VkGraphicsPipelineCreateInfo pi{VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO};
  pi.pNext = &rinfo;
  pi.stageCount = 2;
  pi.pStages = stages;
  pi.pVertexInputState = &vi;
  pi.pInputAssemblyState = &ia;
  pi.pViewportState = &vp;
  pi.pRasterizationState = &rs;
  pi.pMultisampleState = &ms;
  pi.pDepthStencilState = &ds;
  pi.pColorBlendState = &cbs;
  pi.pDynamicState = &dy;
  pi.layout = g_layout;
  VkPipeline p = VK_NULL_HANDLE;
  if (vkCreateGraphicsPipelines(d, VK_NULL_HANDLE, 1, &pi, nullptr, &p) != VK_SUCCESS) log::error("psm.gfx", "pipeline creation failed");
  g_pipelines.emplace(k, p);
  return p;
}

std::map<uint32_t, VkSampler> g_samplers;

VkSampler sampler_for(const PixelBuffer& pb) {
  const uint32_t key = pb.mag | pb.min << 4 | pb.mip << 8 | pb.wrap_s << 12 | pb.wrap_t << 16;
  auto it = g_samplers.find(key);
  if (it != g_samplers.end()) return it->second;
  VkSamplerCreateInfo si{VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO};
  si.magFilter = pb.mag == 0 ? VK_FILTER_NEAREST : VK_FILTER_LINEAR;
  si.minFilter = pb.min == 0 ? VK_FILTER_NEAREST : VK_FILTER_LINEAR;
  si.mipmapMode = pb.mip == 0 ? VK_SAMPLER_MIPMAP_MODE_NEAREST : VK_SAMPLER_MIPMAP_MODE_LINEAR;
  si.maxLod = pb.mip == 2 ? 0.0f : VK_LOD_CLAMP_NONE;  // TextureFilterMode.Disabled: base level only
  si.addressModeU = pb.wrap_s == 1 ? VK_SAMPLER_ADDRESS_MODE_REPEAT : VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
  si.addressModeV = pb.wrap_t == 1 ? VK_SAMPLER_ADDRESS_MODE_REPEAT : VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
  si.addressModeW = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
  VkSampler s = VK_NULL_HANDLE;
  vkCreateSampler(ssn::gfx::device().device(), &si, nullptr, &s);
  g_samplers.emplace(key, s);
  return s;
}

VkSampler null_sampler() {
  static VkSampler s = [] {
    VkSamplerCreateInfo si{VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO};
    VkSampler r = VK_NULL_HANDLE;
    vkCreateSampler(ssn::gfx::device().device(), &si, nullptr, &r);
    return r;
  }();
  return s;
}

void set_viewport_scissor(VkCommandBuffer cb, const GraphicsState& st, const ssn::gfx::Image& target) {
  VkViewport v{};
  v.x = static_cast<float>(st.viewport.x);
  v.y = static_cast<float>(st.viewport.y + st.viewport.height);
  v.width = static_cast<float>(st.viewport.width);
  v.height = -static_cast<float>(st.viewport.height);
  v.minDepth = st.depth_range.x;
  v.maxDepth = st.depth_range.y;
  vkCmdSetViewport(cb, 0, 1, &v);
  // Scissor as clamped by 0x1f32a0; without ScissorTest the whole target.
  VkRect2D r{{0, 0}, {target.width, target.height}};
  if (st.enable & kEnScissorTest) {
    int32_t x = std::max(st.scissor.x, 0), y = std::max(st.scissor.y, 0);
    int32_t w = std::max(st.scissor.width + std::min(st.scissor.x, 0), 0), h = std::max(st.scissor.height + std::min(st.scissor.y, 0), 0);
    x = std::min(x, int32_t(target.width));
    y = std::min(y, int32_t(target.height));
    w = std::min(w, int32_t(target.width) - x);
    h = std::min(h, int32_t(target.height) - y);
    r = {{x, y}, {uint32_t(w), uint32_t(h)}};
  }
  vkCmdSetScissor(cb, 0, 1, &r);
  vkCmdSetDepthBias(cb, st.polygon_offset_units, 0.0f, st.polygon_offset_factor);
}

// Diagnostics: SSN_TRACE_DRAWS=<f1,f2,...> logs every draw of those frames (frame index
// as counted by the capture, i.e. the number of the frame being recorded).
bool trace_this_frame() {
  static std::string list = [] {
    char b[512];
    return GetEnvironmentVariableA("SSN_TRACE_DRAWS", b, sizeof b) ? "," + std::string(b) + "," : std::string();
  }();
  if (list.empty()) return false;
  return list.find("," + std::to_string(Core::get().frame_index() + 1) + ",") != std::string::npos;
}

std::string floats(const std::vector<uint8_t>& v, size_t max_floats) {
  std::string s;
  const size_t n = std::min(v.size() / 4, max_floats);
  for (size_t i = 0; i < n; ++i) {
    float f;
    std::memcpy(&f, v.data() + 4 * i, 4);
    char b[32];
    std::snprintf(b, sizeof b, "%s%g", i ? " " : "", f);
    s += b;
  }
  return s;
}

void trace_draw(const ShaderProgram& prog, uint32_t mode, int32_t first, int32_t count, const GraphicsState& st, const VertexBuffer& vb,
                const ssn::gfx::Image& target) {
  std::string tex;
  for (uint32_t t = 0; t < 2; ++t) {
    PixelBuffer* pb = Objects::get().pixel_buffer(Core::get().bound_textures()[t]);
    char b[96];
    std::snprintf(b, sizeof b, " t%u=%dx%d/f%u", t, pb ? pb->width : 0, pb ? pb->height : 0, pb ? pb->format : 0);
    tex += b;
    if (pb && !pb->name.empty()) tex += "'" + pb->name + "'";
  }
  std::string pos;
  static const bool full = [] {
    char b[8];
    return GetEnvironmentVariableA("SSN_TRACE_DRAWS_FULL", b, sizeof b) != 0;
  }();
  for (size_t s = 0; s < vb.streams.size(); ++s) {
    if (vb.streams[s].empty() || (!full && s > 0)) continue;
    const size_t lim = full ? vb.streams[s].size() : std::min<size_t>(vb.streams[s].size(), 64);
    std::vector<uint8_t> p0(vb.streams[s].begin(), vb.streams[s].begin() + lim);
    pos += std::format(" s{}/{:x}:", s, vb.formats[s]) + floats(p0, lim / 4);
  }
  if (full && vb.index_count > 0) {
    pos += " idx:";
    for (int32_t i = 0; i < vb.index_count; ++i) pos += std::format(" {}", vb.indices[i]);
  }
  log::info("psm.draw",
            "#{} {}+{} m{} {}+{} en={:x} bl={:x}/{:x} cm={:x} sc={},{},{},{} vp={},{},{},{} tgt={}x{} fmt0={:x}{} | vs[{}] | ps[{}] | pos[{}]",
            prog.embedded_type, prog.vs_name, prog.ps_name, mode, first, count, st.enable, st.blend_rgb, st.blend_alpha, st.color_mask,
            st.scissor.x, st.scissor.y, st.scissor.width, st.scissor.height, st.viewport.x, st.viewport.y, st.viewport.width,
            st.viewport.height, target.width, target.height, vb.formats.empty() ? 0 : vb.formats[0], tex, floats(prog.vs_constants, 24),
            floats(prog.ps_constants, 64), pos);
}

}  // namespace

int32_t clear_target(uint32_t mask) {
  std::lock_guard<std::recursive_mutex> record(ssn::gfx::device().record_lock());  // see Device::record_lock
  auto& dev = ssn::gfx::device();
  const GraphicsState& st = Core::get().state();
  dev.ensure_rendering();
  if (!dev.rendering()) return kErrorInvalidOperation;
  VkClearAttachment att[2]{};
  uint32_t n = 0;
  if (mask & kClearColor) {
    att[n].aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
    att[n].colorAttachment = 0;
    att[n].clearValue.color = {{st.clear_color.x, st.clear_color.y, st.clear_color.z, st.clear_color.w}};
    ++n;
  }
  if ((mask & (kClearDepth | kClearStencil)) && dev.target_depth()) {
    VkImageAspectFlags a = 0;
    if (mask & kClearDepth) a |= VK_IMAGE_ASPECT_DEPTH_BIT;
    if ((mask & kClearStencil) && (dev.target_depth()->aspect & VK_IMAGE_ASPECT_STENCIL_BIT)) a |= VK_IMAGE_ASPECT_STENCIL_BIT;
    att[n].aspectMask = a;
    att[n].clearValue.depthStencil = {st.clear_depth, static_cast<uint32_t>(st.clear_stencil)};
    ++n;
  }
  if (n == 0) return kOk;
  ssn::gfx::Image* t = dev.target_color();
  VkClearRect rect{{{0, 0}, {t->width, t->height}}, 0, 1};
  vkCmdClearAttachments(dev.cmd(), n, att, 1, &rect);
  return kOk;
}

// DrawArrays — 0x1e7280: mode > 5 -> 0x80b30001; first > 0xfffe, count or repeat >
// 0xffff, or first + count*repeat beyond the bound vertex (or index) count ->
// 0x80b30003; without a bound shader program nothing is drawn (0). Drawing is indexed
// when the vertex buffer has indices; `repeat` issues the draw `repeat` times with
// first advancing by count.
int32_t draw_arrays(uint32_t mode, int32_t first, int32_t count, int32_t repeat) {
  std::lock_guard<std::recursive_mutex> record(ssn::gfx::device().record_lock());  // see Device::record_lock
  if (mode > 5) return kErrorInvalidParameter;
  if (uint32_t(first) > 0xfffe || (uint32_t(count) | uint32_t(repeat)) > 0xffff) return kErrorOutOfRange;
  Core& core = Core::get();
  const GraphicsState& st = core.state();
  const auto& h = core.handles();
  ShaderProgram* prog = Objects::get().shader_program(h[kHandleShader]);
  if (!prog) return kOk;  // 0x1e7280: no program bound (+172 == 0) -> nothing drawn
  VertexBuffer* vb = Objects::get().vertex_buffer(h[kHandleVertexBuffer0]);
  const int32_t limit = vb ? (vb->index_count > 0 ? vb->index_count : vb->vertex_count) : 0xffff;
  if (int64_t(first) + int64_t(count) * repeat > limit) return kErrorOutOfRange;
  if (!vb || count == 0 || repeat == 0) return kOk;
  if (!prog->vs || !prog->fs) {
    host::note_unimplemented("libScePsm draw with embedded shader " + prog->vs_name + "+" + prog->ps_name + " (not transcribed)");
    return kOk;
  }
  // Canvas textures (type 3) take the compositor canvas's newest frame (canvas_texture.cpp).
  for (uint32_t t = 0; t < kTextureSlots; ++t)
    if (PixelBuffer* pb = Objects::get().pixel_buffer(core.bound_textures()[t]); pb && pb->type == 3) sync_canvas_texture(*pb);
  if (!core.bind_frame_buffer()) return kErrorInvalidOperation;
  ensure_layout();
  auto& dev = ssn::gfx::device();
  ssn::gfx::Image* target = dev.target_color();
  if (trace_this_frame()) trace_draw(*prog, mode, first, count, st, *vb, *target);
  PipelineKey key{};
  key.program = prog;
  for (size_t i = 0; i < vb->formats.size() && i < 8; ++i) key.formats[i] = vb->formats[i];
  key.topology = mode;
  key.enable = st.enable;
  key.blend_rgb = st.blend_rgb;
  key.blend_alpha = st.blend_alpha;
  key.cull = st.cull_face;
  key.depth = st.depth_func;
  key.stencil_ff = st.stencil_func_front;
  key.stencil_of = st.stencil_op_front;
  key.stencil_fb = st.stencil_func_back;
  key.stencil_ob = st.stencil_op_back;
  key.color_mask = st.color_mask;
  key.color_format = target->format;
  key.depth_format = dev.target_depth() ? dev.target_depth()->format : VK_FORMAT_UNDEFINED;
  VkPipeline pipe = pipeline_for(key, *vb);
  if (!pipe) return kOk;

  // Snapshot the vertex streams and indices into the frame's upload ring.
  dev.ensure_rendering();
  VkCommandBuffer cb = dev.cmd();
  std::vector<VkBuffer> bufs;
  std::vector<VkDeviceSize> offs;
  std::vector<uint32_t> bind_ids;
  for (uint32_t s = 0; s < vb->formats.size(); ++s) {
    if (!vb->formats[s] || vb->streams[s].empty()) continue;
    ssn::gfx::Slice sl = dev.alloc(vb->streams[s].size(), 16);
    std::memcpy(sl.data, vb->streams[s].data(), vb->streams[s].size());
    bufs.push_back(sl.buffer);
    offs.push_back(sl.offset);
    bind_ids.push_back(s);
  }
  vkCmdBindPipeline(cb, VK_PIPELINE_BIND_POINT_GRAPHICS, pipe);
  for (size_t i = 0; i < bufs.size(); ++i) vkCmdBindVertexBuffers(cb, bind_ids[i], 1, &bufs[i], &offs[i]);
  set_viewport_scissor(cb, st, *target);

  // Constant buffers and textures.
  std::vector<VkWriteDescriptorSet> writes;
  VkDescriptorBufferInfo bi[3]{};
  auto add_cb = [&](uint32_t binding, const std::vector<uint8_t>& data, int idx) {
    std::vector<uint8_t> padded(std::max<size_t>(data.size(), 16), 0);
    std::memcpy(padded.data(), data.data(), data.size());
    ssn::gfx::Slice sl = dev.alloc(padded.size(), 256);
    std::memcpy(sl.data, padded.data(), padded.size());
    bi[idx] = {sl.buffer, sl.offset, padded.size()};
    VkWriteDescriptorSet w{VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET};
    w.dstBinding = binding;
    w.descriptorCount = 1;
    w.descriptorType = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER;
    w.pBufferInfo = &bi[idx];
    writes.push_back(w);
  };
  add_cb(kBindingVsConstants, prog->vs_constants, 0);
  add_cb(kBindingPsConstants, prog->ps_constants, 1);
  add_cb(kBindingGlobalConstants, core.global_constants(), 2);
  VkDescriptorImageInfo ii[kTextureSlots]{};
  for (uint32_t t = 0; t < kTextureSlots; ++t) {
    PixelBuffer* pb = Objects::get().pixel_buffer(core.bound_textures()[t]);
    if (!pb || !pb->image.valid()) {
      // HOST VALUE: a unit that was never bound has no defined content on the console
      // (whatever the GPU's resource slot held); the host binds a null descriptor, which
      // reads as zero (VK_EXT_robustness2 nullDescriptor).
      ii[t] = {VK_NULL_HANDLE, VK_NULL_HANDLE, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL};
      ii[t].sampler = null_sampler();
      VkWriteDescriptorSet w{VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET};
      w.dstBinding = kBindingTexture0 + t;
      w.descriptorCount = 1;
      w.descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
      w.pImageInfo = &ii[t];
      writes.push_back(w);
      continue;
    }
    if (pb->image.layout != VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL) {
      // A texture never uploaded (or just rendered to) must be readable.
      dev.end_rendering();
      dev.transition(cb, pb->image, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
      dev.ensure_rendering();
      vkCmdBindPipeline(cb, VK_PIPELINE_BIND_POINT_GRAPHICS, pipe);
      for (size_t i = 0; i < bufs.size(); ++i) vkCmdBindVertexBuffers(cb, bind_ids[i], 1, &bufs[i], &offs[i]);
      set_viewport_scissor(cb, st, *target);
    }
    ii[t] = {sampler_for(*pb), pb->image.view, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL};
    VkWriteDescriptorSet w{VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET};
    w.dstBinding = kBindingTexture0 + t;
    w.descriptorCount = 1;
    w.descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
    w.pImageInfo = &ii[t];
    writes.push_back(w);
  }
  g_push(cb, VK_PIPELINE_BIND_POINT_GRAPHICS, g_layout, 0, static_cast<uint32_t>(writes.size()), writes.data());

  if (vb->index_count > 0) {
    ssn::gfx::Slice is = dev.alloc(vb->indices.size() * 2 + 4, 16);
    std::memcpy(is.data, vb->indices.data(), vb->indices.size() * 2);
    vkCmdBindIndexBuffer(cb, is.buffer, is.offset, VK_INDEX_TYPE_UINT16);
    for (int32_t r = 0; r < repeat; ++r) vkCmdDrawIndexed(cb, count, 1, first + r * count, 0, 0);
  } else {
    for (int32_t r = 0; r < repeat; ++r) vkCmdDraw(cb, count, 1, first + r * count, 0);
  }
  return kOk;
}

}  // namespace ssn::psm::gfx
