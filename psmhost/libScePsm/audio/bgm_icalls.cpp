// Sce.PlayStation.Core.Audio.Bgm / BgmPlayer icalls (libScePsm.sprx, FW 4.03, 0x2d7df0 ..
// 0x2da8d0) over the PSS audio system (pss_audio.h) through libScePsm's BGM C wrappers
// (sub_183C60 .. sub_1840A0).
//
// Who plays the home BGM (FW 4.03 IL): Sce.Vsh.ShellUI.Legacy BgmController.PlayMainBgm —
// called by UpdateBgmState (a FrameTask) when BGLayerPlugin.Instance.BgmState becomes Home —
// creates Player("/system_ex/vsh_asset/bgm_home.at9", priority 100, loop, category Main,
// fade in / out 1000 ms) = PUI BgmPlayerWrapper.CreateBgmPlayer + PlayBgm; the wrapper's
// worker thread makes a Bgm from the file and a BgmPlayer from it, sets Volume (0 when it
// resumes a position, else the wrapper volume), Position, Category, Loop,
// ContinuousPause/ContinuousResume (or ContinuousResume alone when it continues), Play, and
// fades with Volume steps every 32 ms (FadeInOutTicker: volume * 32 / FadeInLength). The
// onboarding BGM is the same with bgm_onboarding.at9 (priority 40, cut-in at 27038 ms);
// BgmState Mute stops it with a 2000 ms fade; IsSystemBgmEnable (registry 184877056 == 0),
// LayerManager.SystemBgmEnable and the power state suppress all with a silent
// "suppressor" player (priority 1, empty path, no native objects).
//
// A Bgm (table xmmword_541028, object vtable off_49FFA8, 40 + 56 bytes) owns a native copy
// of its file and the PSS resource made from it (+40, a 4-byte cell holding the handle); its
// creation result is kept at +48. A BgmPlayer (table xmmword_541040, vtable off_49FFC8, 40
// bytes, +32 = owner) exists once: the first one creates the single player state
// qword_5272A8 (32 bytes: +0 the PSS player cell, +8 the Bgm, +16 volume 1.0, +20 rate 1.0,
// +24 loop, +28 category) with a PSS player of flags 0x500480 (sub_183D40); while it lives
// (dword_5272B4) further CreatePlayer calls fail with 0x80B30004. Every BgmPlayer icall
// acts on that state. The continuous-position clock of the Main category lives in
// ymmword_527280 (base ms, end of the resume window ms, loop start ms, loop end ms) and
// qword_5272A0 (length ms), guarded by qword_527278.
#include <chrono>
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

constexpr int32_t kArgument = static_cast<int32_t>(0x80B30002u);
constexpr int32_t kNoPlayer = static_cast<int32_t>(0x80B30004u);   // another BgmPlayer exists
constexpr int32_t kBadHandle = static_cast<int32_t>(0x80B30005u);
constexpr int32_t kCreate = static_cast<int32_t>(0x80B30006u);
constexpr int32_t kFormat = static_cast<int32_t>(0x80B30007u);
constexpr int32_t kShortRead = static_cast<int32_t>(0x80B30010u);
constexpr int32_t kOpen = static_cast<int32_t>(0x80B30011u);
constexpr int32_t kAudioSystem = static_cast<int32_t>(0x80B30022u);

// The Bgm data object (+32 -> 56 bytes): the file name, the image, the resource cell (+40;
// 0 = none) and the creation result (+48).
struct BgmObj {
  std::string name;
  std::shared_ptr<const std::vector<uint8_t>> image;  // +24
  uint32_t cell = 0;                                  // +40
  int32_t status = 0;                                 // +48
  // sub_1E3B30: the resource cell released (sub_183CF0 -> sub_15DE80).
  ~BgmObj() {
    if (cell != 0) pss_resource_release(cell);
  }
};

struct PlayerState {  // qword_5272A8
  uint32_t cell = 0;                 // +0 (the PSS player handle; 0 = none)
  std::shared_ptr<BgmObj> bgm;       // +8
  float volume = 1.0f;               // +16
  float rate = 1.0f;                 // +20
  bool loop = false;                 // +24
  uint32_t category = 0;             // +28
};

struct PlayerObj {
  bool owner = false;  // +32
};

