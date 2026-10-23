// GPU helpers of the background layer host (see bglayer_gpu.h).
#include "eboot/bglayer_gpu.h"
#include "eboot/agc_gpu_address.h"

#include <windows.h>

#include <array>
#include <cstring>
#include <filesystem>
#include <format>
#include <fstream>
#include <string>
#include <map>
#include <mutex>
#include <tuple>
#include <vector>

#include "core/log.h"
#include "eboot/bglayer.h"
#include "host/context.h"

namespace ssn::eboot::bglayer {

// --- eboot data ---------------------------------------------------------------------------
namespace {

// The ShellUI eboot builds the host knows. Every eboot address in psmhost is cited from the
// reference build (FW 4.03 retail, "W:/Build/J01736823/..."). Another build of the same sources
// places the data the host reads elsewhere (the rodata and .data move by several different small
// deltas, so no single offset works); its table lists, for each address the host reads, where
// that build holds the same bytes. Each entry was checked byte for byte between the two images
// and against the code that references it in both builds (work/ida_r7diff/report.md §3a).
struct EbootBuild {
  const char* name;
  uint8_t build_id[16];                                  // GNU build-id (PT_NOTE, NT_GNU_BUILD_ID)
  std::vector<std::pair<uint64_t, uint64_t>> map;        // J01736823 VA -> this build's VA; empty: identity
};

const std::vector<EbootBuild>& eboot_builds() {
  static const std::vector<EbootBuild> builds = {
      {"FW 4.03 retail J01736823 (reference)",
       {0xeb, 0x24, 0x01, 0x4c, 0xdf, 0x2c, 0x2c, 0x34, 0x90, 0xa7, 0xae, 0x2a, 0xd9, 0x6d, 0x66, 0x35},
       {}},
      {"FW 4.03 devkit J01736824",
       {0x45, 0x40, 0xa5, 0x2b, 0x1a, 0x10, 0x55, 0xd3, 0x0a, 0xa7, 0xcd, 0x17, 0x9a, 0x95, 0xd2, 0x02},
       {
           {0xBB0AE0, 0xBB5600}, {0xBB0B98, 0xBB56B8},  // NGS2 Music System rack options (sub_911F0 / sub_91250)
           {0xD61220, 0xD65230}, {0xD612C0, 0xD652D0},  // transition ripple / slide parameters (.data)
           {0xB001B8, 0xB05658},                        // gradation solver pivot epsilon
           {0xB8A530, 0xB8F000}, {0xB8B7F0, 0xB902D0}, {0xAFFC24, 0xB050C4}, {0xB8B200, 0xB8FCE0},
           {0xB8AF60, 0xB8FA40}, {0xB8C870, 0xB91360}, {0xB8A8A0, 0xB8F380},  // gradation inputs (0xA82A0)
           {0xB8D2A0, 0xB91DC0}, {0xB8D3E0, 0xB91F00}, {0xB8D400, 0xB91F20}, {0xB8DC20, 0xB92740},
           {0xB8B670, 0xB90150},                        // preset table (Initialize 0x6FF90)
           {0xBC6180, 0xBCACA0}, {0xBC2080, 0xBC6BA0}, {0xBC1EC0, 0xBC69E0},  // IES floor / volume, colour table
           {0xBC1DB8, 0xBC68D8}, {0xBC1DD0, 0xBC68F0},  // per-mode light scale / effect index
           {0xBB0D80, 0xBB58A0},                        // particle effect blob sizes
           {0xBB0DC0, 0xBB58E0}, {0xBB2D70, 0xBB7890}, {0xBB4B70, 0xBB9690}, {0xBB72E0, 0xBBBE00},
           {0xBB9B40, 0xBBE660}, {0xBBC250, 0xBC0D70}, {0xBBEBB0, 0xBC36D0},  // particle effect blobs
           {0xBE9970, 0xBEE4B0}, {0xBE9AA0, 0xBEE5E0},  // IME keyboard default layout tables (layout 0xCD1F58 / 0xCD5F78)
           {0xBE9010, 0xBEDB50},                        // IME keyboard non-character keycodes (0x1AE590 / devkit)
       }},
  };
  return builds;
}

const EbootBuild* build_of(const std::vector<uint8_t>& id) {
  for (const auto& b : eboot_builds())
    if (id.size() >= sizeof b.build_id && std::memcmp(id.data(), b.build_id, sizeof b.build_id) == 0) return &b;
  return nullptr;
}

struct EbootImage {
  bool loaded = false;
  std::vector<uint8_t> file;
  struct Seg {
    uint64_t vaddr, filesz, offset;
  };
  std::vector<Seg> segs;
  std::vector<uint8_t> build_id;
  const EbootBuild* build = nullptr;
};

EbootImage& eboot() {
  static EbootImage img;
  static std::once_flag once;
  std::call_once(once, [] {
    Firmware* fw = host::ctx().firmware;
    if (!fw) return;
    const std::string path = fw->host_path("/system_ex/app/NPXS40087/eboot.bin");
    std::ifstream f(std::filesystem::path(path), std::ios::binary);
    if (!f) {
      log::error("bglayer", "cannot read the ShellUI eboot {}", path);
      return;
    }
    img.file.assign(std::istreambuf_iterator<char>(f), std::istreambuf_iterator<char>());
    const auto& d = img.file;
    // Decrypted eboot: a plain ELF64 (the dump's NPXS40087/eboot.bin; the analysed IDA database
    // work/ida_eboot/eboot.elf is the same file).
    if (d.size() < 64 || std::memcmp(d.data(), "\x7f" "ELF", 4) != 0 || d[4] != 2) {
      log::error("bglayer", "{} is not an ELF64 image", path);
      return;
    }
    uint64_t phoff;
    uint16_t phentsize, phnum;
    std::memcpy(&phoff, &d[0x20], 8);
    std::memcpy(&phentsize, &d[0x36], 2);
    std::memcpy(&phnum, &d[0x38], 2);
    for (uint32_t i = 0; i < phnum; ++i) {
      const size_t o = static_cast<size_t>(phoff) + size_t(i) * phentsize;
      if (o + 56 > d.size()) break;
      uint32_t type;
      std::memcpy(&type, &d[o], 4);
      if (type != 1) continue;  // PT_LOAD
      EbootImage::Seg s{};
      std::memcpy(&s.offset, &d[o + 8], 8);
      std::memcpy(&s.vaddr, &d[o + 16], 8);
      std::memcpy(&s.filesz, &d[o + 32], 8);
      img.segs.push_back(s);
    }
    // The build: the GNU build-id of the PT_NOTE segment (type 4, note type 3).
    for (uint32_t i = 0; i < phnum; ++i) {
      const size_t o = static_cast<size_t>(phoff) + size_t(i) * phentsize;
      if (o + 56 > d.size()) break;
      uint32_t type;
      uint64_t off, filesz;
      std::memcpy(&type, &d[o], 4);
      std::memcpy(&off, &d[o + 8], 8);
      std::memcpy(&filesz, &d[o + 32], 8);
      if (type != 4) continue;
      for (uint64_t n = off; n + 12 <= off + filesz && n + 12 <= d.size();) {
        uint32_t namesz, descsz, ntype;
        std::memcpy(&namesz, &d[n], 4);
        std::memcpy(&descsz, &d[n + 4], 4);
        std::memcpy(&ntype, &d[n + 8], 4);
        const uint64_t desc = n + 12 + ((namesz + 3) & ~3u);
        if (desc + descsz > d.size()) break;
        if (ntype == 3 && namesz == 4 && std::memcmp(&d[n + 12], "GNU", 4) == 0)
          img.build_id.assign(d.begin() + static_cast<ptrdiff_t>(desc), d.begin() + static_cast<ptrdiff_t>(desc + descsz));
        n = desc + ((descsz + 3) & ~3u);
      }
    }
    img.build = build_of(img.build_id);
    if (!img.build) {
      std::string hex;
      for (uint8_t b : img.build_id) hex += std::format("{:02x}", b);
      log::error("bglayer", "ShellUI eboot {}: unknown build (GNU build-id {}); its data tables are not read", path, hex);
    } else {
      log::info("bglayer", "ShellUI eboot build {}", img.build->name);
    }
    img.loaded = !img.segs.empty();
  });
  return img;
}

// Address of reference-build data in the loaded build.
uint64_t build_va(const EbootImage& img, uint64_t va) {
  if (!img.build) return 0;
  if (img.build->map.empty()) return va;  // the reference build itself
  for (const auto& [from, to] : img.build->map)
    if (from == va) return to;
  log::error("bglayer", "eboot data 0x{:X} (J01736823) has no address in build {}", va, img.build->name);
  return 0;
}

}  // namespace

std::vector<uint8_t> eboot_bytes(uint64_t va_ref, size_t size) {
  const EbootImage& img = eboot();
  if (!img.loaded) return {};
  const uint64_t va = build_va(img, va_ref);
  if (!va) return {};
  for (const auto& s : img.segs) {
    if (va >= s.vaddr && va + size <= s.vaddr + s.filesz) {
      const uint64_t off = s.offset + (va - s.vaddr);
      if (off + size > img.file.size()) return {};
      return {img.file.begin() + static_cast<ptrdiff_t>(off), img.file.begin() + static_cast<ptrdiff_t>(off + size)};
    }
  }
  return {};
}

// --- GNF ------------------------------------------------------------------------------------
bool load_gnf(const uint8_t* data, size_t size, ssn::gfx::Image& out, std::string& err) {
  agc::TextureSpec spec;
  std::vector<uint8_t> linear;
  if (!agc::decode_gnf(data, size, spec, linear, err)) return false;
  return create_gnf_texture(spec, linear, out, err);
}

bool create_gnf_texture(const agc::TextureSpec& spec, const std::vector<uint8_t>& linear, ssn::gfx::Image& out, std::string& err) {
  if (spec.type != 9 || spec.slices != 1) {  // one plain 2D image
    err = "T# type " + std::to_string(spec.type) + " with " + std::to_string(spec.slices) + " slices";
    return false;
  }
  VkFormat vf;
  if (spec.format == 1) {
    vf = VK_FORMAT_R8_UNORM;
  } else if (spec.format == 181) {
    vf = VK_FORMAT_BC7_UNORM_BLOCK;
  } else {
    err = "T# format " + std::to_string(spec.format) + " has no Vulkan mapping here";
    return false;
  }
  const uint32_t width = spec.width, height = spec.height;
  auto& dev = ssn::gfx::device();
  out = dev.create_image(width, height, 1, vf, VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT);
  if (!out.valid()) {
    err = "vkCreateImage failed";
    return false;
  }
  dev.upload_image(out, 0, 0, 0, width, height, linear.data(), linear.size());
  return true;
}

// --- samplers -------------------------------------------------------------------------------
VkSampler sampler_for_ssharp(uint32_t d0, uint32_t d1, uint32_t d2, uint32_t d3) {
  static std::mutex m;
  static std::map<std::tuple<uint32_t, uint32_t, uint32_t, uint32_t>, VkSampler> cache;
  std::lock_guard<std::mutex> lk(m);
  auto key = std::make_tuple(d0, d1, d2, d3);
  if (auto it = cache.find(key); it != cache.end()) return it->second;
  // SQ_TEX_CLAMP: 0 WRAP, 1 MIRROR, 2 CLAMP_LAST_TEXEL, 3 MIRROR_ONCE_LAST_TEXEL, 4 CLAMP_HALF_BORDER,
  // 5 MIRROR_ONCE_HALF_BORDER, 6 CLAMP_BORDER, 7 MIRROR_ONCE_BORDER.
  auto clamp = [](uint32_t c) {
    switch (c) {
      case 0: return VK_SAMPLER_ADDRESS_MODE_REPEAT;
      case 1: return VK_SAMPLER_ADDRESS_MODE_MIRRORED_REPEAT;
      case 2: return VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
      case 3: return VK_SAMPLER_ADDRESS_MODE_MIRROR_CLAMP_TO_EDGE;
      default: return VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_BORDER;  // 4..7 (half-border variants: see note)
    }
  };
  VkSamplerCreateInfo si{VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO};
  si.addressModeU = clamp(d0 & 7);
  si.addressModeV = clamp((d0 >> 3) & 7);
  si.addressModeW = clamp((d0 >> 6) & 7);
  // d1: MIN_LOD [11:0], MAX_LOD [23:12] (4.8 fixed point).
  si.minLod = float(d1 & 0xfff) / 256.0f;
  si.maxLod = float((d1 >> 12) & 0xfff) / 256.0f;
  // d2: XY_MAG_FILTER [21:20], XY_MIN_FILTER [23:22], MIP_FILTER [27:26] (0 point/none, 1 bilinear/point, ...).
  si.magFilter = ((d2 >> 20) & 3) ? VK_FILTER_LINEAR : VK_FILTER_NEAREST;
  si.minFilter = ((d2 >> 22) & 3) ? VK_FILTER_LINEAR : VK_FILTER_NEAREST;
  const uint32_t mip = (d2 >> 26) & 3;  // SQ_TEX_Z_FILTER: 0 NONE, 1 POINT, 2 LINEAR
  si.mipmapMode = mip == 2 ? VK_SAMPLER_MIPMAP_MODE_LINEAR : VK_SAMPLER_MIPMAP_MODE_NEAREST;
  if (mip == 0) si.maxLod = 0.0f;  // no mip filter: base level only
  // d3: BORDER_COLOR_TYPE [31:30]: 0 TRANS_BLACK, 1 OPAQUE_BLACK, 2 OPAQUE_WHITE, 3 register.
  const uint32_t border = d3 >> 30;
  si.borderColor = border == 1 ? VK_BORDER_COLOR_FLOAT_OPAQUE_BLACK
                   : border == 2 ? VK_BORDER_COLOR_FLOAT_OPAQUE_WHITE
                                 : VK_BORDER_COLOR_FLOAT_TRANSPARENT_BLACK;
  VkSampler s = VK_NULL_HANDLE;
  vkCreateSampler(ssn::gfx::device().device(), &si, nullptr, &s);
  cache.emplace(key, s);
  return s;
}

// --- shaders / pipelines -------------------------------------------------------------------
VkShaderModule shader_module(const std::string& name) {
  static std::mutex m;
  static std::map<std::string, VkShaderModule> cache;
  std::lock_guard<std::mutex> lk(m);
  if (auto it = cache.find(name); it != cache.end()) return it->second;
  wchar_t buf[MAX_PATH];
  GetModuleFileNameW(nullptr, buf, MAX_PATH);
  const auto path = std::filesystem::path(buf).parent_path() / "shaders" / (name + ".spv");
  VkShaderModule mod = VK_NULL_HANDLE;
  std::ifstream f(path, std::ios::binary);
  if (f) {
    std::vector<char> code((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
    VkShaderModuleCreateInfo ci{VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO};
    ci.codeSize = code.size();
    ci.pCode = reinterpret_cast<const uint32_t*>(code.data());
    vkCreateShaderModule(ssn::gfx::device().device(), &ci, nullptr, &mod);
  } else {
    log::error("bglayer", "missing shader {}", path.string());
  }
  cache.emplace(name, mod);
  return mod;
}

PassLayout make_pass_layout(uint32_t uniform_buffers, uint32_t textures, uint32_t push_bytes) {
  VkDevice d = ssn::gfx::device().device();
  std::vector<VkDescriptorSetLayoutBinding> b;
  for (uint32_t i = 0; i < uniform_buffers; ++i)
    b.push_back({i, VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, 1, VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT, nullptr});
  for (uint32_t i = 0; i < textures; ++i)
    b.push_back({uniform_buffers + i, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 1, VK_SHADER_STAGE_FRAGMENT_BIT, nullptr});
  PassLayout l;
  VkDescriptorSetLayoutCreateInfo si{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO};
  si.flags = VK_DESCRIPTOR_SET_LAYOUT_CREATE_PUSH_DESCRIPTOR_BIT_KHR;
  si.bindingCount = static_cast<uint32_t>(b.size());
  si.pBindings = b.data();
  vkCreateDescriptorSetLayout(d, &si, nullptr, &l.set);
  VkPushConstantRange pr{VK_SHADER_STAGE_FRAGMENT_BIT, 0, push_bytes};
  VkPipelineLayoutCreateInfo li{VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO};
  li.setLayoutCount = 1;
  li.pSetLayouts = &l.set;
  li.pushConstantRangeCount = push_bytes ? 1 : 0;
  li.pPushConstantRanges = &pr;
  vkCreatePipelineLayout(d, &li, nullptr, &l.layout);
  return l;
}

VkPipeline make_fullscreen_pipeline(const PassLayout& l, VkShaderModule vs, VkShaderModule fs, VkPrimitiveTopology topology,
                                    VkFormat color_format, bool blend) {
  if (!vs || !fs) return VK_NULL_HANDLE;
  VkPipelineShaderStageCreateInfo st[2]{};
  st[0] = {VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO, nullptr, 0, VK_SHADER_STAGE_VERTEX_BIT, vs, "main"};
  st[1] = {VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO, nullptr, 0, VK_SHADER_STAGE_FRAGMENT_BIT, fs, "main"};
  VkPipelineVertexInputStateCreateInfo vi{VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO};
  VkPipelineInputAssemblyStateCreateInfo ia{VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO};
  ia.topology = topology;
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
  ba.blendEnable = blend ? VK_TRUE : VK_FALSE;
  ba.colorBlendOp = ba.alphaBlendOp = VK_BLEND_OP_ADD;                                    // op 0
  ba.srcColorBlendFactor = ba.srcAlphaBlendFactor = VK_BLEND_FACTOR_SRC_ALPHA;            // 4
  ba.dstColorBlendFactor = ba.dstAlphaBlendFactor = VK_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA;  // 5
  ba.colorWriteMask = 0xf;
  VkPipelineColorBlendStateCreateInfo cb{VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO};
  cb.attachmentCount = 1;
  cb.pAttachments = &ba;
  const VkDynamicState dyn[] = {VK_DYNAMIC_STATE_VIEWPORT, VK_DYNAMIC_STATE_SCISSOR};
  VkPipelineDynamicStateCreateInfo dy{VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO};
  dy.dynamicStateCount = 2;
  dy.pDynamicStates = dyn;
  VkPipelineRenderingCreateInfo ri{VK_STRUCTURE_TYPE_PIPELINE_RENDERING_CREATE_INFO};
  ri.colorAttachmentCount = 1;
  ri.pColorAttachmentFormats = &color_format;
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
  pi.layout = l.layout;
  VkPipeline p = VK_NULL_HANDLE;
  vkCreateGraphicsPipelines(ssn::gfx::device().device(), VK_NULL_HANDLE, 1, &pi, nullptr, &p);
  return p;
}

}  // namespace ssn::eboot::bglayer
