// Host output of libScePsm's audio ports — see pss_audio_out.h.
#include "libScePsm/audio/pss_audio_out.h"

#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <audioclient.h>
#include <ks.h>
#include <ksmedia.h>
#include <mmdeviceapi.h>
#include <mmreg.h>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <map>
#include <memory>
#include <string>
#include <vector>

#include "core/log.h"
#include "host/boundary.h"

#pragma comment(lib, "ole32.lib")

namespace ssn::psm::audio {
namespace {

uint32_t frame_bytes(const PortFormat& f) { return f.channels * (f.bits >> 3); }

// SSN_AUDIO_DUMP: a WAV of every grain in the port's format (float for 32-bit, PCM for
// 16-bit); the header sizes are rewritten after each grain so the file stays valid when the
// process ends without notice. Other ports write <path>.<name>.wav (one dump per name for the
// whole process, see dump_for).
class WavDump {
 public:
  explicit WavDump(const std::string& suffix) {
    const char* p = std::getenv("SSN_AUDIO_DUMP");
    if (!p || !*p) return;
    path_ = p;
    if (!suffix.empty()) {
      const auto dot = path_.find_last_of('.');
      path_ = (dot == std::string::npos ? path_ : path_.substr(0, dot)) + suffix + ".wav";
    }
  }
  void append(const PortFormat& f, const uint8_t* data, uint32_t bytes) {
    if (path_.empty()) return;
    if (!f_) {
      f_ = std::fopen(path_.c_str(), "wb");
      if (!f_) {
        log::error("psm.audio", "SSN_AUDIO_DUMP: cannot create {}", path_);
        path_.clear();
        return;
      }
      fmt_ = f;
      write_header();
      log::info("psm.audio", "dumping an audio port ({} Hz, {} ch, {}-bit) to {}", f.rate, f.channels, f.bits, path_);
    }
    if (!(f == fmt_)) return;  // a port reopened with another format is not dumped further
    std::fseek(f_, 0, SEEK_END);
    std::fwrite(data, 1, bytes, f_);
    data_ += bytes;
    write_header();
    std::fflush(f_);
  }

