// The PSS audio system of libScePsm.sprx (FW 4.03) — "scePssCAudioSystem" (C++ object
// qword_4FBEA8, 4448 bytes, constructor sub_16E3B0, set-up sub_16ED10), as far as the
// Sce.PlayStation.Core.Audio.Sound / SoundPlayer icalls reach it:
//
//   * resources: audio data objects (760 bytes, sub_1797C0) in a 256-entry table
//     (sub_173FA0 / sub_174060); a memory image is parsed by sub_171370 (RIFF/WAVE with
//     ATRAC9: sub_178940; VAG: sub_179210);
//   * players ("voices", 344 bytes, sub_15F200) in a 128-entry table; a voice of a
//     compressed resource gets a 64 KiB stream buffer (32 in the pool) and a decoder
//     (HEVAG: 32 in the pool; ATRAC9: 1);
//   * the "ScePsP:AudioStream" thread (sub_16F7F0) decodes into the stream buffers on the
//     requests the mixer queues (sub_1727E0 / sub_1725E0);
//   * the main output port (system +4056): 48 kHz, 2 channels, 32-bit float, 256-frame
//     grains; its driver thread (sub_1900D0) calls the mixer (sub_170450 -> sub_170C80) for
//     every grain and hands the grain to the audio-out port. On the PC that port is the
//     host's audio device (WASAPI shared mode) — see pss_audio_out.h;
//   * the BGM port (system +4240, +4392 = 1): players created with flag 0x400 (the Bgm
//     players, sub_183D40 with 0x500480); the only one becomes the port's direct player
//     (+4368), whose decoded stream the BGM driver copies straight into the port's grains
//     (sub_172320 -> sub_164BB0), with the voice volume as the port volume.
//
// Functions are named after the C API wrappers the icalls call (sub_15DD50.. / sub_15EB90..).
// Results are PSS codes (0x80B3Fxxx); the icalls map them.
#pragma once
#include <cstdint>
#include <memory>
#include <vector>

namespace ssn::psm::audio {

namespace pss {
constexpr uint32_t kFail = 0x80B3F001u;
constexpr uint32_t kNoMemory = 0x80B3F002u;
constexpr uint32_t kArgument = 0x80B3F003u;
constexpr uint32_t kSeekRange = 0x80B3F008u;
constexpr uint32_t kEnd = 0x80B3F009u;
constexpr uint32_t kNotReady = 0x80B3F012u;
constexpr uint32_t kDying = 0x80B3F014u;
constexpr uint32_t kNotFound = 0x80B3F015u;
constexpr uint32_t kBadHandle = 0x80B3F018u;
constexpr uint32_t kNoSlot = 0x80B3F019u;
constexpr uint32_t kUnsupported = 0x80B3F021u;
constexpr uint32_t kBusy = 0x80B3F029u;
}  // namespace pss

struct PssResourceInfo {  // sub_1722C0 (through sub_184720)
  uint32_t rate = 0, channels = 0, bits = 0, bitrate = 0, format = 0;
};

// sub_15EB90 -> sub_171260: a resource from a memory image. `image` stays referenced by the
// resource (the firmware keeps pointing into the caller's buffer). flags 2/4/0x200 as the
// wrapper passes them (the Sound icalls pass 0).
uint32_t pss_resource_create(std::shared_ptr<const std::vector<uint8_t>> image, uint16_t flags, uint32_t* handle);
uint32_t pss_resource_release(uint32_t handle);                       // sub_15F120 -> sub_172A20
uint32_t pss_resource_info(uint32_t handle, PssResourceInfo* info);   // sub_15EBF0 -> sub_1722C0

// sub_15DD80 -> sub_15EC10 -> sub_171DC0: a player for a resource. flags: 0x80 (what every
// Sound.CreatePlayer* passes, sub_1848B0/sub_184800/sub_184740), 0x800/0x1000 pad-speaker /
// personal ports, 0x500480 the Bgm player (sub_183D40: BGM port 0x400, direct 0x400000,
// driver started on play 0x100000).
uint32_t pss_player_create(uint32_t resource, uint32_t flags, int32_t user_id, int32_t app_module, uint32_t* handle);
uint32_t pss_player_destroy(uint32_t handle);                    // sub_15F120 -> sub_172A20
uint32_t pss_player_play(uint32_t handle);                       // sub_15EB20
uint32_t pss_player_stop(uint32_t handle, uint16_t fade_frames); // sub_15ED40
uint32_t pss_player_is_playing(uint32_t handle, int32_t* playing);  // sub_15F140
uint32_t pss_player_set_volume(uint32_t handle, float volume);   // sub_15EAC0
uint32_t pss_player_set_pan(uint32_t handle, float pan);         // sub_15F1B0 (pan in [0, 1])
uint32_t pss_player_set_rate(uint32_t handle, float rate);       // sub_15F0D0
uint32_t pss_player_set_loop(uint32_t handle, bool loop);        // sub_15EFC0
uint32_t pss_player_position(uint32_t handle, uint64_t* ms);     // sub_15EF30
uint32_t pss_player_set_position(uint32_t handle, uint64_t ms);  // sub_15EEA0
uint32_t pss_player_length(uint32_t handle, uint64_t* ms);       // sub_15ECF0
uint32_t pss_player_pause(uint32_t handle);                      // sub_15EDA0
uint32_t pss_player_is_paused(uint32_t handle, int32_t* paused); // sub_15EDE0
uint32_t pss_player_port_status(uint32_t handle, uint32_t* status);  // sub_15EE30
uint32_t pss_bgm_port_status(uint32_t* status);                  // sub_15DDF0
uint32_t pss_player_loop_positions(uint32_t handle, uint64_t* start_ms, uint64_t* end_ms);  // sub_15F010

}  // namespace ssn::psm::audio
