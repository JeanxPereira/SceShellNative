// "libSysBridgeAvControl" monitor and device queries (Sce.Vsh.SysBridge.AvControl, FW 4.03):
// the libSceAvSetting client functions over the AV-setting server's shared state.
//
// On the console libSceAvSetting keeps one object at 0x1C420. Each query takes the server's
// event flag (bit 32 + n, sub_BF10), copies the server's 8080-byte shared block into the
// object (base+48) and reads it; sub_BFB0 releases the flag. Relative to the object's +24
// the queries read:
//   +824       restriction bits: 0x0F withhold deep colour, 0xD0 withhold VRR (0x29E0, 0x1D20)
//   +2108      third word of the detail HDCP status (0x32D0)
//   +2504 = S: S+0..47 the server's video-out mode, used for an unset refresh rate,
//              resolution or content type of a request (0x29E0 -> sub_59A0 / sub_5CC0)
//              S+1444 HDCP version, S+1452 HDCP disabled (0x32D0), S+1460 HDCP status (0x2E70)
//              S+1468 HDMI device present, S+1472 its device id (0x2480, 0x23F0)
//              S+1480 / S+1488 secondary device present / id (device id 0x7004)
//              S+1500 output path: 2 = the monitor is reached through the secondary device
//   +4008 = M: four SceAvControlMonitorInfo (1024 bytes): M+0 native and M+1024 effective
//              info of the HDMI monitor, M+2048 / M+3072 the same with output path 2.
//
// HOST VALUE: the PC has no HDMI sink and no AV-setting server. The host declares one
// generic HDMI monitor, the sink of the declared video out (1080p 59.94 Hz, see
// libScePsm GetVideoOutStatus): CEA-861 VIC 16 (1920x1080p, 148.5/1.001 = 148.35 MHz at
// 59.94 Hz, native) plus the mandatory 720x480p (VIC 2 / 3), 24-bit RGB only, basic audio
// (2-channel LPCM 32/44.1/48 kHz, 16/20/24-bit), no HDR / deep colour / YCbCr / VRR /
// 3D / HDCP 2.2. Only the fields the firmware reads are filled (see kMonitor below); the
// capability checks themselves are the firmware's (mode_support.cpp).
#include <cstdint>
#include <cstring>
#include <mutex>

#include "host/boundary.h"
#include "libSceAvSetting/mode_support.h"

