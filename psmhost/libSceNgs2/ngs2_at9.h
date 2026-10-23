// The ATRAC9 frame decoder of libSceNgs2.native's sampler (decoder class 0, waveform type 64).
//
// On the console it is Sony's software ATRAC9 decoder compiled into libSceNgs2.native (config parse
// 0x35EE0, init 0x36070, frame decode 0x364C0 -> 0x380D0, PCM output 0x3A8D0); the Player asks it
// for output type 288: float32, one buffer per channel, sample = clamp(x * 2^-15, -1, 1) where x is
// the decoder's internal sample in 16-bit scale (no rounding to int16). One call decodes one frame
// (256 samples at 48 kHz) and writes frame samples [skip, skip + count). The bytes used are the
// frame's own; the last frame of a superframe also takes the superframe's padding (0x367B0 /
// 0x36880).
//
// HOST VALUE: that decoder is not transcribed; LibAtrac9 (third_party/atrac9.cmake, efca2e3, MIT —
// the decoder the host already uses in place of libSceAudiodec) decodes in its place. x is
// LibAtrac9's per-channel IMDCT output (Channel.Pcm, double, 16-bit scale) rounded to float; the
// rest of the output contract above is the firmware's. Logged once.
#pragma once
#include <cstdint>

namespace ssn::ngs2 {

class At9FrameDecoder {
 public:
  At9FrameDecoder() = default;
  ~At9FrameDecoder();
  At9FrameDecoder(const At9FrameDecoder&) = delete;
  At9FrameDecoder& operator=(const At9FrameDecoder&) = delete;

  // 0x36070: (re)initialise from the 4 config bytes (big-endian of configData). < 0 on failure.
  int32_t init(const uint8_t config[4], uint32_t num_channels);
  // 0x364C0: decode one frame at `in` (`avail` = bytes to the end of the superframe; the last frame
  // of the superframe takes them all). Writes frame samples [skip, skip + count) of each channel to
  // out[c][0..count). < 0 on a decode error.
  int32_t decode_frame(const uint8_t* in, uint32_t avail, bool last_of_superframe, float* const* out, uint32_t skip,
                       uint32_t count, uint32_t* used);

 private:
  void* handle_ = nullptr;
  uint32_t channels_ = 0;
  uint32_t frame_samples_ = 0;
};

}  // namespace ssn::ngs2
