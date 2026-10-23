// A PSM shader program: one of libScePsm's embedded vertex/pixel shader pairs
// (ShaderManager::LoadShaderNative) and its reflected uniforms and attributes.
#pragma once
#include <cstdint>
#include <string>
#include <vector>

#include <vulkan/vulkan.h>

namespace ssn::psm::gfx {

// Host pipeline-layout convention for the transcribed embedded shaders (set 0, push
// descriptors):
constexpr uint32_t kBindingVsConstants = 0;     // vertex shader __GLOBAL_CB__
constexpr uint32_t kBindingPsConstants = 1;     // pixel shader __GLOBAL_CB__
constexpr uint32_t kBindingGlobalConstants = 2; // globalConstantBuffer (UpdateGlobalConstantBuffer)
constexpr uint32_t kBindingTexture0 = 3;        // textures 0..7 at 3..10
constexpr uint32_t kTextureSlots = 8;

struct Uniform {
  std::string name;
  uint32_t type = 0;      // ShaderUniformType
  uint32_t stage = 0;     // 0 vertex, 1 pixel
  uint32_t offset = 0;    // byte offset inside the stage's constant buffer
  uint32_t size = 0;      // bytes
  int32_t array_count = 1;
  int32_t texture_slot = -1;  // sampler uniforms
};

struct Attribute {
  std::string name;
  uint32_t type = 0;  // ShaderAttributeType
  uint32_t location = 0;
};

struct ShaderProgram {
  int32_t refs = 1;
  uint32_t embedded_type = 0;  // EmbeddedShader
  std::string vs_name, ps_name;
  VkShaderModule vs = VK_NULL_HANDLE, fs = VK_NULL_HANDLE;
  std::vector<Uniform> uniforms;
  std::vector<Attribute> attributes;
  std::vector<uint8_t> vs_constants, ps_constants;  // current values (uploaded per draw)
};

}  // namespace ssn::psm::gfx
