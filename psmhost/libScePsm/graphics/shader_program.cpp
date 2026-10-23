// Sce.PlayStation.Core.Graphics.PsmShaderProgram and Renderer.ShaderManager
// (libScePsm.sprx, FW 4.03). See docs/psm-shaders.md for the program model.
#include "libScePsm/graphics/shader_program.h"

#include <windows.h>

#include <algorithm>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <map>
#include <vector>

#include "common/mono_array.h"
#include "common/mono_util.h"
#include "core/log.h"
#include "gfx/vk_device.h"
#include "host/boundary.h"
#include "libScePsm/error.h"
#include "libScePsm/graphics/core.h"
#include "libScePsm/graphics/objects.h"

namespace ssn::psm::gfx {
namespace {

struct UniformDecl {
  const char* name;
  uint32_t stage;  // 2 PS, 1 VS
  uint32_t offset, size;
};
struct ProgramDecl {
  uint32_t type;
  const char* vs;
  const char* ps;
  uint32_t ps_size, vs_size;
  std::vector<UniformDecl> uniforms;
};

// EmbeddedShader -> (vertex, pixel) shader pair (table 0x461ed0) with the uniform list
// libScePsm builds from the shaders' reflection (sub_1f82a0).
const std::vector<ProgramDecl>& programs() {
  static const std::vector<ProgramDecl> p = {
#include "libScePsm/graphics/embedded_programs.inc"
  };
  return p;
}

// SPIR-V transcriptions shipped next to the launcher (launcher/shaders/psm).
VkShaderModule load_module(const std::string& name) {
  static std::map<std::string, VkShaderModule> cache;
  auto it = cache.find(name);
  if (it != cache.end()) return it->second;
  wchar_t buf[MAX_PATH];
  GetModuleFileNameW(nullptr, buf, MAX_PATH);
  const auto path = std::filesystem::path(buf).parent_path() / "shaders" / (name + ".spv");
  VkShaderModule m = VK_NULL_HANDLE;
  std::ifstream f(path, std::ios::binary);
  if (f) {
    std::vector<char> code((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
    VkShaderModuleCreateInfo ci{VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO};
    ci.codeSize = code.size();
    ci.pCode = reinterpret_cast<const uint32_t*>(code.data());
    vkCreateShaderModule(ssn::gfx::device().device(), &ci, nullptr, &m);
  }
  cache.emplace(name, m);
  return m;
}

// LoadShaderNative(shaderType, out handle) — 0x2d4c90: null handle -> 0x80b30002; type >= 70
// -> 0x80b30003; a shader id missing from the asset table -> 0x80b30011; requires the
// graphics context (0x80b30021).
int32_t LoadShaderNative(uint32_t type, int32_t* handle) {
  if (!handle) return kErrorArgumentNull;
  if (type >= 0x46) return kErrorOutOfRange;
  if (!Core::get().created()) return kErrorGraphicsSystem;
  const ProgramDecl* d = nullptr;
  for (const auto& p : programs())
    if (p.type == type) d = &p;
  if (!d) return kErrorFileNotFound;
  auto prog = std::make_shared<ShaderProgram>();
  prog->embedded_type = type;
  prog->vs_name = d->vs;
  prog->ps_name = d->ps;
  for (const auto& u : d->uniforms) {
    Uniform x;
    x.name = u.name;
    x.stage = u.stage;
    x.offset = u.offset;
    x.size = u.size;
    prog->uniforms.push_back(x);
  }
  prog->ps_constants.assign(d->ps_size, 0);
  prog->vs_constants.assign(d->vs_size, 0);
  prog->vs = load_module(d->vs);
  prog->fs = load_module(d->ps);
  if (!prog->vs || !prog->fs) log::warn("psm.gfx", "embedded shader {} ({}+{}) has no host transcription yet", type, d->vs, d->ps);
  *handle = Objects::get().add_shader_program(std::move(prog));
  return kOk;
}
SSN_ICALL("Sce.PlayStation.Core.Renderer.ShaderManager::LoadShaderNative", LoadShaderNative);

ShaderProgram* find(int32_t h) { return Objects::get().shader_program(h); }

int32_t Delete(int32_t h) {  // 0x1e9ad0
  auto& o = Objects::get();
  o.release(o.shader_programs(), h);
  return kOk;
}
SSN_ICALL("Sce.PlayStation.Core.Graphics.PsmShaderProgram::Delete", Delete);

int32_t AddRef(int32_t h) {  // 0x1e9b70
  auto& o = Objects::get();
  return o.add_ref(o.shader_programs(), h) ? kOk : kErrorObjectDisposed;
}
SSN_ICALL("Sce.PlayStation.Core.Graphics.PsmShaderProgram::AddRef", AddRef);

// GetUniformCount — 0x1e9c80: PS members followed by VS members.
int32_t GetUniformCount(int32_t h, int32_t* out) {
  ShaderProgram* p = find(h);
  if (!p) return kErrorObjectDisposed;
  *out = static_cast<int32_t>(p->uniforms.size());
  return kOk;
}
SSN_ICALL("Sce.PlayStation.Core.Graphics.PsmShaderProgram::GetUniformCount", GetUniformCount);

// GetAttributeCount — 0x1e9d90: the attribute table is never filled for the embedded
// shaders, so 0.
int32_t GetAttributeCount(int32_t h, int32_t* out) {
  if (!find(h)) return kErrorObjectDisposed;
  *out = 0;
  return kOk;
}
SSN_ICALL("Sce.PlayStation.Core.Graphics.PsmShaderProgram::GetAttributeCount", GetAttributeCount);

// FindUniform(name) — 0x1e9ea0: first entry whose name matches, else -1.
int32_t FindUniform(int32_t h, MonoString* name, int32_t* out) {
  ShaderProgram* p = find(h);
  if (!p) return kErrorObjectDisposed;
  const std::string n = utf8(name);
  *out = -1;
  for (size_t i = 0; i < p->uniforms.size(); ++i)
    if (p->uniforms[i].name == n) {
      *out = static_cast<int32_t>(i);
      break;
    }
  return kOk;
}
SSN_ICALL("Sce.PlayStation.Core.Graphics.PsmShaderProgram::FindUniform", FindUniform);

// FindAttribute(name) — 0x1ea060: no attribute entries -> -1.
int32_t FindAttribute(int32_t h, MonoString*, int32_t* out) {
  if (!find(h)) return kErrorObjectDisposed;
  *out = -1;
  return kOk;
}
SSN_ICALL("Sce.PlayStation.Core.Graphics.PsmShaderProgram::FindAttribute", FindAttribute);

// GetUniformName/Type/Size/Texture(i) — 0x1ea9c0/0x1ea740/0x1eac80/0x1eaf00: the name,
// type 0 (None), the array count (1) and texture unit -1.
int32_t GetUniformName(int32_t h, int32_t i, MonoString** out) {
  ShaderProgram* p = find(h);
  if (!p) return kErrorObjectDisposed;
  if (i < 0 || i >= int32_t(p->uniforms.size())) return kErrorOutOfRange;
  store_ref(out, new_string(p->uniforms[i].name));
  return kOk;
}
SSN_ICALL("Sce.PlayStation.Core.Graphics.PsmShaderProgram::GetUniformName", GetUniformName);

int32_t GetUniformType(int32_t h, int32_t i, int32_t* out) {
  ShaderProgram* p = find(h);
  if (!p) return kErrorObjectDisposed;
  if (i < 0 || i >= int32_t(p->uniforms.size())) return kErrorOutOfRange;
  *out = 0;
  return kOk;
}
SSN_ICALL("Sce.PlayStation.Core.Graphics.PsmShaderProgram::GetUniformType", GetUniformType);

int32_t GetUniformSize(int32_t h, int32_t i, int32_t* out) {
  ShaderProgram* p = find(h);
  if (!p) return kErrorObjectDisposed;
  if (i < 0 || i >= int32_t(p->uniforms.size())) return kErrorOutOfRange;
  *out = 1;
  return kOk;
}
SSN_ICALL("Sce.PlayStation.Core.Graphics.PsmShaderProgram::GetUniformSize", GetUniformSize);

int32_t GetUniformTexture(int32_t h, int32_t i, int32_t* out) {
  ShaderProgram* p = find(h);
  if (!p) return kErrorObjectDisposed;
  if (i < 0 || i >= int32_t(p->uniforms.size())) return kErrorOutOfRange;
  *out = -1;
  return kOk;
}
SSN_ICALL("Sce.PlayStation.Core.Graphics.PsmShaderProgram::GetUniformTexture", GetUniformTexture);

// sub_1fbaa0: element size from ShaderUniformType, destination = the stage's CPU copy
// at offset + stride*to (rounded down to 4), length = min(count*elemSize, size - stride*to).
int32_t write_uniform(ShaderProgram* p, int32_t i, int32_t to, const uint8_t* src, size_t src_bytes, uint32_t type, int32_t from,
                      int32_t count) {
  if (i < 0 || i >= int32_t(p->uniforms.size())) return kErrorOutOfRange;
  const Uniform& u = p->uniforms[i];
  if (to < 0 || to >= u.array_count || from < 0 || count < 0) return kErrorOutOfRange;
  const uint32_t elem = (4 * (type & 3) + 4) * (((type >> 4) & 3) + 1);
  if (size_t(elem) * (from + count) > src_bytes) return kErrorOutOfRange;
  auto& cb = (u.stage & 1) ? p->vs_constants : p->ps_constants;
  const uint32_t stride = u.size;
  const uint32_t dst = (u.offset + stride * to) & ~3u;
  const uint32_t len = std::min<uint32_t>(uint32_t(count) * elem, u.size - stride * to);
  if (dst + len > cb.size()) return kErrorOutOfRange;
  std::memcpy(cb.data() + dst, src + size_t(from) * elem, len);
  return kOk;
}

// SetUniformValue(handle, index, to, ref T value, type) — 0x1eb040 (all scalar/vector/
// matrix overloads share the function).
int32_t SetUniformValue(int32_t h, int32_t i, int32_t to, const uint8_t* value, uint32_t type) {
  ShaderProgram* p = find(h);
  if (!p) return kErrorObjectDisposed;
  if (!value) return kErrorArgumentNull;
  return write_uniform(p, i, to, value, 64, type, 0, 1);
}
SSN_ICALL("Sce.PlayStation.Core.Graphics.PsmShaderProgram::SetUniformValue", SetUniformValue);

// SetUniformValue2(handle, index, T[] value, type, to, from, count) — 0x1eb1a0.
int32_t SetUniformValue2(int32_t h, int32_t i, MonoArray* value, uint32_t type, int32_t to, int32_t from, int32_t count) {
  ShaderProgram* p = find(h);
  if (!p) return kErrorObjectDisposed;
  ArrayView v = view(value);
  if (!v.data) return kErrorArgumentNull;
  if (count < 0) {
    // A negative count means "the whole array": the uniform index must be valid
    // (0x80b30003 otherwise) and the count is the array's byte size over the element size
    // of the uniform's own type (entry +12, always 0 = 4 bytes in these programs), rounded up.
    if (i < 0 || i >= int32_t(p->uniforms.size())) return kErrorOutOfRange;
    count = static_cast<int32_t>((v.bytes() + 4 - 1) / 4);
  }
  return write_uniform(p, i, to, v.data, v.bytes(), type, from, count);
}
SSN_ICALL("Sce.PlayStation.Core.Graphics.PsmShaderProgram::SetUniformValue2", SetUniformValue2);

}  // namespace
}  // namespace ssn::psm::gfx
