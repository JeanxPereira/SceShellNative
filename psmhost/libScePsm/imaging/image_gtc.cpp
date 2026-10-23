#include "libScePsm/imaging/image_gtc.h"

#include <windows.h>

#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <mutex>
#include <string>

#include "core/log.h"
#include "gfx/vk_device.h"
#include "host/boundary.h"

namespace ssn::psm::imaging::gtc {
namespace {

// sub_296B70: the input texture is the RGBA8888 frame as an 8_8_8_8 UNORM texture (data format
// 0xFAC00000038 when the source is RGBA8888), the output an image of 128-bit blocks (u0) read
// back as the image's block data. The shader of table entry (format - 2) (0x4A23C0, 4 qwords
// per entry) is selected by the target format.
struct Pipeline {
  VkDescriptorSetLayout set = VK_NULL_HANDLE;
  VkPipelineLayout layout = VK_NULL_HANDLE;
  VkPipeline pipeline = VK_NULL_HANDLE;
  PFN_vkCmdPushDescriptorSetKHR push = nullptr;
};

VkShaderModule load_module(const char* name) {
  wchar_t buf[MAX_PATH];
  GetModuleFileNameW(nullptr, buf, MAX_PATH);
  const auto path = std::filesystem::path(buf).parent_path() / "shaders" / (std::string(name) + ".spv");
  std::ifstream f(path, std::ios::binary);
  if (!f) {
    log::error("psm.image", "missing shader {}", path.string());
    return VK_NULL_HANDLE;
  }
  std::vector<char> code((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
  VkShaderModuleCreateInfo ci{VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO};
  ci.codeSize = code.size();
  ci.pCode = reinterpret_cast<const uint32_t*>(code.data());
  VkShaderModule m = VK_NULL_HANDLE;
  vkCreateShaderModule(ssn::gfx::device().device(), &ci, nullptr, &m);
  return m;
}

const Pipeline* pipeline_for(uint32_t format) {
  static std::mutex mu;
  static Pipeline cache[9];
  static bool tried[9] = {};
  if (format < 2 || format > 8) return nullptr;
  std::lock_guard<std::mutex> lk(mu);
  Pipeline& p = cache[format];
  if (tried[format]) return p.pipeline ? &p : nullptr;
  tried[format] = true;
  static const char* kNames[9] = {nullptr, nullptr, "gtc_bc1", "gtc_bc2", "gtc_bc3", "gtc_bc4", "gtc_bc5", "gtc_bc6", "gtc_bc7"};
  if (format != 8) return nullptr;  // only the BC7 compressor is transcribed so far
  VkDevice d = ssn::gfx::device().device();
  VkShaderModule cs = load_module(kNames[format]);
  if (!cs) return nullptr;
  VkDescriptorSetLayoutBinding b[2] = {
      {0, VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, 1, VK_SHADER_STAGE_COMPUTE_BIT, nullptr},
      {1, VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, 1, VK_SHADER_STAGE_COMPUTE_BIT, nullptr},
  };
  VkDescriptorSetLayoutCreateInfo si{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO};
  si.flags = VK_DESCRIPTOR_SET_LAYOUT_CREATE_PUSH_DESCRIPTOR_BIT_KHR;
  si.bindingCount = 2;
  si.pBindings = b;
  if (vkCreateDescriptorSetLayout(d, &si, nullptr, &p.set) != VK_SUCCESS) return nullptr;
  VkPushConstantRange pr{VK_SHADER_STAGE_COMPUTE_BIT, 0, 4};
  VkPipelineLayoutCreateInfo li{VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO};
  li.setLayoutCount = 1;
  li.pSetLayouts = &p.set;
  li.pushConstantRangeCount = 1;
  li.pPushConstantRanges = &pr;
  if (vkCreatePipelineLayout(d, &li, nullptr, &p.layout) != VK_SUCCESS) return nullptr;
  p.push = reinterpret_cast<PFN_vkCmdPushDescriptorSetKHR>(vkGetDeviceProcAddr(d, "vkCmdPushDescriptorSetKHR"));
  VkComputePipelineCreateInfo ci{VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO};
  ci.stage = {VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO, nullptr, 0, VK_SHADER_STAGE_COMPUTE_BIT, cs, "main"};
  ci.layout = p.layout;
  if (vkCreateComputePipelines(d, VK_NULL_HANDLE, 1, &ci, nullptr, &p.pipeline) != VK_SUCCESS) {
    p.pipeline = VK_NULL_HANDLE;
    return nullptr;
  }
  return &p;
}

struct HostBuffer {
  VkBuffer buffer = VK_NULL_HANDLE;
  VkDeviceMemory memory = VK_NULL_HANDLE;
  void* mapped = nullptr;
};

HostBuffer host_buffer(VkDeviceSize size, VkBufferUsageFlags usage) {
  auto& dev = ssn::gfx::device();
  VkDevice d = dev.device();
  HostBuffer b;
  VkBufferCreateInfo bi{VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO};
  bi.size = size;
  bi.usage = usage;
  vkCreateBuffer(d, &bi, nullptr, &b.buffer);
  VkMemoryRequirements req;
  vkGetBufferMemoryRequirements(d, b.buffer, &req);
  VkMemoryAllocateInfo ai{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO};
  ai.allocationSize = req.size;
  ai.memoryTypeIndex = dev.memory_type(req.memoryTypeBits, VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT);
  vkAllocateMemory(d, &ai, nullptr, &b.memory);
  vkBindBufferMemory(d, b.buffer, b.memory, 0);
  vkMapMemory(d, b.memory, 0, size, 0, &b.mapped);
  return b;
}

void free_buffer(HostBuffer& b) {
  VkDevice d = ssn::gfx::device().device();
  vkDestroyBuffer(d, b.buffer, nullptr);
  vkFreeMemory(d, b.memory, nullptr);
  b = {};
}

void barrier(VkCommandBuffer cb, ssn::gfx::Image& img, VkImageLayout from, VkImageLayout to, VkAccessFlags src, VkAccessFlags dst,
             VkPipelineStageFlags ss, VkPipelineStageFlags ds) {
  VkImageMemoryBarrier b{VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER};
  b.srcAccessMask = src;
  b.dstAccessMask = dst;
  b.oldLayout = from;
  b.newLayout = to;
  b.srcQueueFamilyIndex = b.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
  b.image = img.image;
  b.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
  vkCmdPipelineBarrier(cb, ss, ds, 0, 0, nullptr, 0, nullptr, 1, &b);
  img.layout = to;
}

}  // namespace

bool compress(uint32_t format, const uint8_t* rgba, int32_t width, int32_t height, std::vector<uint8_t>* out) {
  const Pipeline* p = pipeline_for(format);
  if (!p) {
    host::note_unimplemented("libScePsm GPU texture compressor (sub_296B70) for ImageFormat " + std::to_string(format) +
                             ": compressor shader not transcribed");
    return false;
  }
  auto& dev = ssn::gfx::device();
  const uint32_t aw = (uint32_t(width) + 3) & ~3u, ah = (uint32_t(height) + 3) & ~3u;
  const uint32_t bw = aw / 4, bh = ah / 4;
  // The input texture covers the aligned size; texels past the frame are zero.
  ssn::gfx::Image src = dev.create_image(aw, ah, 1, VK_FORMAT_R8G8B8A8_UNORM,
                                         VK_IMAGE_USAGE_STORAGE_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT);
  ssn::gfx::Image dst = dev.create_image(bw, bh, 1, VK_FORMAT_R32G32B32A32_UINT,
                                         VK_IMAGE_USAGE_STORAGE_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT);
  HostBuffer up = host_buffer(VkDeviceSize(aw) * ah * 4, VK_BUFFER_USAGE_TRANSFER_SRC_BIT);
  HostBuffer down = host_buffer(VkDeviceSize(bw) * bh * 16, VK_BUFFER_USAGE_TRANSFER_DST_BIT);
  std::memset(up.mapped, 0, size_t(aw) * ah * 4);
  for (int32_t y = 0; y < height; ++y)
    std::memcpy(static_cast<uint8_t*>(up.mapped) + size_t(y) * aw * 4, rgba + size_t(y) * width * 4, size_t(width) * 4);
  const uint32_t blocks = bw * bh;
  dev.run_now([&](VkCommandBuffer cb) {
    barrier(cb, src, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 0, VK_ACCESS_TRANSFER_WRITE_BIT,
            VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT);
    VkBufferImageCopy c{};
    c.imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
    c.imageExtent = {aw, ah, 1};
    vkCmdCopyBufferToImage(cb, up.buffer, src.image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &c);
    barrier(cb, src, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_IMAGE_LAYOUT_GENERAL, VK_ACCESS_TRANSFER_WRITE_BIT,
            VK_ACCESS_SHADER_READ_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT);
    barrier(cb, dst, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_GENERAL, 0, VK_ACCESS_SHADER_WRITE_BIT,
            VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT);
    vkCmdBindPipeline(cb, VK_PIPELINE_BIND_POINT_COMPUTE, p->pipeline);
    VkDescriptorImageInfo ii[2] = {{VK_NULL_HANDLE, src.view, VK_IMAGE_LAYOUT_GENERAL},
                                   {VK_NULL_HANDLE, dst.view, VK_IMAGE_LAYOUT_GENERAL}};
    VkWriteDescriptorSet w[2]{};
    for (uint32_t i = 0; i < 2; ++i) {
      w[i].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
      w[i].dstBinding = i;
      w[i].descriptorCount = 1;
      w[i].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_IMAGE;
      w[i].pImageInfo = &ii[i];
    }
    p->push(cb, VK_PIPELINE_BIND_POINT_COMPUTE, p->layout, 0, 2, w);
    vkCmdPushConstants(cb, p->layout, VK_SHADER_STAGE_COMPUTE_BIT, 0, 4, &blocks);
    vkCmdDispatch(cb, (blocks + 63) / 64, 1, 1);  // one invocation per block (vertex), 64 per wave
    barrier(cb, dst, VK_IMAGE_LAYOUT_GENERAL, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, VK_ACCESS_SHADER_WRITE_BIT,
            VK_ACCESS_TRANSFER_READ_BIT, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT);
    VkBufferImageCopy r{};
    r.imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
    r.imageExtent = {bw, bh, 1};
    vkCmdCopyImageToBuffer(cb, dst.image, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, down.buffer, 1, &r);
  });
  out->assign(static_cast<const uint8_t*>(down.mapped), static_cast<const uint8_t*>(down.mapped) + size_t(blocks) * 16);
  // Diagnostics: SSN_DUMP_GTC=<dir> writes each input frame (raw RGBA) and its blocks.
  if (const char* dir = std::getenv("SSN_DUMP_GTC")) {
    static int n = 0;
    const std::string base = std::string(dir) + "/gtc_" + std::to_string(n++) + "_f" + std::to_string(format) + "_" +
                             std::to_string(width) + "x" + std::to_string(height);
    std::ofstream(base + ".rgba", std::ios::binary).write(reinterpret_cast<const char*>(rgba), std::streamsize(size_t(width) * height * 4));
    std::ofstream(base + ".blocks", std::ios::binary).write(reinterpret_cast<const char*>(out->data()), std::streamsize(out->size()));
  }
  free_buffer(up);
  free_buffer(down);
  dev.destroy_image(src);
  dev.destroy_image(dst);
  return true;
}

}  // namespace ssn::psm::imaging::gtc
