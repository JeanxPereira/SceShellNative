// The PSS audio system of libScePsm.sprx (FW 4.03) — see pss_audio.h.
//
// Reproduced: memory resources of the VAG format (HEVAG decoder, type 4) and of RIFF/WAVE
// with ATRAC9 data (ATRAC9 decoder, type 3, over libSceAudiodec — audiodec_atrac9.h), players
// on the main output port and on the BGM port (with its direct player), the stream buffers
// and the "ScePsP:AudioStream" decode thread with its request queue, the decoder's loop
// region handling, seeking (pending seeks of playing voices included), pause, the per-grain
// mixer, volume / pan / playback-rate (with their per-frame ramps and the stop fade),
// auto-rewind of finished voices, and the ports' drivers (host audio device,
// pss_audio_out.h).
// Not reproduced (each logged once when reached): RIFF PCM / MP3 / ADPCM resources and their
// decoders (types 0..2; no ShellUI sound uses them), the looping mixer of the main port
// (sub_162E80 / sub_1624A0, SoundPlayer.Loop = true) and pending seeks of main-port voices
// in the mixer, HEVAG seeks to a non-zero position (sub_191A50), file resources
// (sub_179CF0), the pad-speaker / personal / surround ports, a second voice on the BGM port
// while one is the direct player, and the AudioIn mixing path.
//
// Field names carry the firmware offsets of the objects they come from.
#include "libScePsm/audio/pss_audio.h"

#include <array>
#include <atomic>
#include <cmath>
#include <cstring>
#include <deque>
#include <mutex>
#include <thread>

#include "core/log.h"
#include "host/boundary.h"
#include "libScePsm/audio/atrac9_decoder.h"
#include "libScePsm/audio/hevag_decoder.h"
#include "libScePsm/audio/pss_audio_info.h"
#include "libScePsm/audio/pss_audio_out.h"
#include "libScePsm/audio/pss_decoder.h"
#include "libScePsm/audio/pss_mix_converters.h"
#include "libScePsm/graphics/core.h"

#if defined(__clang__)
#pragma clang fp contract(off)
#endif

namespace ssn::psm::audio {
namespace {

using namespace pss;

// sceKernelUsleep (import QcteRwbsnV0) / scePthreadYield (T72hz6ffq08).
void usleep_us(uint32_t us) { std::this_thread::sleep_for(std::chrono::microseconds(us)); }
void yield() { std::this_thread::yield(); }

// (u64) of a float/double the way the firmware converts (cvttsd2si with the 2^63 split).
uint64_t to_u64(double d) {
  if (d >= 9223372036854775808.0) return static_cast<uint64_t>(static_cast<int64_t>(d - 9223372036854775808.0)) ^ 0x8000000000000000ull;
  return static_cast<uint64_t>(static_cast<int64_t>(d));
}
uint64_t to_u64(float f) {
  if (f >= 9.22337204e18f) return static_cast<uint64_t>(static_cast<int64_t>(f - 9.22337204e18f)) ^ 0x8000000000000000ull;
  return static_cast<uint64_t>(static_cast<int64_t>(f));
}
// (float) / (double) of an unsigned 64-bit value (the shr/or/add sequence of the firmware).
float u64_to_f(uint64_t v) {
  if (static_cast<int64_t>(v) < 0) {
    const float h = static_cast<float>(static_cast<int64_t>((v >> 1) | (v & 1)));
    return h + h;
  }
  return static_cast<float>(static_cast<int64_t>(v));
}
double u64_to_d(uint64_t v) { return static_cast<double>(v); }  // the vpunpckldq/vsubpd sequence is exact

// xmmword_34E930: the WAVE_FORMAT_EXTENSIBLE sub-format GUID of ATRAC9
// {47E142D2-36BA-4D8D-88FC-61654F8C836C} as bytes.
constexpr uint8_t kAtrac9Guid[16] = {0xD2, 0x42, 0xE1, 0x47, 0xBA, 0x36, 0x8D, 0x4D,
                                     0x88, 0xFC, 0x61, 0x65, 0x4F, 0x8C, 0x83, 0x6C};

// ---------------------------------------------------------------------------------------
// Audio data object (760 bytes, constructor sub_1797C0, vtable off_49B510).
struct PssData {
  std::atomic<int32_t> refs{1};                       // +8
  uint32_t size = 0;                                  // +16
  const uint8_t* base = nullptr;                      // +24
  uint64_t pos = 0;                                   // +32
  std::shared_ptr<const std::vector<uint8_t>> image;  // keeps the bytes +24 points into
  PssAudioInfo info{};                                // +600 (+600 = type, +604 = storage)
  uint16_t gen = 1;                                   // +680
  int32_t free_on_last_voice = 0;                     // +684
  int32_t valid = 0;                                  // +688
  std::atomic<int32_t> releasing{0};                  // +692
  int32_t keep = 0;                                   // +696
  int32_t random_access = 0;                          // +728 (memory views: 1)

  // sub_179A50 / sub_17AD20 (memory branch): a view of [base, base + size) with the given
  // type; +688 set by sub_179AF0 (succeeds for a memory view with a base).
  void set_memory(std::shared_ptr<const std::vector<uint8_t>> img, const uint8_t* b, uint32_t n, uint32_t type) {
    valid = 0;
    image = std::move(img);
    info.w[0] = type;
    info.w[1] = 2;
    size = n;
    base = b;
    pos = 0;
    random_access = 1;
    valid = base != nullptr ? 1 : 0;
  }
  // sub_179EA0: usable (not releasing; a memory view needs its base).
  bool usable() const {
    if (releasing.load() != 0) return false;
    if (keep == 0 && info.storage() - 2 < 2 && base == nullptr) return false;
    return true;
  }
  // sub_179E20 (vtable+16): usable and still referenced -> one more reference.
  bool acquire() {
    if (!usable()) return false;
    if (refs.load() > 0) {
      refs.fetch_add(1);
      return true;
    }
    return false;
  }
  void release() { refs.fetch_sub(1); }  // sub_17AF40 (vtable+24); table objects are not deleted
  // sub_179E00: mark releasing and drop the table's reference.
  void mark_releasing() {
    releasing = 1;
    keep = 0;
    release();
  }
  bool can_free() const { return releasing.load() != 0 && refs.load() <= 0; }  // sub_179DE0
  // sub_1798B0: ++generation, cleared, reference count back to 1 (vtable+32 = sub_17AF80).
  void reset() {
    ++gen;
    valid = 0;
    image.reset();
    base = nullptr;
    size = 0;
    pos = 0;
    free_on_last_voice = 0;
    releasing = 0;
    keep = 0;
    random_access = 0;
    info = PssAudioInfo{};
    refs.exchange(1);
  }
  // sub_17A1D0, memory view, whence 0.
  uint32_t seek(uint64_t off) {
    if (size <= off) return kEnd;
    pos = off;
    return 0;
  }
  // sub_17A410, memory view, whence 0: the bytes from `at` to the end (the read cache
  // +568..+584 of file views stays unused for memory views).
  uint32_t read(uint64_t at, uint32_t len, const uint8_t** p, uint32_t* n) {
    *p = nullptr;
    if (!(size > at)) return kEnd;
    pos = at;
    const uint32_t avail = static_cast<uint32_t>(size - at);
    *n = avail;
    *p = base + at;
    pos = at + (avail < len ? avail : len);
    return 0;
  }
};

// ---------------------------------------------------------------------------------------
// Stream buffer (88 bytes, sub_16F6D0): a ring of `cap` bytes plus an overflow area so a
// writer / reader can always get `write_min` / `read_min` contiguous bytes.
struct PssRing {
  std::vector<uint8_t> mem;              // +0 (+12 bytes)
  uint32_t cap = 0;                      // +8
  uint32_t write_min = 0;                // +16
  uint32_t read_min = 0;                 // +20
  std::atomic<int32_t> fill{0};          // +24
  uint32_t wpos = 0;                     // +32
  uint32_t wavail = 0, woverflow = 0;    // +36, +40
  uint32_t rpos = 0;                     // +44
  uint32_t ravail = 0, rcopied = 0;      // +48, +52
  std::mutex wlock, rlock;               // +56, +64
  std::atomic<int32_t> state{0};         // +72: 0 new, 1 written, 2..4 reading, 5/6 flushed
  uint64_t read_total = 0;               // +80

  void init(uint32_t capacity, uint32_t wmin, uint32_t rmin) {  // sub_16F6D0
    cap = capacity;
    write_min = wmin;
    read_min = rmin;
    mem.assign(size_t(capacity) + (wmin > rmin ? wmin : rmin), 0);
    std::scoped_lock l(wlock, rlock);
    fill = 0;
    wpos = 0;
    wavail = cap;
    woverflow = rpos = ravail = rcopied = 0;
  }
  // The reset inlined at sub_15F310 / sub_171DC0 / sub_161EF0 / sub_160FC0.
  void reset() {
    std::scoped_lock l(wlock, rlock);
    fill = 0;
    wpos = 0;
    wavail = cap;
    woverflow = rpos = ravail = rcopied = 0;
  }
  // sub_165B90: writable contiguous bytes at the write position.
  uint8_t* write_ptr(uint32_t* avail) {
    uint32_t r;
    {
      std::lock_guard<std::mutex> l(rlock);
      r = rpos;
    }
    std::lock_guard<std::mutex> l(wlock);
    uint32_t space, head;
    if (state.load() == 0) {
      r = 0;
      std::memset(mem.data(), 0, mem.size());
      wpos = 0;
      if (fill.load() != 0) {
        *avail = 0;
        return nullptr;
      }
      space = cap - r;
      head = r;
    } else {
      space = r - wpos;
      head = r;
      if (r == wpos) {
        head = wpos;
        if (fill.load() != 0) {
          *avail = 0;
          return nullptr;
        }
        space = cap - head;
      } else if (r <= wpos) {
        space = cap - wpos;
      } else {
        head = 0;
      }
    }
    wavail = space;
    uint32_t give = write_min;
    if (give <= space) {
      give = space;
      woverflow = 0;
    } else {
      if (space + head < give) {
        wavail = woverflow = 0;
        *avail = 0;
        return nullptr;
      }
      woverflow = give - space;
      wavail = give;
    }
    *avail = give;
    if (state.load() == 0) state = 1;
    return mem.data() + wpos;
  }
  // The commit inlined in sub_160FC0: bytes written past the end wrap to the start.
  void commit(uint32_t n) {
    std::lock_guard<std::mutex> l(wlock);
    if (n != 0 && static_cast<uint32_t>(state.load() - 1) <= 3) {
      uint32_t w = n + wpos;
      wpos = w;
      if (w >= cap) {
        const uint32_t over = w - cap;
        if (over != 0) std::memcpy(mem.data(), mem.data() + cap, over);
        wpos = over;
      }
      fill.fetch_add(static_cast<int32_t>(n));
    }
    wavail = woverflow = 0;
  }
  void write_done() {
    std::lock_guard<std::mutex> l(wlock);
    wavail = woverflow = 0;
  }
  // sub_1648B0 (and sub_165A60, which also returns +80): readable contiguous bytes at the
  // read position (a flushed / unread buffer reads as zeros).
  uint8_t* read_ptr(uint32_t* avail, uint64_t* total = nullptr) {
    uint32_t w;
    {
      std::lock_guard<std::mutex> l(wlock);
      w = wpos;
    }
    std::unique_lock<std::mutex> l(rlock);
    uint32_t n, head = w;
    if (w != rpos) {
      if (w <= rpos) {
        n = cap - rpos;
      } else {
        n = w - rpos;
        head = 0;
      }
    } else if (fill.load() != 0) {
      n = cap - w;
    } else {
      *avail = 0;
      return nullptr;
    }
    ravail = n;
    if (n >= read_min) {
      rcopied = 0;
    } else {
      if (n + head < read_min) {
        ravail = rcopied = 0;
        *avail = 0;
        return nullptr;
      }
      const uint32_t c = write_min - n;
      rcopied = c;
      std::memcpy(mem.data() + cap, mem.data(), c);
      n = read_min;
      ravail = n;
    }
    *avail = n;
    const int32_t s = state.load();
    if (s == 1) {
      if (rpos != 0) {
        // The firmware returns here with +64 still locked; the host releases it.
        std::memset(mem.data() + rpos, 0, n);
        *avail = 0;
        return nullptr;
      }
      read_total = 0;
      state = 2;
    } else if (static_cast<uint32_t>(s - 1) < 4) {
      if (s == 2) state = 3;
    } else {
      std::memset(mem.data() + rpos, 0, n);
    }
    if (total) *total = read_total;
    return mem.data() + rpos;
  }
  // sub_1649E0.
  void consume(uint32_t n) {
    std::lock_guard<std::mutex> l(rlock);
    const int32_t s = state.load();
    uint32_t k = s != 6 ? n : 0;
    if (k > ravail) k = ravail;
    if (static_cast<uint32_t>(s - 2) <= 2) {
      if (read_total != 0) state = 4;
      read_total += k;
    }
    if (k != 0) {
      const uint32_t f = static_cast<uint32_t>(fill.load());
      if (k > f) k = f;
      uint32_t r = k + rpos;
      if (r >= cap) r -= cap;
      rpos = r;
      fill.fetch_sub(static_cast<int32_t>(k));
    }
    ravail = rcopied = 0;
  }
  // sub_1620E0's part: state 5, cleared, state 6.
  void flush() {
    state = 5;
    std::scoped_lock l(wlock, rlock);
    std::memset(mem.data(), 0, mem.size());
    fill = 0;
    wpos = 0;
    wavail = cap;
    woverflow = rpos = ravail = rcopied = 0;
    state = 6;
  }
};

struct PssVoice;

// ---------------------------------------------------------------------------------------
// Output port (152 bytes; system +4056 the main port, +4240 the BGM port; sub_16FF60).
struct PssPort {
  uint32_t rate = 48000, channels = 2, bits = 32;  // +4, +8, +12
  uint32_t granularity = 256;                      // +16 (+44 after the open)
  uint32_t flags = 0;                              // +20 (0x400 BGM, 0x400400 direct BGM)
  uint32_t grain_bytes = 2048;                     // +40 granularity * channels * bits / 8
  uint32_t frame_bytes = 8;                        // +48 (4 * channels)
  std::atomic<float> volume{1.0f};                 // +52 (the direct player's volume)
  std::atomic<int32_t> vol_dirty{0};               // +56
  std::atomic<int32_t> attached{0};                // +104
  std::atomic<int32_t> active{0};                  // +108
  std::mutex lock;                                 // +112
  uint32_t flags120 = 0;                           // +120 (the 0xF00000 part of the player flags)
  std::atomic<PssVoice*> direct{nullptr};          // +128 (system +4368): the direct player
  PssAudioOut* out = nullptr;                      // +24 / +32: the driver