std::mutex g_lock;        // X6AKrGoJOU0_h_A
std::recursive_mutex g_state_lock;  // qword_527278
std::unordered_map<uint32_t, std::shared_ptr<BgmObj>> g_bgms;
std::unordered_map<uint32_t, std::shared_ptr<PlayerObj>> g_players;
std::unique_ptr<PlayerState> g_state;  // qword_5272A8
int32_t g_player_count = 0;            // dword_5272B4
// ymmword_527280: [0] base ms, [1] end of the resume window, [2] loop start, [3] loop end;
// qword_5272A0 length.
uint64_t g_cont[4] = {};
uint64_t g_cont_len = 0;

std::shared_ptr<BgmObj> find_bgm(uint32_t h) {
  std::lock_guard<std::mutex> l(g_lock);
  auto it = g_bgms.find(h);
  return it == g_bgms.end() ? nullptr : it->second;
}
std::shared_ptr<PlayerObj> find_player(uint32_t h) {
  std::lock_guard<std::mutex> l(g_lock);
  auto it = g_players.find(h);
  return it == g_players.end() ? nullptr : it->second;
}

// sceKernelGetProcessTime (import 4J2sUJmuHZQ), microseconds, as milliseconds the way the
// icalls divide it ((t >> 3) * 0x20C49BA5E353F7CF >> 64 >> 4 = t / 1000).
uint64_t now_ms() {
  static const auto start = std::chrono::steady_clock::now();
  const uint64_t us = static_cast<uint64_t>(
      std::chrono::duration_cast<std::chrono::microseconds>(std::chrono::steady_clock::now() - start).count());
  return us / 1000;
}

// The PSS wrappers on the player cell: a null cell is 0x80B3F003 (sub_183E10 etc.).
uint32_t cell_or_arg(uint32_t cell) { return cell != 0 ? 0 : 0x80B3F003u; }

// sub_183ED0: 2 when playing, 1 when paused (over it), else 0.
uint32_t cell_status(uint32_t cell, int32_t* st) {
  *st = 0;
  if (cell == 0) return 0x80B3F003u;
  int32_t playing = 0;
  if (pss_player_is_playing(cell, &playing) == 0) *st = playing != 0 ? 2 : 0;
  int32_t paused = 0;
  const uint32_t r = pss_player_is_paused(cell, &paused);
  if (r == 0 && paused != 0) *st = 1;
  return r;
}

// N0_F2yO3hRc_h_A_0 (0x1e3730): with a base (Main category): now - base, wrapped into the
// loop (loop end, else the length) when past it — the base then moves so the wrapped
// position holds —; the base is cleared once the resume window ([1]) has passed. Without
// a base: the player's position. false when there is neither.
bool continuous_position(uint64_t* out) {
  std::lock_guard<std::recursive_mutex> l(g_state_lock);
  if (g_cont[0] != 0) {
    const uint64_t now = now_ms();
    uint64_t pos = now - g_cont[0];
    uint64_t end = g_cont[3];
    bool wrap = false;
    if (end != 0) {
      wrap = pos > end;
    } else {
      end = g_cont_len;
      wrap = pos > end;
    }
    if (wrap) {
      const uint64_t span = end - g_cont[2];
      pos = g_cont[2] + (span != 0 ? (pos - end) % span : 0);
      g_cont[0] = now - pos;
    }
    if (g_cont[1] != 0 && now > g_cont[1]) {
      g_cont[0] = 0;
      g_cont[1] = 0;
    }
    if (out) *out = pos;
    return true;
  }
  uint64_t p = 0;
  if (g_state && g_state->cell != 0 && pss_player_position(g_state->cell, &p) == 0) {
    if (out) *out = p;
    return true;
  }
  return false;
}

