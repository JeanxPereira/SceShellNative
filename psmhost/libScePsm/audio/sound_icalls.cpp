// Sce.PlayStation.Core.Audio.Sound / SoundPlayer icalls (libScePsm.sprx, FW 4.03) over the
// PSS audio system (pss_audio.h). Addresses are the icall functions libScePsm registers.
//
// A Sound (table xmmword_540FF8, object vtable off_49FF68, 40 + 56 bytes) owns a native copy
// of its file image (+24, holder off_4A0A80) and the PSS resource made from it (+40, a
// 4-byte cell holding the PSS handle); its creation result is kept at +48. A SoundPlayer
// (table xmmword_541010, vtable off_49FF88, 40 + 96 bytes) keeps its parameters (+16 volume
// 1.0, +20 pan 0.0, +24 rate 1.0, +28 loop, +32 port flags, +36 user id, +40 app module)
// and creates a PSS player only in Play: every Play stops and destroys the previous one and
// plays a new one from the start.
#include <cstdint>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <unordered_map>
#include <vector>

#include <mono/metadata/object.h>

#include "common/mono_util.h"
#include "core/firmware.h"
#include "core/log.h"
#include "host/boundary.h"
#include "host/context.h"
#include "libScePsm/audio/pss_audio.h"
#include "libScePsm/imaging/psm_common.h"