 private:
  void write_header() {
    std::fseek(f_, 0, SEEK_SET);
    auto u32 = [&](uint32_t v) { std::fwrite(&v, 4, 1, f_); };
    auto u16 = [&](uint16_t v) { std::fwrite(&v, 2, 1, f_); };
    std::fwrite("RIFF", 1, 4, f_);
    u32(36 + data_);
    std::fwrite("WAVEfmt ", 1, 8, f_);
    u32(16);
    u16(fmt_.bits == 32 ? 3 : 1);  // WAVE_FORMAT_IEEE_FLOAT / WAVE_FORMAT_PCM
    u16(static_cast<uint16_t>(fmt_.channels));
    u32(fmt_.rate);
    u32(fmt_.rate * frame_bytes(fmt_));
    u16(static_cast<uint16_t>(frame_bytes(fmt_)));
    u16(static_cast<uint16_t>(fmt_.bits));
    std::fwrite("data", 1, 4, f_);
    u32(data_);
  }
  std::string path_;
  FILE* f_ = nullptr;
  PortFormat fmt_{};
  uint32_t data_ = 0;
};

// One dump per port name for the whole process: a port that is closed and opened again (the
// eboot's BGLayer port, per BGM initialisation) appends to the same file.
WavDump& dump_for(const std::string& name) {
  static std::mutex lock;
  static std::map<std::string, std::unique_ptr<WavDump>> dumps;
  std::lock_guard<std::mutex> l(lock);
  auto& d = dumps[name];
  if (!d) d = std::make_unique<WavDump>(name == "main" ? "" : "." + name);
  return *d;
}

// The WASAPI render stream of one port; open() false when there is no usable device.
class Wasapi {
 public:
  ~Wasapi() { close(); }
  bool open(const PortFormat& pf) {
    close();
    fmt_ = pf;
    if (FAILED(CoInitializeEx(nullptr, COINIT_MULTITHREADED))) return false;
    IMMDeviceEnumerator* en = nullptr;
    if (FAILED(CoCreateInstance(__uuidof(MMDeviceEnumerator), nullptr, CLSCTX_ALL, __uuidof(IMMDeviceEnumerator),
                                reinterpret_cast<void**>(&en))))
      return false;
    const HRESULT hd = en->GetDefaultAudioEndpoint(eRender, eConsole, &dev_);
    en->Release();
    if (FAILED(hd) || !dev_) return false;
    if (FAILED(dev_->Activate(__uuidof(IAudioClient), CLSCTX_ALL, nullptr, reinterpret_cast<void**>(&client_))))
      return false;
    WAVEFORMATEXTENSIBLE wf{};
    wf.Format.wFormatTag = WAVE_FORMAT_EXTENSIBLE;
    wf.Format.nChannels = static_cast<WORD>(pf.channels);
    wf.Format.nSamplesPerSec = pf.rate;
    wf.Format.wBitsPerSample = static_cast<WORD>(pf.bits);
    wf.Format.nBlockAlign = static_cast<WORD>(frame_bytes(pf));
    wf.Format.nAvgBytesPerSec = wf.Format.nSamplesPerSec * wf.Format.nBlockAlign;
    wf.Format.cbSize = sizeof(WAVEFORMATEXTENSIBLE) - sizeof(WAVEFORMATEX);
    wf.Samples.wValidBitsPerSample = static_cast<WORD>(pf.bits);
    wf.dwChannelMask = pf.channels == 8 ? KSAUDIO_SPEAKER_7POINT1_SURROUND
                                        : (pf.channels == 2 ? KSAUDIO_SPEAKER_STEREO : KSAUDIO_SPEAKER_MONO);
    wf.SubFormat = pf.bits == 32 ? KSDATAFORMAT_SUBTYPE_IEEE_FLOAT : KSDATAFORMAT_SUBTYPE_PCM;
    REFERENCE_TIME def = 0, min = 0;
    client_->GetDevicePeriod(&def, &min);
    // HOST VALUE: 100 ms device buffer; the writer keeps at most two device periods queued.
    const DWORD flags = AUDCLNT_STREAMFLAGS_AUTOCONVERTPCM | AUDCLNT_STREAMFLAGS_SRC_DEFAULT_QUALITY;
    if (FAILED(client_->Initialize(AUDCLNT_SHAREMODE_SHARED, flags, 1000000, 0,
                                   reinterpret_cast<WAVEFORMATEX*>(&wf), nullptr)))
      return false;
    if (FAILED(client_->GetBufferSize(&buffer_frames_))) return false;
    if (FAILED(client_->GetService(__uuidof(IAudioRenderClient), reinterpret_cast<void**>(&render_)))) return false;
    period_frames_ = static_cast<UINT32>((def > 0 ? def : 100000) * pf.rate / 10000000);
    if (period_frames_ < pf.grain) period_frames_ = pf.grain;
    if (FAILED(client_->Start())) return false;
    return true;
  }
  void close() {
    if (client_) client_->Stop();
    if (render_) render_->Release();
    if (client_) client_->Release();
    if (dev_) dev_->Release();
    render_ = nullptr;
    client_ = nullptr;
    dev_ = nullptr;
  }
  // Blocks until the grain fits under the queue limit, then writes it. false on failure.
  bool write(const uint8_t* grain) {
    const UINT32 limit = std::min<UINT32>(buffer_frames_, 2 * period_frames_ + fmt_.grain);
    for (;;) {
      UINT32 pad = 0;
      if (FAILED(client_->GetCurrentPadding(&pad))) return false;
      if (pad + fmt_.grain <= limit) break;
      Sleep(1);
    }
    BYTE* p = nullptr;
    if (FAILED(render_->GetBuffer(fmt_.grain, &p))) return false;
    std::memcpy(p, grain, size_t(fmt_.grain) * frame_bytes(fmt_));
    return SUCCEEDED(render_->ReleaseBuffer(fmt_.grain, 0));
  }