// Bgm.NewFromFilename / NewFromFileImage common part: an MP3 image (after an optional ID3
// tag, a frame sync within the first 1441 bytes) or a RIFF/WAVE image whose "fmt " chunk at
// offset 12 is WAVE_FORMAT_EXTENSIBLE with the ATRAC9 sub-format (xmmword_34E930), else
// 0x80B30007; the PSS resource from it (sub_183C60 -> sub_15DD50, flags 0), failure ->
// 0x80B30006.
int32_t check_and_create(BgmObj& b) {
  const std::vector<uint8_t>& d = *b.image;
  const uint8_t* p = d.data();
  size_t n = d.size();
  bool ok = false;
  if (n >= 0xC) {
    const uint8_t* q = p;
    size_t m = n;
    bool id3_ok = true;
    if (q[0] == 'I' && q[1] == 'D' && q[2] == '3') {
      const uint32_t sz = (uint32_t(q[6] & 0x7F) << 21) | (uint32_t(q[7] & 0x7F) << 14) | (uint32_t(q[8] & 0x7F) << 7) |
                          uint32_t(q[9] & 0x7F);
      if (m < size_t(sz) + 10) {
        id3_ok = false;
      } else {
        m -= size_t(sz) + 10;
        q += size_t(sz) + 10;
      }
    }
    if (id3_ok) {
      const size_t lim = m > 3 ? m - 3 : 0;
      for (size_t i = 0; i < lim && i < 1441; ++i) {
        const uint32_t w = uint32_t(q[i]) << 24 | uint32_t(q[i + 1]) << 16 | uint32_t(q[i + 2]) << 8 | q[i + 3];
        if ((w & 0xFFE20000u) == 0xFFE20000u && w >= 0xFFE00000u && (w & 0x160000u) == 0x120000u &&
            ((w & 0xFFFF) >> 12) != 0 && ((w & 0xFFFF) >> 12) != 15 && (w & 0xC00) != 0xC00) {
          ok = true;
          break;
        }
      }
    }
  }
  if (!ok) {
    static constexpr uint8_t kGuid[16] = {0xD2, 0x42, 0xE1, 0x47, 0xBA, 0x36, 0x8D, 0x4D,
                                          0x88, 0xFC, 0x61, 0x65, 0x4F, 0x8C, 0x83, 0x6C};
    ok = n >= 0x48 && std::memcmp(p, "RIFF", 4) == 0 && std::memcmp(p + 8, "WAVE", 4) == 0 &&
         std::memcmp(p + 12, "fmt", 3) == 0 && p[20] == 0xFE && p[21] == 0xFF && std::memcmp(p + 0x2C, kGuid, 16) == 0;
    if (!ok) return b.status = kFormat;
  }
  uint32_t res = 0;
  if (pss_resource_create(b.image, 0, &res) != 0) return b.status = kCreate;
  b.cell = res;
  return b.status;
}

int32_t register_bgm(const std::shared_ptr<BgmObj>& b, uint32_t* handle) {
  std::lock_guard<std::mutex> l(g_lock);
  const uint32_t h = generate_handle([](uint32_t k) { return g_bgms.count(k) != 0; });
  g_bgms[h] = b;
  *handle = h;
  return 0;
}

// NewFromFilename(string, out int) — 0x2d7df0: the console path is opened through the PSS
// file layer (sub_17C900, failure -> 0x80B30011) and read whole (short read -> 0x80B30010).
int32_t NewFromFilename(MonoString* filename, uint32_t* handle) {
  if (!filename || !handle) return kArgument;
  auto b = std::make_shared<BgmObj>();
  b->name = ssn::psm::utf8(filename);
  std::string path = b->name;
  if (Firmware* fw = host::ctx().firmware) path = fw->host_path(b->name);
  std::ifstream f(std::filesystem::path(path), std::ios::binary);
  if (!f) return kOpen;
  auto image = std::make_shared<std::vector<uint8_t>>(std::istreambuf_iterator<char>(f), std::istreambuf_iterator<char>());
  std::error_code ec;
  const auto sz = std::filesystem::file_size(std::filesystem::path(path), ec);
  if (!ec && sz != image->size()) return kShortRead;
  b->image = std::move(image);
  const int32_t r = check_and_create(*b);
  if (r < 0) return r;
  return register_bgm(b, handle);
}
SSN_ICALL("Sce.PlayStation.Core.Audio.Bgm::NewFromFilename", NewFromFilename);

// NewFromFileImage(byte[], out int) — 0x2d84b0: the bytes are copied into a native buffer.
int32_t NewFromFileImage(MonoArray* file_image, uint32_t* handle) {
  if (!file_image || !handle) return kArgument;
  const auto* p = reinterpret_cast<const uint8_t*>(mono_array_addr_with_size(file_image, 1, 0));
  auto b = std::make_shared<BgmObj>();
  b->image = std::make_shared<const std::vector<uint8_t>>(p, p + mono_array_length(file_image));
  const int32_t r = check_and_create(*b);
  if (r < 0) return r;
  return register_bgm(b, handle);
}
SSN_ICALL("Sce.PlayStation.Core.Audio.Bgm::NewFromFileImage", NewFromFileImage);

