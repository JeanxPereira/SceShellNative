// libSceNgs2.native — internal object model of the reproduction (see ngs2.h). The firmware keeps
// its systems, racks and 784-byte voice entries in arenas carved from the caller's allocator;
// here they are host objects with the fields the reached code reads and writes, each named after
// its firmware offset (voice +N = offset in the 784-byte entry, rack +N = rack object, system +N).
#pragma once
#include <array>
#include <atomic>
#include <cstdint>
#include <memory>
#include <mutex>
#include <vector>

#include "libSceNgs2/ngs2.h"

namespace ssn::ngs2 {

struct System;
struct Rack;
struct Voice;

// Internal module ids (registry qword_6C020[id]).
enum ModuleId : uint8_t {
  kModPlayer = 1,
  kModEnvelope = 16,
  kModDistortion = 17,
  kModCompressor = 18,
  kModFilter = 19,
  kModLfeFilter = 20,
  kModGain = 21,
  kModPeakMeter = 24,
  kModUserFx = 25,
  kModOutput = 26,
  kModChannelConverter = 34,
};

// sub_43D0 / sub_45B0 / sub_4680: the rack's channel-work slot allocator (bitmap with a rotating
// next-free index, rack +424). The slots index the per-channel buffers and the patch gain state.
struct SlotAllocator {
  uint32_t count = 0;  // +0
  uint32_t next = 0;   // +4
  uint32_t used = 0;   // +8
  std::vector<uint32_t> bits;  // +12
  int32_t alloc_one();
  void free_one(uint16_t slot);
};

// Player module parameters / per-voice state (ngs2_player.cpp).
struct PlayerParams;
struct PlayerState;
struct PlayerDeleter { void operator()(PlayerParams* p) const; void operator()(PlayerState* p) const; };
PlayerParams* player_new_params();
PlayerState* player_new_state();
void player_copy_params(PlayerParams& dst, const PlayerParams& src);

// ChannelConverter (desc 0x67080): params 0xBD8 bytes, state 0x108 bytes (ngs2_fx.cpp).
struct CCParams {
  uint32_t num_in, num_out;
  float m[(0xBD8 - 8) / 4];
};
struct CCState {
  uint32_t num_in, num_out;
  float m[(0x108 - 8) / 4];
};

// Port parameters of a voice output port (param table 0x6C850): volume (default 1.0, 0..32),
// matrixId (-1), pannerId (-1).
struct PortParams {
  float volume = 1.0f;
  int8_t matrix_id = -1;
  int8_t panner_id = -1;
};
// Patch of a voice output port (group 3, voice +1360 pending / +1392 current; command 9).
struct PatchEntry {
  uint8_t type = 0;         // +0: 0 none, 1 voice input, 2 render buffer, 3 callback
  uint8_t input = 0;        // +1: destination input index
  uint16_t dest_index = 0;  // +2: destination voice index
  uint32_t dest_rack = 0;   // +4: destination rack id (+68 of the destination rack)
  VoiceHandle dest = nullptr;  // +8
};

// One voice of a rack (the 784-byte entry, rack +416 + 784 * index).
struct Voice {
  Rack* rack = nullptr;                // +32
  std::atomic<uint32_t> flags{0};      // +140: low byte = state flags; 0x100.. event bits; 0x20000000 play
                                       //       pending; 0x40000000 set up; 0x2000000 killed; 0x80000000 lock
  uint16_t index = 0;                  // +144
  uint8_t toggle = 0;                  // +146
  uint8_t type = 0;                    // +147: list (0 / 1 / 2)
  uint8_t setup_in = 0, setup_ch = 0;  // +148 / +149 (setup channels)
  uint8_t slots = 0;                   // +150: channel slots in use
  uint8_t setup_slots = 0;             // +151
  int32_t v152 = 0;                    // +152
  uint8_t pend_in = 0, pend_ch = 0;    // +156 / +157
  uint8_t reserved_slots = 0;          // +158
  uint8_t pend_slots = 0;              // +159
  uint8_t setup_flags4 = 0;            // +160
  uint32_t setup_flags = 0;            // +164
  uint32_t init_dirty = 0;             // +168
  uint32_t mod_state0 = 0, mod_state1 = 0, mod_stage = 0, mod_stage_ports = 0;  // +172 / +176 / +180 / +184
  // Parameter groups (+296 + 24 * g; g = 0 modules, 1 matrices, 2 ports, 3 patches):
  // {+0 dirty, +4 committed, +8 pending, +12 committed this render, +16 first set, +20 accumulated}.
  struct Group { uint32_t dirty = 0, committed = 0, pending = 0, now = 0, first = 0, accum = 0; };
  std::array<Group, 4> g{};
  // Render-time state (set at PLAY by 0x13D70).
  int32_t error = 0;                   // +516
  uint32_t run_flags = 0;              // +520 (= +164)
  uint8_t in_ch = 0, out_ch = 0;       // +524 / +525
  uint8_t num_slots = 0;               // +526
  uint8_t vstate = 0;                  // +527: 0 run, 1 pause fade, 2 resume fade, 3 paused, 4 stop, 5 kill
  uint16_t events = 0;                 // +528
  uint8_t status = 0;                  // +530 (copied into the low byte of +140 after every render)
  uint8_t list = 0;                    // +532
  int8_t shift = 0;                    // +533
  uint8_t grain_div = 0, grain_phase = 0;  // +534 / +535
  uint32_t patch_mask = 0;             // +544: ports patched to voices
  uint32_t output_mask = 0;            // +548: ports patched to buffers / callbacks
  uint32_t alive_mask = 0;             // +552: modules that keep the voice alive
  uint32_t v564 = 0;                   // +564
  uint32_t pass0 = 0, pass1 = 0, staged = 0;  // +568 / +572 / +576
  std::array<uint16_t, 37> slot_map{}; // +64: channel -> slot
  std::array<uint16_t, 37> run_map{};  // +440 (copy of +64 at PLAY)
  // Parameters (pending / committed) of the modules on this path, the ports and the patches.
  std::unique_ptr<PlayerParams, PlayerDeleter> player_pending, player_params;
  std::unique_ptr<PlayerState, PlayerDeleter> player_state;
  CCParams cc_pending{}, cc_params{};
  CCState cc_state{};
  uint32_t out_pending = 0, out_params = 0;  // Output 'outputId'
  std::vector<PortParams> port_pending, port_params;
  std::vector<PatchEntry> patch_pending, patch_params;
  // Inputs: voices patched into this voice, per input (voice +40 lists), in patch order.
  std::vector<std::vector<std::pair<Voice*, uint32_t>>> inputs;  // (source voice, source port)
  bool active_listed = false;
};

struct ModuleSlot {  // one entry of the rack's module list (rack +10296, 24 bytes)
  uint8_t id;
  bool keeps_alive;  // descriptor +0x28 bit0 -> rack +316
};

struct Rack {
  System* system = nullptr;
  uint32_t id = 0;              // rack id (0x1000 / 0x3000)
  uint32_t handle_id = 0;       // +68
  uint32_t max_voices = 0;      // +304 (u16)
  uint8_t num_outputs = 1;      // +308 (output buffer sets)
  uint8_t num_modules = 0;      // +309
  uint8_t num_inputs = 0;       // +310
  uint8_t num_ports = 1;        // +311
  uint8_t max_channels = 8;     // +313
  uint32_t keep_alive = 0;      // +316
  uint32_t voice_type = 0;      // +147 of every voice
  std::vector<ModuleSlot> modules;  // +10296
  std::vector<uint8_t> order;       // +384 (+1608 of the template)
  SlotAllocator slots;              // +424
  // Channel-work buffers (+1744, slot stride +1756, output set stride +1760): grain floats each.
  std::vector<std::vector<float>> slot_buffers;  // [set * slots + slot]
  // Patch gain state per (slot, port): {u32, float gain[out ch]} (+1232, slot +1244, port +1248).
  std::vector<std::array<float, 38>> ramp;  // [slot * ports + port]
  std::vector<std::unique_ptr<Voice>> voices;  // +416
  int32_t active = 0;  // +292
  float* buffer(uint32_t set, uint16_t slot) { return slot_buffers[size_t(set) * slots.count + slot].data(); }
};

struct System {
  std::recursive_mutex lock;      // +176.. the system lock held by API calls and render
  uint32_t max_grain = 512;       // +296
  uint32_t grain = 256;           // +452
  uint32_t sample_rate = 48000;   // +460 (+488 option)
  uint32_t max_channels = 8;
  uint8_t rate_index = 0;         // +448
  uint16_t grain_changed = 0;     // +450 (0x200 when grain / rate changed)
  std::vector<Rack*> racks;       // +224.. lists by rack kind
  std::vector<std::unique_ptr<Rack>> owned;
  // Voice processing lists (+320/+328 type 0, +344/+352 type 1, +368/+376 type 2).
  std::array<std::vector<Voice*>, 3> lists;
  // Render buffers of the current render (+392, count +408, written mask +412).
  const RenderBufferInfo* buffers = nullptr;
  uint32_t num_buffers = 0;
  uint32_t written = 0;
  uint64_t renders = 0;           // +424
  BufferAllocator allocator{};
};

// Gain helpers shared by the patch mixer and the modules (ngs2_fx.cpp).
void write_gain(const float* src, float* dst, uint32_t n, float g_prev, float g_cur);       // sub_50C0
void accumulate_gain(const float* src, float* dst, uint32_t n, float g_prev, float g_cur);  // sub_53E0
inline void clear_samples(float* dst, uint32_t n) { for (uint32_t i = 0; i < n; ++i) dst[i] = 0.0f; }  // sub_4F80
// Default channel matrix (rows = output channel) for the panner kind 0..3 (index table 0x577D0,
// floats 0x54890): nullptr when the counts are out of range.
const float* default_matrix(uint32_t kind, uint32_t num_in, uint32_t num_out);

// Module context (built per call by sub_171F0) — the fields the reached modules use.
struct ModuleCtx {
  Voice* voice;
  uint16_t flags;        // +86: 0x100 reset, 0x200 parameters changed
  uint8_t status;        // +88 (voice +530)
  uint8_t num_channels;  // +92
  uint32_t num_samples;  // +96
  uint32_t sample_rate;  // +100
  float* const* in;      // +56
  float* const* out;     // +72
};
int32_t player_process(ModuleCtx& c);                 // 0x30CF0
void player_reset_params(PlayerParams& p);            // descriptor 0x696E0 defaults
int32_t player_set_param(PlayerParams& p, const VoiceCommand& cmd);  // sub_A5A0 for params 0 / 1
void channel_converter(ModuleCtx& c, const CCParams& p, CCState& st, uint32_t out_limit);  // 0x2C7D0
void output_module(ModuleCtx& c, uint32_t output_id);  // 0x307D0 -> 0x15990
void cc_reset_params(CCParams& p);                     // descriptor 0x67080 defaults

}  // namespace ssn::ngs2
