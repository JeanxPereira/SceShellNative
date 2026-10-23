// "libBtDeviceManager" P/Invokes of Sce.Vsh.ShellUI.Settings.Bt.BtDeviceManager (eboot.bin,
// FW 4.03): Settings > Accessories > Bluetooth Accessories talks to the kernel Bluetooth
// driver through /dev/bt. eboot {name, fn} table entries (0xcf2d30..0xcf2db0):
//   OpenBtFileDescriptor  0x4E7110: close any previous fd/kqueue; open("/dev/bt", O_RDWR)
//     (fail -> 0x80EC1000); write the 8-byte value 254 to it (short write -> 0x80EC1001);
//     kqueue() (fail -> 0x80EC1002); register EVFILT_READ on the fd (fail -> 0x80EC1003); 0.
//   CloseBtFileDescriptor 0x4E7320: removes the kevent, closes kqueue and fd.
//   WaitBtDriverEvent     0x4E73F0: kevent(kq, -, -, out, 1, timeout 0) — a poll:
//     error -> 0x80EC1004; no event -> 0 with the caller's SceBtEvent untouched (type 0 is
//     ignored by BtEventHandlerJob); an event -> read 16 bytes into it (fail -> 0x80EC1005;
//     null buffer -> 0x80EC1200).
//   sceBtStartInquiry     0x4E7750: ioctl(fd, 0x20044204, 0).
//   sceBtStopInquiry      0x4E7770: ioctl(fd, 0x20044205, 0).
// SceBtEvent (16 bytes): type u8, status u8, reserved[2], val u32, addr u64.
//
// HOST VALUE (the Bluetooth radio): the PC exposes no Bluetooth controller to the console
// model. The host /dev/bt accepts the open and the inquiry ioctls, finds no device, and
// answers an inquiry stop with SCE_BT_EVENT_TYPE_INQUIRY_STOP (2, status 0) — the event
// BtDriverEventHandler.NotifyInquiryStop turns into BtState.IDLE. The driver's own event
// timing (and whether an inquiry also ends by itself) is not traced.
//
// Before this, the stubs returned 0 without ever posting an event, so BtInquiryJob (Legacy,
// BtInquiryJob.Run) spun forever waiting for IDLE after sceBtStopInquiry; leaving the page
// made BluetoothScene.OnUnloading run BtSettingManager.Exit on PUI's default job queue, whose
// JobQueue.WaitEmpty then never returned — every later job on that queue (including the
// next Settings scene load) stalled.
#include <cstdint>
#include <cstring>
#include <deque>
#include <mutex>

#include "host/boundary.h"

namespace ssn::eboot {
namespace {

constexpr int32_t kErrNullBuffer = static_cast<int32_t>(0x80EC1200u);
constexpr uint8_t kEventInquiryStop = 2;  // SCE_BT_EVENT_TYPE_INQUIRY_STOP

struct BtDriver {
  std::mutex mu;
  bool open = false;
  bool inquiry = false;
  std::deque<uint8_t> pending;  // event types queued for the fd
};

BtDriver& bt() {
  static BtDriver d;
  return d;
}

void host_note() {
  static const bool noted = (host::note_unimplemented(
                                 "HOST VALUE /dev/bt: no Bluetooth radio; inquiries find no device and a stop posts INQUIRY_STOP"),
                             true);
  (void)noted;
}

}  // namespace

SSN_PINVOKE("libBtDeviceManager", "OpenBtFileDescriptor", int32_t, ssn_BtDeviceManager_Open, ()) {
  host_note();
  BtDriver& d = bt();
  std::lock_guard l(d.mu);
  d.open = true;
  d.inquiry = false;
  d.pending.clear();
  return 0;
}

SSN_PINVOKE("libBtDeviceManager", "CloseBtFileDescriptor", int32_t, ssn_BtDeviceManager_Close, ()) {
  BtDriver& d = bt();
  std::lock_guard l(d.mu);
  d.open = false;
  d.inquiry = false;
  d.pending.clear();
  return 0;
}

SSN_PINVOKE("libBtDeviceManager", "WaitBtDriverEvent", int32_t, ssn_BtDeviceManager_WaitEvent, (uint8_t* ev)) {
  BtDriver& d = bt();
  std::lock_guard l(d.mu);
  if (d.pending.empty()) return 0;  // kevent poll: nothing ready
  if (!ev) return kErrNullBuffer;
  std::memset(ev, 0, 16);
  ev[0] = d.pending.front();
  d.pending.pop_front();
  return 0;
}

SSN_PINVOKE("libBtDeviceManager", "sceBtStartInquiry", int32_t, ssn_BtDeviceManager_StartInquiry, ()) {
  BtDriver& d = bt();
  std::lock_guard l(d.mu);
  d.inquiry = true;
  return 0;
}

SSN_PINVOKE("libBtDeviceManager", "sceBtStopInquiry", int32_t, ssn_BtDeviceManager_StopInquiry, ()) {
  BtDriver& d = bt();
  std::lock_guard l(d.mu);
  if (d.inquiry && d.open) d.pending.push_back(kEventInquiryStop);
  d.inquiry = false;
  return 0;
}

}  // namespace ssn::eboot