// ReleaseNative(int) — 0x2d89c0: drops the table reference (the object releases its
// resource when the last reference goes).
void BgmReleaseNative(uint32_t handle) {
  std::shared_ptr<BgmObj> b;
  std::lock_guard<std::mutex> l(g_lock);
  auto it = g_bgms.find(handle);
  if (it == g_bgms.end()) return;
  b = std::move(it->second);
  g_bgms.erase(it);
}
SSN_ICALL("Sce.PlayStation.Core.Audio.Bgm::ReleaseNative", BgmReleaseNative);

// CreatePlayerNative(int, out int) — 0x2d8ab0.
int32_t CreatePlayerNative(uint32_t bgm, uint32_t* player) {
  if (!player) return kArgument;
  auto b = find_bgm(bgm);
  if (!b) return kBadHandle;
  auto p = std::make_shared<PlayerObj>();
  {
    std::lock_guard<std::recursive_mutex> l(g_state_lock);
    const int32_t before = g_player_count++;
    if (before <= 0) {
      p->owner = true;
      g_state = std::make_unique<PlayerState>();
      g_state->bgm = b;
      // sub_183D40 with flags 0x500480 (unk_500480), retried 10 times 100 us apart while the
      // PSS has no free slot (0x80B3F019).
      for (int i = 0; i < 10; ++i) {
        uint32_t h = 0;
        const uint32_t r = b->cell != 0 ? pss_player_create(b->cell, 0x500480u, 0, 0, &h) : 0x80B3F018u;
        g_state->cell = r == 0 ? h : 0;
        if (r != 0x80B3F019u) break;
        std::this_thread::sleep_for(std::chrono::microseconds(100));
      }
    }
  }
  if (!p->owner) {
    std::lock_guard<std::recursive_mutex> l(g_state_lock);
    --g_player_count;  // the object's destructor (sub_1E3C20)
    return kNoPlayer;
  }
  std::lock_guard<std::mutex> l(g_lock);
  const uint32_t h = generate_handle([](uint32_t k) { return g_players.count(k) != 0; });
  g_players[h] = p;
  *player = h;
  return 0;
}
SSN_ICALL("Sce.PlayStation.Core.Audio.Bgm::CreatePlayerNative", CreatePlayerNative);

// The description getters (0x2d8ef0 ..) through sub_183D20 -> sub_1722C0: 0 when the
// resource gives nothing; the data format is 5 (Unknown) unless < 5.
int32_t bgm_info(uint32_t h, PssResourceInfo* info) {
  auto b = find_bgm(h);
  if (!b) return kBadHandle;
  if (b->cell == 0 || pss_resource_info(b->cell, info) != 0) *info = PssResourceInfo{0, 0, 0, 0, 5};
  return 0;
}
int32_t BgmGetFrameRateNative(uint32_t h, uint32_t* v) {
  if (!v) return kArgument;
  PssResourceInfo i;
  const int32_t r = bgm_info(h, &i);
  if (r == 0) *v = i.rate;
  return r;
}
SSN_ICALL("Sce.PlayStation.Core.Audio.Bgm::GetFrameRateNative", BgmGetFrameRateNative);
int32_t BgmGetChannelNative(uint32_t h, uint32_t* v) {
  if (!v) return kArgument;
  PssResourceInfo i;
  const int32_t r = bgm_info(h, &i);
  if (r == 0) *v = i.channels;
  return r;
}
SSN_ICALL("Sce.PlayStation.Core.Audio.Bgm::GetChannelNative", BgmGetChannelNative);
int32_t BgmGetBitDepthNative(uint32_t h, uint32_t* v) {
  if (!v) return kArgument;
  PssResourceInfo i;
  const int32_t r = bgm_info(h, &i);
  if (r == 0) *v = i.bits;
  return r;
}
SSN_ICALL("Sce.PlayStation.Core.Audio.Bgm::GetBitDepthNative", BgmGetBitDepthNative);
int32_t BgmGetBitRateNative(uint32_t h, uint32_t* v) {
  if (!v) return kArgument;
  PssResourceInfo i;
  const int32_t r = bgm_info(h, &i);
  if (r == 0) *v = i.bitrate;
  return r;
}
SSN_ICALL("Sce.PlayStation.Core.Audio.Bgm::GetBitRateNative", BgmGetBitRateNative);
int32_t BgmGetDataFormatNative(uint32_t h, uint32_t* v) {
  if (!v) return kArgument;
  PssResourceInfo i;
  const int32_t r = bgm_info(h, &i);
  if (r == 0) *v = i.format < 5 ? i.format : 5;
  return r;
}
SSN_ICALL("Sce.PlayStation.Core.Audio.Bgm::GetDataFormatNative", BgmGetDataFormatNative);

