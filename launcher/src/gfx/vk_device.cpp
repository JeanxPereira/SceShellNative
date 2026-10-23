#include "gfx/vk_device.h"

#include <vulkan/vulkan_win32.h>

#include <algorithm>
#include <cstring>
#include <filesystem>
#include <format>

#include "core/log.h"
#include "lodepng/lodepng.h"

namespace ssn::gfx {
namespace {
constexpr VkDeviceSize kRingSize = 256ull << 20;

bool check(VkResult r, const char* what) {
  if (r == VK_SUCCESS) return true;
  log::error("vk", "{} failed: {}", what, static_cast<int>(r));
  return false;
}

Buffer make_buffer(Device& d, VkDeviceSize size, VkBufferUsageFlags usage) {
  Buffer b;
  b.size = size;
  VkBufferCreateInfo bi{VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO};
  bi.size = size;
  bi.usage = usage;
  vkCreateBuffer(d.device(), &bi, nullptr, &b.buffer);
  VkMemoryRequirements req;
  vkGetBufferMemoryRequirements(d.device(), b.buffer, &req);
  VkMemoryAllocateInfo ai{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO};
  ai.allocationSize = req.size;
  ai.memoryTypeIndex = d.memory_type(req.memoryTypeBits, VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT);
  vkAllocateMemory(d.device(), &ai, nullptr, &b.memory);
  vkBindBufferMemory(d.device(), b.buffer, b.memory, 0);
  vkMapMemory(d.device(), b.memory, 0, size, 0, &b.mapped);
  return b;
}
// Top-down 32-bit BMP from RGBA8 or BGRA8 texels.
void write_bmp(const std::string& path, uint32_t w, uint32_t h, const uint8_t* px, VkFormat fmt) {
  FILE* f = fopen(path.c_str(), "wb");
  if (!f) return;
  const uint32_t size = w * h * 4;
  uint8_t hdr[54] = {'B', 'M'};
  auto put32 = [&](int o, uint32_t v) { std::memcpy(hdr + o, &v, 4); };
  put32(2, 54 + size);
  put32(10, 54);
  put32(14, 40);
  put32(18, w);
  put32(22, static_cast<uint32_t>(-static_cast<int32_t>(h)));
  hdr[26] = 1;
  hdr[28] = 32;
  put32(34, size);
  fwrite(hdr, 1, 54, f);
  std::vector<uint8_t> row(w * 4);
  const bool rgba = fmt == VK_FORMAT_R8G8B8A8_UNORM;
  for (uint32_t y = 0; y < h; ++y) {
    const uint8_t* s = px + size_t(y) * w * 4;
    for (uint32_t x = 0; x < w; ++x) {
      row[x * 4 + 0] = rgba ? s[x * 4 + 2] : s[x * 4 + 0];
      row[x * 4 + 1] = s[x * 4 + 1];
      row[x * 4 + 2] = rgba ? s[x * 4 + 0] : s[x * 4 + 2];
      row[x * 4 + 3] = s[x * 4 + 3];
    }
    fwrite(row.data(), 1, row.size(), f);
  }
  fclose(f);
  log::info("vk", "captured {}", path);
}
// Opaque RGBA8 PNG (the display buffer is shown opaque) from RGBA8 or BGRA8 texels.
void write_png(std::string path, uint32_t w, uint32_t h, std::vector<uint8_t> px, VkFormat fmt) {
  const bool rgba = fmt == VK_FORMAT_R8G8B8A8_UNORM;
  for (size_t i = 0; i < px.size(); i += 4) {
    if (!rgba) std::swap(px[i], px[i + 2]);
    px[i + 3] = 0xFF;
  }
  if (unsigned err = lodepng::encode(path, px, w, h))
    log::error("vk", "capture {} failed: {}", path, lodepng_error_text(err));
  else
    log::info("vk", "captured {}", path);
}
}  // namespace

Device& device() {
  static Device d;
  return d;
}

uint32_t Device::memory_type(uint32_t bits, VkMemoryPropertyFlags props) const {
  for (uint32_t i = 0; i < mem_props_.memoryTypeCount; ++i)
    if ((bits & (1u << i)) && (mem_props_.memoryTypes[i].propertyFlags & props) == props) return i;
  return 0;
}

bool Device::init(HINSTANCE hinst, HWND hwnd) {
  hwnd_ = hwnd;
  headless_ = hwnd == nullptr;
  VkApplicationInfo app{VK_STRUCTURE_TYPE_APPLICATION_INFO};
  app.pApplicationName = "SceShellNative";
  app.apiVersion = VK_API_VERSION_1_3;
  const char* exts[] = {VK_KHR_SURFACE_EXTENSION_NAME, VK_KHR_WIN32_SURFACE_EXTENSION_NAME};
  VkInstanceCreateInfo ici{VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO};
  ici.pApplicationInfo = &app;
  ici.enabledExtensionCount = headless_ ? 0 : 2;
  ici.ppEnabledExtensionNames = exts;
  const char* layer = "VK_LAYER_KHRONOS_validation";
  if (GetEnvironmentVariableA("SSN_VK_VALIDATION", nullptr, 0)) {
    ici.enabledLayerCount = 1;
    ici.ppEnabledLayerNames = &layer;
  }
  if (!check(vkCreateInstance(&ici, nullptr, &instance_), "vkCreateInstance")) return false;

  if (!headless_) {
    VkWin32SurfaceCreateInfoKHR sci{VK_STRUCTURE_TYPE_WIN32_SURFACE_CREATE_INFO_KHR};
    sci.hinstance = hinst;
    sci.hwnd = hwnd;
    if (!check(vkCreateWin32SurfaceKHR(instance_, &sci, nullptr, &surface_), "vkCreateWin32SurfaceKHR")) return false;
  }

  uint32_t n = 0;
  vkEnumeratePhysicalDevices(instance_, &n, nullptr);
  std::vector<VkPhysicalDevice> pds(n);
  vkEnumeratePhysicalDevices(instance_, &n, pds.data());
  for (auto pd : pds) {
    VkPhysicalDeviceProperties p;
    vkGetPhysicalDeviceProperties(pd, &p);
    uint32_t qn = 0;
    vkGetPhysicalDeviceQueueFamilyProperties(pd, &qn, nullptr);
    std::vector<VkQueueFamilyProperties> qs(qn);
    vkGetPhysicalDeviceQueueFamilyProperties(pd, &qn, qs.data());
    for (uint32_t i = 0; i < qn; ++i) {
      VkBool32 present = headless_ ? VK_TRUE : VK_FALSE;
      if (!headless_) vkGetPhysicalDeviceSurfaceSupportKHR(pd, i, surface_, &present);
      if ((qs[i].queueFlags & VK_QUEUE_GRAPHICS_BIT) && present && p.apiVersion >= VK_API_VERSION_1_3) {
        if (!physical_ || p.deviceType == VK_PHYSICAL_DEVICE_TYPE_DISCRETE_GPU) {
          physical_ = pd;
          queue_family_ = i;
        }
        break;
      }
    }
  }
  if (!physical_) {
    log::error("vk", "no Vulkan 1.3 device with graphics+present");
    return false;
  }
  VkPhysicalDeviceProperties props;
  vkGetPhysicalDeviceProperties(physical_, &props);
  log::info("vk", "device: {}", props.deviceName);
  vkGetPhysicalDeviceMemoryProperties(physical_, &mem_props_);

  float prio = 1.0f;
  VkDeviceQueueCreateInfo qci{VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO};
  qci.queueFamilyIndex = queue_family_;
  qci.queueCount = 1;
  qci.pQueuePriorities = &prio;
  VkPhysicalDeviceVulkan13Features f13{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_3_FEATURES};
  f13.dynamicRendering = VK_TRUE;
  f13.synchronization2 = VK_TRUE;
  // Embedded PSM shaders that kill pixels keep them as helper lanes (focus_ui3_area_p,
  // focus_ui3_line_p), and solid_fill_light_mat_p reads the GPU real-time clock.
  f13.shaderDemoteToHelperInvocation = VK_TRUE;
  VkPhysicalDeviceShaderClockFeaturesKHR clock{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_SHADER_CLOCK_FEATURES_KHR};
  clock.shaderDeviceClock = VK_TRUE;
  f13.pNext = &clock;
  // Null descriptors for texture units the firmware never bound (psm draw path).
  VkPhysicalDeviceRobustness2FeaturesEXT robust{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_ROBUSTNESS_2_FEATURES_EXT};
  robust.nullDescriptor = VK_TRUE;
  clock.pNext = &robust;
  VkPhysicalDeviceFeatures2 f2{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2};
  f2.pNext = &f13;
  // BC1..BC7 textures (libScePsm PixelFormat Dxt1/3/5, Bc4U..Bc7U: DDS images uploaded
  // compressed by PsmTexture::FromImageInstance).
  VkPhysicalDeviceFeatures supported{};
  vkGetPhysicalDeviceFeatures(physical_, &supported);
  f2.features.textureCompressionBC = supported.textureCompressionBC;
  if (!supported.textureCompressionBC) log::warn("vk", "device has no BC texture compression; DDS textures will fail");
  // The ShellUI eboot's particle_vv writes the particle buffer from the vertex stage
  // (buffer_store_dword at 0x1AC; background layer, psmhost/eboot/bglayer_particle.cpp).
  f2.features.vertexPipelineStoresAndAtomics = supported.vertexPipelineStoresAndAtomics;
  if (!supported.vertexPipelineStoresAndAtomics) log::warn("vk", "device has no vertex-stage storage writes; background particles will fail");
  const char* dexts[] = {VK_KHR_PUSH_DESCRIPTOR_EXTENSION_NAME, VK_KHR_SHADER_CLOCK_EXTENSION_NAME, VK_EXT_ROBUSTNESS_2_EXTENSION_NAME,
                         VK_KHR_SWAPCHAIN_EXTENSION_NAME};
  VkDeviceCreateInfo dci{VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO};
  dci.pNext = &f2;
  dci.queueCreateInfoCount = 1;
  dci.pQueueCreateInfos = &qci;
  dci.enabledExtensionCount = headless_ ? 3 : 4;  // headless: no swapchain
  dci.ppEnabledExtensionNames = dexts;
  if (!check(vkCreateDevice(physical_, &dci, nullptr, &device_), "vkCreateDevice")) return false;
  vkGetDeviceQueue(device_, queue_family_, 0, &queue_);

  VkCommandPoolCreateInfo pci{VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO};
  pci.flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT;
  pci.queueFamilyIndex = queue_family_;
  vkCreateCommandPool(device_, &pci, nullptr, &pool_);
  VkCommandBufferAllocateInfo cai{VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO};
  cai.commandPool = pool_;
  cai.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
  cai.commandBufferCount = 1;
  VkSemaphoreCreateInfo semi{VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO};
  VkFenceCreateInfo fci{VK_STRUCTURE_TYPE_FENCE_CREATE_INFO};
  for (auto& fs : slots_) {
    vkAllocateCommandBuffers(device_, &cai, &fs.cb);
    vkCreateSemaphore(device_, &semi, nullptr, &fs.acquired);
    vkCreateFence(device_, &fci, nullptr, &fs.fence);
  }
  cb_ = slots_[slot_].cb;

  ring_ = make_buffer(*this, kRingSize,
                      VK_BUFFER_USAGE_VERTEX_BUFFER_BIT | VK_BUFFER_USAGE_INDEX_BUFFER_BIT | VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT |
                          VK_BUFFER_USAGE_TRANSFER_SRC_BIT);
  // GPU time of each frame's command buffer (two timestamps), when the queue has them.
  {
    uint32_t qn = 0;
    vkGetPhysicalDeviceQueueFamilyProperties(physical_, &qn, nullptr);
    std::vector<VkQueueFamilyProperties> qs(qn);
    vkGetPhysicalDeviceQueueFamilyProperties(physical_, &qn, qs.data());
    if (queue_family_ < qn && qs[queue_family_].timestampValidBits != 0) {
      VkQueryPoolCreateInfo qpi{VK_STRUCTURE_TYPE_QUERY_POOL_CREATE_INFO};
      qpi.queryType = VK_QUERY_TYPE_TIMESTAMP;
      qpi.queryCount = 4;  // two per frame slot
      if (vkCreateQueryPool(device_, &qpi, nullptr, &timestamps_) != VK_SUCCESS) timestamps_ = VK_NULL_HANDLE;
      timestamp_period_ = props.limits.timestampPeriod;
    }
  }
  if (headless_) {
    pace_timer_ = CreateWaitableTimerExW(nullptr, nullptr, CREATE_WAITABLE_TIMER_HIGH_RESOLUTION, TIMER_ALL_ACCESS);
    if (!pace_timer_) pace_timer_ = CreateWaitableTimerExW(nullptr, nullptr, 0, TIMER_ALL_ACCESS);
    log::info("vk", "headless: no window/swapchain, rendering offscreen at ~60 Hz");
    return true;
  }
  return create_swapchain();
}

bool Device::create_swapchain() {
  VkSurfaceCapabilitiesKHR caps;
  vkGetPhysicalDeviceSurfaceCapabilitiesKHR(physical_, surface_, &caps);
  uint32_t n = 0;
  vkGetPhysicalDeviceSurfaceFormatsKHR(physical_, surface_, &n, nullptr);
  std::vector<VkSurfaceFormatKHR> fmts(n);
  vkGetPhysicalDeviceSurfaceFormatsKHR(physical_, surface_, &n, fmts.data());
  VkSurfaceFormatKHR fmt = fmts[0];
  for (auto& f : fmts)
    if (f.format == VK_FORMAT_B8G8R8A8_UNORM) fmt = f;
  swap_format_ = fmt.format;
  swap_extent_ = caps.currentExtent;
  if (swap_extent_.width == 0 || swap_extent_.height == 0) return false;
  VkSwapchainCreateInfoKHR sci{VK_STRUCTURE_TYPE_SWAPCHAIN_CREATE_INFO_KHR};
  sci.surface = surface_;
  sci.minImageCount = std::max(2u, caps.minImageCount);
  sci.imageFormat = fmt.format;
  sci.imageColorSpace = fmt.colorSpace;
  sci.imageExtent = swap_extent_;
  sci.imageArrayLayers = 1;
  sci.imageUsage = VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT;
  sci.preTransform = caps.currentTransform;
  sci.compositeAlpha = VK_COMPOSITE_ALPHA_OPAQUE_BIT_KHR;
  sci.presentMode = VK_PRESENT_MODE_FIFO_KHR;
  sci.clipped = VK_TRUE;
  sci.oldSwapchain = swapchain_;
  VkSwapchainKHR sc;
  if (!check(vkCreateSwapchainKHR(device_, &sci, nullptr, &sc), "vkCreateSwapchainKHR")) return false;
  if (swapchain_) vkDestroySwapchainKHR(device_, swapchain_, nullptr);
  swapchain_ = sc;
  vkGetSwapchainImagesKHR(device_, swapchain_, &n, nullptr);
  swap_images_.resize(n);
  vkGetSwapchainImagesKHR(device_, swapchain_, &n, swap_images_.data());
  swap_layouts_.assign(n, VK_IMAGE_LAYOUT_UNDEFINED);
  // One "rendering finished" semaphore per image: a present may still wait on the one of an
  // earlier frame. Recreation runs with the queue idle.
  for (VkSemaphore sem : finished_) vkDestroySemaphore(device_, sem, nullptr);
  finished_.assign(n, VK_NULL_HANDLE);
  VkSemaphoreCreateInfo semi{VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO};
  for (auto& sem : finished_) vkCreateSemaphore(device_, &semi, nullptr, &sem);
  return true;
}

void Device::shutdown() {
  if (!device_) return;
  {
    std::lock_guard<std::recursive_mutex> lk(record_lock_);
    for (uint32_t s = 0; s < 2; ++s) retire(s);
  }
  vkDeviceWaitIdle(device_);
}

Image Device::create_image(uint32_t w, uint32_t h, uint32_t levels, VkFormat format, VkImageUsageFlags usage) {
  Image img;
  img.width = w;
  img.height = h;
  img.levels = levels;
  img.format = format;
  const bool ds = format == VK_FORMAT_D24_UNORM_S8_UINT || format == VK_FORMAT_D32_SFLOAT_S8_UINT || format == VK_FORMAT_D16_UNORM ||
                  format == VK_FORMAT_D32_SFLOAT;
  img.aspect = ds ? (VK_IMAGE_ASPECT_DEPTH_BIT |
                     ((format == VK_FORMAT_D24_UNORM_S8_UINT || format == VK_FORMAT_D32_SFLOAT_S8_UINT) ? VK_IMAGE_ASPECT_STENCIL_BIT : 0))
                  : VK_IMAGE_ASPECT_COLOR_BIT;
  VkImageCreateInfo ii{VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO};
  ii.imageType = VK_IMAGE_TYPE_2D;
  ii.format = format;
  ii.extent = {w, h, 1};
  ii.mipLevels = levels;
  ii.arrayLayers = 1;
  ii.samples = VK_SAMPLE_COUNT_1_BIT;
  ii.tiling = VK_IMAGE_TILING_OPTIMAL;
  ii.usage = usage;
  if (!check(vkCreateImage(device_, &ii, nullptr, &img.image), "vkCreateImage")) return {};
  VkMemoryRequirements req;
  vkGetImageMemoryRequirements(device_, img.image, &req);
  VkMemoryAllocateInfo ai{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO};
  ai.allocationSize = req.size;
  ai.memoryTypeIndex = memory_type(req.memoryTypeBits, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
  vkAllocateMemory(device_, &ai, nullptr, &img.memory);
  vkBindImageMemory(device_, img.image, img.memory, 0);
  VkImageViewCreateInfo vi{VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO};
  vi.image = img.image;
  vi.viewType = VK_IMAGE_VIEW_TYPE_2D;
  vi.format = format;
  vi.subresourceRange = {img.aspect, 0, levels, 0, 1};
  vkCreateImageView(device_, &vi, nullptr, &img.view);
  return img;
}

void Device::run_now(const std::function<void(VkCommandBuffer)>& fill) {
  std::lock_guard<std::recursive_mutex> lk(record_lock_);
  VkCommandBufferAllocateInfo ai{VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO};
  ai.commandPool = pool_;
  ai.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
  ai.commandBufferCount = 1;
  VkCommandBuffer cb = VK_NULL_HANDLE;
  if (vkAllocateCommandBuffers(device_, &ai, &cb) != VK_SUCCESS) return;
  VkCommandBufferBeginInfo bi{VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};
  bi.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
  vkBeginCommandBuffer(cb, &bi);
  fill(cb);
  vkEndCommandBuffer(cb);
  VkFenceCreateInfo fi{VK_STRUCTURE_TYPE_FENCE_CREATE_INFO};
  VkFence fence = VK_NULL_HANDLE;
  vkCreateFence(device_, &fi, nullptr, &fence);
  VkSubmitInfo si{VK_STRUCTURE_TYPE_SUBMIT_INFO};
  si.commandBufferCount = 1;
  si.pCommandBuffers = &cb;
  vkQueueSubmit(queue_, 1, &si, fence);
  vkWaitForFences(device_, 1, &fence, VK_TRUE, UINT64_MAX);
  vkDestroyFence(device_, fence, nullptr);
  vkFreeCommandBuffers(device_, pool_, 1, &cb);
}

void Device::destroy_image(Image& img) {
  std::lock_guard<std::recursive_mutex> lk(record_lock_);
  if (!img.valid()) return;
  // Resources may still be referenced by the frame being recorded (retire after it) or by
  // the frame still in flight (the queue is drained in both cases).
  if (in_frame_) {
    end_rendering();
    vkEndCommandBuffer(cb_);
    VkSubmitInfo si{VK_STRUCTURE_TYPE_SUBMIT_INFO};
    si.commandBufferCount = 1;
    si.pCommandBuffers = &cb_;
    vkQueueSubmit(queue_, 1, &si, VK_NULL_HANDLE);
    vkQueueWaitIdle(queue_);
    in_frame_ = false;
    begin_frame();
  } else {
    vkQueueWaitIdle(queue_);
  }
  vkDestroyImageView(device_, img.view, nullptr);
  vkDestroyImage(device_, img.image, nullptr);
  vkFreeMemory(device_, img.memory, nullptr);
  img = {};
}

void Device::begin_frame() {
  if (in_frame_) return;
  vkResetCommandBuffer(cb_, 0);
  VkCommandBufferBeginInfo bi{VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};
  bi.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
  vkBeginCommandBuffer(cb_, &bi);
  // The GPU runs the frames one after the other: everything submitted before (the previous
  // frame, still in flight) completes and is visible before this command buffer's work.
  {
    VkMemoryBarrier2 mb{VK_STRUCTURE_TYPE_MEMORY_BARRIER_2};
    mb.srcStageMask = VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT;
    mb.srcAccessMask = VK_ACCESS_2_MEMORY_WRITE_BIT;
    mb.dstStageMask = VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT;
    mb.dstAccessMask = VK_ACCESS_2_MEMORY_READ_BIT | VK_ACCESS_2_MEMORY_WRITE_BIT;
    VkDependencyInfo di{VK_STRUCTURE_TYPE_DEPENDENCY_INFO};
    di.memoryBarrierCount = 1;
    di.pMemoryBarriers = &mb;
    vkCmdPipelineBarrier2(cb_, &di);
  }
  if (timestamps_) {
    vkCmdResetQueryPool(cb_, timestamps_, 2 * slot_, 2);
    vkCmdWriteTimestamp2(cb_, VK_PIPELINE_STAGE_2_TOP_OF_PIPE_BIT, timestamps_, 2 * slot_);
  }
  in_frame_ = true;
  rendering_ = false;
}

VkCommandBuffer Device::cmd() {
  begin_frame();
  return cb_;
}

Slice Device::alloc(VkDeviceSize size, VkDeviceSize align) {
  ring_head_ = (ring_head_ + align - 1) & ~(align - 1);
  const VkDeviceSize half = ring_.size / 2, base = half * slot_;
  if (ring_head_ + size > half) {
    // The slot's half of the ring exhausted inside one frame: spill into a dedicated buffer
    // released when the slot retires.
    auto& overflow = slots_[slot_].overflow;
    overflow.push_back(make_buffer(*this, std::max<VkDeviceSize>(size, 16ull << 20),
                                   VK_BUFFER_USAGE_VERTEX_BUFFER_BIT | VK_BUFFER_USAGE_INDEX_BUFFER_BIT |
                                       VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT | VK_BUFFER_USAGE_TRANSFER_SRC_BIT));
    auto& b = overflow.back();
    return {b.buffer, 0, b.mapped};
  }
  Slice s{ring_.buffer, base + ring_head_, static_cast<char*>(ring_.mapped) + base + ring_head_};
  ring_head_ += size;
  return s;
}

void Device::transition(VkCommandBuffer cb, Image& img, VkImageLayout to) {
  if (img.layout == to) return;
  VkImageMemoryBarrier2 b{VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER_2};
  b.srcStageMask = VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT;
  b.srcAccessMask = VK_ACCESS_2_MEMORY_WRITE_BIT;
  b.dstStageMask = VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT;
  b.dstAccessMask = VK_ACCESS_2_MEMORY_READ_BIT | VK_ACCESS_2_MEMORY_WRITE_BIT;
  b.oldLayout = img.layout;
  b.newLayout = to;
  b.srcQueueFamilyIndex = b.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
  b.image = img.image;
  b.subresourceRange = {img.aspect, 0, img.levels, 0, 1};
  VkDependencyInfo di{VK_STRUCTURE_TYPE_DEPENDENCY_INFO};
  di.imageMemoryBarrierCount = 1;
  di.pImageMemoryBarriers = &b;
  vkCmdPipelineBarrier2(cb, &di);
  img.layout = to;
}

void Device::upload_image(Image& img, uint32_t level, int x, int y, uint32_t w, uint32_t h, const void* data, size_t bytes) {
  std::lock_guard<std::recursive_mutex> lk(record_lock_);
  VkCommandBuffer cb = cmd();
  end_rendering();
  Slice s = alloc(bytes, 16);
  std::memcpy(s.data, data, bytes);
  transition(cb, img, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL);
  VkBufferImageCopy c{};
  c.bufferOffset = s.offset;
  c.imageSubresource = {img.aspect, level, 0, 1};
  c.imageOffset = {x, y, 0};
  c.imageExtent = {w, h, 1};
  vkCmdCopyBufferToImage(cb, s.buffer, img.image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &c);
  transition(cb, img, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
}

void Device::bind_target(Image* color, Image* depth) {
  if (color == target_color_ && depth == target_depth_) return;
  end_rendering();
  target_color_ = color;
  target_depth_ = depth;
}

void Device::ensure_rendering() {
  VkCommandBuffer cb = cmd();
  if (rendering_ || !target_color_) return;
  transition(cb, *target_color_, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL);
  VkRenderingAttachmentInfo ca{VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO};
  ca.imageView = target_color_->view;
  ca.imageLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
  ca.loadOp = VK_ATTACHMENT_LOAD_OP_LOAD;
  ca.storeOp = VK_ATTACHMENT_STORE_OP_STORE;
  VkRenderingAttachmentInfo da{VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO};
  VkRenderingInfo ri{VK_STRUCTURE_TYPE_RENDERING_INFO};
  ri.renderArea = {{0, 0}, {target_color_->width, target_color_->height}};
  ri.layerCount = 1;
  ri.colorAttachmentCount = 1;
  ri.pColorAttachments = &ca;
  if (target_depth_) {
    transition(cb, *target_depth_, VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL);
    da.imageView = target_depth_->view;
    da.imageLayout = VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL;
    da.loadOp = VK_ATTACHMENT_LOAD_OP_LOAD;
    da.storeOp = VK_ATTACHMENT_STORE_OP_STORE;
    ri.pDepthAttachment = &da;
    if (target_depth_->aspect & VK_IMAGE_ASPECT_STENCIL_BIT) ri.pStencilAttachment = &da;
  }
  vkCmdBeginRendering(cb, &ri);
  rendering_ = true;
}

void Device::end_rendering() {
  if (!rendering_) return;
  vkCmdEndRendering(cb_);
  rendering_ = false;
}

void Device::present(Image& screen, const std::string& capture_path) {
  std::lock_guard<std::recursive_mutex> lk(record_lock_);
  VkCommandBuffer cb = cmd();
  end_rendering();
  const uint64_t frame = ++presented_;
  // The vblank stand-in comes first, as SwapBuffers' wait for the previous frame (sub_2BC0)
  // does on the console: headless, the ~60 Hz timer; with a window, the FIFO acquire.
  if (headless_) pace_frame();
  FrameSlot& fs = slots_[slot_];
  uint32_t index = 0;
  VkResult ar = VK_NOT_READY;
  if (!headless_) ar = vkAcquireNextImageKHR(device_, swapchain_, UINT64_MAX, fs.acquired, VK_NULL_HANDLE, &index);
  const bool have_image = (ar == VK_SUCCESS || ar == VK_SUBOPTIMAL_KHR);
  const bool png = !capture_.dir.empty() && capture_.wants(frame);
  if (have_image) {
    transition(cb, screen, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL);
    Image sw;
    sw.image = swap_images_[index];
    sw.layout = swap_layouts_[index] == VK_IMAGE_LAYOUT_PRESENT_SRC_KHR ? VK_IMAGE_LAYOUT_UNDEFINED : swap_layouts_[index];
    sw.aspect = VK_IMAGE_ASPECT_COLOR_BIT;
    transition(cb, sw, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL);
    VkImageBlit blit{};
    blit.srcSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
    blit.srcOffsets[1] = {static_cast<int32_t>(screen.width), static_cast<int32_t>(screen.height), 1};
    blit.dstSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
    blit.dstOffsets[1] = {static_cast<int32_t>(swap_extent_.width), static_cast<int32_t>(swap_extent_.height), 1};
    vkCmdBlitImage(cb, screen.image, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, sw.image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &blit,
                   VK_FILTER_LINEAR);
    transition(cb, sw, VK_IMAGE_LAYOUT_PRESENT_SRC_KHR);
    swap_layouts_[index] = VK_IMAGE_LAYOUT_PRESENT_SRC_KHR;
  }
  fs.frame = frame;
  fs.readback = {};
  fs.bmp_path = capture_path;
  fs.png = png;
  if (!capture_path.empty() || png) {
    transition(cb, screen, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL);
    fs.readback = alloc(VkDeviceSize(screen.width) * screen.height * 4, 16);
    fs.width = screen.width;
    fs.height = screen.height;
    fs.format = screen.format;
    VkBufferImageCopy c{};
    c.bufferOffset = fs.readback.offset;
    c.imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
    c.imageExtent = {screen.width, screen.height, 1};
    vkCmdCopyImageToBuffer(cb, screen.image, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, fs.readback.buffer, 1, &c);
    VkMemoryBarrier2 mb{VK_STRUCTURE_TYPE_MEMORY_BARRIER_2};
    mb.srcStageMask = VK_PIPELINE_STAGE_2_COPY_BIT;
    mb.srcAccessMask = VK_ACCESS_2_TRANSFER_WRITE_BIT;
    mb.dstStageMask = VK_PIPELINE_STAGE_2_HOST_BIT;
    mb.dstAccessMask = VK_ACCESS_2_HOST_READ_BIT;
    VkDependencyInfo di{VK_STRUCTURE_TYPE_DEPENDENCY_INFO};
    di.memoryBarrierCount = 1;
    di.pMemoryBarriers = &mb;
    vkCmdPipelineBarrier2(cb, &di);
  }
  if (timestamps_) vkCmdWriteTimestamp2(cb, VK_PIPELINE_STAGE_2_BOTTOM_OF_PIPE_BIT, timestamps_, 2 * slot_ + 1);
  vkEndCommandBuffer(cb);
  // Frame N-1 has retired before frame N is submitted (SwapBuffers: sub_2BC0, then sub_78F0).
  retire(slot_ ^ 1u);
  VkPipelineStageFlags wait_stage = VK_PIPELINE_STAGE_TRANSFER_BIT;
  VkSubmitInfo si{VK_STRUCTURE_TYPE_SUBMIT_INFO};
  si.commandBufferCount = 1;
  si.pCommandBuffers = &cb;
  if (have_image) {
    si.waitSemaphoreCount = 1;
    si.pWaitSemaphores = &fs.acquired;
    si.pWaitDstStageMask = &wait_stage;
    si.signalSemaphoreCount = 1;
    si.pSignalSemaphores = &finished_[index];
  }
  vkResetFences(device_, 1, &fs.fence);
  vkQueueSubmit(queue_, 1, &si, fs.fence);
  fs.submitted = true;
  if (have_image) {
    VkPresentInfoKHR pi{VK_STRUCTURE_TYPE_PRESENT_INFO_KHR};
    pi.waitSemaphoreCount = 1;
    pi.pWaitSemaphores = &finished_[index];
    pi.swapchainCount = 1;
    pi.pSwapchains = &swapchain_;
    pi.pImageIndices = &index;
    VkResult pr = vkQueuePresentKHR(queue_, &pi);
    if (pr == VK_ERROR_OUT_OF_DATE_KHR || pr == VK_SUBOPTIMAL_KHR) {
      vkQueueWaitIdle(queue_);
      create_swapchain();
    }
  } else if (ar == VK_ERROR_OUT_OF_DATE_KHR) {
    vkQueueWaitIdle(queue_);
    create_swapchain();
  }
  // The next frame records into the other slot, whose frame (N-1) retired above.
  in_frame_ = false;
  slot_ ^= 1u;
  cb_ = slots_[slot_].cb;
  ring_head_ = 0;
  if (on_frame_presented) on_frame_presented(frame);
}

void Device::retire(uint32_t slot) {
  FrameSlot& fs = slots_[slot];
  if (!fs.submitted) return;
  vkWaitForFences(device_, 1, &fs.fence, VK_TRUE, UINT64_MAX);
  fs.submitted = false;
  if (timestamps_) {
    uint64_t t[2] = {};
    if (vkGetQueryPoolResults(device_, timestamps_, 2 * slot, 2, sizeof t, t, sizeof(uint64_t), VK_QUERY_RESULT_64_BIT) == VK_SUCCESS &&
        t[1] >= t[0]) {
      const double ns = double(t[1] - t[0]) * timestamp_period_;
      gpu_frame_ns_ = ns > 4294967295.0 ? 0xFFFFFFFFu : static_cast<uint32_t>(ns);
    }
  }
  if (fs.readback.data && !fs.bmp_path.empty())
    write_bmp(fs.bmp_path, fs.width, fs.height, static_cast<const uint8_t*>(fs.readback.data), fs.format);
  if (fs.readback.data && fs.png) {
    // Encode on a worker thread so a capture does not stall the frame much.
    std::vector<uint8_t> px(static_cast<const uint8_t*>(fs.readback.data),
                            static_cast<const uint8_t*>(fs.readback.data) + size_t(fs.width) * fs.height * 4);
    std::string path = (std::filesystem::path(capture_.dir) / std::format("frame_{}.png", fs.frame)).string();
    std::lock_guard<std::mutex> cl(captures_mu_);
    if (captures_.size() >= 8) {  // bound the encoders in flight (e.g. --capture-every 1)
      captures_.front().join();
      captures_.erase(captures_.begin());
    }
    captures_.emplace_back(write_png, std::move(path), fs.width, fs.height, std::move(px), fs.format);
  }
  fs.readback = {};
  fs.bmp_path.clear();
  fs.png = false;
  for (auto& b : fs.overflow) {
    vkDestroyBuffer(device_, b.buffer, nullptr);
    vkFreeMemory(device_, b.memory, nullptr);
  }
  fs.overflow.clear();
}

// Headless frame pacing: without a FIFO swapchain the frame would retire as fast as the
// GPU allows; keep the rate the window normally has (~60 Hz) so app timing is the same.
void Device::pace_frame() {
  LARGE_INTEGER freq, now;
  QueryPerformanceFrequency(&freq);
  QueryPerformanceCounter(&now);
  const int64_t period = freq.QuadPart / 60;
  if (next_frame_qpc_ == 0 || now.QuadPart - next_frame_qpc_ > period) next_frame_qpc_ = now.QuadPart;  // first frame / fell behind
  next_frame_qpc_ += period;
  const int64_t wait = next_frame_qpc_ - now.QuadPart;
  if (wait <= 0) return;
  LARGE_INTEGER due;
  due.QuadPart = -(wait * 10000000 / freq.QuadPart);  // relative, 100 ns units
  if (pace_timer_ && SetWaitableTimer(pace_timer_, &due, 0, nullptr, nullptr, FALSE))
    WaitForSingleObject(pace_timer_, INFINITE);
  else
    Sleep(static_cast<DWORD>(wait * 1000 / freq.QuadPart));
}

void Device::flush_captures() {
  {
    std::lock_guard<std::recursive_mutex> lk(record_lock_);
    for (uint32_t s = 0; s < 2; ++s) retire(s);
  }
  std::vector<std::thread> pending;
  {
    std::lock_guard<std::mutex> cl(captures_mu_);
    pending.swap(captures_);
  }
  for (auto& t : pending)
    if (t.joinable()) t.join();
}

bool CaptureSchedule::wants(uint64_t frame) const {
  if (every && frame % every == 0) return true;
  return std::find(frames.begin(), frames.end(), frame) != frames.end();
}

}  // namespace ssn::gfx
