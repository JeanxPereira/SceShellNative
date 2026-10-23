// The BackgroundLayer "LightParticle" layer — see bglayer_particle.h for the object model.
//
// Shaders (embedded in the eboot, table 0xCC95F8): #68 particle_c (compute), #70 particle_vv,
// #69 particle_p, and for the large particles of the boot animations (modes 3 / 4) #72
// large_particle_vv / #71 large_particle_p; transcribed by tools/shaders/isa2glsl.py into
// launcher/shaders/bglayer/particle/ (one GLSL statement per RDNA instruction).
//
// Large particles: Prepare (0x94020) starts the resource job 0x96240, which loads
// /system_ex/vsh_asset/Sce.Vsh.ShellUI.BGLayer.Particle0.gnf / Particle1.gnf (or, when a file
// is missing or unreadable, a 1x1 8_8_8_8 texture of 0xFF000000), allocates the large emitter
// blocks (0x94ED0 / 0x953B0) and, once the job is done, copies the two T#s into the draw blocks
// (+0x20 / +0x40). particle_c simulates them with the regular emitters; large_particle_vv/p
// draw them additively into the same 10_11_11 target (RenderInternal 0x96860).
#include "eboot/bglayer_particle.h"

#include <atomic>
#include <cmath>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <limits>
#include <mutex>
#include <thread>

#include "core/firmware.h"
#include "core/log.h"
#include "eboot/agc_gpu_address.h"
#include "eboot/bglayer.h"
#include "eboot/bglayer_gpu.h"
#include "eboot/bglayer_light.h"
#include "host/boundary.h"
#include "host/context.h"

namespace ssn::eboot::bglayer {
namespace {

constexpr uint32_t kPoolSize = 6000;     // AllocateRegularBuffer counts (0x94020: 0x1770)
constexpr uint32_t kParticleStride = 68;  // sub_87D8B0(v, 0, 68, 6000)
constexpr float kFltMax = std::numeric_limits<float>::max();  // 0x7F7FFFFF

// 0xD60E10: effect names by index (relocated pointers; strings at 0xB23434, 0xB0084A, 0xB63761,
// 0xB52273, 0xB09801, 0xB165C3, 0xB6C60F).
const char* const kEffectNames[7] = {"coldboot", "spread_expanded", "spread_expanded_fadeout", "bottom_camCal",
                                     "bottom_fadeout", "initboot_to_spread_no_movie", "initboot_to_bottom_no_movie"};
constexpr uint64_t kModeLightScaleVa = 0xBC1DB8;  // float[6]: light +0xCC per mode - 1
constexpr uint64_t kModeEffectVa = 0xBC1DD0;      // u32[6]: effect index per mode - 1

// xorshift128+ state of the particle layer (qword_D60E88 / qword_D60E90, initial values in the
// eboot's data; nothing else writes them).
uint64_t g_rng_s0 = 0x112210F47DE98115ull;
uint64_t g_rng_s1 = 0x7Bull;


// V# of a regular buffer (sub_87D8B0 + sub_87DDD0): base 0 (the host binds the buffer itself),
// STRIDE in dword1 [29:16], NUM_RECORDS in dword2, dword3 = FORMAT 5 << 12 | DST_SEL 0x204 |
// OOB_SELECT 0 (structured: index checked against NUM_RECORDS).
void make_vsharp(uint32_t out[4], uint32_t stride, uint32_t count) {
  out[0] = 0;
  out[1] = (stride << 16) & 0x3FFF0000u;
  out[2] = count;
  out[3] = (5u << 12) | 0x204u;
}

}  // namespace

// ---------------------------------------------------------------------------------------------
// GPU objects of the layer
struct ParticleLayer::Gpu {
  ssn::gfx::Buffer particles, perm1, perm2;  // +1872, +1880, +1888
  // The two end-of-pipe labels (+1976 / +1984): Initialize creates them with sub_DF890(label, 0),
  // an 8-byte GPU-visible slot (allocator qword_EC2C20 +32, or sub_DBD20) holding 0. Here one
  // host-visible coherent buffer, label i at offset 8 * i.
  ssn::gfx::Buffer labels;
  VkDescriptorSetLayout set = VK_NULL_HANDLE;
  VkPipelineLayout layout = VK_NULL_HANDLE;
  VkPipeline compute = VK_NULL_HANDLE;
  VkPipeline draw = VK_NULL_HANDLE;
  VkFormat draw_format = VK_FORMAT_UNDEFINED;
  PFN_vkCmdPushDescriptorSetKHR push = nullptr;
  ssn::gfx::Image rt;  // +1896: the 10_11_11_FLOAT target (sub_E1850(w, h, 17))
  // Large particles: layout with the two textures, large_particle_vv/p pipeline, +1768 / +1776.
  VkDescriptorSetLayout set_large = VK_NULL_HANDLE;
  VkPipelineLayout layout_large = VK_NULL_HANDLE;
  VkPipeline draw_large = VK_NULL_HANDLE;
  VkFormat draw_large_format = VK_FORMAT_UNDEFINED;
  ssn::gfx::Image tex[2];

  ~Gpu() {
    auto& dev = ssn::gfx::device();
    VkDevice d = dev.device();
    if (!d) return;
    vkDeviceWaitIdle(d);
    for (auto* b : {&particles, &perm1, &perm2, &labels})
      if (b->buffer) {
        vkDestroyBuffer(d, b->buffer, nullptr);
        vkFreeMemory(d, b->memory, nullptr);
      }
    if (rt.valid()) dev.destroy_image(rt);
    for (auto& t : tex)
      if (t.valid()) dev.destroy_image(t);
  }

  static ssn::gfx::Buffer storage(VkDeviceSize size, VkBufferUsageFlags usage = VK_BUFFER_USAGE_STORAGE_BUFFER_BIT) {
    auto& dev = ssn::gfx::device();
    VkDevice d = dev.device();
    ssn::gfx::Buffer b;
    b.size = size;
    VkBufferCreateInfo bi{VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO};
    bi.size = size;
    bi.usage = usage;
    if (vkCreateBuffer(d, &bi, nullptr, &b.buffer) != VK_SUCCESS) return {};
    VkMemoryRequirements req;
    vkGetBufferMemoryRequirements(d, b.buffer, &req);
    VkMemoryAllocateInfo ai{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO};
    ai.allocationSize = req.size;
    ai.memoryTypeIndex = dev.memory_type(req.memoryTypeBits, VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT);
    vkAllocateMemory(d, &ai, nullptr, &b.memory);
    vkBindBufferMemory(d, b.buffer, b.memory, 0);
    vkMapMemory(d, b.memory, 0, size, 0, &b.mapped);
    std::memset(b.mapped, 0, size);  // AllocateRegularBuffer: memset 0
    return b;
  }