// ---- BgmPlayer ------------------------------------------------------------------------

// ReleaseNative(int) — 0x2d93f0, object destructor sub_1E3C20: --dword_5272B4; the owner
// stops its PSS player (sub_183E30, fade 0) and waits (10 us steps) while it reports
// playing, destroys it (sub_183DE0) and drops the player state.
void PlayerReleaseNative(uint32_t handle) {
  std::shared_ptr<PlayerObj> p;
  {
    std::lock_guard<std::mutex> l(g_lock);
    auto it = g_players.find(handle);
    if (it == g_players.end()) return;
    p = std::move(it->second);
    g_players.erase(it);
  }
  std::lock_guard<std::recursive_mutex> l(g_state_lock);
  --g_player_count;
  if (!p->owner || !g_state) return;
  if (g_state->cell != 0) {
    int32_t st = 2;
    if (pss_player_stop(g_state->cell, 0) == 0) {
      for (;;) {
        const uint32_t r = cell_status(g_state->cell, &st);
        if (st != 2) break;
        std::this_thread::sleep_for(std::chrono::microseconds(10));
        if (r != 0) break;
      }
    }
    pss_player_destroy(g_state->cell);
    g_state->cell = 0;
  }
  g_state.reset();
}
SSN_ICALL("Sce.PlayStation.Core.Audio.BgmPlayer::ReleaseNative", PlayerReleaseNative);

// The player state for a BgmPlayer handle: 0x80B30005 for an unknown handle.
PlayerState* state_for(uint32_t handle, int32_t* err) {
  if (!find_player(handle)) {
    *err = kBadHandle;
    return nullptr;
  }
  *err = 0;
  return g_state.get();
}

// PlayNative — 0x2d94e0: sub_183E10 (sub_15EB20) on the state's player; failure 0x80B30022.
int32_t BgmPlayNative(uint32_t handle) {
  int32_t e;
  PlayerState* s = state_for(handle, &e);
  if (e) return e;
  if (!s || cell_or_arg(s->cell) != 0) return kAudioSystem;
  return pss_player_play(s->cell) == 0 ? 0 : kAudioSystem;
}
SSN_ICALL("Sce.PlayStation.Core.Audio.BgmPlayer::PlayNative", BgmPlayNative);

// StopNative — 0x2d9590: sub_183E30 = sub_15DDB0(handle, fade 0).
int32_t BgmStopNative(uint32_t handle) {
  int32_t e;
  PlayerState* s = state_for(handle, &e);
  if (e) return e;
  if (!s || cell_or_arg(s->cell) != 0) return kAudioSystem;
  return pss_player_stop(s->cell, 0) == 0 ? 0 : kAudioSystem;
}
SSN_ICALL("Sce.PlayStation.Core.Audio.BgmPlayer::StopNative", BgmStopNative);

// GetStatusNative(int, out BgmStatus) — 0x2d9640: paused (1) -> Paused (2), playing (2) ->
// Playing (1), else Stopped.
int32_t BgmGetStatusNative(uint32_t handle, int32_t* status) {
  if (!status) return kArgument;
  int32_t e;
  PlayerState* s = state_for(handle, &e);
  if (e) return e;
  int32_t v = 0;
  if (s && s->cell != 0) {
    int32_t st = 0;
    cell_status(s->cell, &st);
    v = st == 1 ? 2 : (st == 2 ? 1 : 0);
  }
  *status = v;
  return 0;
}
SSN_ICALL("Sce.PlayStation.Core.Audio.BgmPlayer::GetStatusNative", BgmGetStatusNative);

// PauseNative — 0x2d9760: sub_183E50 = sub_15EDA0.
int32_t BgmPauseNative(uint32_t handle) {
  int32_t e;
  PlayerState* s = state_for(handle, &e);
  if (e) return e;
  if (!s || cell_or_arg(s->cell) != 0) return kAudioSystem;
  return pss_player_pause(s->cell) == 0 ? 0 : kAudioSystem;
}
SSN_ICALL("Sce.PlayStation.Core.Audio.BgmPlayer::PauseNative", BgmPauseNative);

// ResumeNative — 0x2d9810: a paused player plays again.
int32_t BgmResumeNative(uint32_t handle) {
  int32_t e;
  PlayerState* s = state_for(handle, &e);
  if (e) return e;
  if (!s) return kAudioSystem;
  int32_t st = 0;
  if (cell_status(s->cell, &st) != 0) return kAudioSystem;
  if (st == 1) return pss_player_play(s->cell) == 0 ? 0 : kAudioSystem;
  return 0;
}
SSN_ICALL("Sce.PlayStation.Core.Audio.BgmPlayer::ResumeNative", BgmResumeNative);

