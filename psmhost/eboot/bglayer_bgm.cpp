// "ShellUIBackgroundLayer" sound P/Invokes and the eboot's NGS2 Music System — see bglayer_bgm.h.
//
// Objects (eboot layouts):
//   BgmMgr (64 bytes, sub_8F1B0): +0 mutex "SceBGLayerBgmMgr", +8 MusicSystem, +0x10.. vector of
//     Sound* (begin +0x18, end +0x20), +48 initialised, +49 volume ramp pending, +52 atomic "update
//     wanted", +56 libSceNgs2 module loaded.
//   MusicSystem (136 bytes, sub_8FE40 / base sub_90D80, vtable 0xCC8D48): +8 allocator {+8 alloc
//     sub_90E00 (malloc), +16 free sub_90E30, +24 user}, +32 system, +40 mastering rack, +48 sampler
//     rack, +56 mastering voice, +64 next sampler voice index, +68 initialised, +72 audio thread
//     object (vtable 0xCC8D98: +80 running, +81 stop, +88 thread, +96 port type, +100 port, +104
//     rate, +108 channels, +112 grain, +120 render callback sub_90FA0, +128 its argument).
//   Waveform (288 bytes, sub_90A00): +0 SceNgs2WaveformInfo, +232 file data (malloc), +240 size,
//     +248 path.
//   SamplerVoice (32 bytes, sub_90070): +0 Waveform*, +8 voice handle, +16 0, +24 active, +28 volume.
//   Sound (32 bytes, sub_8FC70): +0 SamplerVoice*, +8 Waveform*, +16 volume, +20 target, +24 time
//     left (s) of the volume ramp.
// Ids handed to the managed side are vector index + 1.
#include "eboot/bglayer_bgm.h"

#include <atomic>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <memory>
#include <mutex>
#include <string>
#include <vector>

#include "core/firmware.h"
#include "eboot/bglayer.h"
#include "gfx/vk_device.h"
#include "core/log.h"
#include "host/boundary.h"
#include "host/context.h"
#include "libScePsm/audio/pss_audio_out.h"
#include "libSceNgs2/ngs2.h"

namespace ssn::eboot::bglayer::bgm {
namespace {

namespace ngs = ssn::ngs2;

// "[BGLayer Audio][E] " + message (eboot printf / puts).
template <class... A>
void audio_error(std::format_string<A...> f, A&&... a) {
  log::error("bglayer", "[BGLayer Audio][E] {}", std::format(f, std::forward<A>(a)...));
}

// The allocator handlers of the music system (sub_90E00: buffer = malloc(size), 0x804A8024 when it
// fails; sub_90E30: free(buffer)).
int32_t alloc_handler(ngs::ContextBufferInfo* i) {
  i->host_buffer = std::malloc(i->host_buffer_size);
  return i->host_buffer ? 0 : static_cast<int32_t>(0x804A8024u);
}
int32_t free_handler(ngs::ContextBufferInfo* i) {
  std::free(i->host_buffer);
  return 0;
}

// The audio thread object (+72; sub_91050 start, sub_915A0 body, sub_90F50 / sub_90E40 stop).
//
// HOST VALUE: the thread's sceAudioOut port — sceAudioOutOpen(SCE_USER_SERVICE_USER_ID_SYSTEM, MAIN,
// 0, 512-frame grains, 48 kHz, FLOAT_8CH) with sceAudioOutSetVolume 0x8000 on all 8 channels — is a
// host audio-out port (psm::audio::PssAudioOut "ngs2": the default WASAPI render device, its own
// stream; SSN_AUDIO_DUMP writes <dump>.ngs2.wav). The port object runs the thread: each grain it calls
// the render callback (sub_90FA0 -> sceNgs2SystemRender into the grain) and blocks in the port's
// output as sceAudioOutOutput does.
class AudioThread {
 public:
  ~AudioThread() { stop(); }
  // sub_91050(this, type, rate, channels, grain, callback, arg).
  int32_t start(uint32_t type, uint32_t rate, uint32_t channels, uint32_t grain, ngs::SystemHandle system) {
    stop();
    type_ = type;
    rate_ = rate;
    channels_ = channels;
    grain_ = grain;
    system_ = system;
    // sceAudioOutInit (import JfEPXVxhFqA): the host's audio out needs no init (0).
    static std::once_flag once;
    std::call_once(once, [] {
      host::note_unimplemented(
          "HOST VALUE eboot BGLayer audio port (sceAudioOutOpen SYSTEM user, MAIN, 512 frames, 48 kHz, FLOAT_8CH) -> "
          "host WASAPI port 'ngs2'");
    });
    port_ = std::make_unique<psm::audio::PssAudioOut>();
    psm::audio::PortFormat f;
    f.rate = rate;
    f.channels = channels;
    f.bits = 32;
    f.grain = grain;
    port_->set_volume(0x8000);  // b_uAV89IlxE(port, mask, {0x8000 x channels})
    port_->start(f,
                 [this](uint8_t* buf, uint32_t bytes, uint32_t* filled) {
                   render(buf, bytes);  // sub_90FA0, then QOQtbeDqsT4 (sceAudioOutOutput)
                   *filled = grain_;
                 },
                 "ngs2");
    running_ = true;
    return 0;
  }
  // +81 = 1, scePthreadJoin(+88): the loop ends, sceAudioOutOutput(port, NULL), sceAudioOutClose.
  void stop() {
    if (!running_) return;
    port_->close();
    port_.reset();
    running_ = false;
  }

