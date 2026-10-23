// libSceAvSetting (FW 4.03) client-side capability checks over a SceAvControlMonitorInfo.
// Pure functions: no AV-setting server state beyond what the caller passes in.
#pragma once
#include <cstdint>

namespace ssn::avsetting {

constexpr int32_t kErrArg = static_cast<int32_t>(0x802A0001u);      // null pointer
constexpr int32_t kErrParam = static_cast<int32_t>(0x802A0002u);    // bad device type / reserved fields
constexpr int32_t kErrNoDevice = static_cast<int32_t>(0x802A0003u); // device id not current
constexpr int32_t kErrNotConnected = static_cast<int32_t>(0x802A0004u);

constexpr int kMonitorInfoSize = 1024;  // SceAvControlMonitorInfo
constexpr int kVideoModeWords = 12;     // SceAvControlVideoOutMode (48 bytes)

// sub_58D0: the request `mode` (SceAvControlVideoOutMode) against the monitor info `info`,
// the per-call context flags `ctx[6]` (built by sub_29E0 from the server state) and the
// server's video-out mode `cur` (status+0). 1 supported, 0 not, <0 error. `out` (nullable)
// receives the mode the firmware resolves.
int32_t video_mode_supported(const uint8_t* info, const int32_t ctx[6], const int32_t* cur, const int32_t* mode, int32_t* out);

// sub_7960: an SceAvControlAudioOutMode against the monitor's audio blocks.
int32_t audio_mode_supported(const uint8_t* info, const uint8_t* mode);

// 5wP7bivaX7c (0x3710): sceAvControlIsHdrSupportedByMonitorInfo.
int32_t hdr_supported(const uint8_t* info);

}  // namespace ssn::avsetting
