// Sce.PlayStation.Core.Resources.CxmlResources/Native icalls (libScePsm.sprx, FW 4.03):
// Mono glue over cxml_resource.cpp. Addresses are the icall functions libScePsm registers.
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

#include <mono/metadata/appdomain.h>
#include <mono/metadata/class.h>
#include <mono/metadata/object.h>

#include "common/mono_util.h"
#include "host/boundary.h"
#include "host/context.h"
#include "libScePsm/imaging/psm_common.h"
#include "libScePsm/resources/cxml_resource.h"

namespace ssn::psm::resources {
namespace {

// Console path -> host file, through the firmware dump the launcher was given.
std::string resolve_console_path(const std::string& console_path) {
  const auto* fw = host::ctx().firmware;
  return fw ? fw->host_path(console_path) : console_path;
}

// Create(out int, string, bool, bool) — 0x2f7c80.
int32_t Create(uint32_t* handle, MonoString* filename, MonoBoolean extracts_on_memory, MonoBoolean system_common_path) {
  // The native logs "[PSM] MonoStringUtf8() returned NULL" and continues with "".
  return create(handle, utf8(filename), extracts_on_memory != 0, system_common_path != 0, resolve_console_path);
}
SSN_ICALL("Sce.PlayStation.Core.Resources.CxmlResources/Native::Create", Create);

// CreateFromBuffer(out int, byte[]) — 0x2f97b0.
int32_t CreateFromBuffer(uint32_t* handle, MonoArray* buffer) {
  const auto* data = buffer ? reinterpret_cast<const uint8_t*>(mono_array_addr_with_size(buffer, 1, 0)) : nullptr;
  return create_from_buffer(handle, data, buffer ? mono_array_length(buffer) : 0);
}
SSN_ICALL("Sce.PlayStation.Core.Resources.CxmlResources/Native::CreateFromBuffer", CreateFromBuffer);

// Delete(int) — 0x2f9b20.
int32_t Delete(uint32_t handle) { return destroy(handle); }
SSN_ICALL("Sce.PlayStation.Core.Resources.CxmlResources/Native::Delete", Delete);

// SetLocale(int, string) — 0x2f9c10.
int32_t SetLocale(uint32_t handle, MonoString* locale) { return set_locale(handle, utf8(locale)); }
SSN_ICALL("Sce.PlayStation.Core.Resources.CxmlResources/Native::SetLocale", SetLocale);

// HasResourceType(int, uint, out uint) — 0x2fafb0.
int32_t HasResourceType(uint32_t handle, uint32_t type, uint32_t* result) { return has_resource_type(handle, type, result); }
SSN_ICALL("Sce.PlayStation.Core.Resources.CxmlResources/Native::HasResourceType", HasResourceType);

// GetIds(int, uint, out string) — 0x2fa840.
int32_t GetIds(uint32_t handle, uint32_t type, MonoString** ids) {
  std::string s;
  const int32_t r = get_ids(handle, type, &s);
  if (r == 0) store_ref(ids, new_string(s));
  return r;
}
SSN_ICALL("Sce.PlayStation.Core.Resources.CxmlResources/Native::GetIds", GetIds);

// GetIdCount(int, uint, out uint) — 0x2fac70.
int32_t GetIdCount(uint32_t handle, uint32_t type, uint32_t* count) { return get_id_count(handle, type, count); }
SSN_ICALL("Sce.PlayStation.Core.Resources.CxmlResources/Native::GetIdCount", GetIdCount);

// GetTextureResource(int, uint, string, ref TextureResourceOption, out TextureResourceNative)
// — 0x2fb680.
int32_t GetTextureResource(uint32_t handle, uint32_t type, MonoString* id, const TextureResourceOption* option,
                           TextureResourceNative* out) {
  return get_texture_resource(handle, type, utf8(id), option, out);
}
SSN_ICALL("Sce.PlayStation.Core.Resources.CxmlResources/Native::GetTextureResource", GetTextureResource);

// GetFromResourceBufferPtr(int, ref UnmanagedBuffer, byte[]) — 0x2fb570.
int32_t GetFromResourceBufferPtr(uint32_t handle, const UnmanagedBuffer* buffer, MonoArray* managed) {
  auto* dst = managed ? reinterpret_cast<uint8_t*>(mono_array_addr_with_size(managed, 1, 0)) : nullptr;
  return get_from_resource_buffer_ptr(handle, buffer, dst, managed ? uint32_t(mono_array_length(managed)) : 0, managed == nullptr);
}
SSN_ICALL("Sce.PlayStation.Core.Resources.CxmlResources/Native::GetFromResourceBufferPtr", GetFromResourceBufferPtr);

// DisposeResourceBufferPtr(int, ref UnmanagedBuffer) — 0x2fb460.
int32_t DisposeResourceBufferPtr(uint32_t handle, const UnmanagedBuffer* buffer) {
  return dispose_resource_buffer_ptr(handle, buffer);
}
SSN_ICALL("Sce.PlayStation.Core.Resources.CxmlResources/Native::DisposeResourceBufferPtr", DisposeResourceBufferPtr);

// GetString(int, string, out string) — 0x2fa4a0.
int32_t GetString(uint32_t handle, MonoString* id, MonoString** str) {
  std::u16string text;
  bool set = false;
  const int32_t r = get_string(handle, utf8(id), &text, &set);
  if (set) {
    MonoString* s = mono_string_new_utf16(mono_domain_get(), reinterpret_cast<const mono_unichar2*>(text.data()),
                                          static_cast<int32_t>(text.size()));
    store_ref(str, s);
  }
  return r;
}
SSN_ICALL("Sce.PlayStation.Core.Resources.CxmlResources/Native::GetString", GetString);

// GetResourceSize(int, uint, string, out uint) — 0x2fb0e0.
int32_t GetResourceSize(uint32_t handle, uint32_t type, MonoString* id, uint32_t* size) {
  return get_resource_size(handle, type, utf8(id), size);
}
SSN_ICALL("Sce.PlayStation.Core.Resources.CxmlResources/Native::GetResourceSize", GetResourceSize);

// GetResourceBuffer(int, uint, string, byte[]) — 0x2fb1e0.
int32_t GetResourceBuffer(uint32_t handle, uint32_t type, MonoString* id, MonoArray* buffer) {
  auto* dst = buffer ? reinterpret_cast<uint8_t*>(mono_array_addr_with_size(buffer, 1, 0)) : nullptr;
  return get_resource_buffer(handle, type, utf8(id), dst, buffer ? uint32_t(mono_array_length(buffer)) : 0);
}
SSN_ICALL("Sce.PlayStation.Core.Resources.CxmlResources/Native::GetResourceBuffer", GetResourceBuffer);

// GetResourceBufferPtr(int, uint, string, ref UnmanagedBuffer) — 0x2fb300.
int32_t GetResourceBufferPtr(uint32_t handle, uint32_t type, MonoString* id, UnmanagedBuffer* buffer) {
  return get_resource_buffer_ptr(handle, type, utf8(id), buffer);
}
SSN_ICALL("Sce.PlayStation.Core.Resources.CxmlResources/Native::GetResourceBufferPtr", GetResourceBufferPtr);

// Managed CxmlResources.SoundScriptNative (Pack = 4, 280 bytes): Id +0, CommandCount +8,
// Command0/1/2 at +16 + 88 * k, each SoundScriptCommandNative laid out as
//   Id +0, Command +8, Target +16, ValueString +24, RatioSize +32, ValueFromSize +36,
//   ValueSize +40, DurationSize +44, Direction +48, Ratio +52 (3 ints), ValueFrom +64,
//   Value +72, Duration +80 (2 ints each).
constexpr uint32_t kSoundScriptNativeSize = 280;

void store_string(uint8_t* field, const char* text) {
  if (text) store_ref(reinterpret_cast<MonoString**>(field), new_string(text));
}

void store_i32(uint8_t* field, int32_t v) { std::memcpy(field, &v, 4); }

// GetSoundScriptAll(int, SoundScriptNative[], out uint) — 0x2fe080: after the handle test,
// the array's element size must be 280 ("[PSM] Invalid sizeof SoundScript %zu != %d",
// 0x80B30001; a null array reports size 0); null count -> 0x80B30002. The strings are
// stored with the GC write barrier (mono_gc_wbarrier_generic_store), fields the native
// does not write are left as they are.
int32_t GetSoundScriptAll(uint32_t handle, MonoArray* scripts, uint32_t* count) {
  if (const int32_t r = check_resource(handle); r != 0) return r;
  const int32_t elem_size = scripts ? mono_array_element_size(mono_object_get_class(reinterpret_cast<MonoObject*>(scripts))) : 0;
  if (elem_size != int32_t(kSoundScriptNativeSize)) {
    std::fprintf(stderr, "[PSM] Invalid sizeof SoundScript %zu != %d\n", size_t(kSoundScriptNativeSize), elem_size);
    return err::kArgument;
  }
  if (!count) return err::kArgumentNull;
  std::vector<SoundScript> list;
  const int32_t r = get_sound_script_all(handle, mono_array_length(scripts), &list, count);
  for (size_t i = 0; i < list.size(); ++i) {
    auto* e = reinterpret_cast<uint8_t*>(mono_array_addr_with_size(scripts, kSoundScriptNativeSize, i));
    const SoundScript& s = list[i];
    store_string(e + 0, s.id);
    const uint32_t n = s.command_count < 3 ? s.command_count : 3;
    for (uint32_t k = 0; k < n; ++k) {
      uint8_t* c = e + 16 + 88 * k;
      const SoundScriptCommand& m = s.commands[k];
      store_string(c + 0, m.id);
      store_string(c + 8, m.command);
      store_string(c + 16, m.target);
      store_i32(c + 32, int32_t(m.ratio_size));
      std::memcpy(c + 52, m.ratio, 4 * m.ratio_size);
      store_i32(c + 36, int32_t(m.value_from_size));
      std::memcpy(c + 64, m.value_from, 4 * m.value_from_size);
      store_i32(c + 40, int32_t(m.value_size));
      std::memcpy(c + 72, m.value, 4 * m.value_size);
      store_string(c + 24, m.value_str);
      store_i32(c + 48, m.direction);
      store_i32(c + 44, int32_t(m.duration_size));
      std::memcpy(c + 80, m.duration, 4 * m.duration_size);
    }
    store_i32(e + 8, int32_t(s.command_count));
  }
  return r;
}
SSN_ICALL("Sce.PlayStation.Core.Resources.CxmlResources/Native::GetSoundScriptAll", GetSoundScriptAll);

// GetSoundScriptCommand(int, uint, string, int, out string, out string, out string, int[],
// out int, int[], out int, int[], out int, out string, out int, int[], out int) — 0x2fd180.
// The out values and arrays are written only on success.
int32_t GetSoundScriptCommand(uint32_t handle, uint32_t type, MonoString* id, int32_t index, MonoString** command_id,
                              MonoString** command_type, MonoString** tgt, MonoArray* ratio, int32_t* ratio_size,
                              MonoArray* value_from, int32_t* value_from_size, MonoArray* value, int32_t* value_size,
                              MonoString** value_str, int32_t* direction, MonoArray* duration, int32_t* duration_size) {
  MonoArray* arrays[4] = {ratio, value_from, value, duration};
  uint32_t caps[4];
  for (int i = 0; i < 4; ++i) caps[i] = arrays[i] ? uint32_t(mono_array_length(arrays[i])) : 0;
  SoundScriptCommandResult res;
  const int32_t r = get_sound_script_command(handle, type, utf8(id), index, caps, &res);
  if (r != 0) return r;
  store_ref(command_id, new_string(res.id));
  store_ref(command_type, new_string(res.command));
  store_ref(tgt, new_string(res.target));
  store_ref(value_str, new_string(res.value_str));
  const std::vector<int32_t>* vals[4] = {&res.ratio, &res.value_from, &res.value, &res.duration};
  int32_t* sizes[4] = {ratio_size, value_from_size, value_size, duration_size};
  for (int i = 0; i < 4; ++i) {
    if (arrays[i] && !vals[i]->empty())
      std::memcpy(mono_array_addr_with_size(arrays[i], 4, 0), vals[i]->data(), 4 * vals[i]->size());
    *sizes[i] = int32_t(vals[i]->size());
  }
  *direction = res.direction;
  return 0;
}
SSN_ICALL("Sce.PlayStation.Core.Resources.CxmlResources/Native::GetSoundScriptCommand", GetSoundScriptCommand);

// GetSoundGroup(int, uint, string, out int, out int) — 0x2fcd30.
int32_t GetSoundGroup(uint32_t handle, uint32_t type, MonoString* id, int32_t* limit_mode, int32_t* voice_num) {
  int32_t lm = -1, vn = -1;
  const int32_t r = get_sound_group(handle, type, utf8(id), &lm, &vn);
  if (r != 0) return r;
  *limit_mode = lm;
  *voice_num = vn;
  return 0;
}
SSN_ICALL("Sce.PlayStation.Core.Resources.CxmlResources/Native::GetSoundGroup", GetSoundGroup);

}  // namespace
}  // namespace ssn::psm::resources
