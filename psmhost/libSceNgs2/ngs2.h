// libSceNgs2 (Sony's NGS2 sound engine) as the ShellUI eboot's BGLayer sounds use it, reproduced
// from the PS5 module the eboot loads for sceSysmoduleLoadModule(11): libSceNgs2.native.sprx
// (system/common/lib of the dump; libSceSysmodule's native table maps id 11 to it). Addresses are
// offsets in that module.
//
// The eboot ("NGS2 Music System", eboot 0x8FE40..0x915A0) builds one system with a mastering rack
// (one 8-channel mastering voice) and a sampler rack (one sampler voice per loaded sound, patched
// to the mastering voice), and renders 512-frame grains of 8-channel float from its own audio
// thread into an sceAudioOut port. The engine is reproduced as the firmware runs it for that use:
//   * API layer: SystemResetOption 0x1D090, SystemCreateWithAllocator 0x1D2F0 (-> 0x1E7E0),
//     RackCreateWithAllocator 0xEFC0 (-> 0xFFD0 / 0x102C0) for the sampler (0x1000) and mastering
//     (0x3000) racks, RackGetVoiceHandle 0xFBD0, VoiceControl 0x227B0 (parameters -> internal
//     commands), VoiceRunCommands 0x20250 (validate 0x20920, apply 0x212B0), VoiceGetStateFlags
//     0x20690 (query 0x4003 in 0x1F590), ParseWaveformData 0xCED0, SystemRender 0x1E460,
//     RackDestroy 0xF110, SystemDestroy 0x1D410.
//   * render (0x12EF0): event application and parameter commit (0x13D70), voice processing in list
//     order (0x15050 -> 0x15DE0): input patches (0x17620), module passes (0x171F0), voice state
//     fades; the status merge (0x135A0); render buffers nobody wrote are cleared.
//   * modules on this path (a module runs only once a command has set one of its parameters):
//     Player (sampler; ngs2_player.cpp), ChannelConverter and Output (mastering; ngs2_fx.cpp).
// What is not on this path (streams, custom/submixer/reverb racks, user FX, job schedulers, the
// other effect modules' DSP, other waveform types) is not reproduced; reaching it logs once.
#pragma once
#include <cstddef>
#include <cstdint>

namespace ssn::ngs2 {

// --- SDK structures (layouts as the firmware reads them) -----------------------------------------
struct ContextBufferInfo {  // SceNgs2ContextBufferInfo (the eboot's handlers use +0 / +8)
  void* host_buffer;        // +0
  size_t host_buffer_size;  // +8
  uintptr_t reserved[5];    // +16
  void* user_data;          // +56
};
struct BufferAllocator {    // SceNgs2BufferAllocator (eboot 0x90D80: +8 sub_90E00, +16 sub_90E30, +24)
  int32_t (*alloc_handler)(ContextBufferInfo*);
  int32_t (*free_handler)(ContextBufferInfo*);
  void* user_data;
};

// SceNgs2SystemOption (0x90 bytes; defaults 0x54800 copied by sceNgs2SystemResetOption 0x1D090).
struct SystemOption {
  size_t size;                  // +0x00 = 0x90
  char name[16];                // +0x08
  uint32_t flags;               // +0x18
  uint8_t reserved_1c[0x6C - 0x1C];
  uint32_t max_grain_samples;   // +0x6C (default 512, 64..2048, multiple of 64)
  uint32_t num_grain_samples;   // +0x70 (default 256; the eboot sets 512)
  uint32_t sample_rate;         // +0x74 (default 48000)
  uint32_t max_channels;        // +0x78 (default 8, 1..37)
  uint8_t reserved_7c[0x90 - 0x7C];
};
static_assert(sizeof(SystemOption) == 0x90);

// SceNgs2RenderBufferInfo (24 bytes; system +392, count +408).
struct RenderBufferInfo {
  void* buffer;            // +0
  size_t buffer_size;      // +8
  uint32_t waveform_type;  // +16 (24 = float32 interleaved, 18 = int16 interleaved)
  uint32_t num_channels;   // +20
};

// SceNgs2WaveformBlock (40 bytes) and SceNgs2WaveformInfo (232 bytes) as ParseWaveformData fills them.
struct WaveformBlock {
  uint64_t data_offset;       // +0
  uint64_t data_size;         // +8
  uint32_t num_repeats;       // +16
  uint32_t num_skip_samples;  // +20
  uint32_t num_samples;       // +24
  uint32_t reserved;          // +28
  uint64_t user_data;         // +32
};
static_assert(sizeof(WaveformBlock) == 40);
struct WaveformInfo {
  uint32_t waveform_type;     // +0  (64 = ATRAC9)
  uint32_t num_channels;      // +4
  uint32_t sample_rate;       // +8
  uint32_t config_data;       // +12
  uint32_t frame_offset;      // +16
  uint32_t frame_margin;      // +20
  uint32_t data_offset;       // +24
  uint32_t data_size;         // +28
  uint32_t loop_begin;        // +32
  uint32_t loop_end;          // +36
  uint32_t num_samples;       // +40
  uint32_t frame_bytes;       // +44
  uint32_t frame_samples;     // +48
  uint32_t frames_per_sf;     // +52
  uint32_t sf_bytes;          // +56
  uint32_t sf_samples;        // +60
  uint32_t delay;             // +64
  uint32_t num_blocks;        // +68
  WaveformBlock blocks[4];    // +72
};
static_assert(sizeof(WaveformInfo) == 232);

// Parameter blocks of sceNgs2VoiceControl: {u16 size, i16 next (byte offset, 0 = last), u32 id}.
struct VoiceParamHead {
  uint16_t size;
  int16_t next;
  uint32_t id;
};

// Internal voice command (16 bytes) of sceNgs2VoiceRunCommands: dword +0 = (index << 24) |
// (module id << 16) | command / parameter index; +4 u8 flags; +5 u8 value type / size; +6 u16
// count; +8 value (inline) or pointer.
struct VoiceCommand {
  uint32_t id;
  uint8_t flags;
  uint8_t type;
  uint16_t count;
  union {
    uint64_t u64;
    uint32_t u32;
    float f32;
    const void* ptr;
  } value;
};
static_assert(sizeof(VoiceCommand) == 16);

using SystemHandle = void*;
using RackHandle = void*;
using VoiceHandle = void*;

constexpr uint32_t kRackSampler = 0x1000;
constexpr uint32_t kRackMastering = 0x3000;

// --- API (results are the firmware's: 0 or an 0x804Axxxx error) ----------------------------------
int32_t SystemResetOption(SystemOption* option);
int32_t SystemCreateWithAllocator(const SystemOption* option, const BufferAllocator* allocator, SystemHandle* out);
int32_t SystemDestroy(SystemHandle system, ContextBufferInfo* out_info);
int32_t SystemRender(SystemHandle system, const RenderBufferInfo* buffers, uint32_t count);
int32_t RackCreateWithAllocator(SystemHandle system, uint32_t rack_id, const void* option, const BufferAllocator* allocator,
                                RackHandle* out);
int32_t RackDestroy(RackHandle rack, ContextBufferInfo* out_info);
int32_t RackGetVoiceHandle(RackHandle rack, uint32_t index, VoiceHandle* out);
int32_t VoiceControl(VoiceHandle voice, const VoiceParamHead* params);
int32_t VoiceRunCommands(VoiceHandle voice, const VoiceCommand* commands, uint32_t count);
int32_t VoiceGetStateFlags(VoiceHandle voice, uint32_t* out_flags);
int32_t ParseWaveformData(const void* data, size_t size, WaveformInfo* out);

}  // namespace ssn::ngs2
