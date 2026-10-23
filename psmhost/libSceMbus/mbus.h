// The device bus ("mbus") as ShellUI sees it through libSceMbus (FW 4.03).
//
// libSceMbus is a thin IPMI client: sceMbusGetDeviceInfoByCondition (NID KRL_S9qBqXw,
// libSceMbus 0x4a90 -> 0x1690, method 0xCE110029) forwards a 256-byte condition to the
// mbus server in SceSysCore, whose DeviceManager keeps 32 device entries of 256 bytes,
// filled when the kernel reports a device (SceSysCore 0x8ca30 "addDevice", from
// ioctl SCE_MBUS_KMOD_IOCTL_GET_DEVICE_INFO / _PROPERTY). The host reproduces the
// server's matching (0x67ff0 -> 0x7b3e0 / 0x7b590 / 0x7b260) and its entry -> client
// record conversion (0x75500) over a device table that holds the host's one device,
// the emulated DualSense of the input layer.
#pragma once
#include <cstdint>

namespace ssn::mbus {

constexpr int32_t kRecordSize = 256;

// SceMbusDeviceInfo, the client record (written by SceSysCore 0x75500).
struct DeviceInfo {
  uint8_t raw[kRecordSize];
};

// sceMbusGetDeviceInfoByCondition(cond, start, out, max, &total, &generation, recordSize)
// as libSceMbus 0x4a90 returns it: the number of records copied (at most min(max, 16)),
// out records of `record_size` (only 256 copies), total = number of matching devices,
// generation = the DeviceManager's change counter (0x7b9c0).
int32_t get_device_info_by_condition(const uint8_t* cond, int32_t start, uint8_t* out, int32_t max, int32_t* total,
                                     int32_t* generation, int32_t record_size);

// libSceMbus's condition initialiser (NID edYHYROxzx4, 0x4a60): zero, then
// deviceId/+8/+12 = -1 (bytes 0..15 = 0xFF) and userId (+200) = -1.
void init_condition(uint8_t* cond, uint32_t size);

// sceMbusBindDeviceWithUserId(deviceId, userId) (Cpo6sQmnZgU, libSceMbus 0x3cf0 -> IPMI
// 0xCE110004) -> SceSysCore 0x63350: nothing to do when the device already belongs to the
// user (0x7B770); otherwise the entry's owner (+28) becomes the user (0x77470, which keeps a
// non-zero previous owner at +32 when the new one is 0). Unknown device -> -10.
int32_t bind_device_with_user_id(uint64_t device_id, int32_t user_id);

// ---- device events (sceMbusEvent*) --------------------------------------------------
// The mbus server queues one event record per client event handle; clients block on
// their handle and read records one at a time. The fields below are the ones the
// clients read: libScePsm's controller scanner (0x188AF0: id +0, deviceId +8 for ids
// 1/2/8/16/32/0x400000) and the eboot LoginMgr watch thread (0x1CF9B0: id +0, reason +4,
// deviceId +8, type +16, subType +20, userId +28).
enum EventId : int32_t {
  kEventDeviceAdded = 1,
  kEventDeviceRemoved = 2,
  kEventBusTransition = 8,
  kEventOwnerChanged = 16,
  kEventFunctionChanged = 32,
};
struct Event {
  int32_t id = 0;          // +0
  int32_t reason = 0;      // +4
  uint64_t device_id = 0;  // +8
  int32_t type = 0;        // +16 device type (22 DualSense)
  int32_t sub_type = 0;    // +20
  int32_t pad0 = 0;        // +24
  int32_t user_id = -1;    // +28
};
// sceMbusEventCreate-like: a new event handle (each handle gets its own copy of every event).
int32_t create_event_handle();
// Blocks until an event is queued for `handle`, then pops it; returns 1, or 0 when
// `block` is false and nothing is queued.
int32_t receive_event(int32_t handle, Event* out, bool block);

// The emulated DualSense was turned on (platform/input power listener): SceSysCore's
// addDevice registers it and the server queues SCE_MBUS_EVENT_DEVICE_ADDED. Also called
// once at start when the pad is already on (--pad-on-at-boot).
void attach_host_pad();

}  // namespace ssn::mbus
