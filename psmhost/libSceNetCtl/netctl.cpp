// libSceNetCtl (FW 4.03) as ShellUI's process uses it, and the P/Invoke wrappers that
// reach it: "libNetCtlWrapper" (eboot.bin table 0xcf21f0: each entry jumps to the
// libSceNetCtl export of the same meaning) and "NativeExtensions.sprx" (libNativeExtensions
// 0x153d0..0x15480).
//
// The client side is reproduced from libSceNetCtl.sprx: one per-process library (init flag
// byte_10030, lock qword_10020), two callback tables of 8 {id, fn, arg} slots — the
// public one (sceNetCtlRegisterCallback rqkh2kXvLSw 0x2230, table 0x111C0) and the
// IPC-internal one (UJ+Z7Q-4ck0 0x2010, table 0x11040) — and event bits kept by the
// netctl server per category, which sceNetCtlCheckCallback (iQw3iQPhvUQ 0x2650 ->
// 0x2680) fetches and clears (mode 0x22) and turns into callback events:
//   bit 0 -> 2 ConnectReqFinished, bit 1 -> 3 DisconnectReqFinished, bit 2 -> 4
//   ScanReqFinished, bit 3 -> 1 Disconnected, bit 4 -> 5, bit 5 -> 6 (public table);
//   bits 1 / 3 / 5 also reach the internal table as 2 / 1 / 3.
//
// HOST VALUE (the server side, i.e. the network hardware): the PC has no PS5 network
// device. The host netctl server reports state 0 (disconnected, no cable / no Wi-Fi
// association), completes a scan immediately with no networks found, completes a
// disconnect request immediately, and reports result 0 for both.
#include <cstdint>
#include <cstring>
#include <mutex>

#include "host/boundary.h"