 private:
  // sub_90FA0(channels, buffer, bytes, system): sceNgs2SystemRender(system, {buffer, bytes, 24, channels}, 1).
  void render(uint8_t* buf, uint32_t bytes) {
    ngs::RenderBufferInfo rb{buf, bytes, 24, channels_};
    const int32_t r = ngs::SystemRender(system_, &rb, 1);
    if (r < 0) audio_error("Failed sceNgs2SystemRender() {}", r);
  }
  uint32_t type_ = 0, rate_ = 0, channels_ = 0, grain_ = 0;
  ngs::SystemHandle system_ = nullptr;
  std::unique_ptr<psm::audio::PssAudioOut> port_;
  bool running_ = false;
};

struct Waveform {  // 288 bytes (sub_90A00)
  ngs::WaveformInfo info{};  // +0
  std::vector<uint8_t> data; // +232 / +240
  std::string path;          // +248
  bool loaded = false;
};

struct SamplerVoice {  // 32 bytes (sub_90070)
  Waveform* waveform = nullptr;   // +0
  ngs::VoiceHandle handle = nullptr;  // +8
  uint8_t active = 0;             // +24
  float volume = 1.0f;            // +28
};

struct Sound {  // 32 bytes (sub_8FC70)
  std::unique_ptr<SamplerVoice> voice;  // +0
  std::unique_ptr<Waveform> waveform;   // +8
  float volume = 1.0f;                  // +16
  float target = 1.0f;                  // +20
  float time_left = 0.0f;               // +24
};

class MusicSystem {  // 136 bytes (sub_8FE40)
 public:
  ~MusicSystem() { finalize(); }  // sub_90E40

  // vtable +16, sub_8FEA0.
  int32_t initialize() {
    if (initialized_) return 0;
    int32_t r;
    ngs::SystemOption opt{};
    ngs::SystemResetOption(&opt);  // AQkj7C0f3PY
    opt.num_grain_samples = 512;   // v31 = 512 (option +0x70)
    // sub_911A0
    r = ngs::SystemCreateWithAllocator(&opt, &allocator_, &system_);
    if (r < 0) {
      audio_error("Failed sceNgs2SystemCreateWithAllocator(): {}", r);
      return fail(r);
    }
    // sub_911F0: the mastering rack option at 0xBB0AE0 (0xB8 bytes) from the eboot.
    const auto mopt = eboot_bytes(0xBB0AE0, 0xB8);
    const auto sopt = eboot_bytes(0xBB0B98, 0xD8);
    if (mopt.size() != 0xB8 || sopt.size() != 0xD8) {
      log::error("bglayer", "NGS2 rack options (eboot 0xBB0AE0 / 0xBB0B98) not readable");
      return fail(-1);
    }
    r = ngs::RackCreateWithAllocator(system_, ngs::kRackMastering, mopt.data(), &allocator_, &mastering_);
    if (r < 0) {
      audio_error("Failed sceNgs2RackCreateWithAllocator(SCE_NGS2_RACK_ID_MASTERING): {}", r);
      return fail(r);
    }
    // sub_91250: the sampler rack option at 0xBB0B98 (0xD8 bytes).
    r = ngs::RackCreateWithAllocator(system_, ngs::kRackSampler, sopt.data(), &allocator_, &sampler_);
    if (r < 0) {
      audio_error("Failed sceNgs2RackCreateWithAllocator(SCE_NGS2_RACK_ID_SAMPLER): {}", r);
      return fail(r);
    }
    // sub_912B0(this, 8)
    r = setup_mastering(8);
    if (r < 0) return fail(r);
    // sub_91020(this, 0, 48000, 8, 512)
    r = thread_.start(0, 48000, 8, 512, system_);
    if (r < 0) return fail(r);
    initialized_ = true;
    return 0;
  }

