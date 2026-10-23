// The part of Sony's libSceAudiodec that libScePsm's PSS ATRAC9 decoder (type 3, see
// atrac9_decoder.h) uses, as it uses it (FW 4.03 libScePsm imports):
//
//   O3f1sLMWRvs (ctrl, 1)  create an ATRAC9 instance  (sub_191D80; < 0 = failure)
//   KHXHMDLkILw (h, ctrl)  decode one access unit      (sub_192200)
//   6Vf9WTLDoss (h)        clear the decoding context  (sub_191D30 reset, sub_192190 skip)
//   Tp_ZEy69mLk (h)        delete the instance         (sub_191D80, sub_1920C0)
//
// The 136-byte control block libScePsm keeps at decoder +1152 holds: +0 the instance
// handle, +8 -> parameters (+64: size 12, +68 = 1 (16-bit PCM words), +72 the 4 config bytes
// of the RIFF fmt chunk), +16 -> stream info (+76: size 36, channels, rate, superframe bytes,
// frames per superframe, ...; +108 = the decode error word), +24 -> access unit (+40: size
// 24, +48 address, +56 bytes; on return +56 = bytes used), +32 -> PCM item (+112: size 24,
// +120 address, +128 bytes; on return +128 = bytes written).
//
// HOST VALUE: on the console the library hands the work to the ACP/AJM decoder hardware.
// The PC has neither; LibAtrac9 (third_party/atrac9.cmake: github.com/Thealexbarney/
// LibAtrac9 at efca2e3, MIT) decodes in its place. One decode call is one ATRAC9 frame
// (Atrac9Decode), as the firmware sizes it: the PCM item holds 32 * 16-bit * channels bytes
// = one 256-sample frame at 48 kHz, and the access unit is at most 2048 bytes. LibAtrac9 does
// not bound its reads: an access unit shorter than 2048 bytes is first copied into a zero-
// padded buffer, and a frame that would need more bytes than the unit holds is reported as
// a decode error (error word +108 set), as is any LibAtrac9 error. The frames of a
// superframe are byte-aligned one after the other and the superframe is padded to its fixed
// size (fmt block align, 800 bytes for the BGM files); LibAtrac9 reports the bytes of the
// frame only, so the last frame of a superframe reports the padding as used too — the unit
// boundaries the firmware's next call then starts from are the superframe's.
#pragma once
#include <cstdint>

namespace ssn::psm::audio {

class AudiodecAtrac9 {
 public:
  AudiodecAtrac9() = default;
  ~AudiodecAtrac9();
  AudiodecAtrac9(const AudiodecAtrac9&) = delete;
  AudiodecAtrac9& operator=(const AudiodecAtrac9&) = delete;

  // O3f1sLMWRvs: 0 (the handle) or a negative error.
  int32_t create(const uint8_t config[4]);
  // KHXHMDLkILw: decodes one frame of `au` into `pcm`. Returns 0 (the firmware then reads
  // *au_used, *pcm_bytes and *error) or a negative result.
  int32_t decode(const uint8_t* au, uint32_t au_bytes, uint8_t* pcm, uint32_t pcm_cap, uint32_t* au_used,
                 uint32_t* pcm_bytes, uint32_t* error);
  int32_t clear_context();  // 6Vf9WTLDoss
  void destroy();           // Tp_ZEy69mLk
  bool alive() const { return handle_ != nullptr; }
  // The stream info the instance fills (+16 block): superframe bytes (+92 = ctrl dword 23).
  uint32_t superframe_bytes() const { return superframe_bytes_; }

 private:
  void* handle_ = nullptr;
  uint8_t config_[4] = {};
  uint32_t channels_ = 0, frame_samples_ = 0, superframe_bytes_ = 0;
  uint32_t frames_per_superframe_ = 1, frame_index_ = 0, superframe_used_ = 0;
};

}  // namespace ssn::psm::audio
