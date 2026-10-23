// libSceUsbStorage (FW 4.03) as ShellUI's "libSceAutoMounterWrapper" P/Invokes reach it.
//
// The eboot's {name, fn} table entry for "getDeviceList3" (eboot.bin 0xcfd400) jumps to the
// libSceUsbStorage export OhsV6P0FD5A (0x37c0, sceAutoMounterIpcClient::getUsbDeviceList3):
//   - client not created (qword_1C040 == 0)        -> 0x80F40009
//   - null id list or null result-count pointer     -> 0x80F40002
//   - otherwise IPMI method 0x30022 (0x8860) with the list buffer (16 ids) and the count
//     as out-data; the call's result is the server's result.
// SceShellCore serves it: sceAutoMounterIpc::getUsbDeviceList3 (0x125D250) checks the
// buffer size (16 ids, else 0x80F40002) and calls AutoMounter::getUsbDeviceList (0x2A6710)
// -> AutoMount::UsbDeviceManager::getDeviceList (0x2C0870): under the manager lock it walks
// the attached USB storage devices, keeps those that pass the filter (0..6) and writes
// their ids, then stores the number kept in *count and returns 0. The filter is only
// evaluated per device, so with no device attached any filter yields 0.
//
// The client is created by sceUsbStorageInit (BDDZwF5kuTc 0x9460 -> 0x1710), which the
// eboot's BootManager init thread calls ("InitializeUsbStorage", see
// eboot/boot_manager_init.cpp); the host treats it as created.
//
// HOST VALUE (the server side, i.e. the USB hardware): the PC exposes no USB storage
// device to the console model, so the UsbDeviceManager list is empty: count 0, result 0.
#include <cstdint>

#include "host/boundary.h"

namespace ssn::usb_storage {
namespace {

constexpr int32_t kErrInvalidArgument = static_cast<int32_t>(0x80F40002u);

void host_note() {
  static const bool noted = (host::note_unimplemented(
                                 "HOST VALUE AutoMounter UsbDeviceManager: no USB storage device attached (empty device list)"),
                             true);
  (void)noted;
}

}  // namespace

// int getUsbDeviceListNative2(uint[] usbDeviceIdList, out int usbDeviceIdNum, uint filter)
// (Sce.Vsh.AutoMounterWrapper) -> OhsV6P0FD5A (libSceUsbStorage 0x37c0).
SSN_PINVOKE("libSceAutoMounterWrapper", "getDeviceList3", int32_t, ssn_AutoMounterWrapper_getDeviceList3,
            (uint32_t* id_list, int32_t* count, uint32_t filter)) {
  (void)filter;
  if (!id_list || !count) return kErrInvalidArgument;
  host_note();
  *count = 0;  // 0x2C0870 LABEL_52: *p_resultSize = number of devices kept
  return 0;
}

// int getUsbDeviceListNative(uint[] deviceIdList, out int deviceIdNum) (Sce.Vsh.AutoMounterWrapper)
// -> "sceUsbStorageGetDeviceList" = mryrNITeYvI (libSceUsbStorage 0xb350) ->
// sceAutoMounterIpcClient::getUsbDeviceList2 (0x2da0): client not created -> 0x80F40009, null
// list or count -> 0x80F40002, otherwise IPMI with an 8-id buffer (0x81e0). SceShellCore
// sceAutoMounterIpc::getUsbDeviceList2 (0x125CC70: bufSize 8, else 0x80F40002) ->
// AutoMount::getUsbDeviceList2 (0x2A4680) -> UsbDeviceManager::getUsbDeviceList2 (0x2C26C0),
// which stores the number of attached USB storage devices in *count: 0 on the host.
SSN_PINVOKE("libSceAutoMounterWrapper", "sceUsbStorageGetDeviceList", int32_t, ssn_AutoMounterWrapper_GetDeviceList,
            (uint32_t* id_list, int32_t* count)) {
  if (!id_list || !count) return kErrInvalidArgument;
  host_note();
  *count = 0;
  return 0;
}

// int sceAutoMounterM2GetInternalStatus(out bool exists, out M2AvailabilityInternal availability)
// (Sce.Vsh.AutoMounterWrapper) — eboot {name, fn} entry 0xcfd3c0 (devkit build 0xd013e0) -> the
// libSceUsbStorage import gSGasgPNyk (0x44D0): client not created -> 0x80F40009; otherwise IPMI
// method 0x30035 (0x9170) with exists (1 byte) and availability (4 bytes) as out-data.
// SceShellCore (devkit build addresses in parentheses): sceAutoMounterIpc::m2GetInternalStatus
// 0x125E540 (0x126A7C0; null outputs -> 0x80F40002) -> sceAutoMounterM2GetInternalStatus 0x2A79C0
// (0x2B3000): availability = the M2HandlerStatus holder (0x2B3040; 1 at init 0x2B38E0, then what
// the boot mount 0x2AAD10 (0x2B6350) stores: its result mapped by 0x2B2DA0 (0x2BF590)), exists =
// _m2Exists 0x2AE120 (0x2B8C80): sceFsM2StorageGetDeviceInfo, ENODEV -> false, result 0.
// With the slot empty (see README, M.2 slot) the boot mount ends in "m.2 device is not found in
// /dev/" (0x80F40007), which 0x2B2DA0 maps to 0 (NOT_CONNECTED), and the device query answers
// ENODEV: exists false, availability 0, result 0.
SSN_PINVOKE("libSceAutoMounterWrapper", "sceAutoMounterM2GetInternalStatus", int32_t,
            ssn_AutoMounterWrapper_M2GetInternalStatus, (uint8_t* exists, int32_t* availability)) {
  if (!exists || !availability) return kErrInvalidArgument;
  *availability = 0;  // AUTOMOUNTER_M2_AVAILABILITY_INTERNAL_NOT_CONNECTED
  *exists = 0;
  return 0;
}

}  // namespace ssn::usb_storage