  bool make_layout() {
    VkDevice d = ssn::gfx::device().device();
    const VkShaderStageFlags all = VK_SHADER_STAGE_COMPUTE_BIT | VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT;
    // binding 0: the SRT (user-data CB), 1: the emitter block it points to, 2: PermBuf, 3: ParticleBuf
    VkDescriptorSetLayoutBinding b[4] = {
        {0, VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, 1, all, nullptr},
        {1, VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, 1, all, nullptr},
        {2, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 1, all, nullptr},
        {3, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 1, all, nullptr},
    };
    VkDescriptorSetLayoutCreateInfo si{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO};
    si.flags = VK_DESCRIPTOR_SET_LAYOUT_CREATE_PUSH_DESCRIPTOR_BIT_KHR;
    si.bindingCount = 4;
    si.pBindings = b;
    if (vkCreateDescriptorSetLayout(d, &si, nullptr, &set) != VK_SUCCESS) return false;
    VkPipelineLayoutCreateInfo li{VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO};
    li.setLayoutCount = 1;
    li.pSetLayouts = &set;
    if (vkCreatePipelineLayout(d, &li, nullptr, &layout) != VK_SUCCESS) return false;
    push = reinterpret_cast<PFN_vkCmdPushDescriptorSetKHR>(vkGetDeviceProcAddr(d, "vkCmdPushDescriptorSetKHR"));
    VkShaderModule cs = shader_module("bglayer_particle_c");
    if (!cs) return false;
    VkComputePipelineCreateInfo ci{VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO};
    ci.stage = {VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO, nullptr, 0, VK_SHADER_STAGE_COMPUTE_BIT, cs, "main"};
    ci.layout = layout;
    return vkCreateComputePipelines(d, VK_NULL_HANDLE, 1, &ci, nullptr, &compute) == VK_SUCCESS;
  }

  // particle_vv + particle_p, triangle list (sub_86C2A0(4)), no culling, blend control +1912:
  // sub_86E4F0 with {enable 1, colour src 1 ONE, dst 5 ONE_MINUS_SRC_ALPHA, func 0 ADD,
  // alpha src 1, dst 5, func 0} (config xmmword_B8BD00 + 5; CB_BLEND0_CONTROL built by 0x862FD0).
  bool make_draw(VkFormat fmt) {
    draw = make_pipeline(layout, "bglayer_particle_vv", "bglayer_particle_p", fmt, VK_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA);
    if (!draw) return false;
    draw_format = fmt;
    return true;
  }

  // large_particle_vv + large_particle_p, triangle list, blend control +1920: sub_86E4F0 with
  // {enable 1, colour src 1 ONE, dst 1 ONE, func 0 ADD, alpha src 1, dst 1, func 0} (config
  // xmmword_B8B1F0 = {1, 1, 0, 1} + 1, Initialize 0x93AB0): additive.
  bool make_draw_large(VkFormat fmt) {
    VkDevice d = ssn::gfx::device().device();
    if (!layout_large) {
      const VkShaderStageFlags all = VK_SHADER_STAGE_COMPUTE_BIT | VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT;
      // 0 SRT, 1 draw block, 2 PermBuf, 3 ParticleBuf (as the regular draws), 4 / 5 the T#s the
      // eboot stores at block +0x20 / +0x40 with the S# large_particle_p builds from immediates.
      VkDescriptorSetLayoutBinding b[6] = {
          {0, VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, 1, all, nullptr},
          {1, VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, 1, all, nullptr},
          {2, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 1, all, nullptr},
          {3, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 1, all, nullptr},
          {4, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 1, VK_SHADER_STAGE_FRAGMENT_BIT, nullptr},
          {5, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 1, VK_SHADER_STAGE_FRAGMENT_BIT, nullptr},
      };
      VkDescriptorSetLayoutCreateInfo si{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO};
      si.flags = VK_DESCRIPTOR_SET_LAYOUT_CREATE_PUSH_DESCRIPTOR_BIT_KHR;
      si.bindingCount = 6;
      si.pBindings = b;
      if (vkCreateDescriptorSetLayout(d, &si, nullptr, &set_large) != VK_SUCCESS) return false;
      VkPipelineLayoutCreateInfo li{VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO};
      li.setLayoutCount = 1;
      li.pSetLayouts = &set_large;
      if (vkCreatePipelineLayout(d, &li, nullptr, &layout_large) != VK_SUCCESS) return false;
    }
    draw_large = make_pipeline(layout_large, "bglayer_large_particle_vv", "bglayer_large_particle_p", fmt, VK_BLEND_FACTOR_ONE);
    if (!draw_large) return false;
    draw_large_format = fmt;
    return true;
  }

  // Triangle list, no culling, blend {enable, ADD, src ONE, dst `dst`} on colour and alpha.
  static VkPipeline make_pipeline(VkPipelineLayout lay, const char* vs_name, const char* fs_name, VkFormat fmt,
                                  VkBlendFactor dst) {
    VkDevice d = ssn::gfx::device().device();
    VkShaderModule vs = shader_module(vs_name), fs = shader_module(fs_name);
    if (!vs || !fs) return VK_NULL_HANDLE;
    VkPipelineShaderStageCreateInfo st[2]{};
    st[0] = {VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO, nullptr, 0, VK_SHADER_STAGE_VERTEX_BIT, vs, "main"};
    st[1] = {VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO, nullptr, 0, VK_SHADER_STAGE_FRAGMENT_BIT, fs, "main"};
    VkPipelineVertexInputStateCreateInfo vi{VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO};
    VkPipelineInputAssemblyStateCreateInfo ia{VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO};
    ia.topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST;
    VkPipelineViewportStateCreateInfo vp{VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO};
    vp.viewportCount = 1;
    vp.scissorCount = 1;
    VkPipelineRasterizationStateCreateInfo rs{VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO};
    rs.polygonMode = VK_POLYGON_MODE_FILL;
    rs.cullMode = VK_CULL_MODE_NONE;
    rs.frontFace = VK_FRONT_FACE_COUNTER_CLOCKWISE;
    rs.lineWidth = 1.0f;
    VkPipelineMultisampleStateCreateInfo ms{VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO};
    ms.rasterizationSamples = VK_SAMPLE_COUNT_1_BIT;
    VkPipelineDepthStencilStateCreateInfo ds{VK_STRUCTURE_TYPE_PIPELINE_DEPTH_STENCIL_STATE_CREATE_INFO};
    VkPipelineColorBlendAttachmentState ba{};
    ba.blendEnable = VK_TRUE;
    ba.colorBlendOp = ba.alphaBlendOp = VK_BLEND_OP_ADD;
    ba.srcColorBlendFactor = ba.srcAlphaBlendFactor = VK_BLEND_FACTOR_ONE;
    ba.dstColorBlendFactor = ba.dstAlphaBlendFactor = dst;
    ba.colorWriteMask = 0xF;  // CB_TARGET_MASK 0xF (particle_p / large_particle_p cx 0x8F)
    VkPipelineColorBlendStateCreateInfo cb{VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO};
    cb.attachmentCount = 1;
    cb.pAttachments = &ba;
    const VkDynamicState dyn[] = {VK_DYNAMIC_STATE_VIEWPORT, VK_DYNAMIC_STATE_SCISSOR};
    VkPipelineDynamicStateCreateInfo dy{VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO};
    dy.dynamicStateCount = 2;
    dy.pDynamicStates = dyn;
    VkPipelineRenderingCreateInfo ri{VK_STRUCTURE_TYPE_PIPELINE_RENDERING_CREATE_INFO};
    ri.colorAttachmentCount = 1;
    ri.pColorAttachmentFormats = &fmt;
    VkGraphicsPipelineCreateInfo pi{VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO};
    pi.pNext = &ri;
    pi.stageCount = 2;
    pi.pStages = st;
    pi.pVertexInputState = &vi;
    pi.pInputAssemblyState = &ia;
    pi.pViewportState = &vp;
    pi.pRasterizationState = &rs;
    pi.pMultisampleState = &ms;
    pi.pDepthStencilState = &ds;
    pi.pColorBlendState = &cb;
    pi.pDynamicState = &dy;
    pi.layout = lay;
    VkPipeline p = VK_NULL_HANDLE;
    if (vkCreateGraphicsPipelines(d, VK_NULL_HANDLE, 1, &pi, nullptr, &p) != VK_SUCCESS) return VK_NULL_HANDLE;
    return p;
  }

