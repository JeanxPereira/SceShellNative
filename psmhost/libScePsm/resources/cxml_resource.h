// Sce.PlayStation.Core.Resources.CxmlResources — native side of libScePsm.sprx (FW 4.03)
// without the Mono glue (cxml_icalls.cpp), so it can be driven standalone.
//
// A CxmlResources handle maps (table xmmword_542450, shared handle generator) to a small
// holder whose +32 is the "ResourceObj" (72 bytes):
//   +0 fd (-1 for memory documents), +8 file name, +32 CXML document, +40 locale (string)
//   document, +48 locale document buffer, +56 status, +60 extract-on-memory flag,
//   +64 mutex "ScePsmResourceStr".
#pragma once
#include <cstddef>
#include <cstdint>
#include <functional>
#include <string>
#include <vector>

namespace ssn::psm::resources {

// Managed structs, bit-for-bit (Core.dll).
struct UnmanagedBuffer {  // Sce.PlayStation.Core.UnmanagedBuffer
  uint64_t address;
  uint32_t size;
};
struct TextureResourceOption {  // [StructLayout(Sequential, Pack = 4)]
  float pixel_density;
  int32_t width;
  int32_t height;
};
#pragma pack(push, 4)
struct TextureResourceNative {  // CxmlResources.TextureResourceNative, Pack = 4
  int32_t attributes;           // TextureAttributeFlags: 1 HasMargin, 2 HasNinePatchMargin,
                                // 4 IsAlpha8, 8 Is4K, 0x10 Exist4K
  int32_t padding;
  UnmanagedBuffer ubuffer;      // +8
  int32_t nine_patch[4];        // +24 Left, Top, Right, Bottom
  int32_t margin[4];            // +40
};
#pragma pack(pop)
static_assert(sizeof(TextureResourceNative) == 56);

// Maps a console path ("/system_ex/vsh_asset/x.rco") to a host file path.
using PathResolver = std::function<std::string(const std::string& console_path)>;

int32_t create(uint32_t* handle, const std::string& filename, bool extracts_on_memory, bool system_common_path,
               const PathResolver& resolve);                                            // 0x2f7c80
int32_t create_from_buffer(uint32_t* handle, const uint8_t* data, size_t size);          // 0x2f97b0
int32_t destroy(uint32_t handle);                                                       // Delete 0x2f9b20
int32_t set_locale(uint32_t handle, const std::string& locale);                         // 0x2f9c10
int32_t has_resource_type(uint32_t handle, uint32_t type, uint32_t* result);            // 0x2fafb0
int32_t get_ids(uint32_t handle, uint32_t type, std::string* ids);                      // 0x2fa840
int32_t get_id_count(uint32_t handle, uint32_t type, uint32_t* count);                  // 0x2fac70
int32_t get_texture_resource(uint32_t handle, uint32_t type, const std::string& id,
                             const TextureResourceOption* option, TextureResourceNative* out);  // 0x2fb680
// GetString 0x2fa4a0: `out` receives the UTF-16 text (empty on errors); `out_set` tells
// whether the native writes the out string at all.
int32_t get_string(uint32_t handle, const std::string& id, std::u16string* out, bool* out_set);
int32_t get_resource_size(uint32_t handle, uint32_t type, const std::string& id, uint32_t* size);   // 0x2fb0e0
int32_t get_resource_buffer(uint32_t handle, uint32_t type, const std::string& id, uint8_t* buffer,
                            uint32_t buffer_length);                                                 // 0x2fb1e0
int32_t get_resource_buffer_ptr(uint32_t handle, uint32_t type, const std::string& id, UnmanagedBuffer* buffer);  // 0x2fb300
int32_t get_from_resource_buffer_ptr(uint32_t handle, const UnmanagedBuffer* buffer, uint8_t* managed,
                                     uint32_t managed_length, bool managed_null);                    // 0x2fb570
int32_t dispose_resource_buffer_ptr(uint32_t handle, const UnmanagedBuffer* buffer);                 // 0x2fb460

// One <control> of a <soundscript>, as GetSoundScriptAll stores it into a managed
// SoundScriptCommandNative. A null string means the native leaves that managed field
// untouched; int arrays hold their `*_size` first values (the rest is left untouched).
struct SoundScriptCommand {
  const char* id = nullptr;         // "id"        (string attribute, type 3)
  const char* command = nullptr;    // "command"
  const char* target = nullptr;     // "tgt"
  const char* value_str = nullptr;  // "value_str"
  uint32_t ratio_size = 0, value_from_size = 0, value_size = 0, duration_size = 0;
  int32_t direction = -1;           // "direction" (int attribute, type 1), -1 when absent
  int32_t ratio[3] = {}, value_from[2] = {}, value[2] = {}, duration[2] = {};
};
struct SoundScript {                // managed CxmlResources.SoundScriptNative (280 bytes)
  const char* id = nullptr;         // the soundscript's "id" (idtable text)
  uint32_t command_count = 0;       // number of <control> children (all of them)
  SoundScriptCommand commands[3];   // Command0..Command2: the first three controls
};

// GetSoundScriptAll 0x2fe080 (the Mono glue checks the managed element size first).
// Produces up to `capacity` entries; *count = entries produced. The strings point into
// the resource's document and stay valid while the handle is alive.
int32_t get_sound_script_all(uint32_t handle, size_t capacity, std::vector<SoundScript>* scripts, uint32_t* count);
// The handle / document test the icalls start with: 0, or 0x80B30005.
int32_t check_resource(uint32_t handle);

// GetSoundScriptCommand 0x2fd180: control number `index` of element `id` in `type`'s table.
// Strings missing in the document come back as "" (the native substitutes unk_366E35).
// The int arrays are clamped to the managed array lengths `caps` (ratio, value_from,
// value, duration).
struct SoundScriptCommandResult {
  std::string id, command, target, value_str;
  int32_t direction = -1;
  std::vector<int32_t> ratio, value_from, value, duration;  // sizes = the *Size outs
};
int32_t get_sound_script_command(uint32_t handle, uint32_t type, const std::string& id, int32_t index,
                                 const uint32_t caps[4], SoundScriptCommandResult* out);

// GetSoundGroup 0x2fcd30: element `id` of `type`'s table (the managed side passes 8,
// "soundgroup"); its int attributes "limit_mode" and "voice_num" (-1 when absent or not
// int). Failures as GetSoundScriptCommand (0x80B36001 with the exception info).
int32_t get_sound_group(uint32_t handle, uint32_t type, const std::string& id, int32_t* limit_mode, int32_t* voice_num);

// libScePsm's path normalisation sub_2F6840 (collapses "//", resolves "..").
std::string normalize_path(const std::string& path);

}  // namespace ssn::psm::resources
