// Host media framework: the sceMediaFw* boundary libScePsmUtil's VideoPlayerMcs calls
// (libSceMediaFrameworkInterface, an IPC client of the BE core process becore.elf).
//
// DECLARED HOST VALUE (the whole module): becore — demuxer, decoder, A/V clock and the
// canvas it renders into — is a separate console process that is not on the host. Here a
// source is a file of the firmware dump decoded with Windows Media Foundation (Source
// Reader, RGB32 output), its frames are published to a host compositor canvas
// (libSceComposite/canvas.h) at their presentation times, and the media-framework events
// libScePsmUtil's callback (0x16500 -> 0x190c0) understands are raised in the order listed
// in media_fw.cpp. Audio is not reproduced.
#pragma once
#include <cstdint>
#include <string>

namespace ssn::psmutil::mediafw {

using EventCallback = void (*)(int32_t event, void* user);

// sceMediaFwAddSrc: `src_id` receives a non-zero id; the callback runs on the source's thread.
int32_t add_src(const std::string& console_uri, EventCallback cb, void* user, uint32_t* src_id);
int32_t remove_src(uint32_t src);  // sceMediaFwRemoveSrc: stops and joins the source thread
int32_t load(uint32_t src);        // sceMediaFwLoad
int32_t play(uint32_t src);        // sceMediaFwPlay
int32_t pause(uint32_t src);       // sceMediaFwPause
int32_t stop(uint32_t src);        // sceMediaFwStop
int32_t seek(uint32_t src, double seconds);
int32_t set_playback_rate(uint32_t src, double rate);
int32_t set_loop(uint32_t src, bool loop);
int32_t set_muted(uint32_t src, bool muted);
int32_t set_volume(uint32_t src, double volume);
int32_t suppress_bgm(uint32_t src, bool disable);
int32_t set_display_size(uint32_t src, int32_t w, int32_t h);
int32_t change_active_status(uint32_t src, bool inactive);
int32_t cancel_playback_range(uint32_t src);
int32_t get_resolution(uint32_t src, int32_t* w, int32_t* h);
int32_t get_canvas_handle(uint32_t src, uint32_t* canvas, int32_t* dynamic_range);
int32_t get_ready_state(uint32_t src, uint32_t* state);
int32_t get_duration(uint32_t src, double* seconds);
int32_t get_current_time(uint32_t src, double* seconds);

}  // namespace ssn::psmutil::mediafw