 private:
  PortFormat fmt_{};
  IMMDevice* dev_ = nullptr;
  IAudioClient* client_ = nullptr;
  IAudioRenderClient* render_ = nullptr;
  UINT32 buffer_frames_ = 0;
  UINT32 period_frames_ = 0;
};

// What the host's sceAudioOut does with a grain before the device: the port volume and, for
// 8 channels, the 7.1 order of the Windows stream.
void to_device(const PortFormat& f, const uint8_t* in, uint8_t* out, int32_t vol) {
  const uint32_t n = f.grain * f.channels;
  const float g = static_cast<float>(vol) / 32768.0f;
  static constexpr uint32_t kMap8[8] = {0, 1, 2, 3, 6, 7, 4, 5};  // device slot -> port slot
  for (uint32_t i = 0; i < n; ++i) {
    const uint32_t c = i % f.channels;
    const uint32_t src = f.channels == 8 ? (i - c) + kMap8[c] : i;
    if (f.bits == 16) {
      int16_t s;
      std::memcpy(&s, in + 2 * src, 2);
      float x = std::nearbyint(static_cast<float>(s) * g);
      x = std::clamp(x, -32768.0f, 32767.0f);
      const int16_t o = static_cast<int16_t>(x);
      std::memcpy(out + 2 * i, &o, 2);
    } else {
      float s;
      std::memcpy(&s, in + 4 * src, 4);
      s = s * g;
      std::memcpy(out + 4 * i, &s, 4);
    }
  }
}

}  // namespace

PortFormat PssAudioOut::format() const {
  std::lock_guard<std::mutex> l(fmt_lock_);
  return fmt_;
}

void PssAudioOut::start(const PortFormat& fmt, Callback cb, std::string name, bool run) {
  {
    std::lock_guard<std::mutex> l(fmt_lock_);
    fmt_ = fmt;
  }
  cb_ = std::move(cb);
  name_ = std::move(name);
  state_ = run ? 3 : 2;
  run_ = run;
  thread_ = std::thread([this] { thread_main(); });
  thread_.detach();
}

void PssAudioOut::reformat(const PortFormat& fmt) {
  std::lock_guard<std::mutex> l(fmt_lock_);
  if (fmt_ == fmt) return;
  fmt_ = fmt;
  fmt_gen_.fetch_add(1);
  log::info("psm.audio", "{} audio port reopened: {} Hz, {} ch, {}-bit", name_, fmt.rate, fmt.channels, fmt.bits);
}

void PssAudioOut::resume() {
  {
    std::lock_guard<std::mutex> l(run_lock_);
    run_ = true;
    state_ = 3;
  }
  run_cv_.notify_all();
}

void PssAudioOut::stop() {
  std::lock_guard<std::mutex> l(run_lock_);
  run_ = false;
  state_ = 2;
}

void PssAudioOut::set_volume(int32_t v) { volume_ = v; }

void PssAudioOut::close() {
  {
    std::lock_guard<std::mutex> l(run_lock_);
    exit_ = true;
  }
  run_cv_.notify_all();
  std::unique_lock<std::mutex> l(run_lock_);
  run_cv_.wait(l, [this] { return exited_.load(); });
  state_ = 0;
}

// sub_1900D0: for every grain the callback fills one half of the double buffer and the half
// is handed to the port (sceAudioOutOutput), which paces the loop; an empty callback result
// skips the output.
void PssAudioOut::thread_main() {
  WavDump& dump = dump_for(name_);
  Wasapi wasapi;
  const char* null_out = std::getenv("SSN_AUDIO_NULL");
  const bool allow_device = !(null_out && *null_out && *null_out != '0');
  using clock = std::chrono::steady_clock;
  std::vector<uint8_t> buf, dev;
  PortFormat f{};
  uint32_t gen = ~0u;
  bool device = false;
  auto next = clock::now();
  uint32_t half = 0;
  for (;;) {
    {
      std::unique_lock<std::mutex> l(run_lock_);
      if (!run_.load() && !exit_.load()) {
        run_cv_.wait(l, [this] { return run_.load() || exit_.load(); });
        next = clock::now();
      }
      if (exit_.load()) break;
    }
    if (gen != fmt_gen_.load()) {  // (re)open: sceAudioOutOpen with the port's format
      gen = fmt_gen_.load();
      f = format();
      buf.assign(2 * size_t(f.grain) * frame_bytes(f), 0);
      dev.assign(size_t(f.grain) * frame_bytes(f), 0);
      device = allow_device && wasapi.open(f);
      if (!device)
        host::note_unimplemented("HOST VALUE libScePsm audio out: no usable host audio device; grains dropped at 48 kHz");
      else
        log::info("psm.audio", "{} audio port -> default render device (WASAPI shared, {} Hz, {} ch, {}-bit)", name_,
                  f.rate, f.channels, f.bits);
      next = clock::now();
    }
    const uint32_t bytes = f.grain * frame_bytes(f);
    uint8_t* p = buf.data() + half * bytes;
    uint32_t filled = 0;
    cb_(p, bytes, &filled);
    if (filled == 0) continue;
    dump.append(f, p, bytes);
    to_device(f, p, dev.data(), volume_.load());
    if (device && !wasapi.write(dev.data())) {
      device = false;
      next = clock::now();
      host::note_unimplemented("HOST VALUE libScePsm audio out: host audio device failed; grains dropped from now on");
    }
    if (!device) {
      next += std::chrono::nanoseconds(1000000000ll * f.grain / f.rate);
      std::this_thread::sleep_until(next);
    }
    grains_.fetch_add(1);
    half ^= 1;
  }
  wasapi.close();
  {
    std::lock_guard<std::mutex> l(run_lock_);
    exited_ = true;
  }
  run_cv_.notify_all();
}

}  // namespace ssn::psm::audio
