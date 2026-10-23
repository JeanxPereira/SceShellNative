// GPU helpers of the background layer host: GNF textures (PS5 T# + AgcGpuAddress detiling), the S#
// immediates the eboot shaders build, and the Vulkan objects the layers draw with.
#pragma once
#include <vulkan/vulkan.h>

#include <cstdint>
#include <string>
#include <vector>

#include "eboot/agc_gpu_address.h"
#include "gfx/vk_device.h"

namespace ssn::eboot::bglayer {

// A texture created from a GNF image. The container, the T# and the tiled data are decoded by
// agc::decode_gnf (agc_gpu_address.h): the eboot's CreateFromGnf (sub_876D30) container rules,
// then the firmware's Agc::translate / AgcGpuAddress computeAssetSummary / detileSurface as
// reproduced there (swizzle modes 0 and 5, one mip level). The eboot loads its embedded IES
// tables with sub_86E120 (GNF in memory) and the vsh_asset particle textures from files.
// Vulkan formats: T# format 1 -> R8_UNORM, 181 -> BC7_UNORM; one plain 2D image (type 9).
// Anything else fails and says why in `err`.
bool load_gnf(const uint8_t* data, size_t size, ssn::gfx::Image& out, std::string& err);
// The GPU half of load_gnf: the texture for a GNF image already decoded by agc::decode_gnf (the
// particle layer's loading job decodes on its worker thread; the image is created on the render
// thread). Same format / type rules as load_gnf.
bool create_gnf_texture(const agc::TextureSpec& spec, const std::vector<uint8_t>& linear, ssn::gfx::Image& out, std::string& err);

// VkSampler for a GFX10 S# (4 dwords) as the shaders write it in s_mov immediates: CLAMP_X/Y/Z,
// XY_MAG/MIN_FILTER, MIP_FILTER, MIN/MAX_LOD, BORDER_COLOR_TYPE. Cached per S#.
VkSampler sampler_for_ssharp(uint32_t d0, uint32_t d1, uint32_t d2, uint32_t d3);

// SPIR-V module from <exe dir>/shaders/<name>.spv (launcher/shaders/bglayer/*), cached.
VkShaderModule shader_module(const std::string& name);

// Full-screen pass pipeline: no vertex input, the given topology, no culling, blend
// {enable, Add, SrcAlpha, OneMinusSrcAlpha} on colour and alpha (the XF state BackgroundLayer
// Render sets with sub_DF7A0/DF7B0/DF7C0 + sub_DE620) or, with `blend` false, blending disabled
// (the zero state sub_DF760 the particle layer restores), dynamic viewport/scissor, rendering
// into `color_format`.
struct PassLayout {
  VkDescriptorSetLayout set = VK_NULL_HANDLE;  // push-descriptor set 0
  VkPipelineLayout layout = VK_NULL_HANDLE;
};
PassLayout make_pass_layout(uint32_t uniform_buffers, uint32_t textures, uint32_t push_bytes);
VkPipeline make_fullscreen_pipeline(const PassLayout& l, VkShaderModule vs, VkShaderModule fs, VkPrimitiveTopology topology,
                                    VkFormat color_format, bool blend = true);

}  // namespace ssn::eboot::bglayer