  // sub_16FF60 with port parameters {rate, channels, bits, granularity, flags}.
  void configure(uint32_t r, uint32_t c, uint32_t b, uint32_t g, uint32_t f) {
    rate = r;
    channels = c;
    bits = b;
    granularity = g;
    flags = f;
    grain_bytes = g * c * (b >> 3);
    frame_bytes = 4 * c;
  }
};

// ---------------------------------------------------------------------------------------
// Voice (344 bytes, sub_15F200).
struct PssVoice {
  PssDecoder* dec = nullptr;              // +8
  PssRing* ring = nullptr;                // +16
  int mix = 0;                            // +24: 0 none, 1 sub_160380 (stream buffer)
  PssPort* port = nullptr;                // +48
  uint32_t out_rate = 0, out_ch = 0, out_bits = 0;  // +56..+64
  bool allocated = false;                 // +68
  std::atomic<bool> active{false};        // +69
  std::atomic<bool> stopping{false};      // +70
  std::atomic<bool> working{false};       // +71
  std::atomic<bool> paused{false};        // +72
  bool auto_rewind = false;               // +73
  std::atomic<bool> eof_decode{false};    // +74
  std::atomic<bool> eof_ring{false};      // +75
  bool loop = false;                      // +76
  std::atomic<bool> dying{false};         // +79
  bool bgm_direct_capable = false;        // +80
  std::atomic<bool> seek_req{false};      // +81 a seek waits for the decoder
  std::atomic<bool> seek_done{false};     // +82 the decoder has done it
  bool no_stream = false;                 // +83
  std::atomic<int32_t> primed{0};         // +84
  std::atomic<uint16_t> fade_left{0};     // +88
  std::atomic<uint16_t> fade_len{0};      // +90
  uint64_t consumed = 0;                  // +96 source frames mixed / copied
  uint64_t seek_target = 0;               // +104
  uint64_t out_frames = 0;                // +112
  uint64_t loop_start = 0, loop_end = 0;  // +120, +128
  uint64_t loop_restart = 0;              // +136 frame the decoder restarted at for the loop
  std::mutex params;                      // host: guards the float parameters below
  float rate = 1.0f;                      // +144
  float ratio = 1.0f;                     // +148 source rate / output rate
  float pan = 0.5f, dpan = 0.0f, pan_target = 0.5f;  // +152, +156, +160
  uint32_t pan_ramp = 0;                  // +164
  float vol = 1.0f, dvol = 0.0f, vol_target = 1.0f;  // +168, +172, +176
  uint32_t vol_ramp = 0;                  // +180
  int32_t ramp_mode = 0;                  // +184
  float conv_state[5] = {};               // +208
  PssConverter conv = nullptr;            // +232
  PssConverter conv_generic = nullptr;    // +240
  uint32_t res_handle = 0;                // +248
  PssData* data = nullptr;                // +256
  uint64_t read_pos = 0;                  // +264 compressed bytes read
  float bits_ratio = 1.0f;                // +272
  uint32_t chunk = 0;                     // +276
  uint64_t decoded = 0;                   // +280 frames decoded
  uint32_t skip = 0, skip2 = 0;           // +288 frames / +292 output bytes to drop
  uint64_t total = 0;                     // +296
  uint16_t total_open = 0;                // +304
  uint16_t gen = 1;                       // +306
  int32_t dying_count = 0;                // +308
  std::atomic<int32_t> busy{0};           // +312
  uint32_t src_frame_bytes = 0;           // +316
  uint32_t out_frame_bytes = 0;           // +320
  uint64_t seek_req_frame = 0;            // +328
  uint64_t seek_frame = 0;                // +336
};

struct QueueEntry {  // 32-byte entries of the request queue at system +392
  int32_t cmd = 0;
  PssVoice* voice = nullptr;
  uint16_t gen = 0;
  uint32_t arg = 0;
};

// The data description words the decoders' seek reads (sub_165C80).
uint64_t length_frames(PssVoice& v) {
  const PssAudioInfo& i = v.data->info;
  float rate;
  {
    std::lock_guard<std::mutex> l(v.params);
    rate = v.rate;
  }
  const double x = static_cast<double>(rate) * u64_to_d(i.length() * i.rate()) / 1000.0;
  return to_u64(x);
}

// ---------------------------------------------------------------------------------------
class PssSystem {
 public:
  static PssSystem& get() {
    static PssSystem* s = [] {
      auto* p = new PssSystem();
      p->init();
      return p;
    }();
    return *s;
  }

  // config (sub_16E3B0 defaults through sub_18F080, overridden by the PSM initialiser's
  // scePssSoundPlayerInitialize, sub_15DB40, and adjusted by sub_16ED10).
  uint32_t out_rate_ = 48000, out_ch_ = 2, out_bits_ = 32, grain_ = 256;  // +124..+136
  uint32_t voice_count_ = 128, ring_count_ = 32, resource_count_ = 256;   // +140..+148
  uint32_t ring_bytes_ = 0x10000;                                         // +152

  std::array<PssVoice, 128> voices_;
  std::deque<int32_t> free_voices_;     // +296 list, +304/+308
  std::mutex voice_lock_;               // +312
  std::vector<std::unique_ptr<PssDecoder>> dec_store_;
  std::vector<PssDecoder*> dec_pool_[5];  // per type (+216 + 8 * type, count +256 + 4 * type)
  std::mutex dec_lock_;                 // +280
  std::vector<std::unique_ptr<PssRing>> ring_store_;
  std::vector<PssRing*> ring_pool_;     // +328 / +336
  std::mutex ring_lock_;                // +344
  std::array<PssData, 256> resources_;  // resource table (system +4408: +16 objects)
  std::deque<int32_t> free_resources_;  // +24 list, +32/+36
  std::mutex resource_lock_;            // +40
  PssPort main_port_;                   // +4056
  PssPort bgm_port_;                    // +4240
  int32_t has_bgm_port_ = 0;            // +4392
  float master_volume_ = 1.0f;          // +3600
  std::atomic<int32_t> sig360_{0}, sig364_{0};
  std::atomic<int32_t> decoder_voices_{0};  // +376 (word)
  std::array<QueueEntry, 100> queue_;   // +392
  int32_t q_read_ = -1, q_write_ = 0;   // +380, +384
  std::mutex queue_lock_;               // +3592
  std::atomic<bool> stream_started_{false};  // +352
  std::atomic<bool> running_{false};    // +96
  PssAudioOut out_;                     // main port driver
  PssAudioOut bgm_out_;                 // BGM port driver

  void init();
  uint32_t resource_create(std::shared_ptr<const std::vector<uint8_t>> image, uint16_t flags, uint32_t* handle);
  uint32_t release_handle(uint32_t h);
  PssData* resource(uint32_t h);
  PssVoice* voice(uint32_t h);
  uint32_t player_create(uint32_t res, uint32_t flags, int32_t user, int32_t appmod, uint32_t* out);

  uint32_t play_stop(PssVoice& v, bool play, uint16_t fade);   // sub_161C90
  uint32_t set_volume(PssVoice& v, float vol);                  // sub_163AE0
  uint32_t set_pan(PssVoice& v, float pan);                     // sub_1646A0
  uint32_t set_rate(PssVoice& v, float rate);                   // sub_1644A0
  void set_loop(PssVoice& v, bool loop);                        // sub_160CF0
  void pause(PssVoice& v, bool pause);                          // sub_162210
  uint32_t set_position(PssVoice& v, uint64_t frames);          // sub_1622B0
  uint64_t loop_end_of(const PssVoice& v) const;                // sub_163AB0
  bool output_running(const PssVoice& v) const {                // sub_1660F0 (port driver +184)
    return v.port && v.port->out && v.port->out->running();
  }
  uint64_t grains() const { return out_.grains(); }
  uint32_t port_status(const PssPort* p, uint32_t* status) const;  // driver +192