namespace ssn::avsetting {
namespace {

constexpr uint64_t kDeviceIdHdmi = 0x7000;    // the id 0x2770 / 0x1FA0 accept for the HDMI device
constexpr uint64_t kDeviceIdSecond = 0x7004;  // the secondary device's id (0x2770)
constexpr uint64_t kDeviceIdMask = 0xFFFFFFFF00FFFF00ull;

struct MonitorBlock {
  uint8_t b[kMonitorInfoSize] = {};
  void u8(int off, uint8_t v) { b[off] = v; }
  void u16(int off, uint16_t v) { std::memcpy(b + off, &v, 2); }
  void u32(int off, uint32_t v) { std::memcpy(b + off, &v, 4); }
};

// The declared monitor, field by field (SceAvControlMonitorInfo offsets; who reads them):
MonitorBlock make_monitor() {
  MonitorBlock m;
  m.u8(10, 2);          // monitorType: HDMI (0x29E0 / 0x2940 require 2 unless output path 2)
  m.u32(28, 0x4003);    // resolutionCapable60Hz bits as sub_67F0 maps them: 0x4000 1080P (VIC 16),
                        //   1 480P (VIC 2), 2 480P_WIDESCREEN (VIC 3); no 720p/1080i/2160p
                        // 50 Hz (+36), other rates (+44), 3D (+60..+99), 4:2:0 (+100) and
                        // "original" (+108) capability bits: none
  m.u8(116, 0x01);      // colorSpaceCapable.rgb: 24-bit (sub_7460 depth bits 1 / 8 30-bit / 2 36-bit);
                        //   bit 4 (RGB quantisation selectable) clear: RGB on CE formats is
                        //   limited range (sub_59A0)
                        // yuv444 / yuv422 / xvYCC / BT.2020 (+117..+120): none
                        // support_contents (+148): no content types
  m.u16(152, 1);        // audioBlockNum: one short audio descriptor (sub_7960)
  m.u32(156, 0x01);     // speakerPlacement: FL/FR (CEA speaker allocation; sub_7B40)
  m.u8(160, 1);         // audio[0].type: LPCM (sub_7D40)
  m.u8(161, 2);         // audio[0].max_chnum: 2 channels
  m.u16(162, 0x07);     // audio[0].samplebit_or_bitrate: 16 / 20 / 24 bit (sub_7960 word size bits)
  m.u32(164, 0x07);     // audio[0].fs: 32 / 44.1 / 48 kHz (sub_7960 fs bits)
                        // deep colour (+848..), HDR (+856), hdcpCapable (+864, bit 0 HDCP 2.2),
                        // FreeSync (+868..), VRR (+872..), extFlag (+878): none / zero
  return m;
}

struct ServerState {
  int32_t mode[kVideoModeWords];  // S+0
  uint32_t hdcp_version = 0;      // S+1444: SCE_AV_CONTROL_MONITOR_HDCP_VERSION_DEFAULT
  uint32_t hdcp_disabled = 0;     // S+1452
  uint32_t hdcp_status = 1;       // S+1460: SCE_AV_CONTROL_HDCP_STATUS_ENABLED (the PS5 default)
  uint32_t hdmi_present = 1;      // S+1468
  uint64_t hdmi_id = kDeviceIdHdmi;  // S+1472
  uint32_t second_present = 0;    // S+1480
  uint64_t second_id = 0;         // S+1488
  uint32_t output_path = 0;       // S+1500: the monitor on the HDMI port itself
  uint8_t restrictions = 0;       // +824: no user restriction (deep colour / VRR automatic)
  uint32_t hdcp_detail_word = 0;  // +2108
  MonitorBlock monitor;           // M+0 / M+1024 (output path 2 is not used)
};

void note_declared() {
  static std::once_flag once;
  std::call_once(once, [] {
    host::note_unimplemented(
        "HOST VALUE AvControl: one generic HDMI monitor (1080p59.94 VIC 16 native, 480p VIC 2/3, 24-bit RGB, 2ch LPCM), "
        "device 0x7000 connected, HDCP enabled 1.x, no secondary device");
  });
}

// sub_29E0's per-call context (the a3 block of sub_58D0) for monitor info `info`.
void video_context(const ServerState& s, const uint8_t* info, int32_t ctx[6]) {
  ctx[0] = ((info[27] & 1) != 0 || (s.restrictions & 0xF) == 0) ? 1 : 0;
  ctx[1] = 0;
  ctx[2] = 0;
  ctx[3] = 0;
  ctx[4] = (s.output_path == 2 || s.output_path == 0) ? 1 : 0;
  ctx[5] = (s.restrictions & 0xD0) == 0 ? 1 : 0;
}

const ServerState& state() {
  static const ServerState s = [] {
    ServerState st;
    st.monitor = make_monitor();
    // S+0: the mode the firmware's own resolution (sub_58D0) picks on the declared monitor for
    // 1080p 59.94 Hz with every other field left to the defaults (sceAvControlSetDefaultVideoOutMode).
    int32_t req[kVideoModeWords] = {255, 255, 255, 255, 3, 11, 255, -1, -1, -1, -1, -1};
    int32_t seed[kVideoModeWords] = {1, 1, 1, 24, 3, 11, 0, -1, -1, -1, -1, -1};
    MonitorBlock native = st.monitor;
    native.b[27] = 1;
    int32_t ctx[6];
    video_context(st, native.b, ctx);
    if (video_mode_supported(native.b, ctx, seed, req, st.mode) != 1) std::memcpy(st.mode, seed, sizeof seed);
    return st;
  }();
  return s;
}

bool id_matches(uint64_t a, uint64_t b) { return ((a ^ b) & kDeviceIdMask) == 0; }

// 0x2770 (shared by GetMonitorInfo 0x2760, native = 0, and GetNativeMonitorInfo 0x2930,
// native = 1): picks the info of the device id and copies 1024 bytes; native copies set
// flag (+27) to 1, also on the "no such device" error.
int32_t monitor_info(int64_t device_id, uint8_t* out, bool native) {
  if (!out) return kErrArg;
  note_declared();
  const ServerState& s = state();
  const uint64_t id = static_cast<uint64_t>(device_id);
  const bool primary = id == kDeviceIdHdmi || (s.hdmi_present != 0 && id_matches(id, s.hdmi_id));
  const bool second = id == kDeviceIdSecond || (s.second_present != 0 && id_matches(id, s.second_id));
  if (!primary && !second) {
    if (native) out[27] = 1;
    return kErrNoDevice;
  }
  // The HDMI device answers from M+0 / M+1024 (output path != 2); the secondary device (and
  // any device with output path 2) from M+2048 / M+3072, which hold no monitor on the host.
  if (primary && s.output_path != 2)
    std::memcpy(out, s.monitor.b, kMonitorInfoSize);
  else
    std::memset(out, 0, kMonitorInfoSize);
  if (native) out[27] = 1;
  return 0;
}

}  // namespace

// E8HrmeBTXLA (0x35D0 -> 0x2480): device type 1 only; the HDMI device's id when present.
SSN_PINVOKE("libSysBridgeAvControl", "sceAvControlGetCurrentDeviceId", int32_t, ssn_SysBridgeAvControl_GetCurrentDeviceId,
            (int32_t type, int64_t* id)) {
  if (type != 1) return kErrParam;
  if (!id) return kErrArg;
  note_declared();
  const ServerState& s = state();
  if (s.hdmi_present == 0) return kErrNotConnected;
  *id = static_cast<int64_t>(s.hdmi_id);
  return 0;
}

// Hrs7gsCRlk0 (0x3610 -> 0x23F0): 1 when the device the output path goes to is present.
SSN_PINVOKE("libSysBridgeAvControl", "sceAvControlIsDeviceConnected", int32_t, ssn_SysBridgeAvControl_IsDeviceConnected,
            (int32_t type)) {
  if (type != 1) return kErrParam;
  note_declared();
  const ServerState& s = state();
  return s.output_path != 2 ? (s.hdmi_present != 0 ? 1 : 0) : (s.second_present != 0 ? 1 : 0);
}

// eKy0c9bg0VU (0x3840 -> 0x2E70): S+1460.
SSN_PINVOKE("libSysBridgeAvControl", "sceAvControlGetHdcpStatus", int32_t, ssn_SysBridgeAvControl_GetHdcpStatus,
            (int32_t type, uint32_t* status)) {
  if (type != 1 || !status) return kErrParam;
  note_declared();
  *status = state().hdcp_status;
  return 0;
}

// v0dVsqUmQaw (0x3A90 -> 0x32D0): {S+1452, S+1444, (u64) +2108}.
SSN_PINVOKE("libSysBridgeAvControl", "sceAvControlGetDetailHdcpStatus", int32_t, ssn_SysBridgeAvControl_GetDetailHdcpStatus,
            (int32_t type, uint8_t* detail)) {
  if (type != 1 || !detail) return kErrParam;
  note_declared();
  const ServerState& s = state();
  const uint64_t word = s.hdcp_detail_word;
  std::memcpy(detail, &s.hdcp_disabled, 4);
  std::memcpy(detail + 4, &s.hdcp_version, 4);
  std::memcpy(detail + 8, &word, 8);
  return 0;
}

// GpuFjTMZsis (0x3620 -> 0x2760).
SSN_PINVOKE("libSysBridgeAvControl", "sceAvControlGetMonitorInfo", int32_t, ssn_SysBridgeAvControl_GetMonitorInfo,
            (int64_t device_id, uint8_t* info)) {
  return monitor_info(device_id, info, false);
}

// n6F49ESSgEU (0x3640 -> 0x2930).
SSN_PINVOKE("libSysBridgeAvControl", "sceAvControlGetNativeMonitorInfo", int32_t, ssn_SysBridgeAvControl_GetNativeMonitorInfo,
            (int64_t device_id, uint8_t* info)) {
  return monitor_info(device_id, info, true);
}

// KTkFIXpUuCg (0x36A0 -> 0x29E0): the monitor type must match the output path (2 HDMI, 3
// through the secondary device), else 0; then sub_58D0 with the server's mode (S+0) and the
// context built from the restriction bits, the info's native flag and the output path.
SSN_PINVOKE("libSysBridgeAvControl", "sceAvControlIsVideoOutModeSupportedByMonitorInfo", int32_t,
            ssn_SysBridgeAvControl_IsVideoOutModeSupportedByMonitorInfo, (const uint8_t* info, const int32_t* mode)) {
  note_declared();
  const ServerState& s = state();
  const uint8_t type = info[10];  // read before the null checks of sub_58D0, as 0x29E0 does
  if (s.output_path == 2 ? type != 3 : type != 2) return 0;
  int32_t ctx[6];
  video_context(s, info, ctx);
  return video_mode_supported(info, ctx, s.mode, mode, nullptr);
}

// JphQB95YwSo (0x3680 -> 0x2940): same monitor-type gate, then sub_7960.
SSN_PINVOKE("libSysBridgeAvControl", "sceAvControlIsAudioOutModeSupportedByMonitorInfo", int32_t,
            ssn_SysBridgeAvControl_IsAudioOutModeSupportedByMonitorInfo, (const uint8_t* info, const uint8_t* mode)) {
  note_declared();
  const ServerState& s = state();
  const uint8_t type = info[10];
  if (s.output_path == 2 ? type != 3 : type != 2) return 0;
  return audio_mode_supported(info, mode);
}

// 5wP7bivaX7c (0x3710): pure.
SSN_PINVOKE("libSysBridgeAvControl", "sceAvControlIsHdrSupportedByMonitorInfo", int32_t,
            ssn_SysBridgeAvControl_IsHdrSupportedByMonitorInfo, (const uint8_t* info)) {
  return hdr_supported(info);
}

}  // namespace ssn::avsetting
