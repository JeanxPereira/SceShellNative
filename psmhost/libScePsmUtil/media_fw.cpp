// Host media framework (see media_fw.h). DECLARED HOST VALUE throughout.
//
// Event order raised for a source (becore's own order is not in any binary examined; these
// are the media-framework event ids libScePsmUtil's callback 0x190c0 maps, see video_player.cpp):
//   Load:  first frame decoded and published, then 4103 (resolution), 4099 (canvas handle),
//          14 (duration), 5 (metadata, ready state 1), 6 (ready 2), 7 (ready 3), 8 (ready 4)
//   Play:  16 (playing), then 15 (time update) every 250 ms of media time while playing
//   Pause: 17;  Stop: 18
//   End of media: loop set -> back to 0 without an event; else 13 (ended)
//   Decoder failure: 19
#include "libScePsmUtil/media_fw.h"

#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <mfapi.h>
#include <mferror.h>
#include <mfidl.h>
#include <mfreadwrite.h>
#include <propvarutil.h>

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <filesystem>
#include <memory>
#include <mutex>
#include <thread>
#include <unordered_map>
#include <vector>

#include "core/firmware.h"
#include "core/log.h"
#include "host/boundary.h"
#include "host/context.h"
#include "libSceComposite/canvas.h"

#pragma comment(lib, "mfplat.lib")
#pragma comment(lib, "mfreadwrite.lib")
#pragma comment(lib, "mfuuid.lib")
#pragma comment(lib, "ole32.lib")
#pragma comment(lib, "propsys.lib")

namespace ssn::psmutil::mediafw {
namespace {

constexpr int32_t kErrNoEnt = static_cast<int32_t>(0x80020002u);    // SCE_KERNEL_ERROR_ENOENT (host choice)
constexpr int32_t kErrInvalid = static_cast<int32_t>(0x80020016u);  // SCE_KERNEL_ERROR_EINVAL (host choice)

using Clock = std::chrono::steady_clock;

struct Source {
  std::wstring path;
  std::string uri;
  EventCallback cb = nullptr;
  void* user = nullptr;