  void push_set_large(VkCommandBuffer cb, const ssn::gfx::Slice& srt, const ssn::gfx::Slice& blk) {
    VkDescriptorBufferInfo bi[4] = {
        {srt.buffer, srt.offset, 32},
        {blk.buffer, blk.offset, 320},
        {perm1.buffer, 0, VK_WHOLE_SIZE},
        {particles.buffer, 0, VK_WHOLE_SIZE},
    };
    // S# of large_particle_p 0x288..0x294: s[16:19] = {0x92, 0, 37 << 20, 0}.
    const VkSampler smp = sampler_for_ssharp(0x92, 0, 37u << 20, 0);
    VkDescriptorImageInfo ii[2] = {{smp, tex[0].view, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL},
                                   {smp, tex[1].view, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL}};
    VkWriteDescriptorSet w[6]{};
    for (uint32_t i = 0; i < 6; ++i) {
      w[i].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
      w[i].dstBinding = i;
      w[i].descriptorCount = 1;
      if (i < 4) {
        w[i].descriptorType = i < 2 ? VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER : VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
        w[i].pBufferInfo = &bi[i];
      } else {
        w[i].descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
        w[i].pImageInfo = &ii[i - 4];
      }
    }
    push(cb, VK_PIPELINE_BIND_POINT_GRAPHICS, layout_large, 0, 6, w);
  }

  void push_set(VkCommandBuffer cb, VkPipelineBindPoint bp, const ssn::gfx::Slice& srt, const ssn::gfx::Slice& blk) {
    VkDescriptorBufferInfo bi[4] = {
        {srt.buffer, srt.offset, 32},
        {blk.buffer, blk.offset, 320},
        {perm1.buffer, 0, VK_WHOLE_SIZE},
        {particles.buffer, 0, VK_WHOLE_SIZE},
    };
    VkWriteDescriptorSet w[4]{};
    for (uint32_t i = 0; i < 4; ++i) {
      w[i].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
      w[i].dstBinding = i;
      w[i].descriptorCount = 1;
      w[i].descriptorType = i < 2 ? VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER : VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
      w[i].pBufferInfo = &bi[i];
    }
    push(cb, bp, layout, 0, 4, w);
  }
};

// ---------------------------------------------------------------------------------------------
// The resource job (ResourceJob "LightParticle": sub_3AFC70, vtable 0xCC8E40, run 0x976B0 ->
// 0x96240 "ResourceJobLoad"). It runs on a worker thread as on the console; the host splits
// sub_86E120 (CreateFromGnf) in two: the GNF is read and detiled here, the Vulkan image is made
// on the render thread when Prepare finds the job done (make_large_textures).
struct ParticleLayer::Job {
  struct Result {
    bool ok = false;
    std::string why;  // why the firmware's 1x1 fallback is taken
    agc::TextureSpec spec;
    std::vector<uint8_t> linear;
  } tex[2];
  std::atomic<bool> done{false};  // +1944
  std::thread thread;