// ContinuousPauseNative — 0x2d9920: pause; for the Main category the clock base becomes
// now - position (ms) and the resume window is cleared.
int32_t BgmContinuousPauseNative(uint32_t handle) {
  int32_t e;
  PlayerState* s = state_for(handle, &e);
  if (e) return e;
  int32_t r = kAudioSystem;
  if (s && s->cell != 0 && pss_player_pause(s->cell) == 0) r = 0;
  std::lock_guard<std::recursive_mutex> l(g_state_lock);
  if (g_state && g_state->category == 1) {
    uint64_t pos = 0;
    if (g_state->cell != 0 && pss_player_position(g_state->cell, &pos) == 0) {
      g_cont[0] = now_ms() - pos;
      g_cont[1] = 0;
    }
  }
  return r;
}
SSN_ICALL("Sce.PlayStation.Core.Audio.BgmPlayer::ContinuousPauseNative", BgmContinuousPauseNative);

// ContinuousResumeNative — 0x2d9a80: for the Main category with a clock base and no resume
// window: the continuous position is set (sub_184060) and the window ends 100 ms from now;
// then a paused player plays again.
int32_t BgmContinuousResumeNative(uint32_t handle) {
  int32_t e;
  PlayerState* s = state_for(handle, &e);
  if (e) return e;
  {
    std::lock_guard<std::recursive_mutex> l(g_state_lock);
    if (g_state && g_state->category == 1 && g_cont[0] != 0 && g_cont[1] == 0) {
      uint64_t pos = 0;
      continuous_position(&pos);
      if (g_state->cell != 0 && pss_player_set_position(g_state->cell, pos) == 0) {
        log::info("psm.audio", "BgmPlayer continuous resume at {} ms", pos);
        g_cont[1] = now_ms() + 100;
      }
    }
  }
  if (!s) return kAudioSystem;
  int32_t st = 0;
  if (cell_status(s->cell, &st) != 0) return kAudioSystem;
  if (st == 1) return pss_player_play(s->cell) == 0 ? 0 : kAudioSystem;
  return 0;
}
SSN_ICALL("Sce.PlayStation.Core.Audio.BgmPlayer::ContinuousResumeNative", BgmContinuousResumeNative);

// GetContinuousPositionNative(int, out ulong) — 0x2d9c20: the position for other categories,
// else the continuous clock.
int32_t BgmGetContinuousPositionNative(uint32_t handle, uint64_t* ms) {
  if (!ms) return kArgument;
  int32_t e;
  PlayerState* s = state_for(handle, &e);
  if (e) return e;
  uint64_t v = 0;
  if (s && s->category != 1) {
    if (s->cell != 0) pss_player_position(s->cell, &v);
  } else {
    continuous_position(&v);
  }
  *ms = v;
  return 0;
}
SSN_ICALL("Sce.PlayStation.Core.Audio.BgmPlayer::GetContinuousPositionNative", BgmGetContinuousPositionNative);

// SetVolumeNative — 0x2d9d40: clamped to [0, 1] into +16, then sub_183E70 (clamp [0, 10],
// sub_15EAC0).
int32_t BgmSetVolumeNative(uint32_t handle, float v) {
  int32_t e;
  PlayerState* s = state_for(handle, &e);
  if (e) return e;
  float x = 0.0f;
  if (!(0.0f > v)) x = v > 1.0f ? 1.0f : v;
  if (!s) return 0;
  s->volume = x;
  if (s->cell != 0) {
    float y = 0.0f;
    if (!(0.0f > x)) y = x > 10.0f ? 10.0f : x;
    pss_player_set_volume(s->cell, y);
  }
  return 0;
}
SSN_ICALL("Sce.PlayStation.Core.Audio.BgmPlayer::SetVolumeNative", BgmSetVolumeNative);

int32_t BgmGetVolumeNative(uint32_t handle, float* v) {  // 0x2d9e20
  if (!v) return kArgument;
  int32_t e;
  PlayerState* s = state_for(handle, &e);
  if (e) return e;
  *v = s ? s->volume : 0.0f;
  return 0;
}
SSN_ICALL("Sce.PlayStation.Core.Audio.BgmPlayer::GetVolumeNative", BgmGetVolumeNative);

