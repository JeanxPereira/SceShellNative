// The output driver of libScePsm's audio ports (FW 4.03) on the PC.
//
// On the console the PADR output object (sub_18F160, "ScePssPAudioDriverOut") opens a
// sceAudioOut port (sub_18F3E0 picks the entry of the system port table qword_526F20 that
// matches the type, channels and sample size; sub_18F870 calls sceAudioOutOpen, import
// qLpSK75lXI4) and runs the driver thread sub_1900D0: a double buffer of two grains; for each
// grain it calls the port's callback (main port sub_170450, BGM port sub_170790) — or, for a
// BGM port with a direct player, copies the player's stream straight into the grain
// (sub_172320) after applying a pending port volume with sceAudioOutSetVolume (import
// b_uAV89IlxE, volume * 32768 on all channels) — and then sceAudioOutOutput (import
// QOQtbeDqsT4), which blocks until the port has taken the previous grain: that is what
// paces the mixer. The driver object's vtable (off_49C068) starts (+152 sub_18F820: open,
// thread unless asked to wait), resumes (+176 sub_18FBD0: thread), stops (+168 sub_18FB50:
// thread ends when no voice of the port is active) and reports running (+184 sub_18FDF0,
// state 2..4) and the port state (+192 sub_18FD00 over import Y3lXfCFEWFY).
//
// Two ports exist: the main port (48 kHz, 2 ch, float) and the BGM port (sceAudioOut type
// BGM; opened 48 kHz, 8 ch, float by sub_16ED10, reopened 48 kHz, 8 ch, 16-bit for a direct
// ATRAC9 BGM player by sub_16F8B0 / sub_18EDC0).
//
// HOST VALUE: the PC has no PS5 audio-out port. Each port's grains go to the default render
// device through its own WASAPI shared-mode stream (the port's format requested, Windows
// converting to the device mix format), and a write waits until the device queue holds less
// than two device periods — the host's stand-in for sceAudioOutOutput's blocking. The
// 8-channel port's channel order (sceAudioOut 8CH: L, R, C, LFE, Ls, Rs, Lb, Rb) is
// rearranged into the WAVEFORMATEXTENSIBLE 7.1 surround order (FL, FR, FC, LFE, BL, BR, SL,
// SR); sceAudioOutSetVolume is applied to the samples (linear, 32768 = 0 dB). The port state
// reads 0 (no restriction: PortStatus OK). Thread start/stop is a run flag on one host
// thread per port. With no device (or when it fails) the grains are dropped at the port's
// rate, paced by the host clock (logged once), so headless runs need no audio hardware.
//
// Diagnostics: SSN_AUDIO_DUMP=<file.wav> writes every grain handed to the main port (48 kHz,
// 2 ch, 32-bit float WAV) and, to <file>.<port name>.wav, every grain handed to another port in
// the port's own format (before the port volume): <file>.bgm.wav for the BGM port, <file>.ngs2.wav
// for the eboot's BGLayer NGS2 port (8 ch float; a port closed and opened again keeps appending to
// the same file); SSN_AUDIO_NULL=1 takes the no-device path even when a device exists.
#pragma once
#include <atomic>
#include <condition_variable>
#include <cstdint>
#include <functional>
#include <mutex>
#include <string>
#include <thread>

namespace ssn::psm::audio {

struct PortFormat {
  uint32_t rate = 48000, channels = 2, bits = 32;
  uint32_t grain = 256;  // frames (driver +68)
  bool operator==(const PortFormat&) const = default;
};

class PssAudioOut {
 public:
  // (buffer, bytes requested, *frames filled; 0 = nothing to output this time).
  using Callback = std::function<void(uint8_t*, uint32_t, uint32_t*)>;

  // sub_18F870 (sceAudioOutOpen) with the thread started (run = true) or waiting for resume.
  void start(const PortFormat& fmt, Callback cb, std::string name, bool run = true);
  // The port reopened with another format (sub_16FF60 -> sub_18FAC0 / sub_18F3E0).
  void reformat(const PortFormat& fmt);
  void resume();  // +176 sub_18FBD0
  void stop();    // +168 sub_18FB50 (the caller has checked that no voice is active)
  // The port's thread ends and the port closes (sceAudioOutOutput(port, NULL) + sceAudioOutClose
  // of a port whose owner joins its thread, e.g. the eboot's BGLayer audio thread). Returns when
  // the thread has finished; the object may then be destroyed.
  void close();
  bool running() const {  // +184 sub_18FDF0
    const int32_t s = state_.load();
    return s >= 2 && s <= 4;
  }
  int32_t state() const { return state_.load(); }  // driver +80 (3: thread running)
  // sceAudioOutSetVolume(port, all channels, v) — 32768 = 0 dB.
  void set_volume(int32_t v);
  // +192 sub_18FD00: the sceAudioOut port state word (host: 0) -> PortStatus 1 (OK).
  uint32_t port_status() const { return 1; }
  uint64_t grains() const { return grains_.load(); }
  PortFormat format() const;

 private:
  void thread_main();
  Callback cb_;
  std::string name_;
  mutable std::mutex fmt_lock_;
  PortFormat fmt_{};
  std::atomic<uint32_t> fmt_gen_{0};
  std::atomic<int32_t> state_{0};  // driver +80: 2 open, 3 thread running
  std::atomic<bool> run_{false};
  std::atomic<bool> exit_{false}, exited_{false};
  std::atomic<int32_t> volume_{32768};
  std::mutex run_lock_;
  std::condition_variable run_cv_;
  std::atomic<uint64_t> grains_{0};
  std::thread thread_;
};

}  // namespace ssn::psm::audio