namespace ssn::psm::audio {
namespace {

constexpr int32_t kAudioSystem = static_cast<int32_t>(0x80B30022u);  // Error AUDIO_SYSTEM (Play / SetPosition)

struct SoundObj {
  std::shared_ptr<const std::vector<uint8_t>> image;  // +24
  uint32_t resource = 0;                              // +40 cell (0 = none)
  int32_t status = 0;                                 // +48
  // sub_1E3900: the last reference releases the PSS resource (sub_1846F0 -> sub_15DE80).
  ~SoundObj() {
    if (resource != 0) pss_resource_release(resource);
  }
};

struct PlayerObj {
  std::shared_ptr<SoundObj> sound;  // +0
  uint32_t pss = 0;                 // +8 (0 = no PSS player)
  float volume = 1.0f;              // +16
  float pan = 0.0f;                 // +20
  float rate = 1.0f;                // +24
  bool loop = false;                // +28
  uint32_t port_flags = 0;          // +32
  int32_t user_id = -1;             // +36
  int32_t app_module = -1;          // +40
};

std::mutex g_lock;  // X6AKrGoJOU0_h_A, shared by both tables
std::unordered_map<uint32_t, std::shared_ptr<SoundObj>> g_sounds;
std::unordered_map<uint32_t, std::shared_ptr<PlayerObj>> g_players;

std::shared_ptr<SoundObj> find_sound(uint32_t h) {
  std::lock_guard<std::mutex> l(g_lock);
  auto it = g_sounds.find(h);
  return it == g_sounds.end() ? nullptr : it->second;
}
std::shared_ptr<PlayerObj> find_player(uint32_t h) {
  std::lock_guard<std::mutex> l(g_lock);
  auto it = g_players.find(h);
  return it == g_players.end() ? nullptr : it->second;
}

// The wrappers' clamps (sub_15DEA0, sub_15DED0, sub_15DE50) before the PSS setters.
void pss_volume(uint32_t p, float v) {
  float x = 0.0f;
  if (!(0.0f > v)) x = v > 10.0f ? 10.0f : v;
  pss_player_set_volume(p, x);
}
void pss_pan(uint32_t p, float v) {
  float x = 0.0f;
  if (!(0.0f > v)) x = v > 1.0f ? 1.0f : v;
  pss_player_set_pan(p, x);
}
void pss_rate(uint32_t p, float v) {
  float x = 0.004f;
  if (!(0.004f > v)) x = v > 256.0f ? 256.0f : v;
  pss_player_set_rate(p, x);
}

// sub_1849A0(player, 64) then wait (10 us steps) while sub_184A40 reports playing; the
// result of the stop decides whether the player is destroyed (sub_184950).
void stop_and_destroy(PlayerObj& st) {
  if (st.pss == 0) return;
  int32_t playing = 1;
  uint32_t r = pss_player_stop(st.pss, 64);
  if (r == 0) {
    for (;;) {
      r = pss_player_is_playing(st.pss, &playing);
      if (playing != 1) break;
      std::this_thread::sleep_for(std::chrono::microseconds(10));
      if (r != 0) break;
    }
  }
  if (r == 0) pss_player_destroy(st.pss);
  st.pss = 0;
}

// Sound::NewFromFileImage / NewFromFilename common part (0x2d5850 / 0x2d5100 after the
// read): RIFF/WAVE (>= 0x2C bytes) or "VAGp" (>= 0x30 bytes), else 0x80B30007; a PSS
// resource from the image (sub_184660 -> sub_15DD50, flags 0), failure -> 0x80B30006.
int32_t create_sound(std::shared_ptr<const std::vector<uint8_t>> image, uint32_t* handle) {
  auto s = std::make_shared<SoundObj>();
  s->image = image;
  const auto& d = *image;
  const bool wav = d.size() >= 0x2C && std::memcmp(d.data(), "RIFF", 4) == 0 && std::memcmp(d.data() + 8, "WAVE", 4) == 0;
  const bool vag = d.size() >= 0x30 && std::memcmp(d.data(), "VAGp", 4) == 0;
  if (!wav && !vag) {
    s->status = static_cast<int32_t>(0x80B30007u);
  } else {
    uint32_t res = 0;
    if (pss_resource_create(image, 0, &res) != 0) {
      s->status = static_cast<int32_t>(0x80B30006u);
    } else {
      s->resource = res;
    }
  }
  if (s->status < 0) return s->status;
  std::lock_guard<std::mutex> l(g_lock);
  const uint32_t h = generate_handle([](uint32_t k) { return g_sounds.count(k) != 0; });
  g_sounds[h] = s;
  *handle = h;
  return 0;
}

// NewFromFileImage(byte[], out int) — 0x2d5850: the bytes are copied into a native buffer.
int32_t NewFromFileImage(MonoArray* file_image, uint32_t* handle) {
  if (!file_image || !handle) return static_cast<int32_t>(0x80B30002u);
  const auto* p = reinterpret_cast<const uint8_t*>(mono_array_addr_with_size(file_image, 1, 0));
  auto image = std::make_shared<const std::vector<uint8_t>>(p, p + mono_array_length(file_image));
  return create_sound(std::move(image), handle);
}
SSN_ICALL("Sce.PlayStation.Core.Audio.Sound::NewFromFileImage", NewFromFileImage);

// NewFromFilename(string, out int) — 0x2d5100: the console path is opened through the PSS
// file layer (sub_17C900, failure -> 0x80B30011) and read whole (short read -> 0x80B30010).
int32_t NewFromFilename(MonoString* filename, uint32_t* handle) {
  if (!filename || !handle) return static_cast<int32_t>(0x80B30002u);
  const std::string name = ssn::psm::utf8(filename);
  std::string path = name;
  if (Firmware* fw = host::ctx().firmware) path = fw->host_path(name);
  std::ifstream f(std::filesystem::path(path), std::ios::binary);
  if (!f) return static_cast<int32_t>(0x80B30011u);
  auto image = std::make_shared<const std::vector<uint8_t>>(std::istreambuf_iterator<char>(f), std::istreambuf_iterator<char>());
  return create_sound(std::move(image), handle);
}
SSN_ICALL("Sce.PlayStation.Core.Audio.Sound::NewFromFilename", NewFromFilename);

// ReleaseNative(int) — 0x2d5ba0: drops the table reference; the object (sub_1E3900)
// releases its PSS resource (sub_1846F0 -> sub_15DE80) when the last reference goes.
void SoundReleaseNative(uint32_t handle) {
  std::shared_ptr<SoundObj> s;
  {
    std::lock_guard<std::mutex> l(g_lock);
    auto it = g_sounds.find(handle);
    if (it == g_sounds.end()) return;
    s = std::move(it->second);
    g_sounds.erase(it);
  }
}
SSN_ICALL("Sce.PlayStation.Core.Audio.Sound::ReleaseNative", SoundReleaseNative);

// The player constructors (0x2d5c90 / 0x2d5f10 / 0x2d61c0 / 0x2d6490): no PSS call yet.
int32_t new_player(uint32_t sound, uint32_t port_flags, int32_t user, int32_t app_module, uint32_t* out) {
  if (!out) return static_cast<int32_t>(0x80B30002u);
  auto s = find_sound(sound);
  if (!s) return static_cast<int32_t>(0x80B30005u);
  auto p = std::make_shared<PlayerObj>();
  p->sound = s;
  p->port_flags = port_flags;
  p->user_id = user;
  p->app_module = app_module;
  std::lock_guard<std::mutex> l(g_lock);
  const uint32_t h = generate_handle([](uint32_t k) { return g_players.count(k) != 0; });
  g_players[h] = p;
  *out = h;
  return 0;
}

int32_t CreatePlayerNative(uint32_t sound, uint32_t* player) { return new_player(sound, 0, -1, -1, player); }
SSN_ICALL("Sce.PlayStation.Core.Audio.Sound::CreatePlayerNative", CreatePlayerNative);

// 0x2d5f10: port Personal (1) -> flags 0x1000, PadSpk (2) -> 0x800.
int32_t CreatePlayerByUserNative(uint32_t sound, int32_t user, int32_t port, uint32_t* player) {
  const uint32_t flags = port == 1 ? 0x1000u : (port == 2 ? 0x800u : 0u);
  return new_player(sound, flags, user, -1, player);
}
SSN_ICALL("Sce.PlayStation.Core.Audio.Sound::CreatePlayerByUserNative", CreatePlayerByUserNative);

// 0x2d61c0: app module Impose (1) -> 262.
int32_t CreatePlayerByUserAppModuleNative(uint32_t sound, int32_t user, int32_t app_module, int32_t port, uint32_t* player) {
  const uint32_t flags = port == 1 ? 0x1000u : (port == 2 ? 0x800u : 0u);
  return new_player(sound, flags, user, app_module == 1 ? 262 : -1, player);
}
SSN_ICALL("Sce.PlayStation.Core.Audio.Sound::CreatePlayerByUserAppModuleNative", CreatePlayerByUserAppModuleNative);

// 0x2d6490: qword +32 = 0x12000 (flags 0x12000, user 0).
int32_t CreateSurroundPanPlayerNative(uint32_t sound, uint32_t* player) { return new_player(sound, 0x12000, 0, -1, player); }
SSN_ICALL("Sce.PlayStation.Core.Audio.Sound::CreateSurroundPanPlayerNative", CreateSurroundPanPlayerNative);

// The description getters (0x2d6720.. via sub_184720 -> sub_1722C0): 0 when unavailable;
// the data format is 5 (Unknown) unless < 5.
int32_t sound_info(uint32_t handle, PssResourceInfo* info) {
  auto s = find_sound(handle);
  if (!s) return static_cast<int32_t>(0x80B30005u);
  if (s->resource == 0 || pss_resource_info(s->resource, info) != 0) *info = PssResourceInfo{0, 0, 0, 0, 5};
  return 0;
}
int32_t GetFrameRateNative(uint32_t h, uint32_t* v) {
  if (!v) return static_cast<int32_t>(0x80B30002u);
  PssResourceInfo i;
  const int32_t r = sound_info(h, &i);
  if (r == 0) *v = i.rate;
  return r;
}
SSN_ICALL("Sce.PlayStation.Core.Audio.Sound::GetFrameRateNative", GetFrameRateNative);
int32_t GetChannelNative(uint32_t h, uint32_t* v) {
  if (!v) return static_cast<int32_t>(0x80B30002u);
  PssResourceInfo i;
  const int32_t r = sound_info(h, &i);
  if (r == 0) *v = i.channels;
  return r;
}
SSN_ICALL("Sce.PlayStation.Core.Audio.Sound::GetChannelNative", GetChannelNative);
int32_t GetBitDepthNative(uint32_t h, uint32_t* v) {
  if (!v) return static_cast<int32_t>(0x80B30002u);
  PssResourceInfo i;
  const int32_t r = sound_info(h, &i);
  if (r == 0) *v = i.bits;
  return r;
}
SSN_ICALL("Sce.PlayStation.Core.Audio.Sound::GetBitDepthNative", GetBitDepthNative);
int32_t GetBitRateNative(uint32_t h, uint32_t* v) {
  if (!v) return static_cast<int32_t>(0x80B30002u);
  PssResourceInfo i;
  const int32_t r = sound_info(h, &i);
  if (r == 0) *v = i.bitrate;
  return r;
}
SSN_ICALL("Sce.PlayStation.Core.Audio.Sound::GetBitRateNative", GetBitRateNative);
int32_t GetDataFormatNative(uint32_t h, uint32_t* v) {
  if (!v) return static_cast<int32_t>(0x80B30002u);
  PssResourceInfo i;
  const int32_t r = sound_info(h, &i);
  if (r == 0) *v = i.format < 5 ? i.format : 5;
  return r;
}
SSN_ICALL("Sce.PlayStation.Core.Audio.Sound::GetDataFormatNative", GetDataFormatNative);

// ---- SoundPlayer ------------------------------------------------------------------

// ReleaseNative(int) — 0x2d6c20 (object sub_1E39F0): stop / destroy the PSS player, drop
// the sound reference.
void PlayerReleaseNative(uint32_t handle) {
  std::shared_ptr<PlayerObj> p;
  {
    std::lock_guard<std::mutex> l(g_lock);
    auto it = g_players.find(handle);
    if (it == g_players.end()) return;
    p = std::move(it->second);
    g_players.erase(it);
  }
  stop_and_destroy(*p);
}
SSN_ICALL("Sce.PlayStation.Core.Audio.SoundPlayer::ReleaseNative", PlayerReleaseNative);

// PlayNative(int) — 0x2d6d10. The data range is locked in memory around the play (kernel
// imports mTBZfEal2Bw / OG4RsDwLguo); the host has no paging to prevent.
int32_t PlayNative(uint32_t handle) {
  auto p = find_player(handle);
  if (!p) return static_cast<int32_t>(0x80B30005u);
  stop_and_destroy(*p);
  const uint32_t res = p->sound ? p->sound->resource : 0;
  uint32_t r;
  uint32_t player = 0;
  if (res == 0) {
    r = pss::kBadHandle;  // sub_15DD80 with an empty cell
  } else if (p->port_flags != 0 && (p->port_flags & 0x1800) != 0) {
    r = pss_player_create(res, static_cast<uint16_t>(p->port_flags | 0x80), p->user_id, p->app_module, &player);
  } else {
    r = pss_player_create(res, static_cast<uint16_t>(p->port_flags | 0x80), 0, 0, &player);
  }
  if (r != 0) return kAudioSystem;
  p->pss = player;
  pss_volume(player, p->volume);
  pss_pan(player, (p->pan + 1.0f) * 0.5f);
  if (p->loop) pss_player_set_loop(player, true);
  pss_rate(player, p->rate);
  return pss_player_play(player) == 0 ? 0 : kAudioSystem;
}
SSN_ICALL("Sce.PlayStation.Core.Audio.SoundPlayer::PlayNative", PlayNative);

// StopNative(int) — 0x2d6fd0.
int32_t StopNative(uint32_t handle) {
  auto p = find_player(handle);
  if (!p) return static_cast<int32_t>(0x80B30005u);
  if (p->pss == 0) return 0;
  int32_t playing = 1;
  uint32_t r = pss_player_stop(p->pss, 64);
  if (r == 0) {
    for (;;) {
      r = pss_player_is_playing(p->pss, &playing);
      if (playing != 1) break;
      std::this_thread::sleep_for(std::chrono::microseconds(10));
      if (r != 0) break;
    }
  }
  if (r != 0) return kAudioSystem;
  pss_player_destroy(p->pss);
  p->pss = 0;
  return 0;
}
SSN_ICALL("Sce.PlayStation.Core.Audio.SoundPlayer::StopNative", StopNative);

// GetStatusNative(int, out SoundStatus) — 0x2d7130: Playing iff the PSS player reports 1.
int32_t GetStatusNative(uint32_t handle, uint32_t* status) {
  if (!status) return static_cast<int32_t>(0x80B30002u);
  auto p = find_player(handle);
  if (!p) return static_cast<int32_t>(0x80B30005u);
  int32_t playing = 0;
  if (p->pss != 0) pss_player_is_playing(p->pss, &playing);
  *status = playing == 1 ? 1 : 0;
  return 0;
}
SSN_ICALL("Sce.PlayStation.Core.Audio.SoundPlayer::GetStatusNative", GetStatusNative);

// SetVolumeNative — 0x2d7230: clamped to [0, 1].
int32_t SetVolumeNative(uint32_t handle, float v) {
  auto p = find_player(handle);
  if (!p) return static_cast<int32_t>(0x80B30005u);
  float x = 0.0f;
  if (!(0.0f > v)) x = v > 1.0f ? 1.0f : v;
  p->volume = x;
  if (p->pss != 0) pss_volume(p->pss, x);
  return 0;
}
SSN_ICALL("Sce.PlayStation.Core.Audio.SoundPlayer::SetVolumeNative", SetVolumeNative);

int32_t GetVolumeNative(uint32_t handle, float* v) {  // 0x2d7310
  if (!v) return static_cast<int32_t>(0x80B30002u);
  auto p = find_player(handle);
  if (!p) return static_cast<int32_t>(0x80B30005u);
  *v = p->volume;
  return 0;
}
SSN_ICALL("Sce.PlayStation.Core.Audio.SoundPlayer::GetVolumeNative", GetVolumeNative);

// SetPanNative — 0x2d73c0: clamped to [-1, 1]; the PSS pan is (pan + 1) * 0.5.
int32_t SetPanNative(uint32_t handle, float v) {
  auto p = find_player(handle);
  if (!p) return static_cast<int32_t>(0x80B30005u);
  float x = -1.0f;
  if (!(-1.0f > v)) x = v > 1.0f ? 1.0f : v;
  p->pan = x;
  if (p->pss != 0) pss_pan(p->pss, (x + 1.0f) * 0.5f);
  return 0;
}
SSN_ICALL("Sce.PlayStation.Core.Audio.SoundPlayer::SetPanNative", SetPanNative);

int32_t GetPanNative(uint32_t handle, float* v) {  // 0x2d74b0
  if (!v) return static_cast<int32_t>(0x80B30002u);
  auto p = find_player(handle);
  if (!p) return static_cast<int32_t>(0x80B30005u);
  *v = p->pan;
  return 0;
}
SSN_ICALL("Sce.PlayStation.Core.Audio.SoundPlayer::GetPanNative", GetPanNative);

int32_t SetLoopNative(uint32_t handle, MonoBoolean loop) {  // 0x2d7560
  auto p = find_player(handle);
  if (!p) return static_cast<int32_t>(0x80B30005u);
  p->loop = loop != 0;
  if (p->pss != 0) pss_player_set_loop(p->pss, loop != 0);
  return 0;
}
SSN_ICALL("Sce.PlayStation.Core.Audio.SoundPlayer::SetLoopNative", SetLoopNative);

int32_t GetLoopNative(uint32_t handle, uint8_t* loop) {  // 0x2d7620
  if (!loop) return static_cast<int32_t>(0x80B30002u);
  auto p = find_player(handle);
  if (!p) return static_cast<int32_t>(0x80B30005u);
  *loop = p->loop ? 1 : 0;
  return 0;
}
SSN_ICALL("Sce.PlayStation.Core.Audio.SoundPlayer::GetLoopNative", GetLoopNative);

// SetPlaybackRateNative — 0x2d76d0: clamped to [0.25, 4].
int32_t SetPlaybackRateNative(uint32_t handle, float v) {
  auto p = find_player(handle);
  if (!p) return static_cast<int32_t>(0x80B30005u);
  float x = 0.25f;
  if (!(0.25f > v)) x = v > 4.0f ? 4.0f : v;
  p->rate = x;
  if (p->pss != 0) pss_rate(p->pss, x);
  return 0;
}
SSN_ICALL("Sce.PlayStation.Core.Audio.SoundPlayer::SetPlaybackRateNative", SetPlaybackRateNative);

int32_t GetPlaybackRateNative(uint32_t handle, float* v) {  // 0x2d77b0
  if (!v) return static_cast<int32_t>(0x80B30002u);
  auto p = find_player(handle);
  if (!p) return static_cast<int32_t>(0x80B30005u);
  *v = p->rate;
  return 0;
}
SSN_ICALL("Sce.PlayStation.Core.Audio.SoundPlayer::GetPlaybackRateNative", GetPlaybackRateNative);

// GetPosition — 0x2d7860 (sub_184AB0), 0 without a PSS player.
int32_t GetPosition(uint32_t handle, uint64_t* ms) {
  if (!ms) return static_cast<int32_t>(0x80B30002u);
  auto p = find_player(handle);
  if (!p) return static_cast<int32_t>(0x80B30005u);
  uint64_t v = 0;
  if (p->pss != 0) pss_player_position(p->pss, &v);
  *ms = v;
  return 0;
}
SSN_ICALL("Sce.PlayStation.Core.Audio.SoundPlayer::GetPosition", GetPosition);

// SetPosition — 0x2d7960: clamped to the length (sub_184AF0), then sub_184AD0.
int32_t SetPosition(uint32_t handle, uint64_t ms) {
  auto p = find_player(handle);
  if (!p) return static_cast<int32_t>(0x80B30005u);
  if (p->pss == 0) return 0;
  uint64_t len = 0;
  pss_player_length(p->pss, &len);
  if (len < ms) ms = len;
  return pss_player_set_position(p->pss, ms) == 0 ? 0 : kAudioSystem;
}
SSN_ICALL("Sce.PlayStation.Core.Audio.SoundPlayer::SetPosition", SetPosition);

// GetLength — 0x2d7a70 (sub_184AF0), 0 without a PSS player.
int32_t GetLength(uint32_t handle, uint64_t* ms) {
  if (!ms) return static_cast<int32_t>(0x80B30002u);
  auto p = find_player(handle);
  if (!p) return static_cast<int32_t>(0x80B30005u);
  uint64_t v = 0;
  if (p->pss != 0) pss_player_length(p->pss, &v);
  *ms = v;
  return 0;
}
SSN_ICALL("Sce.PlayStation.Core.Audio.SoundPlayer::GetLength", GetLength);

// The surround-pan natives (0x2d7b70 / 0x2d7c10 / 0x2d7cb0 / 0x2d7d50) only look the
// player up and return 0.
int32_t MoveSurroundPanNative(uint32_t handle, float, MonoBoolean, float, double) {
  return find_player(handle) ? 0 : static_cast<int32_t>(0x80B30005u);
}
SSN_ICALL("Sce.PlayStation.Core.Audio.SoundPlayer::MoveSurroundPanNative", MoveSurroundPanNative);
int32_t PauseSurroundPanNative(uint32_t handle, double) {
  return find_player(handle) ? 0 : static_cast<int32_t>(0x80B30005u);
}
SSN_ICALL("Sce.PlayStation.Core.Audio.SoundPlayer::PauseSurroundPanNative", PauseSurroundPanNative);
int32_t MoveSurroundPanAngleNative(uint32_t handle, float, MonoBoolean, double) {
  return find_player(handle) ? 0 : static_cast<int32_t>(0x80B30005u);
}
SSN_ICALL("Sce.PlayStation.Core.Audio.SoundPlayer::MoveSurroundPanAngleNative", MoveSurroundPanAngleNative);
int32_t MoveSurroundPanDistanceNative(uint32_t handle, float, double) {
  return find_player(handle) ? 0 : static_cast<int32_t>(0x80B30005u);
}
SSN_ICALL("Sce.PlayStation.Core.Audio.SoundPlayer::MoveSurroundPanDistanceNative", MoveSurroundPanDistanceNative);

}  // namespace
}  // namespace ssn::psm::audio