// SetLoopNative — 0x2d9ee0: +24, then sub_183E90 (sub_15DE20 -> sub_160CF0).
int32_t BgmSetLoopNative(uint32_t handle, MonoBoolean loop) {
  int32_t e;
  PlayerState* s = state_for(handle, &e);
  if (e) return e;
  if (!s) return 0;
  s->loop = loop != 0;
  if (s->cell != 0) pss_player_set_loop(s->cell, loop != 0);
  return 0;
}
SSN_ICALL("Sce.PlayStation.Core.Audio.BgmPlayer::SetLoopNative", BgmSetLoopNative);

int32_t BgmGetLoopNative(uint32_t handle, uint8_t* loop) {  // 0x2d9fa0
  if (!loop) return kArgument;
  int32_t e;
  PlayerState* s = state_for(handle, &e);
  if (e) return e;
  *loop = s && s->loop ? 1 : 0;
  return 0;
}
SSN_ICALL("Sce.PlayStation.Core.Audio.BgmPlayer::GetLoopNative", BgmGetLoopNative);

// SetPlaybackRateNative — 0x2da050: clamped to [0.25, 4] into +20, then sub_183EB0 (clamp
// [0.004, 256], sub_15F0D0).
int32_t BgmSetPlaybackRateNative(uint32_t handle, float v) {
  int32_t e;
  PlayerState* s = state_for(handle, &e);
  if (e) return e;
  float x = 0.25f;
  if (!(0.25f > v)) x = v > 4.0f ? 4.0f : v;
  if (!s) return 0;
  s->rate = x;
  if (s->cell != 0) {
    float y = 0.004f;
    if (!(0.004f > x)) y = x > 256.0f ? 256.0f : x;
    pss_player_set_rate(s->cell, y);
  }
  return 0;
}
SSN_ICALL("Sce.PlayStation.Core.Audio.BgmPlayer::SetPlaybackRateNative", BgmSetPlaybackRateNative);

int32_t BgmGetPlaybackRateNative(uint32_t handle, float* v) {  // 0x2da130
  if (!v) return kArgument;
  int32_t e;
  PlayerState* s = state_for(handle, &e);
  if (e) return e;
  *v = s ? s->rate : 0.0f;
  return 0;
}
SSN_ICALL("Sce.PlayStation.Core.Audio.BgmPlayer::GetPlaybackRateNative", BgmGetPlaybackRateNative);

// GetPosition — 0x2da1f0 (sub_184040 -> sub_15EF30).
int32_t BgmGetPosition(uint32_t handle, uint64_t* ms) {
  if (!ms) return kArgument;
  int32_t e;
  PlayerState* s = state_for(handle, &e);
  if (e) return e;
  uint64_t v = 0;
  if (s && s->cell != 0) pss_player_position(s->cell, &v);
  *ms = v;
  return 0;
}
SSN_ICALL("Sce.PlayStation.Core.Audio.BgmPlayer::GetPosition", BgmGetPosition);

// SetPosition — 0x2da2f0: clamped to the length (sub_184080), then sub_184060 (sub_15EEA0);
// a failure is logged ("[PSM] Bug#89500 SetPosition failed ...") as 0x80B30022.
int32_t BgmSetPosition(uint32_t handle, uint64_t ms) {
  int32_t e;
  PlayerState* s = state_for(handle, &e);
  if (e) return e;
  if (!s) return kBadHandle;
  uint64_t len = 0;
  if (s->cell != 0) pss_player_length(s->cell, &len);
  if (len < ms) ms = len;
  const uint32_t r = s->cell != 0 ? pss_player_set_position(s->cell, ms) : 0x80B3F003u;
  if (r != 0) {
    log::warn("psm.audio", "[PSM] Bug#89500 SetPosition failed at {}(ms) errno={:08x} - {}", ms, r,
              s->bgm ? s->bgm->name : std::string());
    return kAudioSystem;
  }
  return 0;
}
SSN_ICALL("Sce.PlayStation.Core.Audio.BgmPlayer::SetPosition", BgmSetPosition);

// GetLength — 0x2da450 (sub_184080 -> sub_15ECF0).
int32_t BgmGetLength(uint32_t handle, uint64_t* ms) {
  if (!ms) return kArgument;
  int32_t e;
  PlayerState* s = state_for(handle, &e);
  if (e) return e;
  uint64_t v = 0;
  if (s && s->cell != 0) pss_player_length(s->cell, &v);
  *ms = v;
  return 0;
}
SSN_ICALL("Sce.PlayStation.Core.Audio.BgmPlayer::GetLength", BgmGetLength);

