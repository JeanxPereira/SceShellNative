// libSceNgs2.native — systems, racks, voices, commands and the render loop (see ngs2.h).
//
// Locking: the firmware guards each voice with a spin bit in its flags (+140 bit 31, sub_20450),
// each rack with "sce_Ngs2_rack_flush_lock" and the render with the system lock (+288); the host
// takes one system-wide lock for every API call and for a whole render, so commands land between
// renders exactly as they do on the console when the caller's thread is not racing the render.
#include <algorithm>
#include <cmath>
#include <cstring>
#include <mutex>
#include <set>

#include "core/log.h"
#include "host/boundary.h"
#include "libSceNgs2/ngs2_image.h"
#include "libSceNgs2/ngs2_internal.h"

namespace ssn::ngs2 {
namespace {

// Firmware result codes (0x804A8xxx) of the paths reached.
constexpr int32_t E(uint32_t v) { return static_cast<int32_t>(v); }
constexpr int32_t kErrNullArg = E(0x804A8010);         // 2152366096: "outXxx" null
constexpr int32_t kErrNoAllocator = E(0x804A8023);     // 2152366115
constexpr int32_t kErrOptionSize = E(0x804A8013);      // 2152366099
constexpr int32_t kErrGrainMax = E(0x804A8311);        // 2152366865
constexpr int32_t kErrGrain = E(0x804A8310);           // 2152366864
constexpr int32_t kErrSampleRate = E(0x804A8051);      // 2152366161
constexpr int32_t kErrChannels = E(0x804A8052);        // 2152366162
constexpr int32_t kErrSystemHandle = E(0x804A8201);    // 2152366593
constexpr int32_t kErrVoiceHandle = E(0x804A8203);     // 2152366595
constexpr int32_t kErrRackHandle = E(0x804A8202);      // -2142600702 (0xFBD0: sub_3BB0 type 2 fails)
constexpr int32_t kErrRackId = E(0x804A8300);          // -2142600448: unknown rack id (0x102C0 LABEL_184)
constexpr int32_t kErrVoiceIndex = E(0x804A8305);      // -2142600443 (0xFBD0)
constexpr int32_t kErrEvent = E(0x804A8353);           // 2152366931
constexpr int32_t kErrNoChannels = E(0x804A8351);      // 2152366929
constexpr int32_t kErrPort = E(0x804A8357);            // 2152366935
constexpr int32_t kErrCommand = E(0x804A835A);         // 2152366938: invalid module command
constexpr int32_t kErrParam = E(0x804A8360);           // 2152366944: null parameter block
constexpr int32_t kErrParamId = E(0x804A8362);         // 2152366946: unknown parameter id
constexpr int32_t kErrNoSlot = E(0x804A8001);          // sub_43D0 -2142601215
constexpr int32_t kErrRenderBuffers = E(0x804A830B);   // -2142600437: more than 31 render buffers (0x12EF0)

void note_once(const std::string& what) { host::note_unimplemented("ngs2: " + what); }

// Live handles (sub_3BB0 checks a handle's type tag; a stale or foreign pointer fails).
std::mutex g_handles_lock;
std::set<const void*> g_systems, g_racks, g_voices;
bool live(std::set<const void*>& s, const void* h) {
  std::lock_guard<std::mutex> l(g_handles_lock);
  return h && s.count(h) != 0;
}

// Module descriptor flags (+0x28) of the modules the two built-in racks list: bit0 keeps the voice
// alive until the module reports its end (Player 0x696E0 = 1; Envelope 0x677D0 = 2; Filter 0x67EF0 = 4;
// the others 0).
uint32_t descriptor_flags(uint8_t id) {
  switch (id) {
    case kModPlayer: return 1;
    case kModEnvelope: return 2;
    case kModFilter: return 4;
    default: return 0;
  }
}

// One float buffer per (output set, channel slot), 32-byte aligned as the rack's channel works.
void alloc_slot_buffers(Rack& r, uint32_t grain) {
  r.slot_buffers.assign(size_t(r.num_outputs) * r.slots.count, std::vector<float>(size_t(grain) + 8, 0.0f));
}

}  // namespace

// sub_43D0: the first free slot from the rotating index, then from 0; marks it used.
int32_t SlotAllocator::alloc_one() {
  const uint32_t start = next;
  for (uint32_t i = start; i < count; ++i) {
    uint32_t& w = bits[i >> 5];
    const uint32_t b = 1u << (i & 31);
    if (w & b) continue;
    w |= b;
    next = i >= count - 1 ? 0 : i + 1;
    ++used;
    return static_cast<int32_t>(i);
  }
  for (uint32_t i = 0; i < start; ++i) {
    uint32_t& w = bits[i >> 5];
    const uint32_t b = 1u << (i & 31);
    if (w & b) continue;
    w |= b;
    next = i >= count - 1 ? 0 : i + 1;
    ++used;
    return static_cast<int32_t>(i);
  }
  return kErrNoSlot;
}

// sub_4680 (one slot).
void SlotAllocator::free_one(uint16_t slot) {
  if (slot >= count) return;
  bits[slot >> 5] &= ~(1u << (slot & 31));
  --used;
}

namespace {

// sub_45B0(alloc, map, n): n slots into map[0..n); on failure the slots taken are released.
int32_t alloc_slots(SlotAllocator& a, uint16_t* map, uint32_t n) {
  for (uint32_t k = 0; k < n; ++k) {
    const int32_t s = a.alloc_one();
    if (s < 0) {
      for (uint32_t j = 0; j < k; ++j) a.free_one(map[j]);
      return s;
    }
    map[k] = static_cast<uint16_t>(s);
  }
  return 0;
}

// --- module parameters (group 0) -----------------------------------------------------------------

int module_slot_of(const Rack& r, uint8_t id) {
  for (size_t i = 0; i < r.modules.size(); ++i)
    if (r.modules[i].id == id) return static_cast<int>(i);
  return -1;
}

// The committed parameters of slot `s` <- pending (commit of group 0, 0x13D70 case 0).
void commit_module(Voice& v, uint32_t s) {
  const uint8_t id = v.rack->modules[s].id;
  switch (id) {
    case kModPlayer:
      if (v.player_pending) {
        if (!v.player_params) v.player_params.reset(player_new_params());
        player_copy_params(*v.player_params, *v.player_pending);
      }
      break;
    case kModChannelConverter: v.cc_params = v.cc_pending; break;
    case kModOutput: v.out_params = v.out_pending; break;
    default: break;
  }
}

// sub_222C0(voice, slot + 5, 0, 1): the first write of a module's parameters since the voice setup
// starts from the rack's defaults (and resets its state header, sub_B970); marks the slot's bit
// set (+296) and changed (+304).
void touch_module(Voice& v, uint32_t s) {
  const uint32_t bit = 1u << s;
  if (!(v.g[0].dirty & bit)) {
    switch (v.rack->modules[s].id) {
      case kModPlayer:
        if (!v.player_pending) v.player_pending.reset(player_new_params());
        player_reset_params(*v.player_pending);
        break;
      case kModChannelConverter: cc_reset_params(v.cc_pending); break;
      case kModOutput: v.out_pending = 0; break;
      default: break;
    }
  }
  v.g[0].pending |= bit;
  v.g[0].dirty |= bit;
}

// sub_225C0: a module-addressed command (byte 2 = module id, byte 3 = slot, low 14 bits = param).
int32_t apply_module_command(Voice& v, const VoiceCommand& c) {
  const uint32_t s = c.id >> 24;
  const uint8_t id = static_cast<uint8_t>(c.id >> 16);
  if (s >= v.rack->modules.size() || v.rack->modules[s].id != id) return kErrCommand;  // sub_221F0
  touch_module(v, s);
  const uint32_t param = c.id & 0x3FFF;
  switch (id) {
    case kModPlayer: {
      const int32_t r = player_set_param(*v.player_pending, c);
      // (+24 & ~1) == 0x42 (waveformFormat): the setup channel bytes take effect (0x2262A).
      if (r >= 0 && param == 0 && v.pend_slots != 0) {
        v.setup_in = v.pend_in;
        v.setup_ch = v.pend_ch;
        v.setup_slots = v.pend_slots;
        v.pend_in = v.pend_ch = 0;
        v.pend_slots = 0;
      }
      return r;
    }
    case kModChannelConverter:
      if (param == 0) v.cc_pending.num_in = c.value.u32;
      else if (param == 1) v.cc_pending.num_out = c.value.u32;
      else note_once("ChannelConverter matrix parameters not reproduced");
      return 0;
    case kModOutput:
      if (param == 1 || param == 0) v.out_pending = c.value.u32;  // 'outputId'
      return 0;
    default:
      note_once("parameters of effect module " + std::to_string(id) + " (its DSP is not reproduced)");
      return 0;
  }
}

// --- validation (sub_20920) and application (sub_212B0) of one command -----------------------------

int32_t validate_command(Voice& v, const VoiceCommand& c) {
  if (c.id & 0xFF0000) {  // module command: sub_221F0 lookup, sub_9AB0 value checks
    const uint32_t s = c.id >> 24;
    const uint8_t id = static_cast<uint8_t>(c.id >> 16);
    if (s >= v.rack->modules.size() || v.rack->modules[s].id != id) return kErrCommand;
    if (id == kModPlayer && (c.id & 0x3FFF) == 0 && c.value.ptr) {  // the format's channels (0x209BD)
      const uint32_t ch = static_cast<const uint32_t*>(c.value.ptr)[1];
      const uint8_t a = static_cast<uint8_t>(ch), b = static_cast<uint8_t>(ch >> 8);
      if (a > v.rack->max_channels || (b && b > v.rack->max_channels)) return kErrChannels;
      v.pend_in = b ? b : a;
      v.pend_ch = a;
      v.pend_slots = std::max<uint8_t>({v.pend_in, a, v.pend_slots});
    }
    return 0;
  }
  switch (c.id & 0xFFFFFF) {
    case 0:
      return 0;
    case 1: {  // setup {channels, flags}
      if (!c.value.ptr) return kErrNullArg;
      if ((c.flags & 0xE) >= 6) return E(0x804A8352);
      const uint32_t ch = static_cast<const uint32_t*>(c.value.ptr)[0];
      if (ch) {
        const uint8_t a = static_cast<uint8_t>(ch), b = static_cast<uint8_t>(ch >> 8);
        if (a > v.rack->max_channels || (b && b > v.rack->max_channels)) return kErrChannels;
        v.pend_in = b ? b : a;
        v.pend_ch = a;
        v.pend_slots = std::max<uint8_t>({v.pend_in, a, v.pend_slots});
      }
      v.v152 = -1;
      return 0;
    }
    case 2: {  // event
      const uint32_t ev = c.value.u32;
      if (ev > 32) return kErrEvent;
      if ((0x100010114ull >> ev) & 1) return 0;  // 2, 4, 8, 16, 32
      if (ev != 1) return kErrEvent;
      // PLAY: reserve the channel slots of the voice (0x209F2).
      uint8_t need = v.pend_slots ? v.pend_slots : v.setup_slots;
      if (!need) return kErrNoChannels;
      if (v.reserved_slots) {
        for (uint32_t k = 0; k < v.reserved_slots; ++k) v.rack->slots.free_one(v.slot_map[v.slots + k]);
        v.reserved_slots = 0;
      }
      if (need > v.slots) {
        const int32_t r = alloc_slots(v.rack->slots, &v.slot_map[v.slots], need - v.slots);
        if (r < 0) return E(0x804A8350);
      }
      v.reserved_slots = need;
      return 0;
    }
    case 6:  // port volume
      if ((c.id >> 24) >= v.rack->num_ports) return kErrPort;
      return 0;
    case 7:
      if ((c.id >> 24) >= v.rack->num_ports) return kErrPort;
      return c.value.u32 + 1 < 0x15 ? 0 : E(0x804A8355);
    case 8:
      if ((c.id >> 24) >= v.rack->num_ports) return kErrPort;
      return c.value.u32 + 1 < 0x21 ? 0 : E(0x804A8410);
    case 9:
      if ((c.id >> 24) >= v.rack->num_ports) return kErrPort;
      return 0;
    default:
      note_once("voice command " + std::to_string(c.id & 0xFFFFFF) + " not reproduced");
      return kErrCommand;
  }
}

// Port group (2) entry first written since setup starts from the defaults (rack +1040: volume 1.0,
// matrixId -1, pannerId -1), then marks the port set and changed.
PortParams& touch_port(Voice& v, uint32_t port) {
  const uint32_t bit = 1u << port;
  if (!(v.g[2].dirty & bit)) v.port_pending[port] = PortParams{};
  v.g[2].pending |= bit;
  v.g[2].dirty |= bit;
  return v.port_pending[port];
}

int32_t apply_command(Voice& v, const VoiceCommand& c) {
  if (c.id & 0xFF0000) return apply_module_command(v, c);
  switch (c.id & 0xFFFFFF) {
    case 0:
      return 0;  // user data (+56)
    case 1: {    // setup (0x2131C)
      const auto* p = static_cast<const uint32_t*>(c.value.ptr);
      v.flags.fetch_or(0x40000000);
      if (p[0] != 0 && v.pend_slots != 0) {
        v.setup_in = v.pend_in;
        v.setup_ch = v.pend_ch;
        v.setup_slots = v.pend_slots;
        v.pend_in = v.pend_ch = 0;
        v.pend_slots = 0;
      }
      v.setup_flags4 = static_cast<uint8_t>(p[1] & 0xF);
      v.init_dirty = 3;
      v.setup_flags = p[1];
      for (auto& g : v.g) g = Voice::Group{};
      // rack +0x150.. (module state masks) and +0x180.. (order) are taken again at PLAY.
      v.v152 = 0;
      return 0;
    }
    case 2: {  // event (0x2139A)
      const uint32_t ev = c.value.u32;
      if (ev == 1) {
        if (v.slots == 0) ++v.rack->active;
        if (v.slots > v.reserved_slots)
          for (uint32_t k = v.reserved_slots; k < v.slots; ++k) v.rack->slots.free_one(v.slot_map[k]);
        v.slots = v.reserved_slots;
        v.reserved_slots = 0;
        uint32_t f = v.flags.load();
        f = (ev << 8) | (f & 0xDD000000u) | 0x20000001u;
        v.flags.store(f);
        return 0;
      }
      if ((v.flags.load() & 1) == 0) return 0;  // not in use
      switch (ev) {
        case 2: v.flags.fetch_or(0x200); return 0;
        case 4: v.flags.fetch_or(0x400); return 0;
        case 8: {  // kill (0x21A1C)
          uint32_t f = v.flags.load();
          if (f & 0x20000000) f = (f & 0xDDFFF6FFu) | 0x2000800u;
          else f |= 0x2000800u;
          v.flags.store(f);
          if (v.slots) {
            for (uint32_t k = 0; k < v.slots; ++k) v.rack->slots.free_one(v.slot_map[k]);
            v.slots = 0;
            --v.rack->active;
          }
          return 0;
        }
        case 16: v.flags.store((v.flags.load() & 0xFFFFCFFFu) | 0x1000); return 0;
        case 32: v.flags.store((v.flags.load() & 0xFFFFCFFFu) | 0x2000); return 0;
        default: return kErrEvent;
      }
    }
    case 6: {  // port volume (0x21C5A): max(0, min(32, v))
      const float x = c.value.f32;
      const float m = x < 32.0f ? x : 32.0f;  // vminss dword_4A358, x
      touch_port(v, c.id >> 24).volume = x < 0.0f ? 0.0f : m;
      return 0;
    }
    case 7: touch_port(v, c.id >> 24).matrix_id = static_cast<int8_t>(c.value.u32); return 0;
    case 8: touch_port(v, c.id >> 24).panner_id = static_cast<int8_t>(c.value.u32); return 0;
    case 9: {  // patch (0x21D9F)
      const uint32_t port = c.id >> 24;
      const uint32_t bit = 1u << port;
      if (!(v.g[3].dirty & bit)) v.patch_pending[port] = PatchEntry{};
      v.g[3].pending |= bit;
      v.g[3].dirty |= bit;
      const auto* p = static_cast<const uint8_t*>(c.value.ptr);
      uint32_t type;
      std::memcpy(&type, p, 4);
      PatchEntry& e = v.patch_pending[port];
      if (type == 0) {
        e = PatchEntry{};
        return 0;
      }
      if (type != 1) {
        note_once("patches to render buffers / callbacks not reproduced");
        return 0;
      }
      VoiceHandle h;
      uint32_t input;
      std::memcpy(&h, p + 8, 8);
      std::memcpy(&input, p + 16, 4);
      if (!live(g_voices, h)) return 0;  // sub_3BB0 fails: the entry stays unpatched
      Voice* d = static_cast<Voice*>(h);
      if (d->rack->system != v.rack->system || input >= d->rack->num_inputs) return 0;
      e.type = 1;
      e.dest = h;
      e.dest_rack = d->rack->handle_id;
      e.dest_index = d->index;
      e.input = static_cast<uint8_t>(input);
      return 0;
    }
    default:
      return kErrCommand;
  }
}

}  // namespace

// --- API ---------------------------------------------------------------------------------------------

int32_t SystemResetOption(SystemOption* o) {  // 0x1D090: the 0x90-byte template at 0x54800
  if (!o) return kErrNullArg;
  std::memset(o, 0, sizeof *o);
  o->size = 0x90;
  o->max_grain_samples = 0x200;
  o->num_grain_samples = 0x100;
  o->sample_rate = 48000;
  o->max_channels = 8;
  const auto b = module_bytes(0x54800, 0x90);
  if (b.size() == 0x90) std::memcpy(o, b.data(), 0x90);  // the template as the dump has it
  return 0;
}

int32_t SystemCreateWithAllocator(const SystemOption* o, const BufferAllocator* a, SystemHandle* out) {
  if (!a || !a->alloc_handler) return kErrNoAllocator;
  if (!out) return kErrNullArg;
  SystemOption def;
  if (!o) {
    SystemResetOption(&def);
    o = &def;
  }
  if (o->size != 0x90) return kErrOptionSize;
  const uint32_t mg = o->max_grain_samples, g = o->num_grain_samples;
  if (mg - 64 > 0x7C0 || (mg & 0x3F)) return kErrGrainMax;
  if (mg < g || g - 64 > 0x7C0 || (g & 0x3F)) return kErrGrain;
  switch (o->sample_rate) {
    case 11025: case 12000: case 22050: case 24000: case 44100: case 48000: case 88200: case 96000: case 176400:
    case 192000: break;
    default: return kErrSampleRate;
  }
  if (o->max_channels - 1 > 0x24) return kErrChannels;
  // HOST: the system lives on the host heap; the caller's allocator (the eboot's malloc / free
  // handlers 0x90E00 / 0x90E30) is not asked for the context buffer.
  auto* s = new System;
  s->max_grain = mg;
  s->grain = g;
  s->sample_rate = o->sample_rate;
  s->max_channels = o->max_channels;
  s->allocator = *a;
  {
    std::lock_guard<std::mutex> l(g_handles_lock);
    g_systems.insert(s);
  }
  *out = s;
  return 0;
}

int32_t SystemDestroy(SystemHandle h, ContextBufferInfo*) {  // 0x1D410
  if (!live(g_systems, h)) return kErrSystemHandle;
  auto* s = static_cast<System*>(h);
  {
    std::lock_guard<std::mutex> l(g_handles_lock);
    g_systems.erase(s);
    for (auto& r : s->owned) {
      g_racks.erase(r.get());
      for (auto& v : r->voices) g_voices.erase(v.get());
    }
  }
  delete s;
  return 0;
}

int32_t RackCreateWithAllocator(SystemHandle sh, uint32_t id, const void* option, const BufferAllocator* a,
                                RackHandle* out) {
  if (!a || !a->alloc_handler) return kErrNoAllocator;
  if (!out) return kErrNullArg;
  if (!live(g_systems, sh)) return kErrSystemHandle;
  auto* s = static_cast<System*>(sh);
  std::lock_guard<std::recursive_mutex> lk(s->lock);
  const auto* o = static_cast<const uint8_t*>(option);
  if (!o) {
    note_once("racks created without an option (defaults) not reproduced");
    return kErrNullArg;
  }
  auto u32 = [&](size_t off) {
    uint32_t v;
    std::memcpy(&v, o + off, 4);
    return v;
  };
  uint64_t size;
  std::memcpy(&size, o, 8);
  auto r = std::make_unique<Rack>();
  r->system = s;
  r->id = id;
  r->handle_id = static_cast<uint32_t>(s->owned.size()) + 1;
  // Common rack option checks (0x102C0): grain +76, voices +80, +88 / +92 < 33, channels +96 / +100.
  const uint32_t grain = u32(76), voices = u32(80);
  if (grain - 64 > 0x7C0 || (grain & 0x3F)) return E(0x804A8311);
  if (voices - 1 >= 0x8000) return E(0x804A8309);
  if (u32(92) >= 0x21 || u32(88) >= 0x21) return E(0x804A830E);
  r->max_channels = static_cast<uint8_t>(u32(96) ? u32(96) : 8);
  if (u32(96) >= 0x26 || u32(100) >= 0x26) return E(0x804A8052);
  r->max_voices = voices;
  if (id == kRackSampler) {
    if (size != 216) return kErrOptionSize;
    if (u32(184) - 1 >= 0x20) return E(0x804A8053);    // -2142601133
    if (u32(188) - 1 >= 0x20) return E(0x804A8070);    // -2142601104
    if (u32(208) - 1 >= 0x100) return E(0x804A8092);   // -2142601070
    if (u32(176) - 1 >= 0x8000) return E(0x804A830A);  // -2142600438
    if (u32(92) == 0) return E(0x804A830E);
    if (u32(88) == 0) return E(0x804A830F);
    const uint32_t filters = u32(192);
    if (filters >= 0x11) return E(0x804A8061);
    if (u32(204) != 0) note_once("sampler racks with flag +0xCC (AJM decoder class 1) not reproduced");
    r->slots.count = u32(176);     // channel works
    r->num_ports = static_cast<uint8_t>(u32(92));
    r->num_inputs = 0;
    r->voice_type = 1;             // no inputs: list +344
    r->modules = {{kModPlayer, true}, {kModEnvelope, false}, {kModDistortion, false}, {kModUserFx, false},
                  {kModPeakMeter, false}};
    for (uint32_t i = 0; i < filters; ++i) r->modules.push_back({kModFilter, false});
    r->order.push_back(0);  // 0x1132A: 0, filters 5.., 1, 2, 3, 4
    for (uint32_t i = 0; i < filters; ++i) r->order.push_back(static_cast<uint8_t>(5 + i));
    for (uint8_t i = 1; i <= 4; ++i) r->order.push_back(i);
  } else if (id == kRackMastering) {
    if (size != 184) return kErrOptionSize;
    const uint32_t ch = u32(176);
    if (ch - 1 >= 0x25) return E(0x804A8052);
    if (ch * voices - 1 >= 0x8000) return E(0x804A830A);
    if (u32(92) != 0) return E(0x804A830E);
    if (u32(88) != 0) return E(0x804A830F);
    r->slots.count = ch * voices;
    r->num_ports = 0;
    r->num_inputs = 1;
    r->voice_type = 2;             // inputs: list +368 (dependency ordered)
    r->modules = {{kModPeakMeter, false}, {kModChannelConverter, false}, {kModLfeFilter, false},
                  {kModCompressor, false}, {kModGain, false}, {kModPeakMeter, false}, {kModOutput, false}};
    for (uint8_t i = 0; i < 7; ++i) r->order.push_back(i);
    r->max_channels = static_cast<uint8_t>(ch);
  } else {
    note_once("rack id " + std::to_string(id) + " not reproduced (reverb / submixer / custom racks)");
    return kErrRackId;
  }
  for (size_t i = 0; i < r->modules.size(); ++i)
    if (descriptor_flags(r->modules[i].id) & 1) r->keep_alive |= 1u << i;
  r->num_modules = static_cast<uint8_t>(r->modules.size());
  r->slots.bits.assign((r->slots.count + 31) / 32, 0);
  alloc_slot_buffers(*r, s->max_grain);
  r->ramp.assign(size_t(r->slots.count) * std::max<uint8_t>(r->num_ports, 1), {});  // zeroed (sub_4000 clear)
  r->voices.resize(voices);
  for (uint32_t i = 0; i < voices; ++i) {
    auto v = std::make_unique<Voice>();
    v->rack = r.get();
    v->index = static_cast<uint16_t>(i);
    v->type = static_cast<uint8_t>(r->voice_type);
    v->port_pending.resize(r->num_ports);
    v->port_params.resize(r->num_ports);
    v->patch_pending.resize(r->num_ports);
    v->patch_params.resize(r->num_ports);
    v->inputs.resize(r->num_inputs);
    v->player_state.reset(player_new_state());
    cc_reset_params(v->cc_pending);
    cc_reset_params(v->cc_params);
    r->voices[i] = std::move(v);
  }
  Rack* rp = r.get();
  {
    std::lock_guard<std::mutex> l(g_handles_lock);
    g_racks.insert(rp);
    for (auto& v : rp->voices) g_voices.insert(v.get());
  }
  s->racks.push_back(rp);
  s->owned.push_back(std::move(r));
  *out = rp;
  return 0;
}

int32_t RackDestroy(RackHandle h, ContextBufferInfo*) {  // 0xF110
  if (!live(g_racks, h)) return kErrRackHandle;
  auto* r = static_cast<Rack*>(h);
  System* s = r->system;
  std::lock_guard<std::recursive_mutex> lk(s->lock);
  {
    std::lock_guard<std::mutex> l(g_handles_lock);
    g_racks.erase(r);
    for (auto& v : r->voices) g_voices.erase(v.get());
  }
  for (auto& list : s->lists)
    list.erase(std::remove_if(list.begin(), list.end(), [&](Voice* v) { return v->rack == r; }), list.end());
  for (auto& rk : s->owned)  // patches from or into this rack's voices
    for (auto& v : rk->voices)
      for (auto& in : v->inputs)
        in.erase(std::remove_if(in.begin(), in.end(), [&](auto& p) { return p.first->rack == r; }), in.end());
  s->racks.erase(std::remove(s->racks.begin(), s->racks.end(), r), s->racks.end());
  s->owned.erase(std::remove_if(s->owned.begin(), s->owned.end(), [&](auto& p) { return p.get() == r; }),
                 s->owned.end());
  return 0;
}

int32_t RackGetVoiceHandle(RackHandle h, uint32_t index, VoiceHandle* out) {  // 0xFBD0
  if (!out) return kErrNullArg;
  if (!live(g_racks, h)) return kErrRackHandle;
  auto* r = static_cast<Rack*>(h);
  if (index >= r->voices.size()) return kErrVoiceIndex;
  *out = r->voices[index].get();
  return 0;
}

int32_t VoiceRunCommands(VoiceHandle h, const VoiceCommand* cmds, uint32_t n) {  // 0x20250
  if (!live(g_voices, h)) return kErrVoiceHandle;
  auto* v = static_cast<Voice*>(h);
  std::lock_guard<std::recursive_mutex> lk(v->rack->system->lock);
  for (uint32_t i = 0; i < n; ++i) {
    const int32_t r = validate_command(*v, cmds[i]);
    if (r < 0) return r;  // sub_210C0 rolls back the earlier validations (slot reservations)
  }
  for (uint32_t i = 0; i < n; ++i) {
    const int32_t r = apply_command(*v, cmds[i]);
    if (r < 0) return r;
  }
  v->active_listed = true;  // rack +10320: the voice is visited by the next render's 0x13D70
  return 0;
}

int32_t VoiceGetStateFlags(VoiceHandle h, uint32_t* out) {  // 0x20690 -> 0x1F590 query 0x4003
  if (!out) return kErrNullArg;
  if (!live(g_voices, h)) return kErrVoiceHandle;
  auto* v = static_cast<Voice*>(h);
  std::lock_guard<std::recursive_mutex> lk(v->rack->system->lock);
  const uint32_t f = v->flags.load();
  *out = (f & 0x20000000) ? 1u : (f & 0xFF);
  return 0;
}

// sceNgs2VoiceControl (0x227B0): each parameter block becomes internal commands run through
// VoiceRunCommands; the ids the eboot uses are reproduced.
int32_t VoiceControl(VoiceHandle h, const VoiceParamHead* head) {
  if (!head) return kErrParam;
  if (!live(g_voices, h)) return kErrVoiceHandle;
  int32_t r = 0;
  for (const auto* p = head;; p = reinterpret_cast<const VoiceParamHead*>(reinterpret_cast<const uint8_t*>(p) + p->next)) {
    const auto* b = reinterpret_cast<const uint8_t*>(p);
    auto u32 = [&](size_t off) {
      uint32_t x;
      std::memcpy(&x, b + off, 4);
      return x;
    };
    VoiceCommand c[2]{};
    switch (p->id) {
      case 0x10000000:
      case 0x40010000: {  // sampler voice setup (0x2294B): {u32 numChannels +12, u32 flags +32}
        if (p->size < 0x28) return E(0x804A8361);
        const uint32_t setup[2] = {u32(12), u32(32)};
        c[0].id = 1;
        c[0].type = 0x20;
        c[0].value.ptr = setup;
        if (setup[0] == 0) {
          r = VoiceRunCommands(h, c, 1);
          break;
        }
        c[1].id = 0x00010000;  // Player slot 0, param 0 waveformFormat
        c[1].type = 0x42;
        c[1].value.ptr = b + 8;
        r = VoiceRunCommands(h, c, 2);
        break;
      }
      case 0x10000001:
      case 0x40010001: {  // waveform blocks: {u64 data +8, u32 flags +16, u32 numBlocks +20, blocks* +24}
        if (p->size < 0x20) return E(0x804A8361);
        uint64_t base;
        const uint8_t* blocks;
        std::memcpy(&base, b + 8, 8);
        std::memcpy(&blocks, b + 24, 8);
        const uint32_t flags = u32(16), count = u32(20);
        if (count > 0x20) return E(0x804A8254);
        if (count && !blocks) return E(0x804A8258);
        struct Entry {
          uint64_t data, size;
          uint32_t repeats, skip, samples, zero;
          uint64_t user;
        } e[32];
        for (uint32_t i = 0; i < count; ++i) {
          const uint8_t* q = blocks + 40 * i;
          uint64_t off, sz, user;
          uint32_t rep, skip, smp;
          std::memcpy(&off, q, 8);
          std::memcpy(&sz, q + 8, 8);
          std::memcpy(&rep, q + 16, 4);
          std::memcpy(&skip, q + 20, 4);
          std::memcpy(&smp, q + 24, 4);
          std::memcpy(&user, q + 32, 8);
          e[i] = {base + off, sz, (flags & 2) ? 0u : rep, (flags & 2) ? 0u : skip, (flags & 2) ? 0u : smp, 0, user};
        }
        c[0].id = 0x00010001;  // Player slot 0, param 1 waveformBlocks
        c[0].flags = static_cast<uint8_t>(flags);
        c[0].type = 0x81;
        c[0].count = static_cast<uint16_t>(count);
        c[0].value.ptr = e;
        r = VoiceRunCommands(h, c, 1);
        break;
      }
      case 0x30000000: {  // mastering voice setup (LABEL_66): {u32 numChannels +8, u32 flags +12}
        if (p->size < 0x10) return E(0x804A8361);
        const uint32_t setup[2] = {u32(8), u32(12)};
        if (setup[0] - 1 >= static_cast<Voice*>(h)->rack->max_channels) return kErrChannels;  // query 0x400F
        VoiceCommand m[3]{};
        m[0].id = 1;
        m[0].type = 0x20;
        m[0].value.ptr = setup;
        m[1].id = 0x01220001;  // ChannelConverter (slot 1) param 1 = 0
        m[1].count = 0;
        m[1].type = 0x04;
        m[1].value.u64 = 0;
        m[2].id = 0x061A0001;  // Output (slot 6) param 1 'outputId' = 0
        m[2].type = 0x04;
        m[2].value.u64 = 0;
        r = VoiceRunCommands(h, m, 3);
        break;
      }
      case 2: {  // port volume {u32 port +8, float +12}
        c[0].id = (u32(8) << 24) | 6;
        c[0].type = 1;
        std::memcpy(&c[0].value.f32, b + 12, 4);
        r = VoiceRunCommands(h, c, 1);
        break;
      }
      case 5: {  // patch {u32 port +8, u32 destInputId +12, handle +16}
        if (p->size <= 0x17) return E(0x804A8361);
        struct {
          uint32_t type, pad;
          uint64_t handle;
          uint32_t input, pad2;
        } pp{1, 0, 0, u32(12), 0};
        std::memcpy(&pp.handle, b + 16, 8);
        c[0].id = (u32(8) << 24) | 9;
        c[0].type = 0x21;
        c[0].value.ptr = &pp;
        r = VoiceRunCommands(h, c, 1);
        break;
      }
      case 6: {  // kick event {u32 event +8}
        if (p->size <= 0xB) return E(0x804A8361);
        c[0].id = 2;
        c[0].flags = 0;
        c[0].type = 4;
        c[0].value.u32 = u32(8);
        r = VoiceRunCommands(h, c, 1);
        break;
      }
      default:
        note_once("sceNgs2VoiceControl parameter id " + std::to_string(p->id) + " not reproduced");
        return kErrParamId;
    }
    if (r < 0 || p->next == 0) return r;
  }
}

// --- render ------------------------------------------------------------------------------------------

namespace {

// 0x13D70: per voice touched by commands since the last render: events (PLAY sets the voice up for
// rendering), then the parameter commit of the four groups.
void apply_events_and_commit(System& s, Voice& v) {
  Rack& rk = *v.rack;
  uint32_t f = v.flags.load();
  const uint32_t ev = (f >> 8) & 0xFFFF;
  int32_t first = 0;  // v137
  bool commit = false;
  if (ev) {
    if (f & 0x100) {  // PLAY (0x13E87)
      for (auto& in : v.inputs) in.clear();  // unlink patches into this voice (sub_13BF0)
      // unlink this voice's patches from their destinations
      for (auto& rr : s.owned)
        for (auto& dv : rr->voices)
          for (auto& in : dv->inputs)
            in.erase(std::remove_if(in.begin(), in.end(), [&](auto& p) { return p.first == &v; }), in.end());
      for (auto& list : s.lists) list.erase(std::remove(list.begin(), list.end(), &v), list.end());
      v.run_flags = v.setup_flags;
      v.status = 1;
      v.vstate = 0;
      v.error = 0;
      v.in_ch = v.setup_in;
      v.out_ch = v.setup_ch;
      v.num_slots = v.slots;
      v.patch_mask = 0;
      v.output_mask = 0;
      v.alive_mask = rk.keep_alive;
      v.shift = 0;  // byte_52550[6 * rate index + (setup flags & 15)], 0 at 48 kHz, capped by rack +315
      v.grain_div = s.grain >= rk.system->max_grain ? 0 : static_cast<uint8_t>(rk.system->max_grain / s.grain);
      if (v.grain_div) note_once("racks with a grain larger than the system grain not reproduced");
      v.grain_phase = 0;
      v.v564 = 0;
      v.run_map = v.slot_map;
      for (auto& g : v.g) {
        g.now = 0;
        g.committed = 0;
        g.first = 0;
      }
      v.list = static_cast<uint8_t>(v.type);
      s.lists[v.type].push_back(&v);  // list +344 (type 1) / +368 (type 2, dependency ordered)
      first = -1;
      // Host diagnostic: the grain at which the voice starts (render index, 0-based).
      log::info("ngs2", "rack {:#x} voice {} starts in render {}", rk.id, v.index, s.renders);
    }
    uint8_t st = v.vstate;
    uint8_t status = v.status;
    uint32_t evb = ev;
    if (f & 0x1000) {  // pause (0x14099)
      if (status & 4) {
        evb &= 0xFFEF;
      } else {
        st = 1;  // fade out; the pause itself is taken at the end of the render (0x135A0)
        if (f & 0x100) {
          status |= 4;
          st = 3;
          evb &= 0xFFEF;
        }
      }
    }
    if (evb & 0x20) {  // resume
      if (status & 4) {
        status &= 0xFB;
        st = 2;
      } else {
        evb &= 0xFFDF;
      }
    }
    if (evb & 2) st = 4;    // stop
    if (evb & 0xC) st = 5;  // kill
    v.events = static_cast<uint16_t>(evb);
    v.status = static_cast<uint8_t>(status | (4 * (evb & 2)));
    v.vstate = st;
    commit = (v.status & 1) != 0;
    if (f & 0x100) {
      // +168 bit 0 / 1: the rack's module state masks (+0x150) and order (+0x180) (0x14CBA).
      v.pass0 = (rk.modules.size() >= 32) ? ~0u : ((1u << rk.modules.size()) - 1);  // every module: state 0
      v.pass1 = 0;
      v.staged = 0;
      v.init_dirty = 0;
    }
  } else {
    commit = (v.status & 1) != 0;
  }
  if (commit) {  // LABEL_76: the four parameter groups
    for (uint32_t gi = 0; gi < 4; ++gi) {
      Voice::Group& g = v.g[gi];
      const uint32_t set = g.dirty, prev = g.committed;
      const uint32_t m = set & (static_cast<uint32_t>(first) | g.pending);
      if (!(m | (prev & ~set))) continue;
      g.first |= set & (static_cast<uint32_t>(first) | ~prev);
      g.committed = set;
      g.now |= m;
      g.pending = 0;
      for (uint32_t b = 0; b < 32; ++b) {
        if (!(m & (1u << b))) continue;
        if (gi == 0) {
          if (b < rk.modules.size()) commit_module(v, b);
        } else if (gi == 1) {
          note_once("voice matrices (group 1) not reproduced");
        } else if (gi == 2) {
          v.port_params[b] = v.port_pending[b];
        } else {
          v.patch_params[b] = v.patch_pending[b];
          const PatchEntry& e = v.patch_params[b];
          // sub_13BF0 drops the old link of the port; a voice patch links it into the
          // destination's input list (0x14B30).
          for (auto& rr : s.owned)
            for (auto& dv : rr->voices)
              for (auto& in : dv->inputs)
                in.erase(std::remove_if(in.begin(), in.end(), [&](auto& p) { return p.first == &v && p.second == b; }),
                         in.end());
          v.patch_mask &= ~(1u << b);
          v.output_mask &= ~(1u << b);
          if (e.type == 1 && live(g_voices, e.dest)) {
            Voice* d = static_cast<Voice*>(e.dest);
            if (e.input < d->inputs.size()) d->inputs[e.input].push_back({&v, b});
            v.patch_mask |= 1u << b;
          } else if (e.type == 2 || e.type == 3) {
            v.output_mask |= 1u << b;
          }
        }
      }
    }
  }
  // flags &= 0x3F0000FF: events and the play-pending bit consumed.
  v.flags.store(v.flags.load() & 0x3F0000FFu);
}

// sub_171F0 + the module's process function (descriptor +0xC8).
void run_module(System& s, Voice& v, uint32_t slot, float* const* bufs) {
  Rack& rk = *v.rack;
  const uint8_t id = rk.modules[slot].id;
  ModuleCtx c{};
  c.voice = &v;
  const uint32_t bit = 1u << slot;
  uint16_t fl = static_cast<uint16_t>(v.events | s.grain_changed);
  if (v.g[0].first & bit) fl |= 0x100;
  if (v.g[0].now & bit) fl |= 0x200;
  c.flags = fl;
  c.status = v.status;
  c.num_channels = v.num_slots;
  c.num_samples = s.grain;
  c.sample_rate = s.sample_rate;
  c.in = bufs;
  c.out = bufs;
  int32_t ret = 0;
  switch (id) {
    case kModPlayer: ret = player_process(c); break;
    case kModChannelConverter: {
      // sub_15B30: the channel count of the render buffer the voice's Output module targets.
      uint32_t lim = c.num_channels;
      const int os = module_slot_of(rk, kModOutput);
      if (os >= 0 && (v.g[0].committed & (1u << os)) && v.out_params < s.num_buffers && s.buffers)
        lim = s.buffers[v.out_params].num_channels;
      channel_converter(c, v.cc_params, v.cc_state, lim);
      break;
    }
    case kModOutput: output_module(c, v.out_params); break;
    default:
      note_once("DSP of effect module " + std::to_string(id) + " not reproduced");
      break;
  }
  // Result bits (0x172F6).
  if (ret < 0) {
    v.status = static_cast<uint8_t>((v.status & 0xED) | 0x10);
    v.error = ret;
  } else if (ret) {
    if (ret & 1) v.status |= 2;
    v.status = static_cast<uint8_t>((v.status & 0x5F) | ((4 * ret) & 0x20) | ((8 * ret) & 0x80));
    if (ret & 2) v.events &= ~2;
    if (ret & 4) {
      const bool last = (v.alive_mask & ~bit) == 0;
      v.alive_mask &= ~bit;
      if (last || (descriptor_flags(id) & 2)) v.status = 0;
    }
  }
  v.g[0].now &= ~bit;
  v.g[0].first &= ~bit;
}

// sub_17620: the output port `port` of source voice `src` mixed into `dst_bufs` (dest channels n_out).
void mix_patch(System& s, Voice& src, uint32_t port, float* const* dst_bufs, uint32_t n_out, uint64_t& written) {
  if (src.flags.load() & 0x2000000) return;  // killed: nothing (0x17661)
  Rack& sr = *src.rack;
  const uint32_t n = s.grain;
  const uint32_t n_in = src.out_ch;  // +525
  float* in[38];
  for (uint32_t c = 0; c < src.num_slots && c < 38; ++c) in[c] = sr.buffer(0, src.run_map[c]);  // set of the port (+10304)
  if (src.mod_stage_ports & (1u << port)) note_once("modules staged on a voice port not reproduced");
  // Port parameters: committed (+348) -> level / matrixId / pannerId; else 1.0, -1, -1.
  float level = 1.0f;  // dword_4A278
  int32_t matrix = -1, panner = -1;
  if (src.g[2].committed & (1u << port)) {
    level = src.port_params[port].volume;
    matrix = src.port_params[port].matrix_id;
    panner = src.port_params[port].panner_id;
    if (static_cast<uint32_t>(matrix) <= 0xF && (src.g[1].committed & (1u << matrix)))
      note_once("user voice matrices not reproduced");
  }
  const float* m = nullptr;
  if (n_in <= 8 && n_out <= 8 && panner < 0) {  // 0x17E1B: the default matrix of kind (matrixId 16..19) or 0
    const uint32_t kind = ((static_cast<uint32_t>(matrix) & ~3u) == 16) ? std::min<uint32_t>(matrix - 16, 4) : 0;
    m = default_matrix(kind, n_in, n_out);
  }
  const uint32_t rows = n_out;  // 0x17ED7: (numIn * numOut) / numIn, capped by the destination's channels
  const bool first = (src.g[3].first & (1u << port)) != 0;  // v124: freshly patched -> no ramp
  const float* row = m;
  for (uint32_t o = 0; o < rows; ++o) {
    for (uint32_t i = 0; i < n_in; ++i) {
      float g = m ? level * row[i] : (o == i ? level : 0.0f);
      auto& rs = sr.ramp[size_t(src.run_map[i]) * std::max<uint8_t>(sr.num_ports, 1) + port];
      const float prev = first ? g : rs[o];
      rs[o] = g;
      if (g == 0.0f && prev == 0.0f) continue;
      if (src.status & 0x40) continue;  // silent voice
      if (written & (1ull << o)) accumulate_gain(in[i], dst_bufs[o], n, prev, g);
      else {
        write_gain(in[i], dst_bufs[o], n, prev, g);
        written |= 1ull << o;
      }
    }
    if (row) row += n_in;
  }
}

// sub_15DE0: one voice of the render.
void process_voice(System& s, Voice& v) {
  Rack& rk = *v.rack;
  const uint32_t n = s.grain;
  float* bufs[38];
  for (uint32_t c = 0; c < v.num_slots && c < 38; ++c) bufs[c] = rk.buffer(0, v.run_map[c]);
  // Inputs (0x16090): every voice patched into input k mixes into the voice's channel set k.
  if (rk.num_inputs) {
    uint64_t written = 0;
    for (auto& [src, port] : v.inputs[0]) mix_patch(s, *src, port, bufs, v.in_ch, written);
    if (rk.num_inputs > 1) note_once("voices with more than one input not reproduced");
    if (!(v.status & 0x40))
      for (uint32_t c = 0; c < v.num_slots; ++c)
        if (!(written & (1ull << c))) clear_samples(bufs[c], n);  // sub_4F80 over the voice's slots (+150)
  }
  // Module passes (0x165F0): committed modules in state 0, then state 1, in the rack's order.
  if (!v.grain_phase && !(v.status & 0x40)) {
    for (uint32_t pass = 0; pass < 2; ++pass) {
      uint32_t mask = v.g[0].committed & (pass ? v.pass1 : v.pass0);
      for (uint8_t slot : rk.order) {
        if (!mask) break;
        if (!(mask & (1u << slot))) continue;
        mask &= ~(1u << slot);
        run_module(s, v, slot, bufs);
      }
    }
  }
  if (v.v564 & 4) note_once("voice silence detection (+564 bit 2) not reproduced");
  // Voice state fades (0x1675D): 1 pause (last 64 samples 1 -> 0), 2 resume (first 64 0 -> 1),
  // 3 paused (zero), 4 / 5 stop / kill (first 64 1 -> 0, then zero).
  switch (v.vstate) {
    case 1:
      for (uint32_t c = 0; c < v.num_slots; ++c) write_gain(bufs[c] + n - 64, bufs[c] + n - 64, 64, 1.0f, 0.0f);
      break;
    case 2:
      for (uint32_t c = 0; c < v.num_slots; ++c) write_gain(bufs[c], bufs[c], 64, 0.0f, 1.0f);
      break;
    case 3:
      for (uint32_t c = 0; c < v.num_slots; ++c) clear_samples(bufs[c], n);
      break;
    case 4:
    case 5:
      for (uint32_t c = 0; c < v.num_slots; ++c) {
        write_gain(bufs[c], bufs[c], 64, 1.0f, 0.0f);
        clear_samples(bufs[c] + 64, n - 64);
      }
      break;
    default:
      break;
  }
  if (v.output_mask) note_once("voice ports patched to render buffers / callbacks not reproduced");
}

// 0x135A0: the voice's status into the low byte of its flags; a voice whose status is 0 or that
// was stopped / killed is released.
void merge_status(System& s, Voice& v) {
  uint32_t f = v.flags.load();
  if (f == 0) return;
  if (((f & 0x40000000) && !(f & 0x10000000)) || (f & 0x100) || (f & 0xFF) == 0) return;
  uint8_t st = v.status;
  bool release = st == 0;
  if (!release) {
    if (!(st & 0x44) && v.alive_mask == 0) st |= 2;
    if (!(v.grain_phase | (st & 0x40))) {
      uint16_t e = v.events;
      if (e & 0x10) {
        st = static_cast<uint8_t>((st & 0xF9) | 4);
        v.vstate = 3;
      }
      if (e & 0x20) v.vstate = 0;
      if (e & 0xE) release = true;
    }
  }
  if (release) {  // LABEL_40 / 41: sub_1F3B0 frees the channel slots, sub_13AA0(voice, 0) unlinks its ports
    st = 0;
    if (v.slots) {
      for (uint32_t k = 0; k < v.slots; ++k) v.rack->slots.free_one(v.slot_map[k]);
      v.slots = 0;
      --v.rack->active;
    }
    for (auto& list : s.lists) list.erase(std::remove(list.begin(), list.end(), &v), list.end());
    for (auto& rr : s.owned)
      for (auto& dv : rr->voices)
        for (auto& in : dv->inputs)
          in.erase(std::remove_if(in.begin(), in.end(), [&](auto& p) { return p.first == &v; }), in.end());
    v.patch_mask = 0;
    v.output_mask = 0;
    log::info("ngs2", "rack {:#x} voice {} released after render {}", v.rack->id, v.index, s.renders);
  }
  v.events = 0;
  for (auto& g : v.g) {  // LABEL_47
    g.now = 0;
    g.first = 0;
    g.accum |= g.committed;
  }
  v.toggle ^= 1;
  if (v.grain_div) v.grain_phase = static_cast<uint8_t>((v.grain_phase + 1) % v.grain_div);
  v.status = st;
  v.flags.store((v.flags.load() & 0x4FFFFF00u) | st);
}

}  // namespace

int32_t SystemRender(SystemHandle h, const RenderBufferInfo* buffers, uint32_t count) {  // 0x1E460 -> 0x12EF0
  if (!live(g_systems, h)) return kErrSystemHandle;
  auto* s = static_cast<System*>(h);
  std::lock_guard<std::recursive_mutex> lk(s->lock);
  if (count > 31) return kErrRenderBuffers;
  for (uint32_t i = 0; i < count; ++i) {
    const uint32_t t = buffers[i].waveform_type;
    if (t > 0x19 || !((51118080u >> t) & 1)) return E(0x804A8050);  // -2142601136
  }
  s->buffers = buffers;
  s->num_buffers = count;
  s->written = 0;
  // +460 / +448 / +450: the rate index and the "grain / rate changed" flag of the first render.
  static constexpr uint32_t kRates[] = {12000, 24000, 48000, 96000, 192000};
  uint8_t ri = 0;
  for (uint8_t k = 0; k < 5; ++k)
    if (s->sample_rate == kRates[k]) ri = static_cast<uint8_t>(k + 1);
  s->grain_changed = 0;
  if (s->renders == 0 || ri != s->rate_index) {
    s->rate_index = ri;
    s->grain_changed = 0x200;
  }
  // 0x197C0: streams (none are created on this path). 0x13D70: events and commits.
  for (auto& r : s->owned)
    for (auto& v : r->voices)
      if (v->active_listed) {
        v->active_listed = false;
        apply_events_and_commit(*s, *v);
      }
  // 0x15050: list +344 (samplers) then +368 (mastering); module begin / end hooks of decoder class 0
  // (0x25AA0 / 0x25AB0) do nothing.
  std::vector<Voice*> order;
  for (uint32_t t : {0u, 1u, 2u})
    for (Voice* v : s->lists[t]) order.push_back(v);
  for (Voice* v : order) process_voice(*s, *v);
  for (Voice* v : order) merge_status(*s, *v);  // 0x135A0
  ++s->renders;
  // Render buffers nobody wrote are cleared (0x13340).
  for (uint32_t i = 0; i < count; ++i)
    if (!(s->written & (1u << i)) && buffers[i].buffer) std::memset(buffers[i].buffer, 0, buffers[i].buffer_size);
  s->buffers = nullptr;
  s->num_buffers = 0;
  return 0;
}

}  // namespace ssn::ngs2