  Job() { thread = std::thread([this] { run(); }); }
  ~Job() {
    if (thread.joinable()) thread.join();
  }
  void run() {  // 0x96240
    static const char* const kPath[2] = {"/system_ex/vsh_asset/Sce.Vsh.ShellUI.BGLayer.Particle0.gnf",
                                         "/system_ex/vsh_asset/Sce.Vsh.ShellUI.BGLayer.Particle1.gnf"};
    for (int i = 0; i < 2; ++i) {
      Result& r = tex[i];
      Firmware* fw = host::ctx().firmware;
      const std::string path = fw ? fw->host_path(kPath[i]) : std::string();
      // fopen "rb" + stat + fread of st_size bytes; any failure -> the fallback.
      std::ifstream f(std::filesystem::path(path), std::ios::binary);
      if (!fw || !f) {
        r.why = std::string(kPath[i]) + " not in the firmware roots";
        continue;
      }
      std::vector<uint8_t> data((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
      std::string err;
      if (!agc::decode_gnf(data.data(), data.size(), r.spec, r.linear, err)) {  // sub_86E120 < 0
        r.why = std::string(kPath[i]) + ": " + err;
        continue;
      }
      r.ok = true;
    }
    done.store(true, std::memory_order_release);  // +1944 = 1
  }
};

bool ParticleLayer::make_large_textures() {
  for (int i = 0; i < 2; ++i) {
    const Job::Result& r = job_->tex[i];
    ssn::gfx::Image& t = gpu_->tex[i];
    auto& dev = ssn::gfx::device();
    if (t.valid()) {
      vkDeviceWaitIdle(dev.device());
      dev.destroy_image(t);
    }
    std::string err = r.why;
    if (r.ok) {
      if (r.spec.levels != 1)
        log::warn("bglayer", "LightParticle: Particle{}.gnf has {} mip levels; large_particle_p is transcribed for one", i,
                  r.spec.levels);
      if (create_gnf_texture(r.spec, r.linear, t, err)) {
        log::info("bglayer", "LightParticle: Particle{}.gnf {}x{} (T# format {})", i, r.spec.width, r.spec.height, r.spec.format);
        continue;
      }
    }
    // The fallback of 0x96240: sub_86DC00 with a 1x1x1 description of XF format 18 (qword_C9E4C0[18]
    // = data format 0x38, 8_8_8_8 UNORM, dst_sel XYZW) and the texel 0xFF000000 (0, 0, 0, 1).
    log::info("bglayer", "LightParticle: Particle{} texture is the job's 1x1 fallback ({})", i, err);
    t = dev.create_image(1, 1, 1, VK_FORMAT_R8G8B8A8_UNORM, VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT);
    if (!t.valid()) return false;
    const uint32_t texel = 0xFF000000u;
    dev.upload_image(t, 0, 0, 0, 1, 1, &texel, 4);
  }
  return true;
}

// ---------------------------------------------------------------------------------------------
ParticleLayer::ParticleLayer(LightLayer& light) : light_(light) {}
ParticleLayer::~ParticleLayer() = default;

uint32_t ParticleLayer::xorshift() {  // 0x97740 / 0x97810 / 0x978E0 / 0x979B0
  const uint64_t a = g_rng_s0, b = g_rng_s1;
  const uint64_t x = a ^ (a << 23);
  const uint64_t n = x ^ b ^ (b >> 26) ^ (x >> 17);
  g_rng_s0 = b;
  g_rng_s1 = n;
  return static_cast<uint32_t>(n + b);
}

bool ParticleLayer::initialize() {  // 0x93AB0
  gpu_ = std::make_unique<Gpu>();
  if (!gpu_->make_layout()) {
    log::error("bglayer", "LightParticle: particle_c pipeline not created");
    gpu_.reset();
    return false;
  }
  // +1976 / +1984: the two labels (sub_DF890(label, 0): slot = 0); +1992 = 0.
  gpu_->labels = Gpu::storage(16, VK_BUFFER_USAGE_TRANSFER_DST_BIT);
  if (!gpu_->labels.mapped) {
    log::error("bglayer", "LightParticle: labels not allocated");
    gpu_.reset();
    return false;
  }
  // The effect object (192 bytes, sub_98430) starts empty; the blobs are parsed on demand (0x95920).
  return true;
}

// --- layer base -------------------------------------------------------------------------------
bool ParticleLayer::on_load() {  // 0x94000
  time_ = 0.0f;
  return true;
}
bool ParticleLayer::on_unload() { return true; }  // 0x94010
bool ParticleLayer::label_pending() const {
  // **(label + 24) != 0 for either label: Render (0x971B0) stores 1 when it records the frame's
  // particle work and the GPU stores 0 at the end of the pipe once that work is done (see
  // render()). The CPU reads the slot as the GPU left it; frame N is still in flight while the
  // CPU runs frame N+1 (Device::present, as libScePsm's SwapBuffers).
  if (!gpu_ || !gpu_->labels.mapped) return false;
  const volatile uint64_t* l = static_cast<const volatile uint64_t*>(gpu_->labels.mapped);
  return l[0] != 0 || l[1] != 0;
}

bool ParticleLayer::busy() const {  // 0x97470
  // unk_EC2C40 (0, never written) == 0 and one of the two end-of-pipe labels pending.
  return label_pending();
}

bool ParticleLayer::show() {  // 0xDD640
  if (pending == 1) return false;
  if (pending == 2) {
    pending = 0;
    return state != 1;
  }
  if (static_cast<uint32_t>(state - 2) < 4) return true;
  if (state == 6) {
    pending = 1;
  } else if (state == 0) {
    state = 1;
    if (on_load()) {
      state = 2;
      return true;
    }
  }
  return false;
}

bool ParticleLayer::hide() {  // 0xDD6B0
  if (pending == 2) return false;
  if (pending == 1) {
    pending = 0;
    return false;
  }
  switch (state) {
    case 0: return true;
    case 1: case 3: case 4: case 5: pending = 2; return false;
    case 2:
      state = 6;
      if (!on_unload()) return false;
      state = 0;
      return true;
    default: return false;
  }
}

void ParticleLayer::set_opacity(float o) {  // 0xDD560 (auto-hide +24 = 1)
  opacity = o;
  if (o > 0.0001f) {
    if (state == 2) state = 3;
  } else if (state == 4) {
    state = 5;
    if (!busy() && release()) state = 2;
  }
}

int32_t ParticleLayer::busy_level() {  // 0xDD4F0, type 2
  if (busy()) return 2;
  return (state & ~1) == 4 ? 2 : 0;
}

void ParticleLayer::step(int32_t r15, float) {  // 0xDD210 (type +96 = 2, auto-hide)
  auto try_prepare = [&](bool gate) {  // type 2: prepare only when no other layer is loading
    if (gate) {
      state = 3;
      return;
    }
    state = prepare() ? ((pending == 2) | 4) : 3;
  };
  switch (state) {
    case 1:
      if (!on_load()) break;  // vtable +32
      if (pending == 2) {
        state = 6;
        pending = 0;
        break;
      }
      if (0.0001f >= opacity) {  // auto-hide: vucomiss 0.0001, +0x58; jnb 0xDD411
        state = 2;
        break;
      }
      try_prepare(r15 != 0);
      break;
    case 2:
      if (0.0001f >= opacity) break;  // jnb default
      [[fallthrough]];
    case 3:
      try_prepare(r15 != 0);
      break;
    case 4:
      if (pending == 2 || 0.0001f > opacity) state = 5;  // jbe default otherwise
      break;
    case 5:
      if (busy()) break;      // vtable +104
      if (!release()) break;  // vtable +56
      if (pending == 2) {
        state = 6;
        pending = 0;
      } else {
        state = 2;
      }
      break;
    case 6:
      if (!on_unload()) break;  // vtable +40
      if (pending == 1) {
        state = 1;
        pending = 0;
      } else {
        state = 0;
      }
      break;
    default: break;
  }
  // states 2..5 then run Update (vtable +64): a nullsub for this layer (0x96630).
}

// --- mode / effect ----------------------------------------------------------------------------
void ParticleLayer::switch_generation() {  // 0x94020 tail / 0x97560 / 0x96FF0
  const uint32_t old = generation_ & 1;
  const uint32_t nw = old ? 0u : 1u;
  generation_ = nw;
  gen_start_[nw] = time_;
  gen_end_[nw] = kFltMax;
  // unk_EC2C40 == 0 and a label pending (the inlined 0x97470) -> the old generation runs 7 s
  // more (dword_AFFB7C); otherwise it ends now.
  gen_end_[old] = label_pending() ? time_ + 7.0f : time_;
}

void ParticleLayer::set_mode(int32_t mode) {  // 0x97560
  const int32_t cur = mode_;
  if (cur == mode || (static_cast<uint32_t>(mode - 3) <= 1 && cur != 0)) return;
  mode_ = mode;
  const uint32_t m = static_cast<uint32_t>(mode - 1);
  if (m >= 6) {
    large_ = false;
    return;
  }
  float scale[6];
  uint32_t effect[6];
  const auto s = eboot_bytes(kModeLightScaleVa, sizeof scale);
  const auto e = eboot_bytes(kModeEffectVa, sizeof effect);
  if (s.size() != sizeof scale || e.size() != sizeof effect) return;
  std::memcpy(scale, s.data(), sizeof scale);
  std::memcpy(effect, e.data(), sizeof effect);
  const int32_t old_effect = effect_index_;
  effect_index_ = static_cast<int32_t>(effect[m]);
  large_ = ((0x1010000ull >> (8 * m)) & 0xFF) != 0;  // modes 3 and 4
  light_.intensity_scale = scale[m];                 // light +0xCC
  if (old_effect != effect_index_) {
    switch_generation();
    if (state == 4) load_effect();
  }
}

void ParticleLayer::load_effect() {  // 0x95920
  if (static_cast<uint32_t>(effect_index_) > 6) return;
  if (!effect_.load(kEffectNames[effect_index_])) {  // sub_99280
    log::error("bglayer", "LightParticle: effect '{}' not parsed", kEffectNames[effect_index_]);
    return;
  }
  log::info("bglayer", "LightParticle: effect '{}' (mode {}), generation {}", kEffectNames[effect_index_], mode_, generation_);
  if (mode_ == 3) effect_.shift_times(3.5f);  // dword_AFFF98
  if (mode_ == 4) effect_.shift_times(0.5f);  // dword_AFFD7C
  if (mode_ == 6) effect_.delay_vs(1.0f, 0.6f, 1.0f);  // dword_AFF918, dword_AFFAC4
  // 0x99080: initial blocks of the current generation (window [time 0, FLT_MAX]).
  if (cs_[0][0]) {
    uint8_t* cs[8];
    uint8_t* vs[8];
    for (int i = 0; i < 8; ++i) {
      cs[i] = cs_[generation_ & 1][i] ? cs_[generation_ & 1][i]->data() : nullptr;
      vs[i] = vs_[generation_ & 1][i] ? vs_[generation_ & 1][i]->data() : nullptr;
    }
    const ParticleEffect::VsharpTable vt = {{{nullptr, nullptr, nullptr, nullptr},
                                             {reinterpret_cast<const uint8_t*>(vsharp_perm1_),
                                              reinterpret_cast<const uint8_t*>(vsharp_perm2_), nullptr, nullptr}}};
    effect_.apply_sets(ParticleEffect::kInitCs, 0.0f, kFltMax, cs, vt);
    effect_.apply_sets(ParticleEffect::kInitVs, 0.0f, kFltMax, vs, vt);
    cs_time_offset_ = effect_.tail[3];  // +0x7B0 = effect +0xB4
    vs_time_offset_ = effect_.tail[4];  // +0x7B4 = effect +0xB8
  }
  // 0x99190: the large-particle blocks of the current generation (allocated by Prepare only in
  // modes 3 / 4): vector 18 into the CS blocks (+1496), vector 19 into the VsPs blocks (+1656).
  if (lcs_[0][0]) {
    const uint32_t g = generation_ & 1;
    uint8_t* cs[8] = {};
    uint8_t* vs[8] = {};
    for (int e = 0; e < 2; ++e) {
      cs[e] = lcs_[g][e] ? lcs_[g][e]->data() : nullptr;
      vs[e] = lvs_[g][e] ? lvs_[g][e]->data() : nullptr;
    }
    const ParticleEffect::VsharpTable vt = {{{nullptr, nullptr, nullptr, nullptr},
                                             {reinterpret_cast<const uint8_t*>(vsharp_perm1_),
                                              reinterpret_cast<const uint8_t*>(vsharp_perm2_), nullptr, nullptr}}};
    effect_.apply_sets(ParticleEffect::kInitLargeCs, 0.0f, kFltMax, cs, vt);
    effect_.apply_sets(ParticleEffect::kInitLargeVs, 0.0f, kFltMax, vs, vt);
  }
}

// --- prepare / release --------------------------------------------------------------------------
bool ParticleLayer::prepare() {  // 0x94020
  // Large particles wanted (+1816) and no job yet (+1952): +1944 = 0, the resource job is created
  // and queued (sub_DC460); it loads Particle0/1.gnf on its thread.
  if (large_ && !job_) job_ = std::make_unique<Job>();
  time_ = 0.0f;  // +1792
  if (!gpu_) return false;
  if (!buffers_) {
    // +1872: 6000 x 68-byte particles; +1880 / +1888: 6000 u32 each, an inside-out Fisher-Yates
    // permutation drawn from the xorshift128+ (j = (s0 + s1) mod (i + 1), 32-bit).
    gpu_->particles = Gpu::storage(VkDeviceSize(kPoolSize) * kParticleStride);
    make_vsharp(vsharp_particles_, kParticleStride, kPoolSize);  // +1820
    gpu_->perm1 = Gpu::storage(VkDeviceSize(kPoolSize) * 4);
    make_vsharp(vsharp_perm1_, 4, kPoolSize);                     // +1836
    gpu_->perm2 = Gpu::storage(VkDeviceSize(kPoolSize) * 4);
    make_vsharp(vsharp_perm2_, 4, kPoolSize);                     // +1852
    if (!gpu_->particles.mapped || !gpu_->perm1.mapped || !gpu_->perm2.mapped) return false;
    for (auto* buf : {&gpu_->perm1, &gpu_->perm2}) {
      uint32_t* p = static_cast<uint32_t*>(buf->mapped);
      uint64_t s0 = g_rng_s0, s1 = g_rng_s1;
      for (uint32_t i = 0; i < kPoolSize; ++i) {
        const uint64_t a = s0;
        s0 = s1;
        const uint64_t x = a ^ (a << 23);
        s1 = x ^ s0 ^ (s0 >> 26) ^ (x >> 17);
        const uint32_t j = (static_cast<uint32_t>(s0) + static_cast<uint32_t>(s1)) % (i + 1);
        if (i != j) p[i] = p[j];
        p[j] = i;
      }
      g_rng_s0 = s0;
      g_rng_s1 = s1;
    }
    buffers_ = true;
  }
  // initializeBuffers (0x94820 "m_resSrtCs", 0x94B70 "m_resSrtVsPs"): per generation, the
  // missing parameter blocks are allocated zeroed and seeded by the functors.
  for (int g = 0; g < 2; ++g)
    for (int i = 0; i < 8; ++i)
      if (!cs_[g][i]) {
        cs_[g][i] = std::make_unique<std::array<uint8_t, 248>>();
        cs_[g][i]->fill(0);
        const uint32_t r = xorshift();                   // 0x97740
        std::memcpy(cs_[g][i]->data() + 36, &r, 4);
        std::memcpy(cs_[g][i]->data() + 0, vsharp_perm1_, 16);       // +0x72C
        std::memcpy(cs_[g][i]->data() + 16, vsharp_particles_, 16);  // +0x71C
      }
  for (int g = 0; g < 2; ++g)
    for (int i = 0; i < 8; ++i)
      if (!vs_[g][i]) {
        vs_[g][i] = std::make_unique<std::array<uint8_t, 320>>();
        vs_[g][i]->fill(0);
        const uint32_t r = xorshift();                   // 0x97810
        std::memcpy(vs_[g][i]->data() + 88, &r, 4);
        std::memcpy(vs_[g][i]->data() + 0, vsharp_particles_, 16);  // +0x71C
        std::memcpy(vs_[g][i]->data() + 16, vsharp_perm1_, 16);     // +0x72C
      }
  if (large_) {
    // m_largeParResSrtCs (0x94ED0): per generation two 248-byte CPU blocks, functor 0x978E0 =
    // the regular CS functor: xorshift seed at +36, PermBuf V# at +0, ParticleBuf V# at +16.
    for (int g = 0; g < 2; ++g)
      for (int e = 0; e < 2; ++e)
        if (!lcs_[g][e]) {
          lcs_[g][e] = std::make_unique<std::array<uint8_t, 248>>();
          lcs_[g][e]->fill(0);
          const uint32_t r = xorshift();
          std::memcpy(lcs_[g][e]->data() + 36, &r, 4);
          std::memcpy(lcs_[g][e]->data() + 0, vsharp_perm1_, 16);       // +0x72C
          std::memcpy(lcs_[g][e]->data() + 16, vsharp_particles_, 16);  // +0x71C
        }
    // m_largeParResSrtVsPs (0x953B0): two 236-byte CPU blocks per generation, functor 0x979B0:
    // seed at +168, +100..+107 = 0, +108 = 1.0, +120 = (float)+0x710 / (float)+0x714 (the ctor
    // 0x938D0 stores 0xF00 / 0x870: 3840 / 2160), ParticleBuf V# at +0, PermBuf V# at +16.
    for (int g = 0; g < 2; ++g)
      for (int e = 0; e < 2; ++e)
        if (!lvs_[g][e]) {
          lvs_[g][e] = std::make_unique<std::array<uint8_t, 236>>();
          lvs_[g][e]->fill(0);
          uint8_t* b = lvs_[g][e]->data();
          const uint32_t r = xorshift();
          std::memcpy(b + 168, &r, 4);
          const float one = 1.0f, aspect = static_cast<float>(0xF00) / static_cast<float>(0x870);
          std::memcpy(b + 108, &one, 4);
          std::memcpy(b + 120, &aspect, 4);
          std::memcpy(b + 0, vsharp_particles_, 16);  // +0x71C
          std::memcpy(b + 16, vsharp_perm1_, 16);     // +0x72C
        }
  }
  if (job_) {
    // The job must have finished (+1944); otherwise Prepare fails and runs again next update.
    if (!job_->done.load(std::memory_order_acquire)) return false;
    // T#s of +1768 / +1776 into +0x20 / +0x40 of the four VsPs blocks (sub_877530, 32 bytes each):
    // on the host the two textures are bound to large_particle_p's T# slots for every large draw.
    if (!make_large_textures()) return false;
    job_.reset();  // +1952 / +1960 released
  }
  switch_generation();  // LABEL_38
  load_effect();        // 0x95920
  skip_render_ = 0;     // +1804
  return true;
}

bool ParticleLayer::release() {  // 0x95A10
  // sub_95FC0(+1592) / sub_96100(+1432): the large blocks; +1816 = 0.
  for (auto& g : lvs_) for (auto& p : g) p.reset();
  for (auto& g : lcs_) for (auto& p : g) p.reset();
  large_ = false;
  for (auto& g : cs_) for (auto& p : g) p.reset();
  for (auto& g : vs_) for (auto& p : g) p.reset();
  if (gpu_) {
    auto& dev = ssn::gfx::device();
    VkDevice d = dev.device();
    vkDeviceWaitIdle(d);
    for (auto* b : {&gpu_->particles, &gpu_->perm1, &gpu_->perm2})
      if (b->buffer) {
        vkDestroyBuffer(d, b->buffer, nullptr);
        vkFreeMemory(d, b->memory, nullptr);
        *b = {};
      }
    if (gpu_->rt.valid()) dev.destroy_image(gpu_->rt);  // +1896
    for (auto& t : gpu_->tex)                              // +1768 / +1776 (sub_872270)
      if (t.valid()) dev.destroy_image(t);
  }
  buffers_ = false;
  mode_ = 0;           // +1996
  effect_index_ = 255; // +1904
  return true;
}

void ParticleLayer::set_time_step(bool paused, float frame_ms) {  // 0x97510
  if (paused || state != 4) {
    dt_ = 0.0f;
  } else if (9.99999997e-07f > dt_) {  // dword_AFFDDC
    dt_ = 0.0166700002f;                // 0x3C88AB7D
  } else {
    dt_ = frame_ms * 0.001f;            // dword_B00144
  }
}

// --- per frame ----------------------------------------------------------------------------------
void ParticleLayer::update_timeline() {  // 0x96FF0
  if (static_cast<uint32_t>(mode_ - 3) <= 1) {
    const float base = mode_ == 3 ? 3.5f : 0.5f;  // unk_B8E3B0 {0.5, 3.5}[mode == 3]
    const float x = (time_ - (base + gen_start_[generation_ & 1])) + -1.0f;
    if (x > 1.5f) {  // dword_AFFBB8
      mode_ = 1;
      const int32_t old_effect = effect_index_;
      effect_index_ = 1;
      large_ = false;
      light_.intensity_scale = 1.0f;
      if (old_effect != 1) {
        switch_generation();
        if (state == 4) load_effect();
      }
    } else {
      light_.intensity_scale = x > 0.0f ? x * 0.666666687f : 0.0f;  // dword_AFF93C
    }
  }
  const float t = time_ - gen_start_[generation_ & 1];
  uint8_t* cs[8];
  uint8_t* vs[8];
  for (int i = 0; i < 8; ++i) {
    cs[i] = cs_[generation_ & 1][i] ? cs_[generation_ & 1][i]->data() : nullptr;
    vs[i] = vs_[generation_ & 1][i] ? vs_[generation_ & 1][i]->data() : nullptr;
  }
  const ParticleEffect::VsharpTable vt = {{{nullptr, nullptr, nullptr, nullptr},
                                           {reinterpret_cast<const uint8_t*>(vsharp_perm1_),
                                            reinterpret_cast<const uint8_t*>(vsharp_perm2_), nullptr, nullptr}}};
  if (cs_[0][0]) {  // 0x98CC0 (with interpolation)
    const float ts = t + effect_.tail[0];  // +0xA8
    const float ti = t + effect_.tail[1];  // +0xAC
    effect_.apply_sets(ParticleEffect::kSetCs, ts, dt_, cs, vt);
    effect_.apply_interps(ParticleEffect::kInterpCs, ti, dt_, cs);
    effect_.apply_sets(ParticleEffect::kSetVs, ts, dt_, vs, vt);
    effect_.apply_interps(ParticleEffect::kInterpVs, ti, dt_, vs);
  }
  // 0x98ED0 (interpolation flag 1): the large-particle timelines at the same t and step, without
  // the +0xA8 / +0xAC offsets: sets 10 -> CS blocks, interpolations 12 -> CS, sets 11 -> VsPs,
  // interpolations 13 -> VsPs (blocks +1496 / +1656 of the current generation).
  if (lcs_[0][0]) {
    uint8_t* lcs[8] = {};
    uint8_t* lvs[8] = {};
    for (int e = 0; e < 2; ++e) {
      lcs[e] = lcs_[generation_ & 1][e] ? lcs_[generation_ & 1][e]->data() : nullptr;
      lvs[e] = lvs_[generation_ & 1][e] ? lvs_[generation_ & 1][e]->data() : nullptr;
    }
    effect_.apply_sets(ParticleEffect::kSetLargeCs, t, dt_, lcs, vt);
    effect_.apply_interps(ParticleEffect::kInterpLargeCs, t, dt_, lcs);
    effect_.apply_sets(ParticleEffect::kSetLargeVs, t, dt_, lvs, vt);
    effect_.apply_interps(ParticleEffect::kInterpLargeVs, t, dt_, lvs);
  }
}

// simulateParticles (0x96640): one particle_c dispatch for an emitter block with a non-zero count
// (+40); the caller sets +1817 after every call. The compute pipeline is bound by the caller.
void ParticleLayer::dispatch(uint8_t* blk, uint32_t g, uint32_t cur, float tcur) {
  auto& dev = ssn::gfx::device();
  VkCommandBuffer cb = dev.cmd();
  uint32_t count;
  std::memcpy(&count, blk + 40, 4);
  dispatched_ = true;  // +1817
  if (count == 0) return;
  // SRTCs (28 bytes): +0 pointer to the block, +8 time, +12 step, +16 kill rate, +20 a6 (0),
  // +24 generation byte.
  ssn::gfx::Slice srt = dev.alloc(256, 256);
  std::memset(srt.data, 0, 32);
  const float tt = tcur + cs_time_offset_;  // + (+0x7B0)
  float rate;
  if (cur == g) {
    rate = 1.0f;
  } else {
    float f60;
    std::memcpy(&f60, blk + 0x3C, 4);
    rate = f60 * 0.153846159f;  // dword_AFF98C
  }
  const uint32_t a6 = 0, bits = (g & 0xF) | ((cur << 4) & 0xF0);
  uint8_t* s = static_cast<uint8_t*>(srt.data);
  std::memcpy(s + 8, &tt, 4);
  std::memcpy(s + 12, &dt_, 4);
  std::memcpy(s + 16, &rate, 4);
  std::memcpy(s + 20, &a6, 4);
  std::memcpy(s + 24, &bits, 4);
  ssn::gfx::Slice p = dev.alloc(320, 256);
  std::memset(p.data, 0, 320);
  std::memcpy(p.data, blk, 248);
  gpu_->push_set(cb, VK_PIPELINE_BIND_POINT_COMPUTE, srt, p);
  vkCmdDispatch(cb, (count + 63) >> 6, 1, 1);  // sub_86C800
}

void ParticleLayer::render_internal(ssn::gfx::Image& rt) {  // 0x96860
  update_timeline();
  auto& dev = ssn::gfx::device();
  VkCommandBuffer cb = dev.cmd();
  const uint32_t cur = generation_ & 1;
  const float tcur = time_ - gen_start_[cur];
  // --- simulateParticles (0x96640) for both generations still alive ---------------------------
  dev.end_rendering();
  {
    VkMemoryBarrier2 mb{VK_STRUCTURE_TYPE_MEMORY_BARRIER_2};
    mb.srcStageMask = VK_PIPELINE_STAGE_2_ALL_GRAPHICS_BIT | VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT;
    mb.srcAccessMask = VK_ACCESS_2_SHADER_WRITE_BIT;
    mb.dstStageMask = VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT;
    mb.dstAccessMask = VK_ACCESS_2_SHADER_READ_BIT | VK_ACCESS_2_SHADER_WRITE_BIT;
    VkDependencyInfo di{VK_STRUCTURE_TYPE_DEPENDENCY_INFO};
    di.memoryBarrierCount = 1;
    di.pMemoryBarriers = &mb;
    vkCmdPipelineBarrier2(cb, &di);
  }
  vkCmdBindPipeline(cb, VK_PIPELINE_BIND_POINT_COMPUTE, gpu_->compute);  // sub_8645D0(+104)
  for (uint32_t g = 0; g < 2; ++g) {
    if (time_ + dt_ > gen_end_[g]) continue;  // ja: generation over
    for (int i = 0; i < 8; ++i)
      if (cs_[g][i]) dispatch(cs_[g][i]->data(), g, cur, tcur);
    // The large emitters of the generation (+1496 / +1504 + 80g) with their own constant buffers
    // (+1432 / +1440 and +1464 / +1472 + 80g + 16 * (frame & 1)), through the same sub_96640.
    for (int e = 0; e < 2; ++e)
      if (lcs_[g][e]) dispatch(lcs_[g][e]->data(), g, cur, tcur);
  }
  // sub_883030(dcb + 456, 1, 19, ...): compute results visible to the draws.
  {
    VkMemoryBarrier2 mb{VK_STRUCTURE_TYPE_MEMORY_BARRIER_2};
    mb.srcStageMask = VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT;
    mb.srcAccessMask = VK_ACCESS_2_SHADER_WRITE_BIT;
    mb.dstStageMask = VK_PIPELINE_STAGE_2_VERTEX_SHADER_BIT | VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT;
    mb.dstAccessMask = VK_ACCESS_2_SHADER_READ_BIT | VK_ACCESS_2_SHADER_WRITE_BIT;
    VkDependencyInfo di{VK_STRUCTURE_TYPE_DEPENDENCY_INFO};
    di.memoryBarrierCount = 1;
    di.pMemoryBarriers = &mb;
    vkCmdPipelineBarrier2(cb, &di);
  }
  // --- draws (RenderInternal): particle_vv / particle_p, 6 vertices per particle ----------------
  if (!gpu_->draw || gpu_->draw_format != rt.format) {
    if (!gpu_->make_draw(rt.format)) log::error("bglayer", "LightParticle: particle_vv/particle_p pipeline not created");
  }
  if (gpu_->draw) {
    dev.bind_target(&rt, nullptr);
    dev.ensure_rendering();
    cb = dev.cmd();
    vkCmdBindPipeline(cb, VK_PIPELINE_BIND_POINT_GRAPHICS, gpu_->draw);
    // SetViewport(0, 0, w, h, ...) of the particle target; PS5 clip space +Y is the top.
    VkViewport vp{0.0f, float(rt.height), float(rt.width), -float(rt.height), 0.0f, 1.0f};
    VkRect2D sc{{0, 0}, {rt.width, rt.height}};
    vkCmdSetViewport(cb, 0, 1, &vp);
    vkCmdSetScissor(cb, 0, 1, &sc);
    for (uint32_t g = 0; g < 2; ++g) {
      if (time_ + dt_ > gen_end_[g]) continue;
      for (uint32_t i = 0; i < 8; ++i) {
        const auto& blk = vs_[g][i];
        if (!blk) continue;
        uint32_t count;
        std::memcpy(&count, blk->data() + 32, 4);
        if (count == 0) continue;
        // SRTVsPs (20 bytes): +0 pointer, +8 time (+0x700 + +0x7B4), +12 step, +16 generation byte.
        ssn::gfx::Slice srt = dev.alloc(256, 256);
        std::memset(srt.data, 0, 32);
        const float tt = time_ + vs_time_offset_;
        const uint32_t bits = (g & 0xF) | ((cur << 4) & 0xF0);
        uint8_t* s = static_cast<uint8_t*>(srt.data);
        std::memcpy(s + 8, &tt, 4);
        std::memcpy(s + 12, &dt_, 4);
        std::memcpy(s + 16, &bits, 4);
        ssn::gfx::Slice p = dev.alloc(320, 256);
        std::memcpy(p.data, blk->data(), 320);
        std::memcpy(static_cast<uint8_t*>(p.data) + 316, &i, 4);  // +0x13C = emitter index
        gpu_->push_set(cb, VK_PIPELINE_BIND_POINT_GRAPHICS, srt, p);
        vkCmdDraw(cb, 6 * count, 1, 0, 0);  // sub_86B740(6 * count)
      }
    }
  }
  // --- large particles (0x96B9F..0x96F4D): large_particle_vv / large_particle_p (+128 / +136),
  // blend +1920, 6 vertices per particle, count at +172 of the 236-byte VsPs block -------------
  bool any_large = false;
  for (const auto& g : lvs_)
    for (const auto& b : g) any_large = any_large || b;
  if (any_large && gpu_->tex[0].valid() && gpu_->tex[1].valid()) {
    if (!gpu_->draw_large || gpu_->draw_large_format != rt.format) {
      if (!gpu_->make_draw_large(rt.format))
        log::error("bglayer", "LightParticle: large_particle_vv/large_particle_p pipeline not created");
    }
    if (gpu_->draw_large) {
      dev.bind_target(&rt, nullptr);
      dev.ensure_rendering();
      cb = dev.cmd();
      vkCmdBindPipeline(cb, VK_PIPELINE_BIND_POINT_GRAPHICS, gpu_->draw_large);
      VkViewport vp{0.0f, float(rt.height), float(rt.width), -float(rt.height), 0.0f, 1.0f};
      VkRect2D sc{{0, 0}, {rt.width, rt.height}};
      vkCmdSetViewport(cb, 0, 1, &vp);
      vkCmdSetScissor(cb, 0, 1, &sc);
      for (uint32_t g = 0; g < 2; ++g) {
        if (time_ + dt_ > gen_end_[g]) continue;
        for (uint32_t e = 0; e < 2; ++e) {
          const auto& blk = lvs_[g][e];
          if (!blk) continue;
          uint32_t count;
          std::memcpy(&count, blk->data() + 172, 4);
          if (count == 0) continue;
          // SRTLargeParticleVsPs (20 bytes; CB of 24): +0 pointer, +8 time (+0x700, no offset),
          // +12 step, +16 generation byte.
          ssn::gfx::Slice srt = dev.alloc(256, 256);
          std::memset(srt.data, 0, 32);
          const uint32_t bits = (g & 0xF) | ((cur << 4) & 0xF0);
          uint8_t* s = static_cast<uint8_t*>(srt.data);
          std::memcpy(s + 8, &time_, 4);
          std::memcpy(s + 12, &dt_, 4);
          std::memcpy(s + 16, &bits, 4);
          ssn::gfx::Slice p = dev.alloc(320, 256);
          std::memset(p.data, 0, 320);
          std::memcpy(p.data, blk->data(), 236);
          gpu_->push_set_large(cb, srt, p);
          vkCmdDraw(cb, 6 * count, 1, 0, 0);  // sub_86B740(6 * count)
        }
      }
    }
  }
  time_ = dt_ + time_;  // +0x700 += +0x704
  ++frame_;             // +0x708
}

void ParticleLayer::render(ssn::gfx::Image& target, const int32_t viewport[4]) {  // 0x971B0
  if (!gpu_) return;
  auto& dev = ssn::gfx::device();
  std::lock_guard<std::recursive_mutex> record(dev.record_lock());
  if (skip_render_ == 0 && buffers_) render_frame(viewport);  // +1804 == 0
  write_label();
}

void ParticleLayer::write_label() {  // 0x971B0 tail
  // label = +1976 + 8 * (XF context +24). The context's +24 is flipped by 0x71C80, which the
  // BGLayer's post-swap hooks (0x8F170 / 0x8F190 -> 0x72020 / 0x71FC0) run once after each
  // frame it rendered; the particle layer renders in every such frame, so the index alternates
  // between two consecutive renders, as label_index_ does here.
  // **(label + 24) = 1, then XF WriteAtEndOfPipe (sub_DEAA0(ctx, label, 0) -> sub_889D10(dcb +456,
  // 4, 12, 0, slot address, 2, 0, 0, 0) -> sceAgcCbReleaseMem (NID wr23dPKyWc0): a release_mem
  // packet that stores the immediate 0 into the slot once the work before it has drained).
  // Host: the barrier waits for all commands recorded before it (the frame's particle
  // dispatches and draws), the fill stores the 64-bit 0, then made visible to host reads.
  auto& dev = ssn::gfx::device();
  const uint32_t idx = label_index_;
  label_index_ ^= 1u;
  static_cast<volatile uint64_t*>(gpu_->labels.mapped)[idx] = 1;
  VkCommandBuffer cb = dev.cmd();
  dev.end_rendering();
  VkMemoryBarrier2 mb{VK_STRUCTURE_TYPE_MEMORY_BARRIER_2};
  mb.srcStageMask = VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT;
  mb.srcAccessMask = VK_ACCESS_2_MEMORY_WRITE_BIT;
  mb.dstStageMask = VK_PIPELINE_STAGE_2_CLEAR_BIT;
  mb.dstAccessMask = VK_ACCESS_2_TRANSFER_WRITE_BIT;
  VkDependencyInfo di{VK_STRUCTURE_TYPE_DEPENDENCY_INFO};
  di.memoryBarrierCount = 1;
  di.pMemoryBarriers = &mb;
  vkCmdPipelineBarrier2(cb, &di);
  vkCmdFillBuffer(cb, gpu_->labels.buffer, VkDeviceSize(8) * idx, 8, 0);
  mb.srcStageMask = VK_PIPELINE_STAGE_2_CLEAR_BIT;
  mb.srcAccessMask = VK_ACCESS_2_TRANSFER_WRITE_BIT;
  mb.dstStageMask = VK_PIPELINE_STAGE_2_HOST_BIT;
  mb.dstAccessMask = VK_ACCESS_2_HOST_READ_BIT;
  vkCmdPipelineBarrier2(cb, &di);
}

void ParticleLayer::render_frame(const int32_t viewport[4]) {  // 0x971B0, +1804 == 0
  auto& dev = ssn::gfx::device();
  const uint32_t w = static_cast<uint32_t>(viewport[2]), h = static_cast<uint32_t>(viewport[3]);
  ssn::gfx::Image& rt = gpu_->rt;
  if (!rt.valid() || rt.width != w || rt.height != h) {
    if (rt.valid()) {
      vkDeviceWaitIdle(dev.device());
      dev.destroy_image(rt);
    }
    // XF format 17 -> qword_C9E4C0[17] = data format 0x24 (10_11_11_FLOAT), dst_sel X Y Z 1.
    rt = dev.create_image(w, h, 1, VK_FORMAT_B10G11R11_UFLOAT_PACK32,
                          VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_SAMPLED_BIT);
    if (!rt.valid()) return;
  }
  ssn::gfx::Image* prev_color = dev.target_color();
  ssn::gfx::Image* prev_depth = dev.target_depth();
  // SetRenderTarget(+1896) and a hardware clear to (0, 0, 0, 0) (sub_DF2A0 with a3 = 0).
  dev.bind_target(&rt, nullptr);
  dev.ensure_rendering();
  {
    VkClearAttachment a{VK_IMAGE_ASPECT_COLOR_BIT, 0, {}};
    a.clearValue.color = {{0.0f, 0.0f, 0.0f, 0.0f}};
    VkClearRect r{{{0, 0}, {rt.width, rt.height}}, 0, 1};
    vkCmdClearAttachments(dev.cmd(), 1, &a, 1, &r);
  }
  render_internal(rt);  // 0x96860
  dev.bind_target(prev_color, prev_depth);
  // sub_9EB90(light, target texture, +2016 * +0x58): the light composites the particles.
  light_.set_particle_texture(&rt, particle_alpha_scale_ * opacity);
}

}  // namespace ssn::eboot::bglayer