  // sub_90070: a sampler voice playing `w`, patched to the mastering voice; null on failure.
  std::unique_ptr<SamplerVoice> create_voice(Waveform* w) {
    ngs::VoiceHandle h = nullptr;
    if (get_sampler_voice(&h) < 0) return nullptr;
    // sampler setup {0x28, 0x10000000}: the format (info +0..+23, or unk_BB0C70 zeros), flags 0
    struct alignas(8) {
      ngs::VoiceParamHead head;
      uint8_t format[24];
      uint32_t flags;
      uint32_t pad;
    } setup{{0x28, 0, 0x10000000}, {}, 0, 0};
    if (w) std::memcpy(setup.format, &w->info, 24);
    int32_t r = ngs::VoiceControl(h, &setup.head);
    if (r < 0) {
      audio_error("Failed sceNgs2VoiceSetup() for sampler voice: {}", r);
      return nullptr;
    }
    struct alignas(8) {
      ngs::VoiceParamHead head;
      uint32_t port, input;
      void* dest;
    } patch{{24, 0, 5}, 0, 0, mastering_voice_};
    r = ngs::VoiceControl(h, &patch.head);
    if (r < 0) {
      audio_error("Failed sceNgs2VoicePatch() from sampler voice to mastering voice: {}", r);
      return nullptr;
    }
    auto v = std::make_unique<SamplerVoice>();
    v->waveform = w;
    v->handle = h;
    v->volume = 1.0f;
    // waveform blocks {0x20, 0x10000001}: data (+232), flags 0, numBlocks (+68), blocks (+72)
    struct alignas(8) {
      ngs::VoiceParamHead head;
      const void* data;
      uint32_t flags, num_blocks;
      const void* blocks;
    } blocks{{0x20, 0, 0x10000001}, w->data.data(), 0, w->info.num_blocks, w->info.blocks};
    ngs::VoiceControl(h, &blocks.head);  // result not checked (0x902CE)
    v->active = 1;
    return v;
  }

  int32_t finalize() {  // sub_90E40 / sub_90EF0
    if (!initialized_) {
      destroy_handles();
      return 0;
    }
    thread_.stop();
    destroy_handles();
    initialized_ = false;
    return 0;
  }