// SetCategory — 0x2da550: the owner's +28; category 1 (Main) records the loop start, the
// loop end (at most the length) and the length for the continuous clock (sub_1840A0 ->
// sub_15F010, sub_184080).
int32_t BgmSetCategory(uint32_t handle, uint32_t category) {
  auto p = find_player(handle);
  if (!p) return kBadHandle;
  std::lock_guard<std::recursive_mutex> l(g_state_lock);
  if (p->owner && g_state) {
    g_state->category = category;
    if (category == 1) {
      uint64_t len = 0, ls = 0, le = 0, end = 0;
      if (g_state->cell == 0 || pss_player_length(g_state->cell, &len) != 0) {
        end = 0;
      } else {
        const uint32_t r = pss_player_loop_positions(g_state->cell, &ls, &le);
        end = le;
        if (r == 0 && le > len) end = len;
      }
      g_cont[2] = ls;
      g_cont[3] = end;
      g_cont_len = len;
      log::info("psm.audio", "BgmPlayer category Main: loop {}..{} ms of {} ms", ls, end, len);
    }
  }
  return 0;
}
SSN_ICALL("Sce.PlayStation.Core.Audio.BgmPlayer::SetCategory", BgmSetCategory);

int32_t BgmGetCategoryNative(uint32_t handle, uint32_t* category) {  // 0x2da6c0
  auto p = find_player(handle);
  if (!p) return kBadHandle;
  std::lock_guard<std::recursive_mutex> l(g_state_lock);
  if (category && p->owner && g_state) *category = g_state->category;
  return 0;
}
SSN_ICALL("Sce.PlayStation.Core.Audio.BgmPlayer::GetCategoryNative", BgmGetCategoryNative);

// GetPortUsable(int, out bool) — 0x2da790: sub_183F70 (sub_15DDE0 -> sub_15EE30: the port
// state is PortStatus OK, 1); 0x80B3F014 (a dying player) reads as not usable.
int32_t BgmGetPortUsable(uint32_t handle, uint8_t* usable) {
  if (!find_player(handle)) return kBadHandle;
  std::lock_guard<std::recursive_mutex> l(g_state_lock);
  if (!usable) return kArgument;
  if (!g_state) return kBadHandle;
  uint32_t st = 0;
  uint32_t r = g_state->cell != 0 ? pss_player_port_status(g_state->cell, &st) : 0x80B3F003u;
  bool v = false;
  if (r == 0x80B3F014u) {
    r = 0;
  } else if (r != 0) {
    return static_cast<int32_t>(r);
  } else {
    v = st == 1;
  }
  *usable = v ? 1 : 0;
  return 0;
}
SSN_ICALL("Sce.PlayStation.Core.Audio.BgmPlayer::GetPortUsable", BgmGetPortUsable);

// HOST POLICY console-files=bgm (managed/Runtime ConsoleFiles, Prep ApplyConsoleFilesBgm):
// the host file of a console path in the firmware roots, or null when no root holds it.
MonoString* ConsoleFilesHostPath(MonoString* console_path) {
  if (!console_path) return nullptr;
  Firmware* fw = host::ctx().firmware;
  if (!fw) return nullptr;
  const std::string p = fw->host_path(ssn::psm::utf8(console_path));
  std::error_code ec;
  if (!std::filesystem::is_regular_file(std::filesystem::path(p), ec)) return nullptr;
  return mono_string_new(mono_domain_get(), p.c_str());
}
SSN_ICALL("SceShellNative.ConsoleFiles::HostPath", ConsoleFilesHostPath);
struct ConsoleFilesPolicy {
  ConsoleFilesPolicy() { host::Boundary::get().add_policy("console-files=bgm"); }
} g_console_files_policy;

// GetPortStatusNative(out uint) — 0x2da8d0: sub_183FE0 -> sub_15DDF0 (the BGM port).
int32_t BgmGetPortStatusNative(uint32_t* status) {
  *status = 0;
  uint32_t v = 0;
  const uint32_t r = pss_bgm_port_status(&v);
  if (r == 0) *status = v;
  return static_cast<int32_t>(r);
}
SSN_ICALL("Sce.PlayStation.Core.Audio.BgmPlayer::GetPortStatusNative", BgmGetPortStatusNative);

}  // namespace
}  // namespace ssn::psm::audio
