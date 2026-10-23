// Sce.Vsh.Video.PsmVideoPlayer icalls of libScePsmUtil.sprx (FW 4.03): the handle table, the
// player base (VideoPlayerBase, vtable 0x34260, constructor 0xe7a0) with its command queue,
// worker thread and state machine, and the media-framework player (VideoPlayerMcs, 11496
// bytes, vtable 0x34490, constructor 0x15d80) on top of libSceMediaFrameworkInterface.
//
// libScePsmUtil does no decoding itself: VideoPlayerMcs is a client of the media framework
// (sceMediaFw*, an IPC client of the BE core process NPXS40039 becore.elf, which demuxes,
// decodes and renders into a compositor canvas). Everything in this file follows
// libScePsmUtil; the sceMediaFw* calls go to the host media framework (media_fw.h, a
// declared host value standing in for becore).
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <deque>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <unordered_map>

#include <mono/metadata/object.h>

#include "common/mono_util.h"
#include "core/log.h"
#include "host/boundary.h"
#include "libScePsmUtil/media_fw.h"

namespace ssn::psmutil {
namespace {

constexpr int32_t kErrInvalidPointer = static_cast<int32_t>(0x80B30002u);  // -2135752702
constexpr int32_t kErrState = static_cast<int32_t>(0x80B30004u);           // -2135752700
constexpr int32_t kErrNullPlayer = static_cast<int32_t>(0x80B30005u);      // -2135752699
constexpr int32_t kErrCreate = static_cast<int32_t>(0x80B30013u);          // -2135752685

// Player states (0xf1c0 names them; the managed PlayerStateType uses the same values).
enum State : int32_t {
  kUndef = 0, kError = 1, kPrepared = 2, kPlaying = 3, kPaused = 4, kPausedAtEnd = 5, kStopped = 6,
  kPausedAtLoopEnd = 7, kSuspended = 8, kEmulating = 9
};

// Worker command (0xff20 queues heap blocks {u32 id, u8 flag / u32 at +4, u32/u64 at +8, u32 +16}).
struct Command {
  uint32_t id = 0;
  uint8_t flag = 0;
  int32_t i1 = 0;
  uint64_t q = 0;
  int32_t i2 = 0;
};

struct Player {
  std::mutex data;               // +96
  std::condition_variable cond;  // +128
  std::deque<Command> commands;  // +152 / +160
  bool quit = false;             // +681
  std::thread worker;            // +688 ("ScePsmVideoPlayer", 0x126d0)

  int32_t last_error = 0;         // +168
  int32_t last_error_detail = 0;  // +172
  int32_t last_video_error = 0;   // +176
  // Under the global lock unk_3B948: +392 current state, +408/+416 kept states (at most 4),
  // +424 last pushed video event, +440/+448 video events (at most 16).
  int32_t state = kUndef;
  std::deque<int32_t> history;
  int32_t last_event = 0;
  std::deque<int32_t> events;

