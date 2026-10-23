// "libSysBridgeAvControl" entries (Sce.Vsh.SysBridge.AvControl, FW 4.03) that are pure
// libSceAvSetting client code: no AV-setting server state, no IPC.
//
// libSceSysBridge publishes "libSysBridgeAvControl" as a {name, fn} table whose entries are
// the libSceAvSetting imports themselves:
//   sceAvControlSetDefaultVideoOutMode            table 0x78150 -> WsGR1gzifpE (libSceAvSetting 0x8970)
//   sceAvControlSetDefaultAudioOutMode            ActTHWrmwbo (0x8920)
//   sceAvControlIsColorimetryHdr                  table 0x78190 -> 3jnRsjnzIjI (0x3b20)
//   sceAvControlIsDeepColorSupportedByMonitorInfo fnqFpsRrsg4 (0x3730)
//
// ShellUI's HdmiOutputMonitor (Sce.Vsh.ShellUI.AppSystem) and the RN AV module
// (Sce.Vsh.ShellUI.ReactNativeShellApp) call SetDefaultVideoOutMode once per mode they test
// (10 refresh rates x resolutions), which made it the most frequent AvControl stub.
//
// Not here: the entries that read the AV-setting server's shared state (libSceAvSetting
// object 0x1C420: an 8080-byte snapshot copied under event flag bit 32+n, sub_BF10 /
// sub_BFB0). The monitor and device queries over that state — GetCurrentDeviceId (0x2480),
// IsDeviceConnected (0x23F0), Get(Detail)HdcpStatus (0x2E70 / 0x32D0), Get(Native)MonitorInfo
// (0x2760 / 0x2930 -> 0x2770), IsVideoOutModeSupportedByMonitorInfo (0x29E0 -> 0x58D0),
// IsAudioOutModeSupportedByMonitorInfo (0x2940 -> 0x7960), IsHdrSupportedByMonitorInfo
// (0x3710) — are in av_monitor.cpp over the host's declared monitor. The output-mode
// queries and the IPC requests SetColorEffect (0x3870) and SetColorMode (0x38D0 -> 0x11B0)
// stay stubs.
#include <cstdint>
#include <cstring>

#include "host/boundary.h"

namespace ssn::avsetting {

// WsGR1gzifpE (0x8970): null -> nothing. Otherwise (SceAvControlVideoOutMode, 48 bytes):
//   +44 = -1; +28..+43 = -1 (vpcmpeqd); +0..+15 = 4 x dword_13F94 (0x000000FF:
//   signalEncoding, signalRange, colorimetry, depth = 255); +16 qword = -1 (refreshRate,
//   resolution); +24 = 255 (contentType).
SSN_PINVOKE("libSysBridgeAvControl", "sceAvControlSetDefaultVideoOutMode", void, ssn_SysBridgeAvControl_SetDefaultVideoOutMode,
            (uint8_t* mode)) {
  if (!mode) return;
  const uint32_t ff = 0xFFu, ones = 0xFFFFFFFFu;
  for (int off = 28; off < 48; off += 4) std::memcpy(mode + off, &ones, 4);
  for (int off = 0; off < 16; off += 4) std::memcpy(mode + off, &ff, 4);
  std::memcpy(mode + 16, &ones, 4);
  std::memcpy(mode + 20, &ones, 4);
  std::memcpy(mode + 24, &ff, 4);
}

// ActTHWrmwbo (0x8920): null -> nothing. Otherwise (SceAvControlAudioOutMode, 56 bytes):
//   +0..+55 = -1 (two 32-byte stores at +0 and +0x18); +0..+15 = xmmword_14190
//   {255, 255, 255, 0xFFFFFFFF} (coding, channel, wordSize, fs); +16 qword = -1
//   (speakerLayout, format); +24 = 255 (copyControl).
SSN_PINVOKE("libSysBridgeAvControl", "sceAvControlSetDefaultAudioOutMode", void, ssn_SysBridgeAvControl_SetDefaultAudioOutMode,
            (uint8_t* mode)) {
  if (!mode) return;
  std::memset(mode, 0xFF, 56);
  const uint32_t head[4] = {0xFFu, 0xFFu, 0xFFu, 0xFFFFFFFFu};
  std::memcpy(mode, head, 16);
  const uint32_t ff = 0xFFu;
  std::memcpy(mode + 24, &ff, 4);
}

// 3jnRsjnzIjI (0x3b20): colorimetry 9..20 -> dword_141A0[c - 9]
// {1,1,1,1,1,1,1,1,1,0,1,1}; anything else 0.
SSN_PINVOKE("libSysBridgeAvControl", "sceAvControlIsColorimetryHdr", int32_t, ssn_SysBridgeAvControl_IsColorimetryHdr,
            (uint32_t colorimetry)) {
  static const int32_t kTable[12] = {1, 1, 1, 1, 1, 1, 1, 1, 1, 0, 1, 1};
  const uint32_t i = colorimetry - 9;
  return i > 11 ? 0 : kTable[i];
}

// fnqFpsRrsg4 (0x3730): 1 when (info[116] & 0xA) or (info[117] & 0xA), else (info[850] & 3) != 0.
SSN_PINVOKE("libSysBridgeAvControl", "sceAvControlIsDeepColorSupportedByMonitorInfo", int32_t,
            ssn_SysBridgeAvControl_IsDeepColorSupportedByMonitorInfo, (const uint8_t* info)) {
  if ((info[116] & 0xA) != 0 || (info[117] & 0xA) != 0) return 1;
  return (info[850] & 3) != 0 ? 1 : 0;
}

}  // namespace ssn::avsetting