  std::mutex m;
  std::condition_variable cv;
  std::thread thread;
  bool quit = false, load_requested = false, loaded = false, playing = false, loop = false, restart = false, rebase = false;
  double rate = 1.0;
  double seek_to = -1.0;
  // Read by the getters.
  int32_t w = 0, h = 0;
  uint32_t canvas = 0xFFFFFFFFu;
  uint32_t ready_state = 0;
  double duration = 0.0, current = 0.0;
};

std::mutex g_lock;
std::unordered_map<uint32_t, std::shared_ptr<Source>> g_sources;
uint32_t g_next = 1;

std::shared_ptr<Source> find(uint32_t id) {
  std::lock_guard<std::mutex> l(g_lock);
  auto it = g_sources.find(id);
  return it == g_sources.end() ? nullptr : it->second;
}

void raise(Source& s, int32_t ev) {
  if (s.cb) s.cb(ev, s.user);
}

template <class T>
void release(T*& p) {
  if (p) p->Release();
  p = nullptr;
}

// BGRX (MFVideoFormat_RGB32) -> RGBA8 with alpha 255, top row first.
bool frame_to_rgba(IMFSample* sample, int32_t w, int32_t h, LONG default_stride, std::vector<uint8_t>& out) {
  IMFMediaBuffer* buf = nullptr;
  if (FAILED(sample->ConvertToContiguousBuffer(&buf))) return false;
  out.resize(size_t(w) * h * 4);
  bool ok = false;
  IMF2DBuffer* b2 = nullptr;
  BYTE* scan0 = nullptr;
  LONG pitch = 0;
  if (SUCCEEDED(buf->QueryInterface(IID_PPV_ARGS(&b2))) && SUCCEEDED(b2->Lock2D(&scan0, &pitch))) {
    ok = true;
  } else {
    BYTE* p = nullptr;
    DWORD len = 0;
    if (SUCCEEDED(buf->Lock(&p, nullptr, &len))) {
      pitch = default_stride ? default_stride : w * 4;
      scan0 = pitch < 0 ? p + size_t(-pitch) * (h - 1) : p;
      ok = len >= DWORD(std::abs(pitch)) * DWORD(h);
      if (!ok) buf->Unlock();
    }
  }
  if (ok) {
    for (int32_t y = 0; y < h; ++y) {
      const uint8_t* src = scan0 + ptrdiff_t(pitch) * y;
      uint8_t* dst = out.data() + size_t(y) * w * 4;
      for (int32_t x = 0; x < w; ++x) {
        dst[4 * x + 0] = src[4 * x + 2];
        dst[4 * x + 1] = src[4 * x + 1];
        dst[4 * x + 2] = src[4 * x + 0];
        dst[4 * x + 3] = 255;
      }
    }
    if (b2) b2->Unlock2D();
    else buf->Unlock();
  }
  release(b2);
  release(buf);
  return ok;
}

void source_thread(std::shared_ptr<Source> sp) {
  Source& s = *sp;
  CoInitializeEx(nullptr, COINIT_MULTITHREADED);
  MFStartup(MF_VERSION, MFSTARTUP_LITE);
  {
    std::unique_lock<std::mutex> l(s.m);
    s.cv.wait(l, [&] { return s.load_requested || s.quit; });
    if (s.quit) {
      MFShutdown();
      CoUninitialize();
      return;
    }
  }
  IMFAttributes* attr = nullptr;
  IMFSourceReader* reader = nullptr;
  IMFMediaType* type = nullptr;
  bool ok = SUCCEEDED(MFCreateAttributes(&attr, 1)) && SUCCEEDED(attr->SetUINT32(MF_SOURCE_READER_ENABLE_VIDEO_PROCESSING, TRUE)) &&
            SUCCEEDED(MFCreateSourceReaderFromURL(s.path.c_str(), attr, &reader));
  if (ok) {
    reader->SetStreamSelection(DWORD(MF_SOURCE_READER_ALL_STREAMS), FALSE);
    reader->SetStreamSelection(DWORD(MF_SOURCE_READER_FIRST_VIDEO_STREAM), TRUE);
    ok = SUCCEEDED(MFCreateMediaType(&type)) && SUCCEEDED(type->SetGUID(MF_MT_MAJOR_TYPE, MFMediaType_Video)) &&
         SUCCEEDED(type->SetGUID(MF_MT_SUBTYPE, MFVideoFormat_RGB32)) &&
         SUCCEEDED(reader->SetCurrentMediaType(DWORD(MF_SOURCE_READER_FIRST_VIDEO_STREAM), nullptr, type));
  }
  UINT32 w = 0, h = 0;
  LONG stride = 0;
  double duration = 0.0;
  if (ok) {
    IMFMediaType* cur = nullptr;
    if (SUCCEEDED(reader->GetCurrentMediaType(DWORD(MF_SOURCE_READER_FIRST_VIDEO_STREAM), &cur))) {
      MFGetAttributeSize(cur, MF_MT_FRAME_SIZE, &w, &h);
      stride = static_cast<LONG>(MFGetAttributeUINT32(cur, MF_MT_DEFAULT_STRIDE, w * 4));
      release(cur);
    }
    PROPVARIANT var;
    PropVariantInit(&var);
    if (SUCCEEDED(reader->GetPresentationAttribute(DWORD(MF_SOURCE_READER_MEDIASOURCE), MF_PD_DURATION, &var)) && var.vt == VT_UI8)
      duration = double(var.uhVal.QuadPart) / 1e7;
    PropVariantClear(&var);
    ok = w > 0 && h > 0;
  }
  log::info("psmutil", "[mediafw] {}: Media Foundation {} {}x{} {:.3f} s", s.uri, ok ? "opened" : "failed to open", w, h, duration);
  const uint32_t canvas = composite::canvas_create();
  std::vector<uint8_t> rgba;
  auto read_frame = [&](LONGLONG& ts, bool& eos) -> bool {
    DWORD flags = 0;
    IMFSample* sample = nullptr;
    eos = false;
    if (FAILED(reader->ReadSample(DWORD(MF_SOURCE_READER_FIRST_VIDEO_STREAM), 0, nullptr, &flags, &ts, &sample))) return false;
    if (flags & MF_SOURCE_READERF_ENDOFSTREAM) {
      eos = true;
      release(sample);
      return true;
    }
    if (!sample) return true;  // gap / format change without data
    const bool good = frame_to_rgba(sample, int32_t(w), int32_t(h), stride, rgba);
    release(sample);
    // the caller publishes the frame when it is due
    return good;
  };
  LONGLONG ts = 0;
  bool eos = false;
  if (ok) ok = read_frame(ts, eos) && !eos;
  if (ok) composite::canvas_publish(canvas, int32_t(w), int32_t(h), std::move(rgba));
  if (!ok) {
    raise(s, 19);
  } else {
    {
      std::lock_guard<std::mutex> l(s.m);
      s.w = int32_t(w);
      s.h = int32_t(h);
      s.canvas = canvas;
      s.duration = duration;
      s.current = double(ts) / 1e7;
      s.loaded = true;
    }
    raise(s, 4103);
    raise(s, 4099);
    raise(s, 14);
    for (uint32_t rs = 1; rs <= 4; ++rs) {
      {
        std::lock_guard<std::mutex> l(s.m);
        s.ready_state = rs;
      }
      raise(s, int32_t(4 + rs));  // 5 metadata, 6 data, 7 can play, 8 can play through
    }
    // Playback: present each decoded frame at its time relative to the moment play began.
    Clock::time_point base_wall = Clock::now();
    LONGLONG base_ts = ts;
    double last_update = -1.0;
    bool have_pending = false;  // `ts` holds a decoded frame not yet presented
    for (;;) {
      std::unique_lock<std::mutex> l(s.m);
      s.cv.wait(l, [&] { return s.quit || s.playing || s.restart || s.seek_to >= 0.0; });
      if (s.quit) break;
      if (s.seek_to >= 0.0 || s.restart) {
        const double to = s.seek_to >= 0.0 ? s.seek_to : 0.0;
        s.seek_to = -1.0;
        s.restart = false;
        l.unlock();
        PROPVARIANT pos;
        InitPropVariantFromInt64(LONGLONG(to * 1e7), &pos);
        reader->SetCurrentPosition(GUID_NULL, pos);
        PropVariantClear(&pos);
        have_pending = false;
        base_wall = Clock::now();
        base_ts = LONGLONG(to * 1e7);
        continue;
      }
      const bool rebase = s.rebase;
      s.rebase = false;
      const double rate = s.rate > 0.0 ? s.rate : 1.0;
      const double cur_pos = s.current;
      l.unlock();
      if (rebase) {  // (re)start of playback: the clock restarts at the next frame to present
        base_wall = Clock::now();
        base_ts = have_pending ? ts : LONGLONG(cur_pos * 1e7);
      }
      if (!have_pending) {
        if (!read_frame(ts, eos)) {
          raise(s, 19);
          std::lock_guard<std::mutex> g(s.m);
          s.playing = false;
          continue;
        }
        if (eos) {
          bool loop;
          {
            std::lock_guard<std::mutex> g(s.m);
            loop = s.loop;
            if (!loop) {
              s.playing = false;
              s.current = s.duration;
            }
          }
          if (loop) {
            PROPVARIANT pos;
            InitPropVariantFromInt64(0, &pos);
            reader->SetCurrentPosition(GUID_NULL, pos);
            PropVariantClear(&pos);
            base_wall = Clock::now();
            base_ts = 0;
          } else {
            raise(s, 13);
          }
          continue;
        }
        have_pending = true;
      }
      // Wait for the frame's presentation time (interruptible by pause / seek / quit).
      const auto due = base_wall + std::chrono::microseconds(LONGLONG(double(ts - base_ts) / 10.0 / rate));
      {
        std::unique_lock<std::mutex> w2(s.m);
        if (s.cv.wait_until(w2, due, [&] { return s.quit || !s.playing || s.seek_to >= 0.0 || s.restart; })) continue;
        s.current = double(ts) / 1e7;
      }
      composite::canvas_publish(canvas, int32_t(w), int32_t(h), std::move(rgba));
      have_pending = false;
      if (s.current - last_update >= 0.25 || s.current < last_update) {
        last_update = s.current;
        raise(s, 15);
      }
    }
  }
  composite::canvas_destroy(canvas);
  release(type);
  release(reader);
  release(attr);
  MFShutdown();
  CoUninitialize();
}

}  // namespace

int32_t add_src(const std::string& console_uri, EventCallback cb, void* user, uint32_t* src_id) {
  std::string host;
  if (Firmware* fw = host::ctx().firmware) host = fw->host_path(console_uri);
  std::error_code ec;
  if (host.empty() || !std::filesystem::exists(host, ec)) {
    log::warn("psmutil", "HOST VALUE sceMediaFwAddSrc({}): not in the dump -> 0x80020002", console_uri);
    return kErrNoEnt;
  }
  host::note_unimplemented("HOST VALUE media framework: becore is not on the host; video decoded with Windows Media Foundation, no audio");
  auto s = std::make_shared<Source>();
  s->path = std::filesystem::path(host).wstring();
  s->uri = console_uri;
  s->cb = cb;
  s->user = user;
  uint32_t id;
  {
    std::lock_guard<std::mutex> l(g_lock);
    id = g_next++;
    g_sources[id] = s;
  }
  s->thread = std::thread(source_thread, s);
  *src_id = id;
  return 0;
}

int32_t remove_src(uint32_t src) {
  std::shared_ptr<Source> s;
  {
    std::lock_guard<std::mutex> l(g_lock);
    auto it = g_sources.find(src);
    if (it == g_sources.end()) return kErrInvalid;
    s = it->second;
    g_sources.erase(it);
  }
  {
    std::lock_guard<std::mutex> l(s->m);
    s->quit = true;
  }
  s->cv.notify_all();
  if (s->thread.joinable()) {
    if (s->thread.get_id() == std::this_thread::get_id()) s->thread.detach();
    else s->thread.join();
  }
  return 0;
}

namespace {
template <class F>
int32_t with(uint32_t src, F f, int32_t ev = 0) {
  auto s = find(src);
  if (!s) return kErrInvalid;
  {
    std::lock_guard<std::mutex> l(s->m);
    f(*s);
  }
  s->cv.notify_all();
  if (ev) raise(*s, ev);
  return 0;
}
}  // namespace

int32_t load(uint32_t src) {
  return with(src, [](Source& s) { s.load_requested = true; });
}
int32_t play(uint32_t src) {
  return with(src, [](Source& s) { s.playing = true; s.rebase = true; }, 16);
}
int32_t pause(uint32_t src) {
  return with(src, [](Source& s) { s.playing = false; }, 17);
}
int32_t stop(uint32_t src) {
  return with(src, [](Source& s) { s.playing = false; s.seek_to = 0.0; }, 18);
}
int32_t seek(uint32_t src, double seconds) {
  return with(src, [&](Source& s) { s.seek_to = seconds < 0.0 ? 0.0 : seconds; });
}
int32_t set_playback_rate(uint32_t src, double rate) {
  return with(src, [&](Source& s) { s.rate = rate; });
}
int32_t set_loop(uint32_t src, bool loop) {
  return with(src, [&](Source& s) { s.loop = loop; });
}
int32_t set_muted(uint32_t src, bool) { return find(src) ? 0 : kErrInvalid; }
int32_t set_volume(uint32_t src, double) { return find(src) ? 0 : kErrInvalid; }
int32_t suppress_bgm(uint32_t src, bool) { return find(src) ? 0 : kErrInvalid; }
int32_t set_display_size(uint32_t src, int32_t, int32_t) { return find(src) ? 0 : kErrInvalid; }
int32_t change_active_status(uint32_t src, bool) { return find(src) ? 0 : kErrInvalid; }
int32_t cancel_playback_range(uint32_t src) { return find(src) ? 0 : kErrInvalid; }

int32_t get_resolution(uint32_t src, int32_t* w, int32_t* h) {
  return with(src, [&](Source& s) { *w = s.w; *h = s.h; });
}
int32_t get_canvas_handle(uint32_t src, uint32_t* canvas, int32_t* dynamic_range) {
  return with(src, [&](Source& s) { *canvas = s.canvas; *dynamic_range = 0; });  // SDR content (BT.709)
}
int32_t get_ready_state(uint32_t src, uint32_t* state) {
  return with(src, [&](Source& s) { *state = s.ready_state; });
}
int32_t get_duration(uint32_t src, double* seconds) {
  return with(src, [&](Source& s) { *seconds = s.duration; });
}
int32_t get_current_time(uint32_t src, double* seconds) {
  return with(src, [&](Source& s) { *seconds = s.current; });
}

}  // namespace ssn::psmutil::mediafw