 private:
  int32_t fail(int32_t r) {
    // "Failed to Initialize NGS2 Music System" (puts), then sub_90EF0 destroys what exists.
    log::error("bglayer", "[BGLayer Audio][E] Failed to Initialize NGS2 Music System");
    destroy_handles();
    return r;
  }
  void destroy_handles() {  // sub_90EF0: sampler rack, mastering rack, system
    if (sampler_) ngs::RackDestroy(sampler_, nullptr);
    sampler_ = nullptr;
    if (mastering_) ngs::RackDestroy(mastering_, nullptr);
    mastering_ = nullptr;
    if (system_) ngs::SystemDestroy(system_, nullptr);
    system_ = nullptr;
  }
  // sub_912B0(this, channels): the mastering voice (index 0) set up with `channels` and started.
  int32_t setup_mastering(uint32_t channels) {
    int32_t r = ngs::RackGetVoiceHandle(mastering_, 0, &mastering_voice_);
    if (r < 0) {
      audio_error("Failed sceNgs2RackGetVoiceHandle() for mastering voice: {}", r);
      return r;
    }
    struct alignas(8) {
      ngs::VoiceParamHead head;
      uint32_t channels, flags;
    } setup{{0x10, 0, 0x30000000}, channels, 0};
    ngs::VoiceControl(mastering_voice_, &setup.head);  // result not checked
    ngs::VoiceCommand play{};
    play.id = 2;
    play.type = 4;
    play.value.u32 = 1;  // {0x00000002, 0x400, 1}: SCE_NGS2_VOICE_EVENT_PLAY
    r = ngs::VoiceRunCommands(mastering_voice_, &play, 1);
    if (r < 0) {
      audio_error("Failed sceNgs2VoiceKickEvent(SCE_NGS2_VOICE_EVENT_PLAY) for mastering voice: {}", r);
      return r;
    }
    return 0;
  }
  // sub_913B0: the sampler voice at the rotating index (+64 = (+64 + 1) mod 255).
  int32_t get_sampler_voice(ngs::VoiceHandle* out) {
    ngs::VoiceHandle h = nullptr;
    const int32_t r = ngs::RackGetVoiceHandle(sampler_, next_voice_, &h);
    if (r < 0) {
      audio_error("Failed sceNgs2RackGetVoiceHandle for sampler voice: {}", r);
      *out = nullptr;
      return r;
    }
    next_voice_ = next_voice_ - 255 * ((next_voice_ + 1) / 255) + 1;
    *out = h;
    return 0;
  }

