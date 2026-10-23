// Sce.Vsh.SysBridge.Pad P/Invokes ([DllImport("libSysBridgePad")], FW 4.03).
//
// libSceSysBridge.sprx publishes "libSysBridgePad" as a {name, fn} table (0x78a80 ->
// 0x78aa0) whose every entry is the libScePad import of the same name — e.g.
// scePadGetCapability -> qtasqbvwgV4, scePadDisableTransportButtons -> b+SbV3m6n6U.
// There is no code of the firmware's own between the shell and the pad driver, so these
// calls are the physical-device boundary: the host answers for its one DualSense.
#include <cstdint>

#include "host/boundary.h"
#include "libScePsm/input/pss_input.h"

namespace ssn::sysbridge {
namespace {

constexpr int32_t kOk = 0;
// SCE_PAD_ERROR_INVALID_ARG — the host's answer for a device id it does not own.
constexpr int32_t kScePadErrorInvalidArg = static_cast<int32_t>(0x80920001u);

bool is_host_device(uint32_t device_id) { return device_id == static_cast<uint32_t>(psm::input::kHostDeviceId); }

}  // namespace

// scePadGetCapability(uint deviceId, ref SceHidControllerCapability) — the DualSense
// capability set (SCE_HID_CONTROLLER_CAPABILITY_BOND).
SSN_PINVOKE("libSysBridgePad", "scePadGetCapability", int32_t, ssn_SysBridgePad_scePadGetCapability,
            (uint32_t device_id, uint32_t* capability)) {
  if (!capability || !is_host_device(device_id)) return kScePadErrorInvalidArg;
  *capability = psm::input::kHostCapability;
  return kOk;
}

// Settings of the physical pad (motor enable, light bar, trigger effects, power-off
// timer, tracking LED): the host device has none of that hardware; accepted.
SSN_PINVOKE("libSysBridgePad", "scePadDisableVibration", int32_t, ssn_SysBridgePad_scePadDisableVibration,
            (uint32_t device_id, uint8_t)) {
  return is_host_device(device_id) ? kOk : kScePadErrorInvalidArg;
}
SSN_PINVOKE("libSysBridgePad", "scePadSetLightBarBaseBrightness", int32_t, ssn_SysBridgePad_scePadSetLightBarBaseBrightness,
            (uint32_t device_id, int32_t)) {
  return is_host_device(device_id) ? kOk : kScePadErrorInvalidArg;
}
SSN_PINVOKE("libSysBridgePad", "scePadSetLightBarBlinking", int32_t, ssn_SysBridgePad_scePadSetLightBarBlinking,
            (uint32_t device_id, const uint8_t*)) {
  return is_host_device(device_id) ? kOk : kScePadErrorInvalidArg;
}
SSN_PINVOKE("libSysBridgePad", "scePadDisableTriggerEffect", int32_t, ssn_SysBridgePad_scePadDisableTriggerEffect,
            (uint32_t device_id, uint8_t)) {
  return is_host_device(device_id) ? kOk : kScePadErrorInvalidArg;
}
SSN_PINVOKE("libSysBridgePad", "scePadSetVibrationStrength", int32_t, ssn_SysBridgePad_scePadSetVibrationStrength,
            (uint32_t device_id, int32_t)) {
  return is_host_device(device_id) ? kOk : kScePadErrorInvalidArg;
}
SSN_PINVOKE("libSysBridgePad", "scePadSetTriggerEffectStrength", int32_t, ssn_SysBridgePad_scePadSetTriggerEffectStrength,
            (uint32_t device_id, int32_t)) {
  return is_host_device(device_id) ? kOk : kScePadErrorInvalidArg;
}
SSN_PINVOKE("libSysBridgePad", "scePadSetAutoPowerOffCount", int32_t, ssn_SysBridgePad_scePadSetAutoPowerOffCount,
            (uint32_t)) {
  return kOk;
}
SSN_PINVOKE("libSysBridgePad", "scePadDisableTrackingLedBatterySaver", int32_t,
            ssn_SysBridgePad_scePadDisableTrackingLedBatterySaver, (uint8_t)) {
  return kOk;
}

// scePadDisableTransportButtons(bool) — driver switch for the media transport buttons of
// remote controls; the host DualSense has none, so nothing changes.
SSN_PINVOKE("libSysBridgePad", "scePadDisableTransportButtons", int32_t, ssn_SysBridgePad_scePadDisableTransportButtons,
            (uint8_t)) {
  return kOk;
}

// Driver-side input transformations that the host's libScePad stand-in does not perform.
SSN_PINVOKE("libSysBridgePad", "scePadSetButtonRemappingInfo", int32_t, ssn_SysBridgePad_scePadSetButtonRemappingInfo,
            (uint32_t device_id, const uint8_t* info)) {
  if (!is_host_device(device_id)) return kScePadErrorInvalidArg;
  if (info) host::note_unimplemented("libScePad scePadSetButtonRemappingInfo: custom button assignment not applied by the host pad");
  return kOk;
}
SSN_PINVOKE("libSysBridgePad", "scePadEnablePlayPauseEmulation", int32_t, ssn_SysBridgePad_scePadEnablePlayPauseEmulation,
            (uint8_t enable)) {
  if (enable) host::note_unimplemented("libScePad scePadEnablePlayPauseEmulation: not performed by the host pad");
  return kOk;
}

}  // namespace ssn::sysbridge