namespace ssn::netctl {
namespace {

constexpr int32_t kErrNotInitialized = static_cast<int32_t>(0x80412101u);
constexpr int32_t kErrCallbackMax = static_cast<int32_t>(0x80412103u);
constexpr int32_t kErrIdNotFound = static_cast<int32_t>(0x80412104u);
constexpr int32_t kErrInvalidId = static_cast<int32_t>(0x80412105u);
constexpr int32_t kErrInvalidAddr = static_cast<int32_t>(0x80412107u);
constexpr int32_t kErrInvalidAddrScan = static_cast<int32_t>(0x80412107u);

using Callback = void (*)(int32_t event_type, void* arg);

struct Slot {
  Callback fn = nullptr;
  void* arg = nullptr;
};

struct Lib {
  std::recursive_mutex mu;      // qword_10020
  bool initialized = false;     // byte_10030
  Slot pub[8];                  // 0x111C0
  Slot ipc[8];                  // 0x11040
  uint32_t pending = 0;         // server event bits, category 0
  int32_t result[8] = {};       // server: last result per event type
};

Lib& lib() {
  static Lib l;
  return l;
}

void host_note() {
  host::note_unimplemented("HOST VALUE netctl server: no network hardware (state disconnected; scans complete with no networks)");
}

// 0x8OJ86vFucfo table dword_A034: event type 1..6 -> server event bit.
int bit_of(int32_t event_type) {
  static const int kBits[6] = {3, 0, 1, 2, 4, 5};
  return (event_type >= 1 && event_type <= 6) ? kBits[event_type - 1] : 15;
}

int32_t reg(Slot* table, Callback fn, void* arg, int32_t* cid) {
  Lib& l = lib();
  std::lock_guard<std::recursive_mutex> g(l.mu);
  if (!l.initialized) return kErrNotInitialized;
  if (!fn || !cid) return kErrInvalidAddr;
  for (int32_t i = 0; i < 8; ++i)
    if (!table[i].fn) {
      table[i] = {fn, arg};
      *cid = i;
      return 0;
    }
  return kErrCallbackMax;
}

int32_t unreg(Slot* table, int32_t cid) {
  Lib& l = lib();
  std::lock_guard<std::recursive_mutex> g(l.mu);
  if (!l.initialized) return kErrNotInitialized;
  if (uint32_t(cid) > 7) return kErrInvalidId;
  if (!table[cid].fn) return kErrIdNotFound;
  table[cid].fn = nullptr;
  return 0;
}

}  // namespace

// sceNetCtlInit (gky0_oaNM4k 0x1be0): connects to the server once, clears every event
// bit and marks the library initialised.
int32_t init() {
  Lib& l = lib();
  std::lock_guard<std::recursive_mutex> g(l.mu);
  if (!l.initialized) {
    l.pending = 0;
    l.initialized = true;
    host_note();
  }
  return 0;
}

int32_t check_callback() {
  Lib& l = lib();
  uint32_t bits;
  {
    std::lock_guard<std::recursive_mutex> g(l.mu);
    if (!l.initialized) return kErrNotInitialized;
    bits = l.pending;
    l.pending = 0;
  }
  static const int kPub[6] = {2, 3, 4, 1, 5, 6};
  static const int kIpc[6] = {0, 2, 0, 1, 0, 3};
  static const bool kAlsoIpc[6] = {false, true, false, true, false, true};
  for (int i = 0; i < 15; ++i) {
    if (!(bits & (1u << i)) || i > 5) continue;
    Slot pub[8], ipc[8];
    {
      std::lock_guard<std::recursive_mutex> g(l.mu);
      std::memcpy(pub, l.pub, sizeof pub);
      std::memcpy(ipc, l.ipc, sizeof ipc);
    }
    for (auto& s : pub)
      if (s.fn) s.fn(kPub[i], s.arg);
    if (kAlsoIpc[i])
      for (auto& s : ipc)
        if (s.fn) s.fn(kIpc[i], s.arg);
  }
  return 0;
}

int32_t clear_event(int32_t event_type) {
  Lib& l = lib();
  std::lock_guard<std::recursive_mutex> g(l.mu);
  if (!l.initialized) return kErrNotInitialized;
  l.pending &= ~(1u << bit_of(event_type));
  return 0;
}

int32_t get_result(int32_t event_type, int32_t* error_code) {
  Lib& l = lib();
  std::lock_guard<std::recursive_mutex> g(l.mu);
  if (!l.initialized) return kErrNotInitialized;
  if (!error_code) return kErrInvalidAddr;
  *error_code = (event_type >= 0 && event_type < 8) ? l.result[event_type] : 0;
  return 0;
}

int32_t get_state(int32_t* state) {
  Lib& l = lib();
  std::lock_guard<std::recursive_mutex> g(l.mu);
  if (!l.initialized) return kErrNotInitialized;
  if (!state) return kErrInvalidAddr;
  *state = 0;  // HOST VALUE: disconnected
  return 0;
}

// sceNetCtlScan (reIsHryCDx4 0x3790): ssids (<= 10, each 1..32 chars) or none; the server
// answers immediately on the host (bit 2, result 0).
int32_t scan(const char* const* ssids, uint32_t n) {
  Lib& l = lib();
  std::lock_guard<std::recursive_mutex> g(l.mu);
  if (!l.initialized) return kErrNotInitialized;
  if (!ssids && n != 0) return kErrInvalidAddrScan;
  if (n > 10) return static_cast<int32_t>(0x80412191u);
  for (uint32_t i = 0; i < n; ++i) {
    const size_t len = strnlen(ssids[i], 33);
    if (len - 1 > 0x1F) return static_cast<int32_t>(0x80412190u);
  }
  l.result[4] = 0;
  l.pending |= 1u << 2;
  return 0;
}

int32_t disconnect() {
  Lib& l = lib();
  std::lock_guard<std::recursive_mutex> g(l.mu);
  if (!l.initialized) return kErrNotInitialized;
  l.result[3] = 0;
  l.pending |= 1u << 1;
  return 0;
}

int32_t get_scan_info_bssid(void* bssids, uint32_t n) {
  Lib& l = lib();
  std::lock_guard<std::recursive_mutex> g(l.mu);
  if (!l.initialized) return kErrNotInitialized;
  if (!bssids || n == 0) return kErrInvalidAddr;
  return 0;  // no networks found
}

int32_t set_error_notification_enabled(int32_t) {
  Lib& l = lib();
  std::lock_guard<std::recursive_mutex> g(l.mu);
  if (!l.initialized) return kErrNotInitialized;
  return 0;
}

// ---- "liblx.a" (eboot) -------------------------------------------------------------------
// sceNetCtlGetState2IpcInt (JXlI9EZVjf4, libSceNetCtl 0x32E0): like sceNetCtlGetState
// (uBPlr0lbuiI 0x31E0) — not initialised 0x80412101, null 0x80412107 — but the server query is
// 0x20009 instead of 0x20004. The host server answers the same state for both (HOST VALUE: 0,
// disconnected).
int32_t get_state2(int32_t* state) { return get_state(state); }

// lx::lxNetCtl::GetState() — eboot 0x9100: sceNetCtlGetState2IpcInt(&state); the state, or 0
// when the call fails.
SSN_PINVOKE("liblx.a", "lx::lxNetCtl::GetState", int32_t, ssn_lx_NetCtl_GetState, ()) {
  int32_t st = 0;
  return get_state2(&st) >= 0 ? st : 0;
}

// "libSysBridgeNetCtl" sceNetCtlGetState2IpcInt — libSceSysBridge table 0x78A60: the import itself.
SSN_PINVOKE("libSysBridgeNetCtl", "sceNetCtlGetState2IpcInt", int32_t, ssn_SysBridgeNetCtl_GetState2IpcInt, (int32_t* state)) {
  return get_state2(state);
}

// sceNetCtlGetInfoIpcInt (xstcTqAhTys, libSceNetCtl 0x3440; libSceSysBridge table 0x78A20 -> the
// import): not initialised 0x80412101, null 0x80412107; otherwise (no client hook at qword_117C0)
// IPMI method 0x20005 (0x300) with the code and a 256-byte output. The server (SceShellCore
// dispatcher 0x5F3D0 case 0x20005 -> 0x3FF600; devkit build 0x5F3D0 -> 0x408610) zeroes the 256
// bytes and asks 0x401000 (devkit 0x40A010) -> 0x4029E0 (devkit 0x40BA40) with the state
// dword_1A4202C. That function answers only when the server's flags (dword_1A42040) have 0x20 (set
// by a connect request, 0x403940 with mode 1) or the state is not 0 and flag 2 is clear; else
// 0x80412108 for every code. The flags start as 2 (server init 0x403210, devkit 0x40C270) and the
// state as 0. On the host no connection is ever made (see HOST VALUE above), so the info is 256
// zero bytes and the result 0x80412108.
int32_t get_info_ipc(int32_t /*code*/, void* info) {
  Lib& l = lib();
  std::lock_guard<std::recursive_mutex> g(l.mu);
  if (!l.initialized) return kErrNotInitialized;
  if (!info) return kErrInvalidAddr;
  std::memset(info, 0, 256);
  return static_cast<int32_t>(0x80412108u);
}

// sceNetCtlGetConnectionElapsedTimeIpcInt (15JUzFaWTkY, libSceNetCtl 0x44E0; libSceSysBridge table
// 0x78A50): not initialised 0x80412101, null 0x80412107; IPMI method 0x20021 with an 8-byte output.
// The server (dispatcher case 0x20021 -> 0x400560, devkit 0x409570) refuses callers whose authority
// id (syscall 587) is 0x4800000010000004 / ..11 / ..12 / ..13 or 0x4800000000001010 (0x794630,
// devkit 0x79FB70; 0x804121A1). HOST VALUE: ShellUI's authority id is in the SELF auth info, which a
// decrypted dump does not carry; the host treats ShellUI as not one of those ids. Then
// 0x401320 -> 0x4041C0 (devkit
// 0x40D220): elapsed = now - connection start (qword_1A42060), or 0 while that start is 0. The host
// never connects, so the start stays 0: elapsed 0, result 0.
int32_t get_connection_elapsed_time_ipc(uint64_t* seconds) {
  Lib& l = lib();
  std::lock_guard<std::recursive_mutex> g(l.mu);
  if (!l.initialized) return kErrNotInitialized;
  if (!seconds) return kErrInvalidAddr;
  *seconds = 0;
  return 0;
}

SSN_PINVOKE("libSysBridgeNetCtl", "sceNetCtlGetInfoIpcInt", int32_t, ssn_SysBridgeNetCtl_GetInfoIpcInt,
            (int32_t code, void* info)) {
  return get_info_ipc(code, info);
}
SSN_PINVOKE("libSysBridgeNetCtl", "sceNetCtlGetConnectionElapsedTimeIpcInt", int32_t,
            ssn_SysBridgeNetCtl_GetConnectionElapsedTimeIpcInt, (uint64_t* seconds)) {
  return get_connection_elapsed_time_ipc(seconds);
}

// sceNetConnSwitcherStart (7BXL2Wg5jNU, libSceNetCtl 0x58A0): not initialised 0x80412101; else IPMI
// method 0x20036 with no data. Server: dispatcher case 0x20036 -> 0x400E30 (devkit 0x409E40): the
// same authority check as above (0x804121A1), then 0x3FACE0 (devkit 0x403CF0): when the
// connection switcher is running (byte_1A41088, set by its start 0x3FAB20) its event flag gets
// bit 0x40 cleared and bit 0x20 set; result 0 either way. What the switcher thread then does
// (switching between network devices) needs network hardware; the host has none (HOST VALUE
// above), so only the result is reproduced.
int32_t net_conn_switcher_start() {
  Lib& l = lib();
  std::lock_guard<std::recursive_mutex> g(l.mu);
  if (!l.initialized) return kErrNotInitialized;
  return 0;
}

// sceInetStatGetState (sngJX5nUjVM, libSceNetCtl 0x57A0): not initialised 0x80412380, null 0x80412384;
// IPMI method 0x20033 with a 4-byte output. Server: dispatcher case 0x20033 -> 0x400CF0 (devkit
// 0x409D00): the authority check above, then 0x3F2770 (devkit 0x3FB730): the InetStat state
// dword_1A3F7FC (server started by 0x3F26E0). Its init 0x3F2840 (devkit 0x3FB800) sets it to 0; the
// InetStat thread 0x3F3F60 (devkit 0x3FCF20) changes it only when its state-0 handler sees a
// connection on the netctl flags, which the host never raises: state 0, result 0.
int32_t inet_stat_get_state(int32_t* state) {
  Lib& l = lib();
  std::lock_guard<std::recursive_mutex> g(l.mu);
  if (!l.initialized) return static_cast<int32_t>(0x80412380u);
  if (!state) return static_cast<int32_t>(0x80412384u);
  *state = 0;
  return 0;
}

// "libReactNative.Modules.Vsh.sprx" InetStatGetStateNative — export katLYKl5Xhw (0x44E0), a thunk to
// sceInetStatGetState.
SSN_PINVOKE("libReactNative.Modules.Vsh.sprx", "InetStatGetStateNative", int32_t, ssn_RNModulesVsh_InetStatGetState,
            (int32_t* state)) {
  return inet_stat_get_state(state);
}

// ---- "libNetCtlWrapper" (eboot) ------------------------------------------------------
// NetConnSwitcherStartNative — {name, fn} entry 0xcf2700 (devkit build 0xcf6720) -> 0x4DA050
// (devkit 0x4DB400), a thunk to sceNetConnSwitcherStart.
SSN_PINVOKE("libNetCtlWrapper", "NetConnSwitcherStartNative", int32_t, ssn_NetCtlWrapper_NetConnSwitcherStart, ()) {
  return net_conn_switcher_start();
}
SSN_PINVOKE("libNetCtlWrapper", "NetCtlInitNative", int32_t, ssn_NetCtlWrapper_Init, ()) { return init(); }
SSN_PINVOKE("libNetCtlWrapper", "NetCtlCheckCallbackNative", int32_t, ssn_NetCtlWrapper_CheckCallback, ()) { return check_callback(); }
SSN_PINVOKE("libNetCtlWrapper", "NetCtlClearEventNative", int32_t, ssn_NetCtlWrapper_ClearEvent, (int32_t t)) { return clear_event(t); }
SSN_PINVOKE("libNetCtlWrapper", "NetCtlGetResultNative", int32_t, ssn_NetCtlWrapper_GetResult, (int32_t t, int32_t* e)) {
  return get_result(t, e);
}
SSN_PINVOKE("libNetCtlWrapper", "NetCtlRegisterCallbackNative", int32_t, ssn_NetCtlWrapper_RegisterCallback,
            (Callback fn, void* arg, int32_t* cid)) {
  return reg(lib().pub, fn, arg, cid);
}
SSN_PINVOKE("libNetCtlWrapper", "NetCtlUnregisterCallbackNative", int32_t, ssn_NetCtlWrapper_UnregisterCallback, (int32_t cid)) {
  return unreg(lib().pub, cid);
}
SSN_PINVOKE("libNetCtlWrapper", "NetCtlGetStateNative", int32_t, ssn_NetCtlWrapper_GetState, (int32_t* s)) { return get_state(s); }
SSN_PINVOKE("libNetCtlWrapper", "NetCtlDisconnectNative", int32_t, ssn_NetCtlWrapper_Disconnect, ()) { return disconnect(); }
// NetCtlScanNative (0x4d9c70): an empty option pointer is passed as null.
SSN_PINVOKE("libNetCtlWrapper", "NetCtlScanNative", int32_t, ssn_NetCtlWrapper_Scan, (const char* const* ssids, uint32_t n, void*)) {
  return scan(ssids, n);
}
SSN_PINVOKE("libNetCtlWrapper", "NetCtlAnyScanNative", int32_t, ssn_NetCtlWrapper_AnyScan, ()) { return scan(nullptr, 0); }
SSN_PINVOKE("libNetCtlWrapper", "NetCtlGetScanInfoBssidNative", int32_t, ssn_NetCtlWrapper_GetScanInfoBssid, (void* b, uint32_t n)) {
  return get_scan_info_bssid(b, n);
}
SSN_PINVOKE("libNetCtlWrapper", "NetCtlSetErrorNotificationEnabledNative", int32_t, ssn_NetCtlWrapper_SetErrorNotificationEnabled,
            (int32_t on)) {
  return set_error_notification_enabled(on);
}

// ---- "NativeExtensions.sprx" (libNativeExtensions 0x153d0..0x15480) -------------------
SSN_PINVOKE("NativeExtensions.sprx", "NetCtlInitNative", int32_t, ssn_NativeExtensions_NetCtlInit, ()) { return init(); }
SSN_PINVOKE("NativeExtensions.sprx", "NetCtlGetStateNative", int32_t, ssn_NativeExtensions_NetCtlGetState, (int32_t* s)) {
  return get_state(s);  // uBPlr0lbuiI: same server state
}
// NetCtlGetDeviceTypeNative (export z4zamLphAP4, 0x15400): sceNetCtlGetInfo (obuxdTiwkF8, libSceNetCtl
// 0x3360: the same IPMI method 0x20005 as sceNetCtlGetInfoIpcInt above; its code-10 adjustment only
// on success) with code 1 into a 256-byte local; on result 0 *type = its first word. The host
// server answers 0x80412108, so *type is not written.
SSN_PINVOKE("NativeExtensions.sprx", "NetCtlGetDeviceTypeNative", int32_t, ssn_NativeExtensions_NetCtlGetDeviceType,
            (uint32_t* type)) {
  uint32_t info[64];
  const int32_t r = get_info_ipc(1, info);
  if (r == 0) *type = info[0];
  return r;
}
// RegisterCallback (0x15460): the IPC-internal table with arg 0.
SSN_PINVOKE("NativeExtensions.sprx", "NetCtlRegisterCallbackNative", int32_t, ssn_NativeExtensions_NetCtlRegisterCallback,
            (Callback fn, int32_t* cid)) {
  return reg(lib().ipc, fn, nullptr, cid);
}
SSN_PINVOKE("NativeExtensions.sprx", "NetCtlUnregisterCallbackNative", int32_t, ssn_NativeExtensions_NetCtlUnregisterCallback,
            (int32_t cid)) {
  return unreg(lib().ipc, cid);
}
SSN_PINVOKE("NativeExtensions.sprx", "NetCtlCheckCallbackNative", int32_t, ssn_NativeExtensions_NetCtlCheckCallback, ()) {
  return check_callback();
}

}  // namespace ssn::netctl
