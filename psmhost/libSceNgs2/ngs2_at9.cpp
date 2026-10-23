// HOST VALUE stand-in for libSceNgs2.native's software ATRAC9 frame decoder — see ngs2_at9.h.
#include "libSceNgs2/ngs2_at9.h"

#include <cstring>
#include <mutex>
#include <vector>

#include "core/log.h"
#include "host/boundary.h"
#include "libatrac9.h"
extern "C" {
#include "structures.h"
}

namespace ssn::ngs2 {
namespace {

constexpr float kScale = 1.0f / 32768.0f;  // 0x38000000 (vmulss at 0x3A8D0)

void note_once() {
  static std::once_flag once;
  std::call_once(once, [] {
    host::note_unimplemented(
        "HOST VALUE libSceNgs2 sampler ATRAC9 (Sony's in-library decoder 0x36070/0x364C0/0x3A8D0): frames decoded by "
        "LibAtrac9 efca2e3 (Channel.Pcm -> float, x 2^-15, clamped to [-1, 1])");
  });
}

}  // namespace

At9FrameDecoder::~At9FrameDecoder() {
  if (handle_) Atrac9ReleaseHandle(handle_);
}

int32_t At9FrameDecoder::init(const uint8_t config[4], uint32_t num_channels) {
  note_once();
  if (handle_) Atrac9ReleaseHandle(handle_);
  handle_ = Atrac9GetHandle();
  if (!handle_) return -1;
  uint8_t cfg[4];
  std::memcpy(cfg, config, 4);
  if (Atrac9InitDecoder(handle_, cfg) != 0) {
    Atrac9ReleaseHandle(handle_);
    handle_ = nullptr;
    return -1;
  }
  Atrac9CodecInfo info{};
  Atrac9GetCodecInfo(handle_, &info);
  channels_ = static_cast<uint32_t>(info.channels);
  frame_samples_ = static_cast<uint32_t>(info.frameSamples);
  if (channels_ != num_channels) log::warn("ngs2", "ATRAC9 config has {} channels, the voice {}", channels_, num_channels);
  return 0;
}

int32_t At9FrameDecoder::decode_frame(const uint8_t* in, uint32_t avail, bool last_of_superframe, float* const* out,
                                      uint32_t skip, uint32_t count, uint32_t* used) {
  *used = 0;
  if (!handle_ || avail == 0) return -1;
  // LibAtrac9 reads the frame without a length bound: decode from a zero-padded copy of the bytes
  // the firmware lets the frame see (to the end of the superframe).
  thread_local std::vector<uint8_t> pad;
  pad.assign(size_t(avail) + 2048, 0);
  std::memcpy(pad.data(), in, avail);
  thread_local std::vector<int16_t> pcm16;
  pcm16.resize(size_t(frame_samples_) * channels_ + 16);
  int bytes = 0;
  if (Atrac9Decode(handle_, pad.data(), pcm16.data(), &bytes) != 0 || bytes <= 0 ||
      static_cast<uint32_t>(bytes) > avail)
    return -1;
  auto* h = static_cast<Atrac9Handle*>(handle_);
  for (uint32_t c = 0; c < channels_; ++c) {
    const double* src = h->Frame.Channels[c]->Pcm;
    for (uint32_t i = 0; i < count && skip + i < frame_samples_; ++i) {
      float v = static_cast<float>(src[skip + i]) * kScale;
      if (v > 1.0f) v = 1.0f;     // 0x3A8D0 clamp to 0x3F800000
      if (v < -1.0f) v = -1.0f;   //                and 0xBF800000
      out[c][i] = v;
    }
  }
  // 0x367B0 / 0x36880: the last frame of a superframe takes the superframe's remaining bytes.
  *used = last_of_superframe ? avail : static_cast<uint32_t>(bytes);
  return 0;
}

}  // namespace ssn::ngs2