  ngs::BufferAllocator allocator_{alloc_handler, free_handler, nullptr};
  ngs::SystemHandle system_ = nullptr;      // +32
  ngs::RackHandle mastering_ = nullptr;     // +40
  ngs::RackHandle sampler_ = nullptr;       // +48
  ngs::VoiceHandle mastering_voice_ = nullptr;  // +56
  uint32_t next_voice_ = 0;                 // +64
  bool initialized_ = false;                // +68
  AudioThread thread_;                      // +72
};

// sub_90A00: the file read into memory and parsed.
std::unique_ptr<Waveform> load_waveform(const char* path) {
  auto w = std::make_unique<Waveform>();
  w->path = path ? path : "";
  // open(path, O_RDONLY) through the console file system (the firmware roots on the host).
  std::string host;
  if (Firmware* fw = host::ctx().firmware) host = fw->host_path(w->path);
  FILE* f = host.empty() ? nullptr : std::fopen(host.c_str(), "rb");
  if (!f) return w;  // +232 stays 0
  std::fseek(f, 0, SEEK_END);  // lseek(fd, 0, SEEK_END)
  const long size = std::ftell(f);
  std::fseek(f, 0, SEEK_SET);
  if (size >= 0) {
    w->data.resize(static_cast<size_t>(size));
    const size_t got = std::fread(w->data.data(), 1, w->data.size(), f);
    (void)got;  // read() < 0 is the only failure the eboot checks
    w->loaded = true;
  }
  std::fclose(f);
  if (w->loaded) {
    const int32_t r = ngs::ParseWaveformData(w->data.data(), w->data.size(), &w->info);
    if (r < 0) {
      audio_error("Failed sceNgs2ParseWaveformData(): {}", r);
      w->data.clear();
      w->loaded = false;
    }
  }
  return w;
}

// sub_903A0: a sampler voice that was set up is killed ({2, 0x400, 8}).
void destroy_voice(SamplerVoice& v) {
  if (!v.active) return;
  ngs::VoiceCommand c{};
  c.id = 2;
  c.type = 4;
  c.value.u32 = 8;
  ngs::VoiceRunCommands(v.handle, &c, 1);
}

class BgmMgr {  // 64 bytes (sub_8F1B0)
 public:
  // sub_8F350
  int32_t initialize() {
    std::lock_guard<std::mutex> l(lock_);
    if (initialized_) {
      finalize_locked();
      return -1;
    }
    if (!module_loaded_) {
      // sceSysmoduleLoadModule(SCE_SYSMODULE_NGS2 = 11): libSceNgs2.native is part of the host.
      module_loaded_ = true;
    }
    system_ = std::make_unique<MusicSystem>();
    if (system_->initialize() < 0) {
      finalize_locked();
      return -1;
    }
    initialized_ = true;
    log::info("bglayer", "BGM initialised: NGS2 music system up (frame {})", ssn::gfx::device().frames_presented());
    return 0;
  }
  int32_t finalize() {  // sub_8F270
    std::lock_guard<std::mutex> l(lock_);
    if (initialized_) log::info("bglayer", "BGM finalized (frame {})", ssn::gfx::device().frames_presented());
    return finalize_locked();
  }
  // sub_8F520(path, *id)
  int32_t load(const char* path, int32_t* id) {
    std::lock_guard<std::mutex> l(lock_);
    if (id) *id = 0;
    if (!initialized_) return -1;
    auto s = std::make_unique<Sound>();
    // sub_8FC70 -> sub_90300 (waveform; destroyed when it did not load) -> sub_90070
    auto w = load_waveform(path);
    if (w->loaded) {
      s->waveform = std::move(w);
      s->voice = system_->create_voice(s->waveform.get());
    }
    if (!s->voice) {
      if (s->waveform) s->waveform.reset();  // sub_90C80 + free
      return -1;
    }
    size_t slot = 0;
    while (slot < sounds_.size() && sounds_[slot]) ++slot;
    if (slot >= sounds_.size()) sounds_.push_back(nullptr);
    sounds_[slot] = std::move(s);
    if (id) *id = static_cast<int32_t>(slot) + 1;
    log::info("bglayer", "sound {} loaded {} (frame {})", slot + 1, path ? path : "", ssn::gfx::device().frames_presented());
    return 0;
  }
  int32_t unload(int32_t id) {  // sub_8F770
    std::lock_guard<std::mutex> l(lock_);
    Sound* s = find(id);
    if (!s && !valid(id)) return -1;
    if (s) {
      if (s->voice) destroy_voice(*s->voice);
      sounds_[id - 1].reset();
      log::info("bglayer", "sound {} unloaded (frame {})", id, ssn::gfx::device().frames_presented());
    }
    return 0;
  }
  int32_t play(int32_t id, bool play) {  // sub_8F830 -> sub_90400 (PLAY) / sub_90580 (STOP)
    std::lock_guard<std::mutex> l(lock_);
    Sound* s = find(id);
    if (!s) return -1;
    ngs::VoiceCommand c{};
    c.id = 2;
    c.type = 4;
    c.value.u32 = play ? 1 : 2;
    const int32_t r = ngs::VoiceRunCommands(s->voice->handle, &c, 1);
    if (!play && r < 0) audio_error("Failed sceNgs2VoiceKickEvent(SCE_NGS2_VOICE_EVENT_STOP) {}", r);
    // Host diagnostic (no console counterpart): when each sound starts / stops, for the boot timeline.
    log::info("bglayer", "sound {} {} {} (frame {})", id, play ? "PLAY" : "STOP", s->waveform ? s->waveform->path : "",
              ssn::gfx::device().frames_presented());
    return 0;
  }
  int32_t set_volume(int32_t id, float volume, float delay) {  // sub_8F8C0
    std::lock_guard<std::mutex> l(lock_);
    Sound* s = find(id);
    if (!s) return -1;
    s->target = volume;
    s->time_left = delay;
    if (!(delay > 0.0f)) {
      s->volume = volume;
      voice_volume(*s->voice, volume);
    }
    pending_ = true;
    update_wanted_.store(1);
    return 0;
  }
  int32_t status(int32_t id) {  // sub_8F980 -> sub_90610
    std::lock_guard<std::mutex> l(lock_);
    Sound* s = find(id);
    if (!s || !s->voice) return -1;
    uint32_t flags = 0;
    const int32_t r = ngs::VoiceGetStateFlags(s->voice->handle, &flags);
    if (r < 0) return r;
    if (r > 0) return -1;
    return static_cast<int32_t>(flags);
  }
  int32_t position(int32_t id, int64_t* pos) {  // sub_8FA30 -> sub_90710 (returns 0, writes nothing)
    std::lock_guard<std::mutex> l(lock_);
    Sound* s = find(id);
    if (!s || !pos || !s->voice) return -1;
    return 0;
  }
  int32_t length(int32_t id, int64_t* len) {  // sub_8FAC0 -> sub_90720
    std::lock_guard<std::mutex> l(lock_);
    Sound* s = find(id);
    if (!s || !len || !s->voice) return -1;
    const Waveform* w = s->voice->waveform;
    if (!w) return -1;
    *len = w->info.sample_rate ? static_cast<int64_t>(1000ull * w->info.num_samples / w->info.sample_rate) : 0;
    return 0;
  }
  int32_t loop_position(int32_t id, int64_t* ts, int64_t* te) {  // sub_8FB50 -> sub_90780
    std::lock_guard<std::mutex> l(lock_);
    Sound* s = find(id);
    if (!s || !s->voice || !ts || !te) return -1;
    const Waveform* w = s->voice->waveform;
    if (!w) {
      *ts = *te = 0;
      return -1;
    }
    if (w->info.sample_rate == 0) {
      *ts = *te = 0;
      return 0;
    }
    const float k = 1.0f / static_cast<float>(w->info.sample_rate);  // dword_AFF918 / rate
    *ts = static_cast<int64_t>(static_cast<uint64_t>(static_cast<float>(w->info.loop_begin) * 1000.0f * k));
    *te = static_cast<int64_t>(static_cast<uint64_t>(static_cast<float>(w->info.loop_end) * 1000.0f * k));
    return 0;
  }
  int32_t dump(int32_t id) {  // sub_8FBF0 (nullsub_9)
    std::lock_guard<std::mutex> l(lock_);
    Sound* s = find(id);
    return s && s->voice && s->waveform ? 0 : -1;
  }
  // sub_8F3E0: the volume ramps of SetBgmVolume(id, v, delay > 0), per frame.
  void update(float frame_ms) {
    if (update_wanted_.load() == 0) return;
    std::lock_guard<std::mutex> l(lock_);
    if (!initialized_ || !pending_) return;
    pending_ = false;
    update_wanted_.store(0);
    const float dt = frame_ms * 0.001f;  // dword_B00144
    for (auto& sp : sounds_) {
      Sound* s = sp.get();
      if (!s) continue;
      const float d = s->time_left;
      if (!(0.0f < d)) continue;
      float left = d - dt;
      if (left > 0.0f) {
        pending_ = true;
        update_wanted_.store(1);
      }
      left = left > 0.0f ? left : 0.0f;  // vmaxss
      s->time_left = left;
      s->volume = s->target + left * (s->volume - s->target) / d;
      voice_volume(*s->voice, s->volume);
    }
  }

