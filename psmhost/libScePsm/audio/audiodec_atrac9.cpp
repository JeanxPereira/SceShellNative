// HOST stand-in for libSceAudiodec's ATRAC9 decoder (LibAtrac9) — see audiodec_atrac9.h.
#include "libScePsm/audio/audiodec_atrac9.h"

#include <cstring>
#include <mutex>
#include <vector>

#include "core/log.h"
#include "libatrac9.h"

namespace ssn::psm::audio {
namespace {

constexpr int32_t kErrFail = -1;  // any negative result: libScePsm maps it to 0x80B3F001
constexpr uint32_t kAuMax = 2048;  // the largest access unit sub_192200 hands over

void note_once() {
  static std::once_flag once;
  std::call_once(once, [] {
    log::info("psm.audio",
              "HOST VALUE libSceAudiodec ATRAC9: decoded by LibAtrac9 (github.com/Thealexbarney/LibAtrac9 @ efca2e3, "
              "MIT) in place of the console's ACP/AJM decoder");
  });
}

}  // namespace

AudiodecAtrac9::~AudiodecAtrac9() { destroy(); }

int32_t AudiodecAtrac9::create(const uint8_t config[4]) {
  note_once();
  destroy();
  std::memcpy(config_, config, 4);
  void* h = Atrac9GetHandle();
  if (!h) return kErrFail;
  if (Atrac9InitDecoder(h, config_) != 0) {
    Atrac9ReleaseHandle(h);
    return kErrFail;
  }
  Atrac9CodecInfo info{};
  Atrac9GetCodecInfo(h, &info);
  channels_ = static_cast<uint32_t>(info.channels);
  frames_per_superframe_ = static_cast<uint32_t>(info.framesInSuperframe);
  frame_index_ = 0;
  superframe_used_ = 0;
  frame_samples_ = static_cast<uint32_t>(info.frameSamples);
  superframe_bytes_ = static_cast<uint32_t>(info.superframeSize);
  handle_ = h;
  return 0;
}

// A fresh LibAtrac9 instance with the same configuration: the frame index within the
// superframe and the overlap history start over, as after the console's context clear.
int32_t AudiodecAtrac9::clear_context() {
  if (!handle_) return kErrFail;
  uint8_t cfg[4];
  std::memcpy(cfg, config_, 4);
  return create(cfg);
}

void AudiodecAtrac9::destroy() {
  if (handle_) Atrac9ReleaseHandle(handle_);
  handle_ = nullptr;
}

int32_t AudiodecAtrac9::decode(const uint8_t* au, uint32_t au_bytes, uint8_t* pcm, uint32_t pcm_cap, uint32_t* au_used,
                               uint32_t* pcm_bytes, uint32_t* error) {
  *au_used = 0;
  *pcm_bytes = 0;
  *error = 0;
  if (!handle_) return kErrFail;
  const uint32_t frame_bytes = frame_samples_ * channels_ * 2;
  if (pcm_cap < frame_bytes || au_bytes == 0) {
    *error = 1;
    return 0;
  }
  // LibAtrac9 reads the frame without a length: a short unit goes through a padded copy.
  thread_local std::vector<uint8_t> pad;
  const uint8_t* src = au;
  if (au_bytes < kAuMax) {
    pad.assign(kAuMax + 64, 0);
    std::memcpy(pad.data(), au, au_bytes);
    src = pad.data();
  }
  thread_local std::vector<int16_t> out;
  out.resize(size_t(frame_samples_) * channels_);
  int used = 0;
  if (Atrac9Decode(handle_, src, out.data(), &used) != 0 || used <= 0 || static_cast<uint32_t>(used) > au_bytes) {
    *error = 1;
    return 0;
  }
  std::memcpy(pcm, out.data(), frame_bytes);
  // The frames of a superframe follow each other byte-aligned; the superframe is padded to
  // its fixed size, which the last frame's unit takes along.
  uint32_t take = static_cast<uint32_t>(used);
  superframe_used_ += take;
  if (++frame_index_ >= frames_per_superframe_) {
    if (superframe_used_ < superframe_bytes_) take += superframe_bytes_ - superframe_used_;
    frame_index_ = 0;
    superframe_used_ = 0;
  }
  if (take > au_bytes) take = au_bytes;
  *au_used = take;
  *pcm_bytes = frame_bytes;
  return 0;
}

}  // namespace ssn::psm::audio