 private:
  uint32_t voice_init(PssVoice& v, uint32_t res_handle, PssData* data, PssDecoder* dec, const PssAudioInfo& info);
  uint32_t set_output_port(PssVoice& v, uint32_t flags, const PssAudioInfo& info);  // sub_16F8B0
  void voice_reset(PssVoice& v);                                // sub_160D70
  void free_voice(uint32_t slot);                               // sub_170F00
  void detach(PssVoice& v);                                     // sub_170280
  void port_up(PssPort* p);                                     // sub_170810
  void port_down(PssPort* p);                                   // sub_170880
  void flush(PssVoice& v);                                      // sub_1620E0
  uint32_t seek(PssVoice& v, uint64_t target, bool flush_ring); // sub_161EF0
  void seek_position(PssVoice& v, uint64_t* frame, uint64_t* byte, uint32_t* skip, uint32_t* skip2);  // sub_165C80
  void deactivate(PssVoice& v);                                 // the inline "voice ends" sequence
  uint32_t decode(PssVoice& v);                                 // sub_160FC0
  uint32_t mix(PssVoice& v, float* out);                        // sub_160380
  uint32_t direct_copy(PssVoice& v, uint32_t bytes, uint8_t* dst, uint32_t grain, uint32_t* frames);  // sub_164BB0
  bool end_check(PssVoice& v, uint32_t avail);                  // sub_164A70
  void request_decode(PssVoice& v);                             // sub_164820 / inlined in sub_160380
  void post(const QueueEntry& e);                               // sub_1727E0
  void process_queue();                                         // sub_1725E0
  void stream_thread();                                         // sub_16F7F0
  void ensure_stream_thread();                                  // sub_16F780
  void frame(float* out);                                       // sub_170C80
  void bgm_grain(uint8_t* buf, uint32_t bytes, uint32_t* filled);  // sub_1900D0 for the BGM port
  void free_resource(uint32_t h);                               // sub_174060
  uint32_t parse_riff(const std::shared_ptr<const std::vector<uint8_t>>& image, PssData& d);  // sub_178940
};

// sub_16E3B0 + sub_16ED10 + the ports' sub_16FF60 / PADR start.
void PssSystem::init() {
  // Stream buffer contiguity: the largest / smallest input chunk of every pooled decoder
  // (vtable+16 with the output description +124), starting from the output grain bytes
  // (+4396 = 256 * 2 * 4): type 1 sub_190610 = 0x2000, types 2/3 sub_1925B0 / sub_191D10 =
  // 144 * 32 * 2 = 9216, type 4 sub_190C10 = 1024 -> 9216 and 1024.
  const uint32_t grain_bytes = grain_ * out_ch_ * (out_bits_ >> 3);
  uint32_t max_chunk = grain_bytes, min_chunk = grain_bytes;
  const uint32_t chunk_of_type[5] = {0, 0x2000u, (144u * out_bits_ * out_ch_) & 0x1FFFFFFFu,
                                     (144u * out_bits_ * out_ch_) & 0x1FFFFFFFu, 1024u};
  for (int t = 1; t <= 4; ++t) {
    if (chunk_of_type[t] > max_chunk) max_chunk = chunk_of_type[t];
    if (chunk_of_type[t] < min_chunk) min_chunk = chunk_of_type[t];
  }
  if (ring_bytes_ < max_chunk) ring_bytes_ = max_chunk;
  // Decoder pools (sub_1745B0 -> sub_190350): 32 HEVAG decoders (+168 = dword_4FA64C) and
  // one ATRAC9 decoder. HOST: the type 1 / 2 pools (ADPCM 32, MP3 1) are not reproduced.
  for (int i = 0; i < 32; ++i) {
    dec_store_.push_back(std::make_unique<HevagDecoder>());
    dec_pool_[4].push_back(dec_store_.back().get());
  }
  dec_store_.push_back(std::make_unique<Atrac9Decoder>());
  dec_pool_[3].push_back(dec_store_.back().get());
  for (uint32_t i = 0; i < ring_count_; ++i) {
    ring_store_.push_back(std::make_unique<PssRing>());
    ring_store_.back()->init(ring_bytes_, max_chunk, min_chunk);
    ring_pool_.push_back(ring_store_.back().get());
  }
  for (uint32_t i = 0; i < voice_count_; ++i) free_voices_.push_back(static_cast<int32_t>(i));
  for (uint32_t i = 0; i < resource_count_; ++i) free_resources_.push_back(static_cast<int32_t>(i));
  HevagDecoder::load_tables();  // host: read the decoder tables now, not in the first decode
  running_ = true;
  // Main port: sub_18E990(0) -> 48000 Hz, 2 ch, 32-bit; sub_16FF60 (callback sub_170450);
  // sub_1841D0(+4080, 0) starts its driver thread.
  main_port_.configure(48000, 2, 32, grain_, 0);
  main_port_.out = &out_;
  out_.start(PortFormat{48000, 2, 32, grain_}, [this](uint8_t* buf, uint32_t bytes, uint32_t* filled) {
    // sub_170450: a full grain (bytes == +4396) is mixed straight into the port buffer.
    if (!running_.load()) return;
    if (bytes > grain_ * out_ch_ * 4) bytes = grain_ * out_ch_ * 4;
    if (static_cast<uint16_t>(bytes) == grain_ * out_ch_ * 4) {
      frame(reinterpret_cast<float*>(buf));
      *filled = grain_;
    } else {
      *filled = 0;
    }
  }, "main", true);
  // BGM port: sub_18E990(1024) -> 48000 Hz, 8 ch, 32-bit, flags 0x400; sub_16FF60 (callback
  // sub_170790, sys +4384 being 0); +4392 = 1; sub_1841D0(+4264, 1) opens it without its
  // thread (state 2), which the first active voice resumes (sub_170810).
  bgm_port_.configure(48000, 8, 32, grain_, 0x400);
  bgm_port_.out = &bgm_out_;
  bgm_out_.start(PortFormat{48000, 8, 32, grain_},
                 [this](uint8_t* buf, uint32_t bytes, uint32_t* filled) { bgm_grain(buf, bytes, filled); }, "bgm", false);
  has_bgm_port_ = 1;
  log::info("psm.audio", "PSS audio system: {} Hz, {} ch, float, {}-frame grains; {} voices, {} stream buffers of {} bytes; BGM port",
            out_rate_, out_ch_, grain_, voice_count_, ring_count_, ring_bytes_);
}

// sub_174100: (index + 0x1000) << 16 | generation.
PssData* PssSystem::resource(uint32_t h) {
  const uint32_t idx = (h >> 16) - 0x1000u;
  if (idx >= resource_count_) return nullptr;
  PssData& d = resources_[idx];
  return d.gen == static_cast<uint16_t>(h) ? &d : nullptr;
}

// sub_172CC0: slot << 16 | voice generation (+306).
PssVoice* PssSystem::voice(uint32_t h) {
  const uint32_t slot = h >> 16;
  if (slot >= voice_count_) return nullptr;
  PssVoice& v = voices_[slot];
  return v.gen == static_cast<uint16_t>(h) ? &v : nullptr;
}

// sub_174060: reset the data object (sub_1798B0) and put the slot back on the free list.
void PssSystem::free_resource(uint32_t h) {
  const uint32_t idx = (h >> 16) - 0x1000u;
  if (idx >= resource_count_) return;
  std::lock_guard<std::mutex> l(resource_lock_);
  resources_[idx].reset();
  bool present = false;
  for (int32_t i : free_resources_) present |= i == static_cast<int32_t>(idx);
  if (!present) free_resources_.push_back(static_cast<int32_t>(idx));
}

// sub_178940 over a memory container (sub_17AAA0 reads, sub_17A1D0 whence 2 skips). The
// chunks are walked until a header cannot be read: "fmt " (at least 14 bytes, at most 52
// kept; kind 1 / 2 / 3 for <= 17, <= 51, 52 bytes), "data" (size and offset; for a non-PCM
// format the size is cut to the fact sample count when larger, which ends the walk), "fact"
// (at least 4 bytes, at most 12 kept: samples, overlap delay, encoder delay), "smpl" (at least
// 36 bytes; the start / end of the last 24-byte loop record kept), others skipped; odd sizes
// are padded. A data chunk with an offset and a WAVE_FORMAT_EXTENSIBLE-sized fmt succeeds.
// The element is a sub-view [offset, offset + size) described by the format tag:
//   1 (PCM): type 0, bits, frames = size / ch / (bits / 8), bytes/s, length ms;
//   2 (MS ADPCM): type 1, 16-bit, block align, frames = (4 * size / ch) >> 1, length ms;
//   0xFFFE with the ATRAC9 GUID: type 3, 16-bit, frames = fact samples, bitrate = 8 * bytes/s,
//     block bytes = block align, block frames = samples per block, config bytes, encoder delay
//     (info[15]), loop = [smpl start - delay, smpl end - delay + 1) when both >= delay;
//   0xFFFE otherwise: type 0 like PCM (no length); other tags: 0x80B3F021.
uint32_t PssSystem::parse_riff(const std::shared_ptr<const std::vector<uint8_t>>& image, PssData& d) {
  const std::vector<uint8_t>& img = *image;
  const uint64_t size = img.size();
  uint64_t pos = 0;
  auto rd = [&](uint32_t n, uint8_t* to, uint32_t* got) -> uint32_t {  // sub_17AAA0
    if (pos >= size) return kEnd;
    const uint64_t k = std::min<uint64_t>(n, size - pos);
    std::memcpy(to, img.data() + pos, size_t(k));
    pos += k;
    *got = static_cast<uint32_t>(k);
    return 0;
  };
  auto skip = [&](uint64_t n) -> uint32_t {  // sub_17A1D0 whence 2 on a memory view
    if (size <= n) return kEnd;
    if (pos + n > size) return 0x80B3F008u;
    pos += n;
    return 0;
  };
  auto le32 = [](const uint8_t* p) { return uint32_t(p[0]) | uint32_t(p[1]) << 8 | uint32_t(p[2]) << 16 | uint32_t(p[3]) << 24; };
  auto le16 = [](const uint8_t* p) { return uint32_t(p[0]) | uint32_t(p[1]) << 8; };
  // The 88-byte parse state: +0 kind, +4 fmt (52 bytes), +56 data size, +60 data offset,
  // +68 fact (12 bytes), +80 / +84 loop start / end.
  uint32_t kind = 0;
  uint8_t fmt[52] = {};
  uint32_t data_size = 0;
  uint64_t data_off = 0;
  uint32_t fact[3] = {};
  uint32_t loop_s = 0, loop_e = 0;
  uint32_t r = kFail;
  uint8_t hdr[12];
  uint32_t got = 0;
  if (rd(12, hdr, &got) != 0 || got < 12 || std::memcmp(hdr, "RIFF", 4) != 0 || std::memcmp(hdr + 8, "WAVE", 4) != 0)
    return kFail;
  uint8_t ch[8];
  if (rd(8, ch, &got) == 0 && got == 8) {
    r = kFail;
    bool out = false;
    do {
      const uint32_t csize = le32(ch + 4);
      if (std::memcmp(ch, "fmt ", 4) == 0) {
        if (csize < 0xE) {
          skip(csize);
        } else {
          const uint32_t n = csize >= 0x34 ? 52 : csize;
          uint8_t tmp[52] = {};
          if (rd(n, tmp, &got) != 0 || got != n) break;
          if (csize > n) skip(csize - n);
          std::memcpy(fmt, tmp, 52);
          kind = (n > 0x11) + 1;
          if (n > 0x33) kind = 3;
        }
      } else if (std::memcmp(ch, "data", 4) == 0) {
        data_size = csize;
        data_off = pos;  // sub_179F10
        if (le16(fmt) == 1) {
          r = skip(csize);
        } else if (fact[0] == 0) {
          skip(csize);
          r = 0;
        } else if (data_size > fact[0]) {
          data_size = fact[0];
          r = skip(fact[0]);
          if (data_size != 0) {
            out = true;
            break;
          }
          return r != 0 ? r : kFail;
        } else {
          r = skip(data_size);
        }
      } else if (std::memcmp(ch, "fact", 4) == 0) {
        if (csize < 4) {
          skip(csize);
        } else {
          const uint32_t n = csize >= 0xC ? 12 : csize;
          uint8_t tmp[12] = {};
          if (rd(n, tmp, &got) != 0 || got != n) break;
          std::memcpy(fact, tmp, n);
          if (csize > n) skip(csize - n);
        }
      } else if (std::memcmp(ch, "smpl", 4) == 0 && csize >= 0x24) {
        uint8_t head[36];
        if (rd(36, head, &got) != 0 || got != 36) break;
        uint32_t used = 36;
        if (csize >= 0x3C) {
          uint32_t at = 36;
          bool ok = true;
          for (;;) {
            uint8_t rec[24];
            if (rd(24, rec, &got) != 0 || got != 24) {
              ok = false;
              break;
            }
            loop_s = le32(rec + 8);
            loop_e = le32(rec + 12);
            used = at + 24;
            if (!(at + 48 <= csize)) break;
            at += 24;
          }
          if (!ok) return kFail;
        }
        if (csize > used) skip(csize - used);
      } else {
        skip(csize);
      }
      if ((csize & 1) != 0) skip(1);
    } while (rd(8, ch, &got) == 0 && got == 8);
    (void)out;
  }
  if (data_size == 0) return r != 0 ? r : kFail;
  if (data_off != 0 && (kind & ~1u) == 2) r = 0;
  if (r != 0) return r;
  const uint32_t tag = le16(fmt);
  const uint32_t channels = le16(fmt + 2), rate = le32(fmt + 4), avg = le32(fmt + 8);
  const uint32_t align = le16(fmt + 12), bits = le16(fmt + 14), spb = le16(fmt + 18);
  PssAudioInfo info{};
  info.w[1] = 2;  // container info +4 (memory)
  if (channels == 0 || rate == 0) return kUnsupported;  // the firmware divides by them
  if (tag == 0xFFFE && std::memcmp(fmt + 24, kAtrac9Guid, 16) == 0) {
    info.w[0] = 3;
    info.w[2] = rate;
    info.w[3] = channels;
    info.w[4] = 16;
    info.w[5] = fact[0];
    info.w[6] = 16 * rate * channels;
    info.w[7] = 8 * avg;
    const float bb = static_cast<float>(static_cast<int32_t>(align));
    std::memcpy(&info.w[9], &bb, 4);
    info.w[10] = spb;
    info.set_length(1000ull * fact[0] / rate);
    std::memcpy(&info.w[14], fmt + 44, 4);
    info.w[15] = fact[2];
    if (loop_s >= fact[2] && loop_e >= fact[2]) {
      const uint64_t s = uint64_t(loop_s) - fact[2], e = uint64_t(loop_e) - fact[2] + 1;
      info.w[16] = uint32_t(s);
      info.w[17] = uint32_t(s >> 32);
      info.w[18] = uint32_t(e);
      info.w[19] = uint32_t(e >> 32);
    }
  } else if (tag == 0xFFFE || tag == 1) {
    info.w[0] = 0;
    info.w[2] = rate;
    info.w[3] = channels;
    info.w[4] = bits;
    if ((bits >> 3) == 0) return kUnsupported;
    info.w[5] = data_size / channels / (bits >> 3);
    info.w[6] = info.w[7] = bits * rate * channels;
    if (tag == 1) info.set_length(1000ull * info.w[5] / rate);
  } else if (tag == 2) {
    info.w[0] = 1;
    const float bb = static_cast<float>(static_cast<int32_t>(align));
    std::memcpy(&info.w[9], &bb, 4);
    info.w[2] = rate;
    info.w[3] = channels;
    info.w[4] = 16;
    info.w[5] = (4 * data_size / channels) >> 1;
    info.w[6] = (4 * rate * channels) & 0x3FFFFFFCu;
    info.w[7] = (rate * channels) & 0xFFFFFFFu;
    info.set_length(1000ull * info.w[5] / rate);
  } else {
    return kUnsupported;
  }
  d.set_memory(image, img.data() + data_off, data_size, info.w[0]);
  d.info = info;
  return 0;
}

// sub_171260 -> sub_173FA0 (slot, handle = idx << 16 + gen + 0x10000000) -> sub_171370 with
// a memory image: the container (sub_177CE0 / sub_179A50, type 6) is parsed by sub_177AF0
// (RIFF sub_178940, MP3 sub_177F80, ADPCM sub_179200, VAG sub_179210 in this order); the
// parsed element becomes the resource's view (sub_177D90 / sub_17AD20) with its
// description; 8/16/32-bit samples and 1, 2, 6, 7 or 8 channels are accepted.
uint32_t PssSystem::resource_create(std::shared_ptr<const std::vector<uint8_t>> image, uint16_t flags,
                                    uint32_t* handle) {
  int32_t idx;
  {
    std::lock_guard<std::mutex> l(resource_lock_);
    if (free_resources_.empty()) return kNoSlot;
    idx = free_resources_.front();
    free_resources_.pop_front();
  }
  PssData& d = resources_[size_t(idx)];
  *handle = (static_cast<uint32_t>(idx) << 16) + d.gen + 0x10000000u;
  const auto fail = [&](uint32_t r) {
    free_resource(*handle);
    return r;
  };
  if ((flags & 4) != 0) {
    host::note_unimplemented("libScePsm PSS resource flag 4 (decode on load, sub_171810) not reproduced");
    return fail(kUnsupported);
  }
  const std::vector<uint8_t>& img = *image;
  if (img.empty()) return fail(kArgument);  // sub_177CE0
  const uint8_t* b = img.data();
  const uint32_t n = static_cast<uint32_t>(img.size());
  // sub_178940 (RIFF/WAVE), sub_177F80 (MP3: a frame sync word or an ID3 tag).
  const uint32_t be = n >= 4 ? (uint32_t(b[0]) << 24 | uint32_t(b[1]) << 16 | uint32_t(b[2]) << 8 | b[3]) : 0;
  if (n >= 4 && std::memcmp(b, "RIFF", 4) == 0) {
    const uint32_t r = parse_riff(image, d);
    if (r == 0) {
      if (d.info.type() != 3)
        host::note_unimplemented("libScePsm PSS RIFF PCM / ADPCM resources (types 0 / 1: PCM voice sub_15FC00, ADPCM decoder) not reproduced");
      goto check;
    }
    // sub_177AF0 goes on with the other parsers; a RIFF image matches none of them.
    return fail(kUnsupported);
  }
  if (be > 0xFFDFFFFFu || (n >= 3 && std::memcmp(b, "ID3", 3) == 0)) {
    host::note_unimplemented("libScePsm PSS MP3 resources (sub_177F80, decoder type 2) not reproduced");
    return fail(kUnsupported);
  }
  {
    // sub_179210: 48-byte "VAGp" header (big endian): +0x0C data bytes, +0x10 rate, byte
    // +0x1E channels (0 -> 1). Element = bytes 48.. of the image, described as HEVAG.
    if (n < 48 || std::memcmp(b, "VAGp", 4) != 0) return fail(kUnsupported);
    auto be32 = [&](uint32_t o) { return uint32_t(b[o]) << 24 | uint32_t(b[o + 1]) << 16 | uint32_t(b[o + 2]) << 8 | b[o + 3]; };
    const uint32_t ch = b[0x1E] != 0 ? b[0x1E] : 1;
    const uint32_t rate = be32(0x10);
    const uint32_t blocks = be32(0x0C) / (16 * ch);
    PssAudioInfo info{};
    info.w[0] = 4;
    info.w[1] = 2;
    info.w[2] = rate;
    info.w[3] = ch;
    info.w[4] = 32;
    info.w[5] = 28 * blocks;
    info.w[6] = 32 * rate * ch;
    info.w[7] = static_cast<uint32_t>((613566757ull * (info.w[6] & 0xFFFFFFFu)) >> 32);
    info.w[8] = 0;
    info.w[9] = 0x41800000u;  // 16.0f bytes per block
    info.w[10] = 0x1C;        // 28 frames per block
    info.set_length(rate != 0 ? 784000ull * blocks / rate : 0);
    d.set_memory(image, b + 48, n - 48, 4);
    d.info = info;
    if (rate == 0) host::note_unimplemented("libScePsm PSS VAG with rate 0 (the firmware divides by it)");
  }
check: {
  // sub_171370 LABEL_14: format check, then sub_17AC90(+696 = 0).
  const PssAudioInfo& info = d.info;
  const bool bits_ok = info.bits() <= 32 && ((0x100010100ull >> info.bits()) & 1);
  const bool ch_ok = info.channels() <= 8 && ((454u >> info.channels()) & 1);
  if (!bits_ok || !ch_ok) return fail(kUnsupported);
  d.keep = 0;
  return 0;
}
}

// sub_15F200 state for a slot: generation, and what sub_160D70 clears.
void PssSystem::voice_reset(PssVoice& v) {
  ++v.gen;
  v.active = false;
  v.primed = 0;
  v.stopping = false;
  v.working = false;
  v.dec = nullptr;
  v.eof_ring = false;
  v.res_handle = 0;
  v.mix = 0;
  v.data = nullptr;
  v.read_pos = 0;
  {
    std::lock_guard<std::mutex> l(v.params);
    v.ratio = 1.0f;
    v.bits_ratio = 1.0f;
  }
  v.src_frame_bytes = v.out_frame_bytes = 0;
  v.total_open = 0;
  v.skip = 0;
  v.fade_left = 0;
  v.fade_len = 0;
  v.seek_req = false;
  v.seek_done = false;
  v.seek_req_frame = 0;
  v.seek_target = 0;
  v.dying = false;
  v.auto_rewind = false;
  v.eof_decode = false;
  v.dying_count = 0;
  v.conv = v.conv_generic = nullptr;
  if (v.port) detach(v);
}

// sub_170280: --attached. The BGM port's driver is stopped (+168) when its last voice goes
// unless its flags hold 0x100000; the main port only counts. (The direct player, system
// +4368, is left as it is.)
void PssSystem::detach(PssVoice& v) {
  PssPort* p = v.port;
  v.port = nullptr;
  if (p == &bgm_port_) {
    if (p->attached.fetch_sub(1) == 1 && (p->flags120 & 0x100000u) == 0) p->out->stop();
  } else {
    p->attached.fetch_sub(1);
  }
}

// sub_170810: ++active; the driver is resumed (+176) unless it runs (state 3).
void PssSystem::port_up(PssPort* p) {
  p->active.fetch_add(1);
  if (p->out && p->out->state() != 3) p->out->resume();
}

// sub_170880: --active; the driver stops (+168) when that was the last active voice of a
// port whose flags hold 0x100000.
void PssSystem::port_down(PssPort* p) {
  if (p->active.fetch_sub(1) == 1 && (p->flags120 & 0x100000u) != 0 && p->out) p->out->stop();
}

// sub_170F00: decoder and stream buffer back to their pools, the resource freed when it was
// waiting for its last voice (+684), the voice cleared and its slot back on the free list.
void PssSystem::free_voice(uint32_t slot) {
  std::lock_guard<std::mutex> l(voice_lock_);
  PssVoice& v = voices_[slot];
  if (v.dec) {
    {
      std::lock_guard<std::mutex> dl(dec_lock_);
      dec_pool_[v.dec->type()].push_back(v.dec);
    }
    v.dec = nullptr;
    if (decoder_voices_.load() != 0) decoder_voices_.fetch_sub(1);
  }
  if (v.ring) {
    std::lock_guard<std::mutex> rl(ring_lock_);
    ring_pool_.push_back(v.ring);
    v.ring = nullptr;
  }
  const uint32_t res = v.res_handle;
  PssData* d = resource(res);
  voice_reset(v);
  if (d && d->free_on_last_voice != 0) free_resource(res);
  bool present = false;
  for (int32_t i : free_voices_) present |= i == static_cast<int32_t>(slot);
  if (!present) free_voices_.push_back(static_cast<int32_t>(slot));
}

// sub_16F8B0 for the ports the PC has. Flag 0x400 with a BGM port (+4392): the BGM port
// (+4240, sub_15F350), its +120 = flags & 0xF00000; the only voice on it (attached == 1)
// with flag 0x400000 becomes the direct player (+4368) when sub_18EDC0 accepts its data
// (BGM table entry of the same rate, 2 / 6 / 8 channels, 16 or 32 bits -> port 8 channels
// (6 -> 8), the data's bits, granularity 256, flags 0x400400; otherwise the plain BGM
// parameters 8 ch / 32-bit / 0x400); a port whose rate / channels / bits change is reopened
// (sub_16FF60) and started (sub_1841D0 with flags & 0x100000: opened, thread on the first
// active voice). Without 0x400000 the driver is resumed unless flags hold 0x100000.
// Other players: no port flags and at most 2 channels -> the main port (+4056); the pad-
// speaker / personal / surround ports do not exist on the PC.
uint32_t PssSystem::set_output_port(PssVoice& v, uint32_t flags, const PssAudioInfo& info) {
  if ((flags & 0x400) != 0 && has_bgm_port_ != 0) {
    PssPort& p = bgm_port_;
    p.attached.fetch_add(1);  // sub_15F350
    v.port = &p;
    p.flags120 = flags & 0xF00000u;
    if (p.attached.load() == 1) {
      if ((flags & 0x400000) != 0) {
        uint32_t ch = 8, bits = 32, f = 0x400;
        PssVoice* direct = nullptr;
        const uint32_t c = info.channels(), b = info.bits();
        // sub_18EDC0: the BGM entry of the host's port table is 48000 Hz.
        if (info.rate() == 48000 && c <= 8 && ((324u >> c) & 1) && (b == 32 || b == 16)) {
          ch = c == 6 ? 8 : c;
          bits = b;
          f = 0x400400;
          direct = &v;
        }
        p.direct = direct;
        const bool changed = p.rate != 48000 || p.channels != ch || p.bits != bits;
        p.configure(48000, ch, bits, 256, f);
        if (changed) {
          p.out->reformat(PortFormat{48000, ch, bits, 256});
          if ((flags & 0x100000) == 0) p.out->resume();  // sub_1841D0(driver, 0)
          else p.out->stop();                            // sub_1841D0(driver, 1): state 2
        }
      } else if ((flags & 0x100000) == 0) {
        p.out->resume();
      }
    } else if (p.direct.load() != nullptr) {
      host::note_unimplemented("libScePsm PSS a second voice on the BGM port beside its direct player (sub_16F8B0 / sub_18EDC0(1024)) not reproduced");
      p.direct = nullptr;
      p.out->resume();
    }
    return 0;
  }
  if ((flags & 0xFFC00) != 0 || (flags & 0x10000) != 0 || info.channels() > 2) {
    host::note_unimplemented(
        "HOST VALUE libScePsm PSS pad-speaker / personal / surround audio-out ports: absent on the PC, the player "
        "is not created (sub_16F8B0 finds no port)");
    return kNoSlot;
  }
  main_port_.attached.fetch_add(1);  // sub_15F350
  v.port = &main_port_;
  return 0;
}

// sub_171DC0 (+ sub_16F8B0, sub_15F670, sub_166190).
uint32_t PssSystem::player_create(uint32_t res, uint32_t flags, int32_t user, int32_t appmod, uint32_t* out) {
  PssData* d = resource(res);
  if (!d) return kBadHandle;
  if (d->valid == 0) return kNotReady;
  if (!d->usable()) return kBadHandle;
  int32_t slot;
  {
    std::lock_guard<std::mutex> l(voice_lock_);
    if (free_voices_.empty()) return kNoSlot;
    slot = free_voices_.front();
    free_voices_.pop_front();
  }
  PssVoice& v = voices_[size_t(slot)];
  const PssAudioInfo info = d->info;
  const bool need_stream = (info.storage() - 2 < 2) ? (info.type() | ((flags & 0x100) != 0)) != 0
                                                    : !(info.storage() != 0 && (flags & 0x100) == 0);
  PssDecoder* dec = nullptr;
  if (need_stream) {
    PssRing* ring = nullptr;
    {
      std::lock_guard<std::mutex> l(ring_lock_);
      if (!ring_pool_.empty()) {
        ring = ring_pool_.back();
        ring_pool_.pop_back();
      }
    }
    if (!ring) {
      free_voice(uint32_t(slot));
      return kNoSlot;
    }
    ring->reset();
    v.ring = ring;  // sub_15F310
    if (info.type() != 0) {
      if (info.type() != HevagDecoder::kType && info.type() != Atrac9Decoder::kType) {
        host::note_unimplemented("libScePsm PSS decoders of types 1 / 2 (ADPCM, MP3) not reproduced");
        free_voice(uint32_t(slot));
        return kNoSlot;
      }
      std::lock_guard<std::mutex> l(dec_lock_);
      auto& pool = dec_pool_[info.type()];
      if (!pool.empty()) {
        dec = pool.back();
        pool.pop_back();
      }
    }
    if (info.type() != 0 && !dec) {
      free_voice(uint32_t(slot));
      return kNoSlot;
    }
  } else {
    host::note_unimplemented("libScePsm PSS voices without a stream buffer (PCM, sub_15FC00) not reproduced");
    free_voice(uint32_t(slot));
    return kUnsupported;
  }
  const uint32_t pr = set_output_port(v, flags, info);
  if (pr != 0) {
    if (dec) {
      std::lock_guard<std::mutex> l(dec_lock_);
      dec_pool_[dec->type()].push_back(dec);
    }
    free_voice(uint32_t(slot));
    return pr;
  }
  v.bgm_direct_capable = has_bgm_port_ != 0 && (flags & 0x400) != 0;  // sub_15F670 a8 (+80)
  const uint32_t r = voice_init(v, res, d, dec, info);
  if (r != 0) {
    if (dec) {
      std::lock_guard<std::mutex> l(dec_lock_);
      dec_pool_[dec->type()].push_back(dec);
      v.dec = nullptr;
    }
    free_voice(uint32_t(slot));
    return r;
  }
  v.auto_rewind = (flags & 0x80) != 0;  // sub_166190(voice, a5)
  if ((flags & 0x10) != 0 || (flags & 8) != 0)
    host::note_unimplemented("libScePsm PSS player flags 0x10 / 8 (pre-decode / auto-play) not reproduced");
  *out = (static_cast<uint32_t>(slot) << 16) | v.gen;
  (void)user;
  (void)appmod;
  return 0;
}

// sub_15F670 for a stream-buffer voice (a7 = 0, so the mixer is sub_160380) with the system
// output description (+124: 48000 Hz, 2 ch, 32-bit), then sub_15F360 for the voice's port.
uint32_t PssSystem::voice_init(PssVoice& v, uint32_t res_handle, PssData* d, PssDecoder* dec,
                               const PssAudioInfo& info) {
  v.busy.fetch_add(1);
  v.seek_target = 0;
  v.stopping = false;
  v.working = false;
  v.decoded = 0;
  v.skip = 0;
  v.consumed = 0;
  v.loop = false;
  v.loop_restart = 0;
  v.paused = false;
  v.eof_decode = false;
  v.eof_ring = false;
  v.primed = 0;
  v.fade_left = 0;
  v.fade_len = 0;
  {
    std::lock_guard<std::mutex> l(v.params);
    v.rate = 1.0f;
    v.pan = 0.5f;
    v.dpan = 0.0f;
    v.pan_target = 0.5f;  // xmmword_34D810 {0.5, 0, 1.0, 0} at +160
    v.pan_ramp = 0;
    v.vol = 1.0f;
    v.dvol = 0.0f;
    v.vol_target = 1.0f;
    v.vol_ramp = 0;
    v.ramp_mode = 0;
  }
  v.loop_start = v.loop_end = 0;
  v.out_frames = 0;
  v.out_rate = out_rate_;
  v.out_ch = out_ch_;
  v.out_bits = out_bits_;
  v.res_handle = res_handle;
  v.read_pos = 0;
  v.no_stream = false;
  const float ratio = static_cast<float>(info.rate()) / static_cast<float>(v.out_rate);
  v.src_frame_bytes = info.channels() * (info.bits() >> 3);
  v.out_frame_bytes = v.out_ch * (v.out_bits >> 3);
  v.dying_count = 0;
  v.total_open = 1;
  v.mix = 1;
  {
    std::lock_guard<std::mutex> l(v.params);
    v.ratio = ratio;
    v.bits_ratio = ratio * static_cast<float>(info.channels()) / static_cast<float>(v.out_ch) *
                   static_cast<float>(info.bits()) / static_cast<float>(v.out_bits);
    std::memset(v.conv_state, 0, sizeof(v.conv_state));
    v.conv_state[0] = -1.0f;
  }
  if (info.frames() != 0) {
    v.total_open = 0;
    v.total = info.frames();
  }
  // Converter pair (see pss_mix_converters.h): sub_15F670 for 32-bit sources of 1 or 2
  // channels; sub_15F360 replaces them for an 8-channel port (6-channel 16-bit: sub_1665C0).
  v.conv = v.conv_generic = nullptr;
  if (info.bits() == 32 && info.channels() == 2) {
    v.conv = pss_converter(ratio == 1.0f ? 11 : 10);
    v.conv_generic = pss_converter(10);
  } else if (info.bits() == 32 && info.channels() == 1) {
    v.conv = pss_converter(ratio == 1.0f ? 9 : 8);
    v.conv_generic = pss_converter(8);
  } else if (info.channels() <= 2) {
    pss_converter(0);  // logs: 8/16-bit sources
  }
  if (v.port->channels == 8) {
    v.conv = v.conv_generic = nullptr;
    if (info.bits() == 16 && info.channels() == 6) {
      v.conv = v.conv_generic = pss_conv_6ch16_8ch;
    } else {
      host::note_unimplemented("libScePsm PSS converters of an 8-channel port for this source (sub_15F360 case 8) not reproduced");
    }
  }
  // sub_15F360: out_frame_bytes = 4 * port channels; the ratio against the port's rate.
  v.out_frame_bytes = 4 * v.port->channels;
  {
    std::lock_guard<std::mutex> l(v.params);
    v.ratio = static_cast<float>(info.rate()) / static_cast<float>(v.port->rate);
  }
  uint32_t r = kUnsupported;
  if (v.conv && v.conv_generic) {
    v.data = d;
    v.dec = dec;
    r = 0;
    if (dec) {
      ensure_stream_thread();
      decoder_voices_.fetch_add(1);
      dec->set_info(info);  // vtable+72
      const uint32_t desc[3] = {info.rate(), info.channels(), info.bits()};
      v.chunk = dec->chunk_size(desc);  // vtable+16 with the data's description
      if (v.ring) v.ring->reset();
    }
    // A loop region in the data: +120 / +128 from it (the whole data when they are equal:
    // xmmword_34EEA0 = {0, 0x7FFFFFFF}, +136 = 0), the looping mixer, +76 = 1.
    if (info.loop_start() < info.loop_end()) {
      v.loop_start = info.loop_start();
      v.loop_end = info.loop_end();
      if (v.loop_start == v.loop_end) {
        v.loop_start = 0;
        v.loop_end = 0x7FFFFFFF;
        v.loop_restart = 0;
      }
      v.loop = true;
    }
    v.allocated = true;
  }
  v.busy.fetch_sub(1);
  return r;
}

// The "voice ends" sequence inlined in sub_160380 / sub_164A70 / sub_163C10 / sub_164BB0.
void PssSystem::deactivate(PssVoice& v) {
  v.working = false;
  if (v.port) {
    std::lock_guard<std::mutex> l(v.port->lock);
    if (v.active.exchange(false)) v.port->active.fetch_sub(1);
  } else {
    v.active = false;
  }
}

// sub_1620E0: stop now: inactive, stream buffer flushed, decoder told to stop (5).
void PssSystem::flush(PssVoice& v) {
  v.active = false;
  if (v.ring) v.ring->flush();
  if (v.dec) v.dec->request_state(5);
}

// sub_165C80 with a decoder: the decoder's +32 for *frame, with the total frames known
// (+296 once the decoder reached the end) or else estimated from the length: (u64)(rate *
// (double)(length ms * data rate) / 1000.0), at least +296.
void PssSystem::seek_position(PssVoice& v, uint64_t* frame, uint64_t* byte, uint32_t* skip, uint32_t* skip2) {
  if (!v.dec) {
    host::note_unimplemented("libScePsm PSS positions of voices without a decoder (sub_165C80 PCM branch) not reproduced");
    *byte = 0;
    *skip = 0;
    *skip2 = 0;
    *frame = 0;
    return;
  }
  uint64_t total = 0;
  if (v.data) {
    if (v.total != 0 && v.eof_decode.load()) {
      total = v.total;
    } else {
      total = length_frames(v);
    }
    if (total < v.total) total = v.total;
  }
  *byte = 0;
  *skip = 0;
  *skip2 = 0;
  v.dec->seek(v.data ? v.data->info : PssAudioInfo{}, total, frame, byte, skip, skip2);
}

// sub_161EF0: when the mixed position (+96) is not the target: the decoder position of the
// target (sub_165C80); with flush_ring the stream buffer is reset and the decoder told to
// seek (2); the data seeks to its byte (whence 0); +280 / +96 = the restart frame, +112 =
// (u64)(target / (ratio * rate) + 0.5), +288 / +292 the skips, +104 = target.
uint32_t PssSystem::seek(PssVoice& v, uint64_t target, bool flush_ring) {
  v.busy.fetch_add(1);
  if (v.auto_rewind) {
    v.eof_decode = false;
    v.eof_ring = false;
  }
  uint32_t r = kDying;
  if (!v.dying.load()) {
    r = 0;
    if (v.consumed != target) {
      uint64_t frame = target, byte = 0;
      uint32_t sk = 0, sk2 = 0;
      seek_position(v, &frame, &byte, &sk, &sk2);
      if (flush_ring && v.ring) {
        v.ring->reset();
        if (v.dec) v.dec->request_state(2);
      }
      v.read_pos = byte;
      r = v.data ? v.data->seek(byte) : 0;
      if (r == 0) {
        v.decoded = frame;
        v.consumed = frame;
        float rr;
        {
          std::lock_guard<std::mutex> l(v.params);
          rr = v.rate * v.ratio;
        }
        v.out_frames = to_u64(static_cast<double>(u64_to_f(target) / rr) + 0.5);
        v.skip = sk;
        v.skip2 = sk2;
        v.seek_target = target;
      }
    }
  }
  v.busy.fetch_sub(1);
  return r;
}

// sub_1622B0: a non-zero position needs a seekable decoder (+96) and random-access data
// (+728), else 0x80B3F021. A playing voice with a stream buffer leaves the seek to its
// decoder (+328 = frames, +81 = 1); otherwise it seeks now (sub_161EF0 with a flush).
uint32_t PssSystem::set_position(PssVoice& v, uint64_t frames) {
  v.busy.fetch_add(1);
  uint32_t r = kDying;
  if (!v.dying.load()) {
    if (frames != 0 && ((v.dec != nullptr && !v.dec->seekable()) || !v.data || v.data->random_access == 0)) {
      r = kUnsupported;
    } else if (v.active.load() && v.ring != nullptr) {
      v.seek_req_frame = frames;
      v.seek_req = true;
      r = 0;
    } else {
      r = seek(v, frames, true);
    }
  }
  v.busy.fetch_sub(1);
  return r;
}

// sub_161C90.
uint32_t PssSystem::play_stop(PssVoice& v, bool play, uint16_t fade) {
  v.busy.fetch_add(1);
  uint32_t r = kNotReady;
  if (!v.dying.load()) {
    PssPort* port = v.port;
    std::unique_lock<std::mutex> pl;
    if (port) pl = std::unique_lock<std::mutex>(port->lock);
    bool a2 = play;
    bool was_running = false;  // v14
    if (v.active.load()) {
      const bool was_paused = v.paused.load();
      was_running = !was_paused;
      if (!play) {
        v.paused = false;
        if (fade != 0) {
          if (v.dec != nullptr || v.primed.load() != 0) {
            a2 = true;
            if (v.fade_len.load() == 0) {
              v.fade_left = 0;
              v.fade_len = fade;
            }
            goto label30;
          }
          v.active = false;
        } else {
          flush(v);
          v.active = false;
          v.primed = 0;
        }
        a2 = false;
        goto label30;
      }
      if (!was_paused) {
        a2 = true;
        was_running = true;
        goto label30;
      }
      v.paused = false;
      v.active = play;
      was_running = false;
      goto label30;
    }
    if (!play) {
      a2 = false;
      was_running = false;
      goto label30;
    }
    if (!v.paused.load()) {
      if (pl.owns_lock()) pl.unlock();
      while (v.working.load() || v.busy.load() >= 2) usleep_us(100);
      if (port) pl.lock();
      v.fade_len = 0;
      v.working = true;
      if (v.dec) {
        sig360_.fetch_add(5);
        v.primed = 0;
        v.dec->request_state(7);
      } else {
        v.primed = 1;
      }
      seek(v, v.seek_target, true);
      v.active = true;
      if (v.dec == nullptr && v.primed.load() == 0) v.primed = 1;
      was_running = false;
      goto label30;
    }
    v.paused = false;
    v.active = play;
    was_running = false;
  label30:
    {
      bool now_paused;
      if (a2) {
        now_paused = v.paused.load();
        if (!now_paused && !was_running) {
          if (port) port_up(port);  // sub_170810
          goto label40;
        }
      } else {
        now_paused = true;
      }
      if (now_paused && was_running && port) port_down(port);  // sub_170880
    }
  label40:
    r = 0;
  }
  v.busy.fetch_sub(1);
  return r;
}

// sub_162210 (the port's lock held): pausing an active, running voice sets +72 and takes it
// off the port (sub_170880); un-pausing puts it back (sub_170810); an inactive voice is
// simply not paused.
void PssSystem::pause(PssVoice& v, bool pause) {
  PssPort* port = v.port;
  std::unique_lock<std::mutex> pl;
  if (port) pl = std::unique_lock<std::mutex>(port->lock);
  if (v.active.load()) {
    const bool was = v.paused.load();
    if (pause && !was) {
      v.paused = true;
      if (port) port_down(port);
    } else if (!pause && was) {
      v.paused = false;
      if (port) port_up(port);
    }
  } else {
    v.paused = false;
  }
}

// sub_163AE0(voice, frames 0, mode 0, volume): inactive -> immediate; active -> a ramp over
// one grain (frames 0 rounds up to the granularity, sub_16F6C0). The port also gets the
// volume for its direct player: +56 = 1, +52 = master (+3600) * volume.
uint32_t PssSystem::set_volume(PssVoice& v, float vol) {
  v.busy.fetch_add(1);
  uint32_t r = kDying;
  if (!v.dying.load()) {
    {
      std::lock_guard<std::mutex> l(v.params);
      const uint32_t frames = 0;
      if (frames != 0 || v.active.load()) {
        v.vol_target = vol;
        uint32_t n = grain_;
        if (grain_ <= frames) n = frames - frames % grain_;
        v.vol_ramp = n;
        v.dvol = (v.vol_target - v.vol) / static_cast<float>(static_cast<int64_t>(n));
      } else {
        v.vol_ramp = 0;
        v.dvol = 0.0f;
        v.vol_target = vol;
        v.vol = vol;
      }
      v.ramp_mode = 0;
    }
    r = 0;
    if (v.port) {
      v.port->vol_dirty = 1;
      v.port->volume = master_volume_ * vol;
    }
  }
  v.busy.fetch_sub(1);
  return r;
}

// sub_1646A0(voice, frames 0, pan): a pan off the centre switches the voice to the
// generic converter for good; frames 0 -> immediate.
uint32_t PssSystem::set_pan(PssVoice& v, float pan) {
  v.busy.fetch_add(1);
  if (!(pan == 0.5f)) v.conv = v.conv_generic;
  uint32_t r = kDying;
  if (!v.dying.load()) {
    std::lock_guard<std::mutex> l(v.params);
    v.pan_target = pan;
    v.pan_ramp = 0;
    v.pan = pan;
    v.dpan = 0.0f;
    r = 0;
  }
  v.busy.fetch_sub(1);
  return r;
}

// sub_1644A0: effective step ratio * rate outside [0.999, 1.0001] switches to the generic
// converter; the step is kept within [0.004, 256] by adjusting the rate.
uint32_t PssSystem::set_rate(PssVoice& v, float rate) {
  v.busy.fetch_add(1);
  uint32_t r = kDying;
  if (!v.dying.load()) {
    std::lock_guard<std::mutex> l(v.params);
    const float ratio = v.ratio;
    float x = rate;
    const float e = ratio * x;
    if (0.999f > e || e > 1.0001f) v.conv = v.conv_generic;
    if (0.004f > e) {
      x = 0.004f / ratio;
    } else if (e > 256.0f) {
      x = 256.0f / ratio;
    }
    v.rate = x;
    float k = ratio * x;
    const PssAudioInfo& info = v.data ? v.data->info : PssAudioInfo{};
    k = static_cast<float>(int64_t(info.channels())) * k / static_cast<float>(int64_t(v.out_ch));
    v.bits_ratio = static_cast<float>(int64_t(info.bits())) * k / static_cast<float>(int64_t(v.out_bits));
    r = 0;
  }
  v.busy.fetch_sub(1);
  return r;
}

// sub_160CF0: an empty loop region (+120 == +128) becomes the whole data ({0, 0x7FFFFFFF},
// +136 = 0); +76 = loop and the mixer follows (sub_162E80 looping / sub_160380).
void PssSystem::set_loop(PssVoice& v, bool loop) {
  if (v.loop_start == v.loop_end) {
    v.loop_start = 0;
    v.loop_end = 0x7FFFFFFF;
    v.loop_restart = 0;
  }
  v.loop = loop;
}

// sub_163AB0: the loop end, at most the decoded total once the decoder reached the end
// (+74) or the total turned known in a loop (+304 == 2).
uint64_t PssSystem::loop_end_of(const PssVoice& v) const {
  if (!v.eof_decode.load() && v.total_open != 2) return v.loop_end;
  return v.loop_end <= v.total ? v.loop_end : v.total;
}

// The port driver's +192 (sub_18FD00): 0x80B3F021 for a voice without a port.
uint32_t PssSystem::port_status(const PssPort* p, uint32_t* status) const {
  if (!p) return kUnsupported;
  *status = p->out ? p->out->port_status() : 1;
  return 0;
}

// sub_164820 / the request inlined in sub_160380 / sub_164BB0: when both end flags are not
// set and the stream buffer is at most a quarter full, queue a decode request (command 1).
void PssSystem::request_decode(PssVoice& v) {
  if (!v.ring) return;
  const uint32_t cap = v.ring->cap;
  const uint32_t fill = static_cast<uint32_t>(v.ring->fill.load());
  if (v.eof_decode.load() && v.eof_ring.load()) return;
  if (static_cast<float>(int64_t(cap)) * 0.25f >= static_cast<float>(int64_t(fill))) {
    QueueEntry e;
    e.cmd = 1;
    e.voice = &v;
    e.gen = v.gen;
    e.arg = cap - fill;
    post(e);
  }
}

// sub_1727E0: the queue holds 100 entries; a request for the same command and voice still
// queued is cancelled (its voice reference dropped) before the new one is added; a voice
// reference is taken (sub_1661B0). A full queue drops the request.
void PssSystem::post(const QueueEntry& e) {
  std::lock_guard<std::mutex> l(queue_lock_);
  int32_t rd = q_read_;
  const int32_t wr = q_write_;
  if (rd == wr) return;
  if (rd >= 0) {
    for (;;) {
      QueueEntry& q = queue_[size_t(rd)];
      if (q.cmd == e.cmd && q.voice == e.voice) {
        if (q.voice) {
          q.voice->busy.fetch_sub(1);  // sub_1661C0
          q.voice->dying_count = 0;
        }
        q = QueueEntry{};
        break;
      }
      if (++rd == 100) rd = 0;
      if (rd == wr) break;
    }
  }
  if (e.voice) e.voice->busy.fetch_add(1);  // sub_1661B0
  queue_[size_t(q_write_)] = e;
  if (q_read_ < 0) q_read_ = q_write_;
  const int32_t next = q_write_ + 1 != 100 ? q_write_ + 1 : 0;
  q_write_ = next;
}

// sub_1725E0: command 1 decodes into the voice's stream buffer when its generation still
// matches; command 7 retries freeing a released resource.
void PssSystem::process_queue() {
  std::unique_lock<std::mutex> l(queue_lock_);
  for (int32_t i = q_read_; i >= 0; i = q_read_) {
    int32_t next = i + 1;
    if (next == 100) next = 0;
    q_read_ = next;
    const QueueEntry e = queue_[size_t(i)];
    if (next == q_write_) q_read_ = -1;
    l.unlock();
    switch (e.cmd) {
      case 1:
        if (e.voice) {
          if (e.voice->gen == e.gen) decode(*e.voice);
          e.voice->busy.fetch_sub(1);
          e.voice->dying_count = 0;
        }
        break;
      case 7: {
        PssData* d = resource(e.arg);
        if (d) {
          d->mark_releasing();
          if (d->can_free()) {
            free_resource(e.arg);
          } else {
            post(e);
          }
        }
        break;
      }
      default:
        break;
    }
    l.lock();
  }
}

// sub_16F7F0: until the system stops: run the queue, then take the mixer's signals (+360,
// +364): more than one -> go on; one -> yield; none -> sleep 20 ms, or 5000 - 2000 * (+364)
// us (at most 20 ms) while decoding voices exist.
void PssSystem::stream_thread() {
  while (running_.load()) {
    process_queue();
    const int32_t a = sig360_.exchange(0);
    const int32_t b = sig364_.exchange(0);
    if (a <= 1) {
      if (a == 1) {
        yield();
      } else {
        int32_t us = 20000;
        if (b != 0 || decoder_voices_.load() != 0) {
          us = -2000 * b + 5000;
          if (us >= 20000) us = 20000;
        }
        usleep_us(static_cast<uint32_t>(us));
      }
    }
  }
}

void PssSystem::ensure_stream_thread() {  // sub_16F780 ("ScePsP:AudioStream")
  bool expected = false;
  if (stream_started_.compare_exchange_strong(expected, true)) {
    std::thread([this] { stream_thread(); }).detach();
  }
}

// sub_160FC0: fill the stream buffer with decoded frames while it has room and the voice is
// active. Before the first decode, and for a pending seek (+81: the stream buffer reset, the
// target +328), the decoder position comes from sub_165C80 (+264 byte, +280 = +336 frame,
// +288 / +292 skips; +81 -> +82). With a loop (+76): reaching the loop end (or the end of
// the data, +74) records the total (+296) and restarts the decoder at the loop start (+120:
// position, +136 = its frame, decoder reset), and a request that would cross the loop end
// is cut there (v74); the output past the end is dropped. Frames to skip (+288) are decoded
// into a local buffer (8172 bytes) and dropped; a decode error skips input (decoder +48).
uint32_t PssSystem::decode(PssVoice& v) {
  v.busy.fetch_add(1);
  uint32_t r = kDying;
  if (v.dying.load() || !v.data || !v.data->acquire()) {
    v.busy.fetch_sub(1);
    return r;
  }
  if (!v.dec) {
    host::note_unimplemented("libScePsm PSS stream voices without a decoder (sub_160FC0 PCM copy) not reproduced");
    v.data->release();
    v.busy.fetch_sub(1);
    return 0;
  }
  const PssAudioInfo& info = v.data->info;
  const uint32_t sfb = v.src_frame_bytes;
  bool first = v.decoded == 0;  // v7
  uint32_t free_bytes = 0;      // v83
  uint8_t* wp = v.ring->write_ptr(&free_bytes);
  r = 0;
  int32_t budget = 0;  // v76
  if (v.primed.load() != 0) {
    float rr;
    {
      std::lock_guard<std::mutex> l(v.params);
      rr = v.rate * v.ratio;
    }
    budget = static_cast<int32_t>(rr * static_cast<float>(int64_t(v.port ? v.port->granularity : grain_)));
  }
  static thread_local std::vector<uint8_t> local(8176);  // v87
  if (wp != nullptr && free_bytes != 0 && !v.dying.load() && v.active.load()) {
    uint32_t want = free_bytes;  // v17
    bool from_start = first;     // v5
    bool at_loop_end = false;    // v77
    bool cut = false;            // v74
    bool loop_read = false;      // v75
    do {
      const uint32_t want_frames = want / sfb;  // v19
      if (v.seek_req.load() || from_start) {
        uint64_t target;
        if (v.seek_req.load()) {
          v.ring->reset();
          target = v.seek_req_frame;
        } else {
          target = v.decoded;
        }
        uint64_t frame = target, byte = 0;
        uint32_t sk = 0, sk2 = 0;
        seek_position(v, &frame, &byte, &sk, &sk2);
        v.dec->request_state(2);
        v.read_pos = byte;
        v.decoded = frame;
        v.seek_frame = frame;
        v.skip = sk;
        v.skip2 = sk2;
        if (v.seek_req.load()) {
          v.seek_req = false;
          v.seek_done = true;
        }
      }
      if (v.loop) {
        uint64_t pos = v.decoded;  // v27
        uint64_t end = v.loop_end; // v28
        if (v.eof_decode.load() || pos >= end) {
          v.eof_decode = false;
          v.eof_ring = false;
          v.total = pos;
          if (!v.dec->seekable() && v.loop_start != 0) v.loop_start = 0;
          uint64_t frame = v.loop_start, byte = 0;
          uint32_t sk = 0, sk2 = 0;
          seek_position(v, &frame, &byte, &sk, &sk2);
          v.read_pos = byte;
          v.decoded = frame;
          v.loop_restart = frame;
          v.skip = sk;
          v.skip2 = sk2;
          v.seek_req_frame = 0;
          v.dec->reset();
          pos = v.decoded;
          end = v.loop_end;
          cut = false;
        }
        if (end <= pos || pos + want_frames <= end) {
          if (at_loop_end) {
            v.eof_decode = false;
            v.eof_ring = false;
            cut = false;
          }
          at_loop_end = false;
        } else {
          cut = true;
          at_loop_end = false;
          want = sfb * static_cast<uint32_t>(end - pos);
        }
      }
      const uint32_t to_read = v.dec->input_size(want);  // v82 (decoder +24)
      bool near_loop = false;                            // v37 (file views only)
      if (info.loop_flag() != 0 && v.loop_start > v.decoded && v.loop_start - v.decoded <= info.block_frames())
        near_loop = true;
      const uint8_t* src = nullptr;
      uint32_t src_len = 0;
      r = v.data->read(v.read_pos, to_read, &src, &src_len);
      bool go_end = false;
      if (r != 0) {
        if (r == kEnd || v.read_pos == v.total * sfb) v.eof_decode = true;
        if (v.eof_ring.load()) break;
        if (!cut && r == kEnd) {
          v.eof_decode = true;
          go_end = true;
        } else if (r != kEnd) {
          break;
        }
      } else {
        if (info.loop_flag() != 0) loop_read = near_loop;
      }
      if (go_end || v.eof_decode.load()) {
        if (v.loop) {
          at_loop_end = true;
          if (v.total_open == 1) v.total_open = 2;
        }
      }
      uint32_t used = 0, made = 0;  // v79, v78
      if (v.skip == 0) {
        const uint32_t e = v.dec->decode(src, src_len, &used, wp, free_bytes, &made, &v.skip2);
        if (e == 0) {
          r = 0;
        } else {
          if (!v.active.load()) {
            r = e;
            break;
          }
          uint32_t skipped = 0;
          r = v.dec->skip(src, src_len, &skipped, v.read_pos);
          if (r != 0)
            used = src_len;
          else if (skipped != 0)
            used = skipped;
          else
            ++used;
          made = 0;
        }
      } else {
        uint32_t acc = 0, off = 0;  // v39, v40
        for (;;) {
          uint32_t sk = 0, dn = 0;  // v86, v85
          uint32_t step = 0;        // v41
          uint32_t rem = 0;         // v42
          bool l64 = true;
          if (src != nullptr) {
            const uint32_t e = v.dec->skip(src + off, src_len, &sk, v.read_pos + off);
            step = sk;
            if (e != 0) {
              r = e;
              acc = used;
            } else {
              uint32_t at = off + sk;  // v44
              const uint32_t e2 = v.dec->decode(src + at, src_len, &dn, local.data(), 8172, &made, &v.skip2);
              if (e2 != 0) {
                r = e2;
                made = 0;
                at += 1;
                acc = sk + used;
                rem = src_len - sk;
                used += sk;
                src_len -= sk;
              } else {
                acc = dn + used;
                rem = src_len;
                r = 0;
                used += dn;
              }
              off = at;
              if (r == 0) break;  // -> LABEL_73
              l64 = false;        // -> LABEL_65
            }
          } else {
            r = kEnd;
            step = 0;
          }
          if (l64) {  // LABEL_64
            acc += step;
            off += step + 1;
            used = acc;
            rem = src_len - step;
            src_len -= step;
          }
          if (rem <= 1) break;  // LABEL_65
        }
        (void)acc;
        const uint32_t frames_made = made / sfb;
        if (v.skip >= frames_made) {
          v.skip -= frames_made;
          made = 0;
        } else {
          made = sfb * (frames_made - v.skip);
          std::memcpy(wp, local.data() + size_t(sfb) * v.skip, made);
          v.skip = 0;
        }
      }
      v.read_pos += used;
      bool tail = false;
      if (cut && !v.eof_decode.load()) {
        const uint64_t now = v.decoded + made / sfb;  // v60
        v.decoded = now;
        if (now >= v.loop_end) {
          at_loop_end = true;
          const uint32_t over = static_cast<uint32_t>(now - v.loop_end) * sfb;
          made = made < over ? 0 : made - over;
        } else if (!at_loop_end) {
          tail = true;
        }
        if (!tail) {
          if (v.total < now) v.total = now;
          if (!v.dec->seekable() && v.loop_start != 0) v.loop_start = 0;
          uint64_t frame = v.loop_start, byte = 0;
          uint32_t sk = 0, sk2 = 0;
          seek_position(v, &frame, &byte, &sk, &sk2);
          v.read_pos = byte;
          v.decoded = frame;
          v.loop_restart = frame;
          v.skip = sk;
          v.skip2 = sk2;
          v.seek_req_frame = 0;
          v.dec->reset();
          r = 0;
        }
      } else {
        v.decoded += made / sfb;
      }
      if (v.total_open != 0 && v.total < v.decoded) v.total = v.decoded;
      if (budget > 0) {
        budget -= static_cast<int32_t>(made / sfb);
        if (budget <= 0) v.primed = 1;
      }
      v.ring->commit(made);
      if (made == 0 && v.eof_decode.load()) v.eof_ring = true;
      wp = v.ring->write_ptr(&free_bytes);
      if (!wp || free_bytes == 0 || v.dying.load()) break;
      from_start = false;
      want = free_bytes;
      (void)loop_read;
    } while (v.active.load());
  }
  v.ring->write_done();
  v.data->release();
  if (v.active.load()) v.primed = 1;
  v.busy.fetch_sub(1);
  return r;
}

// sub_164A70: false when the voice ended. Mixed frames past the total end the voice once
// the decoder reached the end of the data and at most two frames' worth is left.
bool PssSystem::end_check(PssVoice& v, uint32_t avail) {
  if (v.consumed <= v.total) return true;
  const uint64_t over = v.consumed - v.total;
  bool end = false;
  if (v.eof_decode.load()) {
    if (avail == 0) {
      end = true;
    } else if (v.dec != nullptr) {
      float rate, ratio;
      {
        std::lock_guard<std::mutex> l(v.params);
        rate = v.rate;
        ratio = v.ratio;
      }
      const float k = static_cast<float>(int64_t(avail)) / (static_cast<float>(int64_t(v.src_frame_bytes)) * rate * ratio);
      if (2.0f >= k) end = true;
    }
  }
  if (end) {
    v.ring->consume(0);
    deactivate(v);
    v.paused = false;
    return false;
  }
  if (v.total_open != 0) {
    v.total = v.consumed;
  } else {
    v.consumed = over;
    float rr;
    {
      std::lock_guard<std::mutex> l(v.params);
      rr = v.ratio * v.rate;
    }
    v.out_frames = to_u64(static_cast<double>(u64_to_f(over) / rr) + 0.5);
  }
  return true;
}

// sub_160380 (stream-buffer voices; no seek in flight, no start skip): mix one grain.
uint32_t PssSystem::mix(PssVoice& v, float* out) {
  v.busy.fetch_add(1);
  if (v.dying.load() || !v.data || !v.data->acquire()) {
    deactivate(v);
    v.paused = false;
    v.busy.fetch_sub(1);
    return kDying;
  }
  if (v.loop) host::note_unimplemented("libScePsm PSS looping mixer (sub_162E80 / sub_1624A0) not reproduced; mixed as sub_160380");
  if (v.seek_done.load())
    host::note_unimplemented("libScePsm PSS pending seek of a mixed voice (sub_160380 +82) not reproduced");
  PssRing& ring = *v.ring;
  const uint32_t gran = v.port->granularity;
  const uint32_t want_bytes = gran * v.out_frame_bytes;
  request_decode(v);
  uint32_t done = 0;  // v152, bytes of output
  uint32_t r = 0;
  if (want_bytes != 0 && !v.dying.load()) {
    for (;;) {
      uint32_t avail = 0;
      uint8_t* p = ring.read_ptr(&avail);
      if (v.eof_ring.load() && avail == 0) {
        deactivate(v);
        v.decoded = 0;
        ring.consume(0);
        break;
      }
      if (avail == 0) {
        // Underrun: wake the decoder and wait for it (100 us), then try again.
        ring.consume(0);
        if (v.dying.load() || !v.active.load()) break;
        sig360_.fetch_add(5);
        request_decode(v);
        usleep_us(100);
        if (done >= want_bytes || v.dying.load()) break;
        continue;
      }
      if (!p) break;
      uint32_t src_frames = avail / v.src_frame_bytes;            // v156
      const uint32_t dst_off = done / v.out_frame_bytes;           // v54
      uint32_t dst_frames = gran - done / v.out_frame_bytes;      // v53 / v154
      uint32_t produced = dst_frames;
      if (v.src_frame_bytes <= avail) {
        float vol, dvol, pan, dpan, step;
        PssConverter conv;
        {
          std::lock_guard<std::mutex> l(v.params);
          step = v.ratio * v.rate;
          vol = v.vol;
          dvol = v.dvol;
          pan = v.pan;
          dpan = v.dpan;
          conv = v.conv;
        }
        const uint32_t reachable = static_cast<uint32_t>(static_cast<int64_t>(static_cast<float>(int64_t(src_frames)) / step));
        const uint16_t fl = v.fade_len.load();
        if (fl != 0) {
          uint16_t left = v.fade_left.load();
          if (left == 0) {
            left = fl;
            v.fade_left = fl;
          }
          if (reachable >= left && dst_frames > left) dst_frames = left;
        }
        if (dst_frames >= reachable) {
          sig360_.fetch_add(2);
          produced = reachable;
        } else {
          const int32_t k = 2 - static_cast<int32_t>(reachable / gran);
          if (k != 0) (k > 0 ? sig360_ : sig364_).fetch_add(k);
          produced = dst_frames;
        }
        const uint16_t left = v.fade_left.load();
        if (left != 0) {
          // The stop fade: from vol * left / len down by vol / len per frame.
          const float lenf = static_cast<float>(fl);
          dvol = -vol / lenf;
          vol = (vol * static_cast<float>(left)) / lenf;
        }
        float state[5];
        {
          std::lock_guard<std::mutex> l(v.params);
          std::memcpy(state, v.conv_state, sizeof(state));
        }
        r = conv(reinterpret_cast<const float*>(p), out, dst_off, &src_frames, &produced, state, vol, dvol, pan, dpan,
                 step);
        {
          std::lock_guard<std::mutex> l(v.params);
          std::memcpy(v.conv_state, state, sizeof(state));
        }
        if (r != 0) {
          ring.consume(0);
          break;
        }
        const uint16_t fl2 = v.fade_left.load();
        if (fl2 != 0) v.fade_left = produced >= fl2 ? 0 : static_cast<uint16_t>(fl2 - produced);
        v.consumed += src_frames;
        v.out_frames += produced;
        if (!end_check(v, avail)) break;
        done += produced * v.out_frame_bytes;
        const uint32_t used = src_frames * v.src_frame_bytes;
        if (used != 0) {
          ring.consume(used);
          std::lock_guard<std::mutex> l(v.params);
          if (v.vol_ramp != 0) {
            const bool last = v.vol_ramp <= produced;
            v.vol_ramp = v.vol_ramp < produced ? 0 : v.vol_ramp - produced;
            v.vol = static_cast<float>(int64_t(produced)) * v.dvol + v.vol;
            if (last) {
              v.dvol = 0.0f;
              v.vol = v.vol_target;
            }
          }
          if (v.pan_ramp != 0) {
            const bool last = v.pan_ramp <= produced;
            v.pan_ramp = v.pan_ramp < produced ? 0 : v.pan_ramp - produced;
            v.pan = static_cast<float>(int64_t(produced)) * v.dpan + v.pan;
            if (last) {
              v.dpan = 0.0f;
              v.pan = v.pan_target;
            }
          }
        }
      } else {
        sig360_.fetch_add(3);
        ring.consume(0);
      }
      if (v.fade_len.load() != 0 && v.fade_left.load() == 0) {
        // The fade has run out: the voice stops (+72 = 0, qword +84 = 0: primed and the fade).
        deactivate(v);
        v.paused = false;
        v.primed = 0;
        v.fade_left = 0;
        v.fade_len = 0;
        break;
      }
      if (done >= want_bytes || v.dying.load()) break;
    }
  }
  v.data->release();
  v.busy.fetch_sub(1);
  return r;
}

// sub_164BB0 for a voice with a stream buffer: the direct player copies its decoded frames
// straight into the port's grain (`bytes` = grain frames * port frame bytes; a source frame
// narrower than a port frame leaves the rest zero: 6 channels into the 8-channel slots 0..5).
// The mixed position +96 / +112 advances with the copy; after a seek done by the decoder
// (+82) it restarts at +336 and the frames before the target (+328) are dropped; at the loop
// end (+128, at most the total +296) the copy is cut, the position restarts at +136 and the
// frames before the loop start (+120) are dropped. Decode requests are queued as the mixer
// queues them; an empty buffer waits 100 us. The voice ends (inline sequence) when the
// decoder reached the end and the buffer is empty, or, without a loop, past the total.
uint32_t PssSystem::direct_copy(PssVoice& v, uint32_t bytes, uint8_t* dst, uint32_t grain, uint32_t* frames) {
  v.busy.fetch_add(1);
  auto end_voice = [&]() {
    v.working = false;
    {
      std::lock_guard<std::mutex> l(v.port->lock);
      if (v.active.exchange(false)) v.port->active.fetch_sub(1);
    }
  };
  if (v.dying.load() || !v.data || !v.data->acquire()) {
    end_voice();
    v.paused = false;
    v.busy.fetch_sub(1);
    return kDying;
  }
  if (v.ring && v.ring->state.load() >= 5) v.ring->state = 0;  // sub_163C10
  PssRing& ring = *v.ring;
  const uint32_t sfb = v.src_frame_bytes;  // v157
  const uint32_t pfb = bytes / grain;      // v161
  uint64_t lend = v.loop_end;              // v24
  if (lend > v.total) lend = v.total;
  bool crossing = false;                   // v160
  uint32_t until_end = 0x7FFFFFFF;         // v158 frames
  uint32_t until_bytes = 0;                // v26 / v138
  if (v.loop) {
    uint64_t c = v.consumed;
    if (c > lend) {
      c = v.loop_restart;
      v.consumed = c;
      v.out_frames = c;
    }
    if (lend >= c && c + grain > lend) {
      crossing = true;
      until_end = static_cast<uint32_t>(lend - c);
      until_bytes = until_end * sfb;
    }
  }
  if (!crossing) {
    if (!(v.eof_decode.load() && v.eof_ring.load())) request_decode(v);
  }
  if (sfb < pfb) std::memset(dst, 0, bytes);
  uint32_t written = 0;  // v39
  if (bytes != 0) {
    const uint32_t per = pfb < sfb ? pfb : sfb;  // v137
    bool after_end = false;                     // v38
    for (;;) {
      uint32_t avail = 0;  // v163
      uint64_t total_read = 0;
      uint8_t* p = ring.read_ptr(&avail, &total_read);
      if (v.eof_ring.load() && avail == 0) {
        end_voice();
        v.decoded = 0;
        ring.consume(0);
        break;
      }
      if (avail == 0) {
        ring.consume(0);
        if (v.dying.load() || !v.active.load()) break;
        sig360_.fetch_add(5);
        if (!(v.eof_decode.load() && v.eof_ring.load())) request_decode(v);
        usleep_us(100);
        if (written >= bytes) break;
        continue;
      }
      if (v.seek_done.load()) {
        v.consumed = v.seek_frame;
        v.out_frames = v.seek_frame;
        v.seek_done = false;
      }
      const uint64_t c = v.consumed;
      uint32_t drop;     // v43
      bool all_drop;     // v139
      if (after_end) {
        drop = v.loop_start > c ? static_cast<uint32_t>(v.loop_start - c) : 0;
        all_drop = sfb * drop >= avail;
      } else {
        drop = v.seek_req_frame > c ? static_cast<uint32_t>(v.seek_req_frame - c) : 0;
        all_drop = false;
      }
      const uint32_t have = avail / sfb;  // v54
      if (have <= drop) {
        ring.consume(sfb * have);
        v.consumed += have;
        v.out_frames = v.consumed;
        sig360_.fetch_add(3);
        if (!(v.eof_decode.load() && v.eof_ring.load())) request_decode(v);
        usleep_us(10);
        after_end = all_drop;
        if (written >= bytes) break;
        continue;
      }
      if (!p) break;
      if (drop != 0) {
        v.consumed += drop;
        v.out_frames += drop;
      }
      uint32_t n = have - drop;  // v55
      if (n >= until_end) n = until_end;
      uint32_t room = grain - written / pfb;  // v56
      if (n <= room) {
        sig360_.fetch_add(2);
        room = n;
      } else {
        const int32_t k = 2 - static_cast<int32_t>(n / grain);
        if (k != 0) (k > 0 ? sig360_ : sig364_).fetch_add(k);
      }
      const uint8_t* s = p + size_t(sfb) * drop;
      if (sfb == pfb) {
        std::memcpy(dst + written, s, size_t(pfb) * room);
        written += pfb * room;
      } else {
        for (uint32_t i = 0; i < room; ++i) {
          std::memcpy(dst + written, s, per);
          written += pfb;
          s += sfb;
        }
      }
      const uint64_t now = v.consumed + room;  // v77
      v.consumed = now;
      v.out_frames += room;
      after_end = all_drop;
      bool to_l94 = false;
      uint32_t span_bytes = 0;  // v82
      if (v.loop) {
        if (v.eof_decode.load() || v.total_open == 2) {
          if (v.loop_end > v.total) {
            v.loop_end = v.total;
            until_end = static_cast<uint32_t>(v.total - now);
            span_bytes = until_end * sfb;
            to_l94 = true;
          }
        }
      } else if (now > v.total) {
        if (v.eof_decode.load() && avail / sfb <= 2) {
          ring.consume(0);
          end_voice();
          v.paused = false;
          break;
        }
        if (v.total_open != 0) {
          v.total = now;
        } else {
          v.consumed = now - v.total;
          v.out_frames = v.consumed;
        }
      }
      const uint32_t used = sfb * (drop + room);  // v83
      if (!to_l94) {
        if (!crossing) {
          ring.consume(used);
          if (written >= bytes) break;
          continue;
        }
        span_bytes = until_bytes;
      }
      // L94: the copy reached (or passed) the loop end.
      if (span_bytes <= used) {
        crossing = false;
        after_end = true;
        until_end = static_cast<uint32_t>(lend) - static_cast<uint32_t>(v.loop_start);
        until_bytes = until_end * sfb;
        v.consumed = v.loop_restart;
        v.out_frames = v.loop_restart;
      } else {
        until_bytes = span_bytes - used;
        until_end -= drop + room;
        crossing = true;
      }
      ring.consume(used);
      if (written >= bytes) break;
    }
  }
  *frames = written / pfb;
  v.data->release();
  v.busy.fetch_sub(1);
  return 0;
}

// sub_1900D0 for the BGM port (type 1 in the port table): with voices attached but none
// active the thread waits 100 us unless the port flags hold 0x100000; with none attached
// it waits 500 us when they do. A direct player (+4368, port +60 clear) is copied by
// sub_172320 -> sub_164BB0 after a pending port volume is applied (sceAudioOutSetVolume
// with (int)(+52 * 1.0 * 32768)); otherwise the port mixer sub_170790 -> sub_1710D0 runs.
void PssSystem::bgm_grain(uint8_t* buf, uint32_t bytes, uint32_t* filled) {
  PssPort& p = bgm_port_;
  *filled = 0;
  if (p.attached.load() != 0) {
    if (p.active.load() == 0 && (p.flags120 & 0x100000u) == 0) {
      usleep_us(100);
      return;
    }
  } else if ((p.flags120 & 0x100000u) != 0) {
    usleep_us(500);
    return;
  }
  PssVoice* dv = p.direct.load();
  if (dv != nullptr) {
    if (p.vol_dirty.exchange(0) != 0) {
      const int32_t v = static_cast<int32_t>(p.volume.load() * 1.0f * 32768.0f);
      p.out->set_volume(v);
      log::info("psm.audio", "BGM port volume {} (sceAudioOutSetVolume) at BGM grain {}", v, p.out->grains());
    }
    // sub_172320: the system runs; an inactive or paused player gives nothing (100 us).
    if (!running_.load()) return;
    if (!dv->active.load() || dv->paused.load()) {
      usleep_us(100);
      return;
    }
    if (!dv->ring) {
      host::note_unimplemented("libScePsm PSS direct player without a stream buffer (sub_172320 circular-buffer path) not reproduced");
      usleep_us(100);
      return;
    }
    uint32_t frames = 0;
    if (direct_copy(*dv, bytes, buf, p.granularity, &frames) == 0) *filled = frames;
    return;
  }
  // sub_170790 -> sub_1710D0: the port's voices mixed into a zeroed grain.
  if (!running_.load()) return;
  if (bytes != p.grain_bytes) {
    usleep_us(100);
    return;
  }
  std::memset(buf, 0, bytes);
  bool any = false;
  for (uint32_t i = 0; i < voice_count_; ++i) {
    PssVoice& v = voices_[i];
    if (!v.active.load() || v.paused.load() || v.port != &p) continue;
    if (v.dying.load()) continue;  // sub_163C10 refuses a dying voice
    if (v.ring && v.ring->state.load() >= 5) v.ring->state = 0;
    if (v.primed.load() != 0) {
      any = mix(v, reinterpret_cast<float*>(buf)) == 0;
    } else if (v.dec) {
      request_decode(v);
    }
  }
  if (any) {
    *filled = p.granularity;
  } else {
    usleep_us(100);
  }
}

// sub_170C80 (grain mixed straight into the port's float buffer — the port grain equals
// +4396 — so the system master volume conversion, sys +4400, is not involved). The end of
// voices is handled here for every port; only the main port's voices are mixed.
void PssSystem::frame(float* out) {
  std::memset(out, 0, size_t(grain_) * out_ch_ * 4);
  for (uint32_t i = 0; i < voice_count_; ++i) {
    PssVoice& v = voices_[i];
    if (v.dying.load() && !v.active.load() && v.busy.load() <= 0) {  // sub_166140
      free_voice(i);
    } else if (v.active.load() && !v.paused.load()) {
      // sub_163C10: a flushed stream buffer becomes new again (state 0).
      if (v.dying.load()) {
        deactivate(v);
        v.paused = false;
        continue;
      }
      if (v.ring) {
        if (v.ring->state.load() >= 5) v.ring->state = 0;
      } else if (v.primed.load() == 0) {
        v.primed = 1;
      }
      if (v.port == &main_port_) {
        if (v.primed.load() != 0) {
          if (v.mix == 1) mix(v, out);
        } else if (v.dec) {
          request_decode(v);
        }
      }
    } else if (v.eof_decode.load() && v.eof_ring.load()) {  // sub_162370
      std::unique_lock<std::mutex> l(voice_lock_, std::try_to_lock);  // sub_18C6D0
      if (l.owns_lock()) {
        if (v.auto_rewind) {  // sub_1661A0
          if (!v.stopping.load()) {
            v.paused = false;  // sub_161C40
            v.active = false;
            seek(v, 0, true);  // sub_161EF0(voice, 0, 1)
          }
          v.working = false;
          v.primed = 0;
        } else {
          v.working = false;
          v.primed = 0;
          v.dying = true;  // sub_166160
          v.paused = false;
        }
      }
    } else if (!v.active.load()) {
      v.working = false;
      v.primed = 0;
    }
  }
}

// sub_172A20: a voice handle stops and frees the voice (sub_1621A0 waits up to 100 ms for
// the mixer to let go, a busy voice is marked dying and freed by the mixer); a resource
// handle releases the resource — at once when no voice uses it, else after its voices
// (marked dying) are gone (command 7).
uint32_t PssSystem::release_handle(uint32_t h) {
  const uint32_t slot = h >> 16;
  if (slot < voice_count_ && voices_[slot].gen == static_cast<uint16_t>(h)) {
    PssVoice& v = voices_[slot];
    uint32_t r = 0;
    if (!v.dying.load()) {  // sub_1621A0
      v.active = false;
      v.stopping = true;
      uint32_t n = 0;
      while (v.working.load()) {
        usleep_us(100);
        if (++n == 1000) break;
      }
    }
    if (v.active.load() || v.busy.load() > 0) {  // sub_166120
      v.dying = true;
      v.paused = false;
    } else {
      free_voice(slot);
    }
    return r;
  }
  PssData* d = resource(h);
  if (!d) return kBadHandle;
  bool in_use = false;
  for (uint32_t i = 0; i < voice_count_; ++i) {
    PssVoice& v = voices_[i];
    if (v.res_handle != h || v.gen == 0) continue;
    if (!v.allocated) continue;
    if (v.dying.load()) {  // sub_166170
      ++v.dying_count;
      in_use = true;
    } else if (v.active.load() || v.busy.load() > 0) {
      v.dying = true;
      v.paused = false;
      in_use = true;
    } else {
      free_voice(i);
    }
  }
  if (in_use) {
    d->mark_releasing();
    if (!stream_started_.load()) {
      if (!d->can_free()) {
        usleep_us(2000);
        d->mark_releasing();
      }
      if (!d->can_free()) ensure_stream_thread();
    }
    if (!d->can_free()) {
      QueueEntry e;
      e.cmd = 7;
      e.arg = h;
      post(e);
      return 0;
    }
  }
  free_resource(h);
  return 0;
}

// sub_162390: frames -> ms as (u64)((float)frames / (float)rate * 1000.0f).
uint64_t frames_to_ms(const PssVoice& v, uint64_t frames) {
  const float rate = v.data ? static_cast<float>(int64_t(v.data->info.rate())) : 0.0f;
  return to_u64(u64_to_f(frames) / rate * 1000.0f);
}

}  // namespace

// ---- C API (sub_15DD50..., the wrappers sub_184660.. / sub_183C60.. call) -----------------

uint32_t pss_resource_create(std::shared_ptr<const std::vector<uint8_t>> image, uint16_t flags, uint32_t* handle) {
  if (!image || image->empty() || !handle) return kArgument;  // sub_15DD50
  return PssSystem::get().resource_create(std::move(image), flags, handle);
}

uint32_t pss_resource_release(uint32_t handle) { return PssSystem::get().release_handle(handle); }

uint32_t pss_resource_info(uint32_t handle, PssResourceInfo* info) {
  if (!info) return kArgument;
  PssData* d = PssSystem::get().resource(handle);
  if (!d) return kBadHandle;
  info->rate = d->info.rate();
  info->channels = d->info.channels();
  info->bits = d->info.bits();
  info->bitrate = d->info.bitrate();
  info->format = d->info.type();
  return 0;
}

uint32_t pss_player_create(uint32_t resource, uint32_t flags, int32_t user_id, int32_t app_module, uint32_t* handle) {
  if (!handle) return kArgument;
  return PssSystem::get().player_create(resource, flags, user_id, app_module, handle);
}

uint32_t pss_player_destroy(uint32_t handle) { return PssSystem::get().release_handle(handle); }

// sub_15EB20: the voice must have usable data; then sub_161C90(voice, 1, 0).
uint32_t pss_player_play(uint32_t handle) {
  auto& s = PssSystem::get();
  PssVoice* v = s.voice(handle);
  if (!v) return kBadHandle;
  if (!v->data) return kDying;
  if (!v->data->usable()) return kDying;
  if (s.play_stop(*v, true, 0) != 0) return kBadHandle;
  log::info("psm.audio", "play voice {:#x} resource {:#x} ({}) at output grain {} (video frame {})", handle,
            v->res_handle, v->port == &s.bgm_port_ ? "BGM port" : "main port", s.grains(),
            gfx::Core::get().frame_index());
  return 0;
}

uint32_t pss_player_stop(uint32_t handle, uint16_t fade_frames) {
  auto& s = PssSystem::get();
  PssVoice* v = s.voice(handle);
  if (!v) return kBadHandle;
  return s.play_stop(*v, false, fade_frames) == 0 ? 0 : kBadHandle;
}

// sub_15F140: playing = the voice is active and its port's output runs.
uint32_t pss_player_is_playing(uint32_t handle, int32_t* playing) {
  auto& s = PssSystem::get();
  PssVoice* v = s.voice(handle);
  if (!v) return kBadHandle;
  *playing = v->active.load() && s.output_running(*v) ? 1 : 0;
  return 0;
}

// sub_15EDA0 -> sub_162210(voice, 1).
uint32_t pss_player_pause(uint32_t handle) {
  auto& s = PssSystem::get();
  PssVoice* v = s.voice(handle);
  if (!v) return kBadHandle;
  s.pause(*v, true);
  return 0;
}

// sub_15EDE0 -> sub_1622A0: +72.
uint32_t pss_player_is_paused(uint32_t handle, int32_t* paused) {
  auto& s = PssSystem::get();
  PssVoice* v = s.voice(handle);
  if (!v) return kBadHandle;
  *paused = v->paused.load() ? 1 : 0;
  return 0;
}

// sub_15EE30 -> sub_163CC0: the port driver's +192 for the voice's port.
uint32_t pss_player_port_status(uint32_t handle, uint32_t* status) {
  auto& s = PssSystem::get();
  PssVoice* v = s.voice(handle);
  if (!v) return kBadHandle;
  return s.port_status(v->port, status);
}

// sub_15DDF0: the BGM port's driver +192; 0 and 0x80B3F012 without a BGM port.
uint32_t pss_bgm_port_status(uint32_t* status) {
  auto& s = PssSystem::get();
  if (s.has_bgm_port_ == 0) {
    *status = 0;
    return kNotReady;
  }
  return s.port_status(&s.bgm_port_, status);
}

uint32_t pss_player_set_volume(uint32_t handle, float volume) {
  auto& s = PssSystem::get();
  PssVoice* v = s.voice(handle);
  return v ? s.set_volume(*v, volume) : kBadHandle;
}

uint32_t pss_player_set_pan(uint32_t handle, float pan) {
  auto& s = PssSystem::get();
  PssVoice* v = s.voice(handle);
  return v ? s.set_pan(*v, pan) : kBadHandle;
}

uint32_t pss_player_set_rate(uint32_t handle, float rate) {
  auto& s = PssSystem::get();
  PssVoice* v = s.voice(handle);
  return v ? s.set_rate(*v, rate) : kBadHandle;
}

uint32_t pss_player_set_loop(uint32_t handle, bool loop) {
  auto& s = PssSystem::get();
  PssVoice* v = s.voice(handle);
  if (!v) return kBadHandle;
  s.set_loop(*v, loop);
  return 0;
}

// sub_15EF30 -> sub_162390(voice, +96): mixed frames / rate * 1000.
uint32_t pss_player_position(uint32_t handle, uint64_t* ms) {
  auto& s = PssSystem::get();
  PssVoice* v = s.voice(handle);
  if (!v) return kBadHandle;
  *ms = frames_to_ms(*v, v->consumed);
  return 0;
}

// sub_15EEA0: sub_162420 (ms -> frames: (u64)((float)(ms * rate) / 1000.0f)), then sub_1622B0.
uint32_t pss_player_set_position(uint32_t handle, uint64_t ms) {
  auto& s = PssSystem::get();
  PssVoice* v = s.voice(handle);
  if (!v) return kBadHandle;
  const uint64_t rate = v->data ? v->data->info.rate() : 0;
  const uint64_t frames = to_u64(u64_to_f(ms * rate) / 1000.0f);
  return s.set_position(*v, frames);
}

// sub_15ECF0 -> sub_1645C0: the decoded total once the decoder reached the end, else the
// length the parser stored (+48).
uint32_t pss_player_length(uint32_t handle, uint64_t* ms) {
  auto& s = PssSystem::get();
  PssVoice* v = s.voice(handle);
  if (!v) return kBadHandle;
  if (!v->data) return 0;
  if (v->total != 0 && v->eof_decode.load()) {
    float rate;
    {
      std::lock_guard<std::mutex> l(v->params);
      rate = v->rate;
    }
    const float x = u64_to_f(v->total) / static_cast<float>(int64_t(v->data->info.rate())) * 1000.0f / rate;
    *ms = to_u64(std::ceil(x));
    v->data->info.set_length(*ms);
  } else {
    *ms = v->data->info.length();
  }
  return 0;
}

// sub_15F010: the loop start (+120) and end (sub_163AB0) in ms (sub_162390).
uint32_t pss_player_loop_positions(uint32_t handle, uint64_t* start_ms, uint64_t* end_ms) {
  auto& s = PssSystem::get();
  PssVoice* v = s.voice(handle);
  if (!v) return kBadHandle;
  *start_ms = frames_to_ms(*v, v->loop_start);
  *end_ms = frames_to_ms(*v, s.loop_end_of(*v));
  return 0;
}

}  // namespace ssn::psm::audio