 private:
  int32_t finalize_locked() {
    for (auto& s : sounds_)
      if (s && s->voice) destroy_voice(*s->voice);
    sounds_.clear();
    system_.reset();  // vtable +8: deleting destructor
    initialized_ = false;
    pending_ = false;
    update_wanted_.store(0);
    return 0;
  }
  bool valid(int32_t id) const { return id > 0 && initialized_ && static_cast<size_t>(id) <= sounds_.size(); }
  Sound* find(int32_t id) { return valid(id) ? sounds_[id - 1].get() : nullptr; }
  // sub_90670: port 0 volume ({(0 << 24) | 6, 0x100, v}), remembered at +28.
  static void voice_volume(SamplerVoice& v, float vol) {
    ngs::VoiceCommand c{};
    c.id = 6;
    c.type = 1;
    c.value.f32 = vol;
    ngs::VoiceRunCommands(v.handle, &c, 1);
    v.volume = vol;
  }

  std::mutex lock_;                          // +0
  std::unique_ptr<MusicSystem> system_;      // +8
  std::vector<std::unique_ptr<Sound>> sounds_;  // +0x18 .. +0x28
  bool initialized_ = false;                 // +48
  bool pending_ = false;                     // +49
  std::atomic<int32_t> update_wanted_{0};    // +52
  bool module_loaded_ = false;               // +56
};

// root +40 (qword_EC0080 -> +40). The manager outlives FinalizeNative (see root_finalized).
std::atomic<BgmMgr*> g_root_mgr{nullptr};

}  // namespace

void create_manager() {
  // A second InitializeNative builds a new root and a new manager (the old one is unreachable).
  g_root_mgr.store(new BgmMgr);
}

void root_finalized() { g_root_mgr.store(nullptr); }

void update(float frame_milliseconds) {
  if (BgmMgr* m = g_root_mgr.load()) m->update(frame_milliseconds);
}

// --- P/Invokes ({name, fn} table 0xCC88F0) -----------------------------------------------------------

// InitializeBgmNative — 0x8E840 -> 0x8F350.
SSN_PINVOKE("ShellUIBackgroundLayer", "InitializeBgmNative", int32_t, ssn_BGLayer_InitializeBgmNative, ()) {
  BgmMgr* m = g_root_mgr.load();
  return m ? m->initialize() : 0;
}
// FinalizeBgmNative — 0x8E860 -> 0x8F270.
SSN_PINVOKE("ShellUIBackgroundLayer", "FinalizeBgmNative", int32_t, ssn_BGLayer_FinalizeBgmNative, ()) {
  BgmMgr* m = g_root_mgr.load();
  return m ? m->finalize() : 0;
}
// LoadBgmNative(string path, out int id) — 0x8E880 -> 0x8F520.
SSN_PINVOKE("ShellUIBackgroundLayer", "LoadBgmNative", int32_t, ssn_BGLayer_LoadBgmNative, (const char* path, int32_t* id)) {
  BgmMgr* m = g_root_mgr.load();
  return m ? m->load(path, id) : 0;
}
// UnloadBgmNative(int id) — 0x8E8B0 -> 0x8F770.
SSN_PINVOKE("ShellUIBackgroundLayer", "UnloadBgmNative", int32_t, ssn_BGLayer_UnloadBgmNative, (int32_t id)) {
  BgmMgr* m = g_root_mgr.load();
  return m ? m->unload(id) : 0;
}
// PlayBgmNative(int id, bool doPlay) — 0x8E8D0 -> 0x8F830.
SSN_PINVOKE("ShellUIBackgroundLayer", "PlayBgmNative", int32_t, ssn_BGLayer_PlayBgmNative, (int32_t id, uint8_t play)) {
  BgmMgr* m = g_root_mgr.load();
  return m ? m->play(id, play != 0) : 0;
}
// SetBgmVolumeNative(int id, float volume, float delay) — 0x8E900 -> 0x8F8C0.
SSN_PINVOKE("ShellUIBackgroundLayer", "SetBgmVolumeNative", int32_t, ssn_BGLayer_SetBgmVolumeNative,
            (int32_t id, float volume, float delay)) {
  BgmMgr* m = g_root_mgr.load();
  return m ? m->set_volume(id, volume, delay) : 0;
}
// GetBgmStatusNative(int id) — 0x8E920 -> 0x8F980.
SSN_PINVOKE("ShellUIBackgroundLayer", "GetBgmStatusNative", int32_t, ssn_BGLayer_GetBgmStatusNative, (int32_t id)) {
  BgmMgr* m = g_root_mgr.load();
  return m ? m->status(id) : -1;
}
// GetBgmPositionNative(int id, out long pos) — 0x8E950 -> 0x8FA30.
SSN_PINVOKE("ShellUIBackgroundLayer", "GetBgmPositionNative", int32_t, ssn_BGLayer_GetBgmPositionNative,
            (int32_t id, int64_t* pos)) {
  BgmMgr* m = g_root_mgr.load();
  return m ? m->position(id, pos) : -1;
}
// GetBgmLengthNative(int id, out long len) — 0x8E980 -> 0x8FAC0.
SSN_PINVOKE("ShellUIBackgroundLayer", "GetBgmLengthNative", int32_t, ssn_BGLayer_GetBgmLengthNative,
            (int32_t id, int64_t* len)) {
  BgmMgr* m = g_root_mgr.load();
  return m ? m->length(id, len) : -1;
}
// GetBgmLoopPositionNative(int id, out long ts, out long te) — 0x8E9B0 -> 0x8FB50.
SSN_PINVOKE("ShellUIBackgroundLayer", "GetBgmLoopPositionNative", int32_t, ssn_BGLayer_GetBgmLoopPositionNative,
            (int32_t id, int64_t* ts, int64_t* te)) {
  BgmMgr* m = g_root_mgr.load();
  return m ? m->loop_position(id, ts, te) : -1;
}
// DumpBgmDataNative(int id) — 0x8E9E0 -> 0x8FBF0.
SSN_PINVOKE("ShellUIBackgroundLayer", "DumpBgmDataNative", int32_t, ssn_BGLayer_DumpBgmDataNative, (int32_t id)) {
  BgmMgr* m = g_root_mgr.load();
  return m ? m->dump(id) : -1;
}

}  // namespace ssn::eboot::bglayer::bgm