  bool emulating = false;  // +32 (EmulatePlayWithSpeed; not implemented here)
  bool visible = false;    // +680, cleared by the base constructor 0xe7a0
  bool cmd_ok = false;     // +682
  std::string uri;         // +232 / +240 (0x11220)
  std::string content_type, user_agent, access_token;
  int32_t video_core_type = 0, create_mode = 0, width = 0, height = 0;
  int32_t display_w = 0, display_h = 0;  // +220 / +224 (0x16b40)
  int32_t dynamic_range = 0;             // +384 (event 4099, 0x17c90)
  uint64_t position_ms = 0;              // +456 (0x178a0)
  uint64_t duration_ms = 0;              // +464 (0x177a0)
  int32_t speed = 100;                   // +488 (0x16a50; 100 after Uninit 0x10290)
  uint32_t src_id = 0;                   // +11344: sceMediaFwAddSrc's source id (0x16260)
  uint32_t canvas_handle = 0xFFFFFFFFu;  // +11356, 0xFFFFFFFF from 0x15d80 until event 4099 (0x17c90)
  bool loop = false, muted = false, bgm_suppression_disabled = false;  // +11376 / +11377 / +11379
  int32_t res_w = 0, res_h = 0;          // +11380 / +11384 (event 4103, 0x17db0)
  int32_t ready_state = 0;               // +11396 (0x176b0)
  int32_t network_state = 0;             // +11400 (event 4, 0x175c0)
  bool active = false;                   // +11404 (0x16180)
  uint64_t load_pos = 0;                 // +11488
};

std::mutex g_lock;  // unk_3B948
std::unordered_map<uint32_t, std::shared_ptr<Player>> g_players;  // unk_3B8E8 (handle -> player)
uint32_t g_counter = 0xFFFFFFFFu;  // dword_3B958

// 0xe670: next handle — (counter++ ^ 0x512F967B) with its bytes permuted by the
// shift/mask tables at 0x30240 / 0x30250 ({8,16,16,8} / {0xFF0000, 0xFF, 0xFF000000, 0xFF00});
// zero and handles already in the table are skipped. Called with g_lock held.
uint32_t next_handle() {
  for (;;) {
    const uint32_t x = (g_counter++) ^ 0x512F967Bu;
    const uint32_t h = ((x >> 8) & 0x00FF0000u) | ((x >> 16) & 0x000000FFu) | ((x << 16) & 0xFF000000u) | ((x << 8) & 0x0000FF00u);
    if (h != 0 && !g_players.count(h)) return h;
  }
}

std::shared_ptr<Player> find(int32_t handle) {
  std::lock_guard<std::mutex> l(g_lock);
  auto it = g_players.find(static_cast<uint32_t>(handle));
  return it == g_players.end() ? nullptr : it->second;
}

void player_is_null(const char* fn) { log::error("psmutil", "[PSM/N video_csharp][E] {} : player is null", fn); }

const char* state_name(int32_t s) {  // 0xf1c0
  static const char* n[] = {"Undef", "Error", "Prepared", "Playing", "Paused", "PausedAtEnd", "Stopped", "PausedAtLoopEnd", "Suspended", "Emulating"};
  return s >= 0 && s < 10 ? n[s] : "?";
}

const char* command_name(uint32_t c) {  // 0x11d90
  static const char* n[] = {"Init", "Uninit", "UpdatePlayerState", "Load", "Play", "Stop", "Pause", "SetPos", "SetDisplaySize",
                            "SetLoopRange", "CancelLoopRange", "SetLoop", "SetIsMuted", "SetClosedCaptionEnabled",
                            "SetBGMSuppressionDisabled", "SetSpeed", "SetVolume", "EmulateVariableSpeedPlayback"};
  return c < 18 ? n[c] : "?";
}

// The pair every failing step stores (+168 = code, +176 = 1, each under +96).
void set_error(Player& p, int32_t code) {
  std::lock_guard<std::mutex> l(p.data);
  p.last_error = code;
  p.last_video_error = 1;
}

// 0xff20: append the command and wake the worker.
void enqueue(Player& p, const Command& c) {
  {
    std::lock_guard<std::mutex> l(p.data);
    p.commands.push_back(c);
  }
  p.cond.notify_one();
}

uint32_t source(Player& p) {
  std::lock_guard<std::mutex> l(p.data);
  return p.src_id;
}

int32_t no_source(const char* what, int32_t code = kErrState) {
  log::error("psmutil", "[PSM/N VideoPlayerMcs][E]    {}() is Skipped. Source is invalid.", what);
  return code;
}

// 0x16180 (vtable +192) ChangeActiveStatus(visible): no source -> 0x80B30004; when the value
// differs from +11404, sceVideoCoreChangeActiveStatus(src, !visible) and +11404 = visible.
int32_t mcs_change_active_status(Player& p, bool visible) {
  const uint32_t src = source(p);
  if (src == 0) return kErrState;
  std::lock_guard<std::mutex> l(g_lock);
  if (p.active == visible) return 0;
  const int32_t r = mediafw::change_active_status(src, !visible);
  if (r != 0) {
    log::error("psmutil", "[PSM/N VideoPlayerMcs][E]    sceVideoCoreChangeActiveStatus() fail. srcId[{}] ret[{:08x}]", src, static_cast<uint32_t>(r));
    return r;
  }
  p.active = visible;
  return 0;
}

// 0xee80 ChangeState: the transition guards of the current state (+392), then the append to
// the kept states (the oldest dropped once 4 are kept). Entering Prepared from another state
// first applies the visible flag through vtable +192; its failure queues Stop(withError).
int32_t change_state(Player& p, int32_t to) {
  std::unique_lock<std::mutex> l(g_lock);
  const int32_t cur = p.state;
  bool allowed = true;
  switch (cur) {
    case kError:
      allowed = to == kStopped || to == kUndef;
      break;
    case kPausedAtEnd:
      // -> PausedAtLoopEnd: ignored; -> Paused: only while +520 is set (Play-at-end pending,
      // never set on the paths reproduced here)
      if (to == kPausedAtLoopEnd || to == kPaused) return 0;
      break;
    case kPausedAtLoopEnd:
      allowed = (to & ~1) != kPaused;
      break;
    case kEmulating:
      if (to == kPaused) return 0;
      break;
    default:
      break;
  }
  if (to == kPrepared && cur != kPrepared) {
    l.unlock();
    bool vis;
    {
      std::lock_guard<std::mutex> d(p.data);
      vis = p.visible;
    }
    const int32_t r = mcs_change_active_status(p, vis);
    if (r != 0) {
      enqueue(p, Command{5, 1, r});
      return r;
    }
    l.lock();
  }
  if (!allowed || p.state == to) return 0;
  log::info("psmutil", "[PSM/N VideoPlayerBase][I] state[{}->{}]", state_name(p.state), state_name(to));
  p.state = to;
  if (p.history.size() >= 4) p.history.pop_front();
  p.history.push_back(to);
  return 0;
}

// 0x18fc0: an event equal to the last pushed one is dropped; at most 16 are kept.
void push_event(Player& p, int32_t e) {
  std::lock_guard<std::mutex> l(g_lock);
  if (p.last_event == e) return;
  p.last_event = e;
  p.events.push_back(e);
  if (p.events.size() >= 17) p.events.pop_front();
}

// --- media framework event callback (0x16500 -> 0x190c0) and its field updates ---

void update_ready_state(Player& p) {  // 0x176b0 (the binary's log calls it GetNetworkState)
  const uint32_t src = source(p);
  if (!src) return (void)no_source("UpdateReadyState");
  uint32_t v = 0;
  const int32_t r = mediafw::get_ready_state(src, &v);
  std::lock_guard<std::mutex> l(p.data);
  p.ready_state = r != 0 ? 0 : (v < 5 ? static_cast<int32_t>(v) : -1);
}

void update_duration(Player& p) {  // 0x177a0: seconds * 1000 (qword_30D40) -> +464
  double d = 0.0;
  if (mediafw::get_duration(source(p), &d) != 0) d = 0.0;
  std::lock_guard<std::mutex> l(p.data);
  p.duration_ms = static_cast<uint64_t>(d * 1000.0);
}

void update_current_time(Player& p) {  // 0x178a0: seconds * 1000 -> +456
  const uint32_t src = source(p);
  if (!src) return (void)no_source("UpdateCurrentTime");
  double t = 0.0;
  if (mediafw::get_current_time(src, &t) != 0) return;
  std::lock_guard<std::mutex> l(p.data);
  p.position_ms = static_cast<uint64_t>(t * 1000.0);
}

void update_canvas(Player& p) {  // 0x17c90: sceMediaFwGetCanvasHandle (NoZv1zNq3Ds) -> +11356, +384
  const uint32_t src = source(p);
  uint32_t canvas = 0xFFFFFFFFu;
  int32_t dr = 0;
  int32_t r = 0;
  if (src) {
    r = mediafw::get_canvas_handle(src, &canvas, &dr);
    if (r != 0) {
      log::error("psmutil", "[PSM/N VideoPlayerMcs][E] sceMediaFwGetCanvasHandle() fail. srcId[{}] ret[{:08x}]", src, static_cast<uint32_t>(r));
      canvas = 0xFFFFFFFFu;
    }
  }
  std::lock_guard<std::mutex> l(p.data);
  p.canvas_handle = canvas;
  if (src) p.dynamic_range = r != 0 ? -1 : dr;
}

void update_resolution(Player& p) {  // 0x17db0: sceMediaFwGetResolution -> +11380 / +11384
  const uint32_t src = source(p);
  int32_t w = 0, h = 0;
  if (src && mediafw::get_resolution(src, &w, &h) != 0) w = h = 0;
  std::lock_guard<std::mutex> l(p.data);
  p.res_w = w;
  p.res_h = h;
}

void on_media_event(int32_t ev, void* user) {  // 0x16500 -> 0x190c0
  Player& p = *static_cast<Player*>(user);
  switch (ev) {
    case 0: case 1: case 2: case 3: push_event(p, ev + 1); break;
    case 5: update_duration(p); update_ready_state(p); push_event(p, 5); break;
    case 6: case 7: case 8: case 9: update_ready_state(p); push_event(p, ev); break;
    case 10: update_ready_state(p); break;
    case 11: push_event(p, 10); break;
    case 12: update_current_time(p); push_event(p, 11); break;
    case 13: change_state(p, kPausedAtEnd); update_current_time(p); push_event(p, 12); break;
    case 14: update_duration(p); push_event(p, 13); break;
    case 15: update_current_time(p); push_event(p, 14); break;
    case 16: change_state(p, kPlaying); break;
    case 17: change_state(p, kPaused); break;
    case 18: change_state(p, kStopped); break;
    case 19:  // 0x18160 UpdateErrorState: the host media framework reports no error detail
      set_error(p, kErrState);
      change_state(p, kError);
      push_event(p, 22);
      break;
    case 21: push_event(p, 19); break;
    case 4099: update_canvas(p); break;
    case 4103: update_resolution(p); break;
    default: break;  // 4 / 20 / 22 / 27 / 4100 are not raised by the host media framework
  }
}

// --- VideoPlayerMcs (vtable 0x34490) ---

// 0x16260 (vtable +208) VideoCoreInit: no uri -> 0x80B30004; else sceMediaFwAddSrc with the
// callback {0x16500, player} and the source id written to +11344.
int32_t mcs_init(Player& p) {
  std::string uri;
  {
    std::lock_guard<std::mutex> l(p.data);
    p.src_id = 0;
    uri = p.uri;
  }
  if (uri.empty()) {
    log::error("psmutil", "[PSM/N VideoPlayerMcs][E] VideoCoreInit() fail. Video uri is not set");
    return kErrState;
  }
  uint32_t id = 0;
  const int32_t r = mediafw::add_src(uri, on_media_event, &p, &id);
  if (r < 0) log::error("psmutil", "[PSM/N VideoPlayerMcs][E]    sceMediaFwAddSrc() fail. ret[{:08x}]", static_cast<uint32_t>(r));
  {
    std::lock_guard<std::mutex> l(p.data);
    if (r == 0) p.src_id = id;
  }
  log::info("psmutil", "[PSM/N VideoPlayerMcs][I] Add Stream!!!   srcId = {}", r == 0 ? id : 0);
  return r;
}

// 0x16510 (vtable +216): sceMediaFwRemoveSrc when a source exists; +11344 = 0.
int32_t mcs_uninit(Player& p) {
  const uint32_t src = source(p);
  int32_t r = 0;
  if (src) {
    r = mediafw::remove_src(src);
    log::info("psmutil", "[PSM/N VideoPlayerMcs][I] Delete Stream!!!   m_srcId[{}], ret[{:08x}]", src, static_cast<uint32_t>(r));
  }
  std::lock_guard<std::mutex> l(p.data);
  p.src_id = 0;
  return r;
}

// 0x16770 (vtable +248) Load(pos): sceMediaFwLoad, +11488 = pos.
int32_t mcs_load(Player& p, uint64_t pos) {
  const uint32_t src = source(p);
  if (!src) return no_source("VideoCoreLoad");
  const int32_t r = mediafw::load(src);
  std::lock_guard<std::mutex> l(p.data);
  p.load_pos = pos;
  return r;
}

// 0x167e0 (vtable +256) Play(seek, pos, speed): playback rate speed/100; with `seek` a seek to
// pos ms (/1000.0, qword_30D40) and +11488 = pos; sceMediaFwPlay.
int32_t mcs_play(Player& p, bool seek, uint64_t pos, int32_t speed) {
  const uint32_t src = source(p);
  if (!src) return no_source("VideoCorePlay");
  mediafw::set_playback_rate(src, speed / 100.0);
  if (seek) {
    mediafw::seek(src, static_cast<double>(pos) / 1000.0);
    const int32_t r = mediafw::play(src);
    std::lock_guard<std::mutex> l(p.data);
    p.load_pos = pos;
    return r;
  }
  return mediafw::play(src);
}

int32_t mcs_pause(Player& p) {  // 0x16900 (vtable +264): sceMediaFwPause
  const uint32_t src = source(p);
  return src ? mediafw::pause(src) : no_source("VideoCorePause");
}

int32_t mcs_stop(Player& p) {  // 0x168c0 (vtable +272): sceMediaFwStop
  const uint32_t src = source(p);
  return src ? mediafw::stop(src) : no_source("VideoCoreStop");
}

int32_t mcs_set_speed(Player& p, int32_t speed) {  // 0x16a50 (vtable +304)
  const uint32_t src = source(p);
  if (!src) return no_source("VideoCoreSetSpeed");
  log::info("psmutil", "VideoCoreSetSpeed: {}", speed);
  const int32_t r = mediafw::set_playback_rate(src, speed / 100.0);
  if (r != 0) return r;
  std::lock_guard<std::mutex> l(p.data);
  p.speed = speed;
  return 0;
}

int32_t mcs_set_display_size(Player& p, int32_t w, int32_t h) {  // 0x16b40 (vtable +336)
  const uint32_t src = source(p);
  if (!src) return no_source("VideoCoreSetDisplaySize");
  {
    std::lock_guard<std::mutex> l(p.data);
    p.display_w = w;
    p.display_h = h;
  }
  mediafw::set_display_size(src, w, h);
  return 0;
}

int32_t mcs_cancel_loop_range(Player& p) {  // 0x16df0 (vtable +360): playback range {0, 0}
  const uint32_t src = source(p);
  return src ? mediafw::cancel_playback_range(src) : no_source("VideoCoreCancelLoopRange");
}

template <class F>
int32_t mcs_flag(Player& p, const char* what, bool v, bool Player::* field, F call) {  // 0x16f60 / 0x17040 / 0x17280
  const uint32_t src = source(p);
  if (!src) return no_source(what);
  const int32_t r = call(src, v);
  if (r != 0) return r;
  std::lock_guard<std::mutex> l(p.data);
  p.*field = v;
  return 0;
}

int32_t mcs_set_volume(Player& p, int32_t v) {  // 0x17200 (vtable +440): volume/100, always 0
  const uint32_t src = source(p);
  if (!src) return no_source("VideoCoreSetVolume");
  mediafw::set_volume(src, v / 100.0);
  return 0;
}

// --- base player commands (run on the worker) ---

bool oldest_state(Player& p, int32_t& s) {
  std::lock_guard<std::mutex> l(g_lock);
  if (p.history.empty()) return false;
  s = p.history.front();
  return true;
}

// 0xfff0 Init: only while the oldest kept state is Undef (else 0x80B30004); vtable +208; its
// failure stores the error and enters Error, success enters Prepared.
int32_t do_init(Player& p) {
  int32_t head;
  if (oldest_state(p, head) && head != kUndef) return kErrState;
  const int32_t r = mcs_init(p);
  if (r != 0) {
    set_error(p, r);
    change_state(p, kError);
    return r;
  }
  change_state(p, kPrepared);
  return 0;
}

// 0x10290 Uninit: vtable +216; failure -> error + Error; else Undef and the playback fields
// reset (+456..+487 zeroed, +488 speed = 100, +520 cleared).
int32_t do_uninit(Player& p) {
  const int32_t r = mcs_uninit(p);
  if (r != 0) {
    set_error(p, r);
    change_state(p, kError);
    return r;
  }
  change_state(p, kUndef);
  std::lock_guard<std::mutex> l(p.data);
  p.position_ms = p.duration_ms = 0;
  p.speed = 100;
  return 0;
}

// 0x103c0 Load(pos): vtable +248; failure -> error + Error; else vtable +192 with +680.
int32_t do_load(Player& p, uint64_t pos) {
  const int32_t r = mcs_load(p, pos);
  if (r != 0) {
    set_error(p, r);
    change_state(p, kError);
    return r;
  }
  bool vis;
  {
    std::lock_guard<std::mutex> l(p.data);
    vis = p.visible;
  }
  return mcs_change_active_status(p, vis);
}

// 0x104d0 Play(seek, pos, speed): oldest kept state Error -> 0x80B30004; else vtable +256,
// whose failure stores the error and enters Error.
int32_t do_play(Player& p, bool seek, uint64_t pos, int32_t speed) {
  int32_t head;
  if (oldest_state(p, head) && head != kEmulating && head == kError) return kErrState;
  const int32_t r = mcs_play(p, seek, pos, speed);
  if (r != 0) {
    set_error(p, r);
    change_state(p, kError);
  }
  return r;
}

// 0x105c0 Stop(withError, code): with +682 set vtable +272 runs (its failure replaces the
// code); with withError or a failed stop the code is stored and the state becomes Error.
int32_t do_stop(Player& p, bool with_error, int32_t code) {
  bool ok;
  {
    std::lock_guard<std::mutex> l(p.data);
    ok = p.cmd_ok;
  }
  int32_t ret = 0;
  if (ok) {
    ret = mcs_stop(p);
    if (ret != 0) code = ret;
    else if (!with_error) return 0;
  } else if (!with_error) {
    return 0;
  }
  set_error(p, code);
  change_state(p, kError);
  return ret;
}

// 0x11790: the worker. Waits up to 300 ms on +128, runs vtable +24 (0x16130: 0), then takes
// the queued commands one by one and evaluates each against the newest kept state: with none
// (or Undef) only ids < 3 run, in Error only Uninit / UpdatePlayerState / Stop (mask 38); the
// rest log "EvaluateCommand fail". After the queue, 0x113a0 (no speed emulation: 0) sets +682.
void worker_main(std::shared_ptr<Player> pp) {
  Player& p = *pp;
  std::unique_lock<std::mutex> l(p.data);
  for (;;) {
    p.cond.wait_for(l, std::chrono::milliseconds(300), [&] { return !p.commands.empty() || p.quit; });
    while (!p.commands.empty()) {
      const Command c = p.commands.front();
      p.commands.pop_front();
      l.unlock();
      int32_t cur = kUndef;
      {
        std::lock_guard<std::mutex> g(g_lock);
        if (!p.history.empty()) cur = p.history.back();
      }
      const bool run = cur == kUndef ? c.id < 3 : cur == kError ? (c.id <= 5 && ((38u >> c.id) & 1u)) : true;
      if (!run) {
        log::error("psmutil", "[PSM/N VideoPlayerBase][E] EvaluateCommand fail command={} (state={})", command_name(c.id), state_name(cur));
      } else {
        log::info("psmutil", "[PSM/N VideoPlayerBase][I] ProcessCommand : {} (State={})", command_name(c.id), state_name(cur));
        int32_t r = 0;
        switch (c.id) {
          case 0: r = do_init(p); break;
          case 1: r = do_uninit(p); break;
          case 3: r = do_load(p, c.q); break;
          case 4: r = do_play(p, c.flag != 0, c.q, c.i2); break;
          case 5: r = do_stop(p, c.flag != 0, c.i1); break;
          case 6: {  // Pause: vtable +264; on failure back to the oldest kept state (Emulating -> Playing)
            int32_t keep = kUndef;
            oldest_state(p, keep);
            if (keep == kEmulating) keep = kPlaying;
            r = mcs_pause(p);
            if (r != 0) change_state(p, keep);
            break;
          }
          case 7: r = no_source("VideoCoreSetPos"); break;  // SetPosition is not reached by the shell paths reproduced
          case 8: r = mcs_set_display_size(p, c.i1, static_cast<int32_t>(c.q)); break;
          case 9: r = no_source("VideoCoreSetLoopRange"); break;
          case 10: r = mcs_cancel_loop_range(p); break;
          case 11: r = mcs_flag(p, "VideoCoreSetLoop", c.flag != 0, &Player::loop, mediafw::set_loop); break;
          case 12: r = mcs_flag(p, "VideoCoreSetIsMuted", c.flag != 0, &Player::muted, mediafw::set_muted); break;
          case 13: r = no_source("VideoCoreSetClosedCaptionEnabled"); break;
          case 14: r = mcs_flag(p, "VideoCoreSetDisableBGMSuppression", c.flag != 0, &Player::bgm_suppression_disabled, mediafw::suppress_bgm); break;
          case 15: r = mcs_set_speed(p, c.i1); break;
          case 16: r = mcs_set_volume(p, c.i1); break;
          default: break;
        }
        // +682 after the command: Uninit / Stop clear it; Init / Load / Play / Emulate set it
        // when they succeeded; UpdatePlayerState and ids 6..16 leave it.
        if (c.id == 1 || c.id == 5) {
          std::lock_guard<std::mutex> d(p.data);
          p.cmd_ok = false;
        } else if ((c.id == 0 || c.id == 3 || c.id == 4 || c.id == 17) && r == 0) {
          std::lock_guard<std::mutex> d(p.data);
          p.cmd_ok = true;
        }
      }
      l.lock();
    }
    if (p.quit) return;
    if (!p.emulating) p.cmd_ok = true;
  }
}

bool to_utf8(MonoString* s, std::string& out) {
  if (!s) return false;
  char* u = mono_string_to_utf8(s);
  if (!u) return false;
  out = u;
  mono_free(u);
  return true;
}

// int Create(out int result, string uri, int videoCoreType, int createMode, int width,
//   int height, int renderBufferType, int coreResourceType, int coreAudioResourceType,
//   int autoStopType, int autoStopTime, string contentType, int behaviorTypeOnActive,
//   int behaviorTypeOnInactive, int http404RetryCount, int http404RetryInteval,
//   int playerStateModeType, string userAgent, string accessToken, uint deviceID) — 0x94f0.
// null `result` -> 0x80B30002; *result = 0; uri (and any non-null contentType / userAgent /
// accessToken) not convertible -> 0x80B30013. VideoPlayerFactory (0x15430) allocates the
// player, registers it under a new handle and stores the handle in *result; vtable +64
// (0x11220) stores the uri and 0x126d0 starts the worker 0x11790.
int32_t Create(int32_t* result, MonoString* uri, int32_t video_core_type, int32_t create_mode, int32_t width, int32_t height,
               int32_t, int32_t, int32_t, int32_t, int32_t, MonoString* content_type, int32_t, int32_t, int32_t, int32_t,
               int32_t, MonoString* user_agent, MonoString* access_token, uint32_t) {
  if (!result) return kErrInvalidPointer;
  *result = 0;
  auto p = std::make_shared<Player>();
  if (!to_utf8(uri, p->uri)) return kErrCreate;
  if (content_type && !to_utf8(content_type, p->content_type)) return kErrCreate;
  if (user_agent && !to_utf8(user_agent, p->user_agent)) return kErrCreate;
  if (access_token && !to_utf8(access_token, p->access_token)) return kErrCreate;
  p->video_core_type = video_core_type;
  p->create_mode = create_mode;
  p->width = width;
  p->height = height;
  uint32_t handle;
  {
    std::lock_guard<std::mutex> l(g_lock);
    handle = next_handle();
    g_players[handle] = p;
  }
  *result = static_cast<int32_t>(handle);
  log::info("psmutil", "[PSM/N VideoPlayerFactory][I] CreateVideoplayer Succcess. handle[{:08x}] uri={}", handle, p->uri);
  p->worker = std::thread(worker_main, p);
  return 0;
}

// int Delete(int handle) — 0x9ad0: stops the worker (0x12820: +681 = 1, signal, join; 0x127e0)
// and removes the handle (0x15530); an unknown handle only logs "player is null". The host
// also removes a source still attached (the player object the callback points at goes away).
int32_t Delete(int32_t handle) {
  std::shared_ptr<Player> p;
  {
    std::lock_guard<std::mutex> l(g_lock);
    auto it = g_players.find(static_cast<uint32_t>(handle));
    if (it != g_players.end()) {
      p = it->second;
      g_players.erase(it);
    }
  }
  if (!p) {
    player_is_null("VideoPlayer_Delete");
    return 0;
  }
  {
    std::lock_guard<std::mutex> l(p->data);
    p->quit = true;
  }
  p->cond.notify_one();
  if (p->worker.joinable()) p->worker.join();
  if (const uint32_t src = source(*p)) mediafw::remove_src(src);
  return 0;
}

// The command icalls: unknown handle -> 0x80B30005, else the command is queued (0xff20) and 0.
int32_t queue(int32_t handle, const char* fn, const Command& c) {
  auto p = find(handle);
  if (!p) {
    player_is_null(fn);
    return kErrNullPlayer;
  }
  enqueue(*p, c);
  return 0;
}

int32_t Init(int32_t h) { return queue(h, "VideoPlayer_Init", Command{0}); }                          // 0xad90 -> 0xfef0
int32_t Uninit(int32_t h) { return queue(h, "VideoPlayer_Uninit", Command{1}); }                      // 0xae80 -> 0x10260
int32_t Load(int32_t h, uint64_t pos) { return queue(h, "VideoPlayer_Load", Command{3, 0, 0, pos}); }  // 0xaf70 -> 0x10380
int32_t Play0(int32_t h) { return queue(h, "VideoPlayer_Play", Command{4, 0, 0, 0, 100}); }             // 0xb060 -> 0x10480(p, 0, 0, 100)
int32_t Play(int32_t h, uint64_t pos) { return queue(h, "VideoPlayer_Play", Command{4, 1, 0, pos, 100}); }  // 0xb150 -> 0x10480(p, 1, pos, 100)
int32_t Stop(int32_t h) { return queue(h, "VideoPlayer_Stop", Command{5, 0, 0}); }                    // 0xb340 -> 0xf170(p, 0, 0)
int32_t Pause(int32_t h) { return queue(h, "VideoPlayer_Pause", Command{6}); }                        // 0xb530 -> 0x10670
int32_t SetDisplaySize(int32_t h, int32_t w, int32_t hh) {                                           // 0xbbc0 -> 0x10d70
  return queue(h, "VideoPlayer_SetDisplaySize", Command{8, 0, w, static_cast<uint64_t>(hh)});
}
int32_t CancelLoopRange(int32_t h) { return queue(h, "VideoPlayer_CancelLoopRange", Command{10}); }    // 0xc340 -> 0x10fe0
int32_t SetLoop(int32_t h, uint8_t v) { return queue(h, "VideoPlayer_SetLoop", Command{11, v}); }      // 0xc560 -> 0x11030
int32_t SetIsMuted(int32_t h, uint8_t v) { return queue(h, "VideoPlayer_SetIsMuted", Command{12, v}); }  // 0xc740 -> 0x11090
int32_t SetBGMSuppressionDisabled(int32_t h, uint8_t v) {                                             // 0xcb00 -> 0x11130
  return queue(h, "VideoPlayer_SetBGMSuppressionDisabled", Command{14, v});
}
int32_t SetSpeed(int32_t h, int32_t v) { return queue(h, "VideoPlayer_SetSpeed", Command{15, 0, v}); }    // 0xb9e0 -> 0x10cc0
int32_t SetVolume(int32_t h, int32_t v) { return queue(h, "VideoPlayer_SetVolume", Command{16, 0, v}); }  // 0xcce0 -> 0x111b0

// void Update(int handle) — 0x9bc0: vtable +16 (0x16120 -> 0xf880(player, 0)), a read of the
// oldest kept state without popping (the result is discarded by the icall).
void Update(int32_t h) {
  if (!find(h)) player_is_null("VideoPlayer_Update");
}

// void UpdatePlayerState(int handle) — 0x9cb0: vtable +24 (0x16130) returns 0.
void UpdatePlayerState(int32_t h) {
  if (!find(h)) player_is_null("VideoPlayer_UpdatePlayerState");
}

// int GetPlayerStatus(int handle, out int status, bool popQueue, out int remain) — 0x9da0 ->
// 0xf920: the oldest kept state (popped first when asked and more than one is kept; Emulating
// reads as Playing), remain = kept - 1; nothing kept -> status 0, remain -1.
int32_t GetPlayerStatus(int32_t h, int32_t* status, uint8_t pop, int32_t* remain) {
  if (status) *status = 0;
  if (remain) *remain = 0;
  auto p = find(h);
  if (!p) {
    player_is_null("VideoPlayer_GetPlayerStatus");
    return kErrNullPlayer;
  }
  std::lock_guard<std::mutex> l(g_lock);
  int32_t s = 0, rem = -1;
  if (!p->history.empty()) {
    const int32_t head = p->history.front();
    if (p->history.size() != 1 && pop) p->history.pop_front();
    rem = static_cast<int32_t>(p->history.size()) - 1;
    s = head == kEmulating ? kPlaying : head;
  }
  if (remain) *remain = rem;
  if (status) *status = s;
  return 0;
}

// int GetLastPlayerStatus(int handle, out int status) — 0x9ed0 -> 0xfaf0: the newest kept
// state (0 with none).
int32_t GetLastPlayerStatus(int32_t h, int32_t* status) {
  auto p = find(h);
  if (!p) {
    player_is_null("VideoPlayer_GetLastPlayerStatus");
    return kErrNullPlayer;
  }
  std::lock_guard<std::mutex> l(g_lock);
  if (status) *status = p->history.empty() ? 0 : p->history.back();
  return 0;
}

// Synchronous getters reading one field under +96. `fail` is what an unknown handle returns.
template <class T>
T field(int32_t h, const char* fn, T Player::* f, T fail) {
  auto p = find(h);
  if (!p) {
    player_is_null(fn);
    return fail;
  }
  std::lock_guard<std::mutex> l(p->data);
  return (*p).*f;
}
// GetLastErrorCode / GetLastErrorCodeDetail / GetLastVideoPlayerError — 0x9fd0 / 0xa0b0 /
// 0xa190 -> 0xf9e0 / 0xfa10 / 0xfa40: +168 / +172 / +176; unknown handle -> 0.
int32_t GetLastErrorCode(int32_t h) { return field(h, "VideoPlayer_GetLastErrorCode", &Player::last_error, 0); }
int32_t GetLastErrorCodeDetail(int32_t h) { return field(h, "VideoPlayer_GetLastErrorCodeDetail", &Player::last_error_detail, 0); }
int32_t GetLastVideoPlayerError(int32_t h) { return field(h, "VideoPlayer_GetLastVideoPlayerError", &Player::last_video_error, 0); }
// GetVideoWidth / GetVideoHeight — 0xbcc0 / 0xbdb0 -> vtable +320 / +328 (0x16c20 / 0x16c50):
// +11380 / +11384. GetDynamicRange — 0xceb0 -> vtable +456 (0x173d0): +384. GetReadyState —
// 0xa510 -> vtable +88 (0x174a0): +11396. GetNetworkState — 0xa420 -> vtable +96 (0x17470):
// +11400. Unknown handle -> 0x80B30005.
int32_t GetVideoWidth(int32_t h) { return field(h, "VideoPlayer_GetVideoWidth", &Player::res_w, kErrNullPlayer); }
int32_t GetVideoHeight(int32_t h) { return field(h, "VideoPlayer_GetVideoHeight", &Player::res_h, kErrNullPlayer); }
int32_t GetDynamicRange(int32_t h) { return field(h, "VideoPlayer_GetDynamicRange", &Player::dynamic_range, kErrNullPlayer); }
int32_t GetReadyState(int32_t h) { return field(h, "VideoPlayer_GetReadyState", &Player::ready_state, kErrNullPlayer); }
int32_t GetNetworkState(int32_t h) { return field(h, "VideoPlayer_GetNetworkState", &Player::network_state, kErrNullPlayer); }
// ulong GetDuration(int) — 0xa9b0 -> 0x10350 -> vtable +224 (0x166a0): +464 (ms).
uint64_t GetDuration(int32_t h) { return field<uint64_t>(h, "VideoPlayer_GetDuration", &Player::duration_ms, 0); }
// ulong GetPosition(int) — 0xb810 -> 0x10980: without speed emulation vtable +288 (0x169e0): +456 (ms).
uint64_t GetPosition(int32_t h) { return field<uint64_t>(h, "VideoPlayer_GetPosition", &Player::position_ms, 0); }

// int GetVideoEventQueue(int handle, int[] queue) — 0xda20 -> 0xfc00: copies the kept events
// in order and clears them (a single 0 written when there are none); +424 is reset.
int32_t GetVideoEventQueue(int32_t h, MonoArray* arr) {
  auto p = find(h);
  if (!p) {
    player_is_null("VideoPlayer_GetVideoEventQueue");
    return kErrNullPlayer;
  }
  int32_t* dst = arr ? reinterpret_cast<int32_t*>(mono_array_addr_with_size(arr, 4, 0)) : nullptr;
  const size_t cap = arr ? mono_array_length(arr) : 0;
  std::lock_guard<std::mutex> l(g_lock);
  if (!p->events.empty()) {
    size_t i = 0;
    for (int32_t e : p->events)
      if (i < cap) dst[i++] = e;  // at most 16 kept, the size of the managed array
    p->events.clear();
  } else if (cap) {
    dst[0] = 0;
  }
  p->last_event = 0;
  return 0;
}

// int GetUri(int handle, out string uri) — 0xbea0 -> 0x10df0: the uri at +232.
int32_t GetUri(int32_t h, MonoString** out) {
  auto p = find(h);
  if (!p) {
    player_is_null("VideoPlayer_GetUri");
    return kErrNullPlayer;
  }
  std::string u;
  {
    std::lock_guard<std::mutex> l(p->data);
    u = p->uri;
  }
  if (out) psm::store_ref(out, psm::new_string(u));
  return 0;
}

// int SetVisible(int handle, bool visible) — 0xa7e0 -> 0xfe40: unknown handle ->
// 0x80B30005; when the value changes vtable +192 runs (its 0x80B30004 counts as success)
// and the flag at +680 is stored.
int32_t SetVisible(int32_t handle, uint8_t visible) {
  auto p = find(handle);
  if (!p) {
    player_is_null("VideoPlayer_SetVisible");
    return kErrNullPlayer;
  }
  bool changed;
  {
    std::lock_guard<std::mutex> l(p->data);
    changed = p->visible != (visible != 0);
  }
  if (changed) {
    const int32_t r = mcs_change_active_status(*p, visible != 0);
    if (r != 0 && r != kErrState) return r;
    std::lock_guard<std::mutex> l(p->data);
    p->visible = visible != 0;
  }
  return 0;
}

// bool GetVisible(int handle) — 0xa8d0 -> 0xfec0: the flag at +680; false when unknown.
uint8_t GetVisible(int32_t handle) {
  auto p = find(handle);
  if (!p) {
    player_is_null("VideoPlayer_GetVisible");
    return 0;
  }
  std::lock_guard<std::mutex> l(p->data);
  return p->visible ? 1 : 0;
}

// int GetStreamId(int handle) — 0xa600: unknown handle -> 0x80B30005; else vtable +0x30
// (0x16170): the source id at +11344.
int32_t GetStreamId(int32_t handle) {
  auto p = find(handle);
  if (!p) {
    player_is_null("VideoPlayer_GetStreamId");
    return kErrNullPlayer;
  }
  return static_cast<int32_t>(source(*p));
}

// uint GetCanvasHandle(int handle, int streamId) — 0xa6f0: unknown handle -> 0x80B30005;
// else vtable +0x38 (0x173a0): the canvas handle at +11356.
uint32_t GetCanvasHandle(int32_t handle, int32_t) {
  auto p = find(handle);
  if (!p) {
    player_is_null("VideoPlayer_GetCanvasHandle");
    return static_cast<uint32_t>(kErrNullPlayer);
  }
  std::lock_guard<std::mutex> l(p->data);
  return p->canvas_handle;
}

}  // namespace

SSN_ICALL("Sce.Vsh.Video.PsmVideoPlayer::Create", Create);
SSN_ICALL("Sce.Vsh.Video.PsmVideoPlayer::Delete", Delete);
SSN_ICALL("Sce.Vsh.Video.PsmVideoPlayer::Init", Init);
SSN_ICALL("Sce.Vsh.Video.PsmVideoPlayer::Uninit", Uninit);
SSN_ICALL("Sce.Vsh.Video.PsmVideoPlayer::Load", Load);
SSN_ICALL("Sce.Vsh.Video.PsmVideoPlayer::Play(int)", Play0);
SSN_ICALL("Sce.Vsh.Video.PsmVideoPlayer::Play(int,ulong)", Play);
SSN_ICALL("Sce.Vsh.Video.PsmVideoPlayer::Stop", Stop);
SSN_ICALL("Sce.Vsh.Video.PsmVideoPlayer::Pause", Pause);
SSN_ICALL("Sce.Vsh.Video.PsmVideoPlayer::SetDisplaySize", SetDisplaySize);
SSN_ICALL("Sce.Vsh.Video.PsmVideoPlayer::CancelLoopRange", CancelLoopRange);
SSN_ICALL("Sce.Vsh.Video.PsmVideoPlayer::SetLoop", SetLoop);
SSN_ICALL("Sce.Vsh.Video.PsmVideoPlayer::SetIsMuted", SetIsMuted);
SSN_ICALL("Sce.Vsh.Video.PsmVideoPlayer::SetBGMSuppressionDisabled", SetBGMSuppressionDisabled);
SSN_ICALL("Sce.Vsh.Video.PsmVideoPlayer::SetSpeed", SetSpeed);
SSN_ICALL("Sce.Vsh.Video.PsmVideoPlayer::SetVolume", SetVolume);
SSN_ICALL("Sce.Vsh.Video.PsmVideoPlayer::Update", Update);
SSN_ICALL("Sce.Vsh.Video.PsmVideoPlayer::UpdatePlayerState", UpdatePlayerState);
SSN_ICALL("Sce.Vsh.Video.PsmVideoPlayer::GetPlayerStatus", GetPlayerStatus);
SSN_ICALL("Sce.Vsh.Video.PsmVideoPlayer::GetLastPlayerStatus", GetLastPlayerStatus);
SSN_ICALL("Sce.Vsh.Video.PsmVideoPlayer::GetLastErrorCode", GetLastErrorCode);
SSN_ICALL("Sce.Vsh.Video.PsmVideoPlayer::GetLastErrorCodeDetail", GetLastErrorCodeDetail);
SSN_ICALL("Sce.Vsh.Video.PsmVideoPlayer::GetLastVideoPlayerError", GetLastVideoPlayerError);
SSN_ICALL("Sce.Vsh.Video.PsmVideoPlayer::GetVideoWidth", GetVideoWidth);
SSN_ICALL("Sce.Vsh.Video.PsmVideoPlayer::GetVideoHeight", GetVideoHeight);
SSN_ICALL("Sce.Vsh.Video.PsmVideoPlayer::GetDynamicRange", GetDynamicRange);
SSN_ICALL("Sce.Vsh.Video.PsmVideoPlayer::GetReadyState", GetReadyState);
SSN_ICALL("Sce.Vsh.Video.PsmVideoPlayer::GetNetworkState", GetNetworkState);
SSN_ICALL("Sce.Vsh.Video.PsmVideoPlayer::GetDuration(int)", GetDuration);
SSN_ICALL("Sce.Vsh.Video.PsmVideoPlayer::GetPosition(int)", GetPosition);
SSN_ICALL("Sce.Vsh.Video.PsmVideoPlayer::GetVideoEventQueue", GetVideoEventQueue);
SSN_ICALL("Sce.Vsh.Video.PsmVideoPlayer::GetUri", GetUri);
SSN_ICALL("Sce.Vsh.Video.PsmVideoPlayer::SetVisible", SetVisible);
SSN_ICALL("Sce.Vsh.Video.PsmVideoPlayer::GetVisible", GetVisible);
SSN_ICALL("Sce.Vsh.Video.PsmVideoPlayer::GetStreamId", GetStreamId);
SSN_ICALL("Sce.Vsh.Video.PsmVideoPlayer::GetCanvasHandle", GetCanvasHandle);

}  // namespace ssn::psmutil
