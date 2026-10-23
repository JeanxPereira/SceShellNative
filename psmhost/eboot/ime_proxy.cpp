// Sce.Vsh.ShellUI.Ime.ImeProxy / ImeDialog.ImeDialogProxy internal calls and the ImeNativeCall
// P/Invokes (eboot.bin, FW 4.03). Addresses: devkit build J01736824 (retail J01736823 is
// 0xF40 lower for the ImeProxy block, e.g. OnFrontSceneAppId 0x146560 devkit = 0x145620 retail).
//
// The eboot's IME is created by BackgroundInit(InitIme) (0xD210 case 7; ime_server.cpp), which
// sets byte_EC9538. Every ImeProxy entry tests that flag first and does nothing but return 0 while
// it is clear; the host reproduces the initialised branch of each entry the OSK session uses.
#include <cstdint>
#include <string>

#include <windows.h>

#include <mono/metadata/object.h>

#include "eboot/ime_server.h"
#include "host/boundary.h"

namespace ssn::eboot {
namespace {

void note_once(const char* what) { host::note_unimplemented(std::string("eboot ImeProxy ") + what + " not reproduced"); }

// OnFrontSceneAppId(int, int) — 0x146560: under the app-id mutex +9312 / +9316, signal +9328.
int32_t OnFrontSceneAppId(int32_t app, int32_t scene) {
  ime::set_front_scene(app, scene);
  return 0;
}
SSN_ICALL("Sce.Vsh.ShellUI.Ime.ImeProxy::OnFrontSceneAppId", OnFrontSceneAppId);

// OnGameSceneShown(bool, int) — 0x146530: shown -> +9308 = appId.
int32_t OnGameSceneShown(uint8_t shown, int32_t app) {
  ime::set_game_scene_shown(shown != 0, app);
  return 0;
}
SSN_ICALL("Sce.Vsh.ShellUI.Ime.ImeProxy::OnGameSceneShown", OnGameSceneShown);

// OnGetSceneChangeMessage(ref SuspendInfo) — 0x146490: hands the info to the IME (0x112740) and
// runs 0x1A7480 / 0x195D60 (suspend / resume across scene changes).
int32_t OnGetSceneChangeMessage(void*) {
  if (ime::initialized()) note_once("OnGetSceneChangeMessage (0x146490 suspend info)");
  return 0;
}
SSN_ICALL("Sce.Vsh.ShellUI.Ime.ImeProxy::OnGetSceneChangeMessage", OnGetSceneChangeMessage);

// OnGetAbortMessage() — 0x1464F0: aborts the session (0x1126A0).
int32_t OnGetAbortMessage() {
  if (ime::initialized()) note_once("OnGetAbortMessage (0x1464F0)");
  return 0;
}
SSN_ICALL("Sce.Vsh.ShellUI.Ime.ImeProxy::OnGetAbortMessage", OnGetAbortMessage);

// OnShellUIAppId(int) — 0x1465F0: +9320 = appId; 0x1A2C90 (keyboard monitor focus).
int32_t OnShellUIAppId(int32_t id) {
  ime::set_shell_ui_app_id(id);
  return 0;
}
SSN_ICALL("Sce.Vsh.ShellUI.Ime.ImeProxy::OnShellUIAppId", OnShellUIAppId);

// NotifyApplicationId(int) — 0x1462D0: 0x1A2C50 (keyboard monitor: release held keys).
int32_t NotifyApplicationId(int32_t) { return 0; }
SSN_ICALL("Sce.Vsh.ShellUI.Ime.ImeProxy::NotifyApplicationId", NotifyApplicationId);

// SetInitInputMode(int) — 0x146190: UI singleton +16.
int32_t SetInitInputMode(int32_t mode) {
  ime::set_init_input_mode(mode);
  return 0;
}
SSN_ICALL("Sce.Vsh.ShellUI.Ime.ImeProxy::SetInitInputMode", SetInitInputMode);

// SetImeCallbackTableList(ref ImeCallBackTable) — 0x146430: the delegates at +8 / +16
// (largeTextCheck, uiEvent) become function pointers at framework +15512 / +15528.
int32_t SetImeCallbackTableList(MonoObject** table) {
  if (ime::initialized() && table) ime::set_ui_event_delegates(table[1], table[2]);
  return 0;
}
SSN_ICALL("Sce.Vsh.ShellUI.Ime.ImeProxy::SetImeCallbackTableList", SetImeCallbackTableList);

// OnChangeUIStatus(int) — 0x145D80 -> 0x12A940 (UI singleton +48) -> 0x11B7C0.
int32_t OnChangeUIStatus(int32_t status) {
  ime::on_change_ui_status(status);
  return 0;
}
SSN_ICALL("Sce.Vsh.ShellUI.Ime.ImeProxy::OnChangeUIStatus", OnChangeUIStatus);

// OnChangeUnit(lang, type, method, plugin, device) — 0x145CF0 -> 0x112F90.
int32_t OnChangeUnit(int32_t lang, int32_t type, int32_t method, int32_t plugin, int32_t device) {
  ime::on_change_unit(lang, type, method, plugin, device);
  return 0;
}
SSN_ICALL("Sce.Vsh.ShellUI.Ime.ImeProxy::OnChangeUnit", OnChangeUnit);

// UpdatePositionAndForm(ref ImePositionAndForm) — 0x146310 -> 0x122880.
int32_t UpdatePositionAndForm(const void* form) {
  ime::update_position_and_form(form);
  return 0;
}
SSN_ICALL("Sce.Vsh.ShellUI.Ime.ImeProxy::UpdatePositionAndForm", UpdatePositionAndForm);

// OnHasFocusedCandidate(bool) — 0x145CC0: UI singleton +9.
int32_t OnHasFocusedCandidate(uint8_t b) {
  ime::set_has_focused_candidate(b != 0);
  return 0;
}
SSN_ICALL("Sce.Vsh.ShellUI.Ime.ImeProxy::OnHasFocusedCandidate", OnHasFocusedCandidate);

// GetCandidate(ref ImeCandidateInfo[], ref int) — 0x145A70: the UI singleton's candidate list
// (0x12C680 / 0x12BE80). The BasicLatin unit has no conversion engine: size 0, result 0.
int32_t GetCandidate(void*, int32_t* size) {
  if (size && ime::initialized()) *size = 0;
  return 0;
}
SSN_ICALL("Sce.Vsh.ShellUI.Ime.ImeProxy::GetCandidate", GetCandidate);

// OnChangeCandidatesVisible(bool) — 0x145C90 -> 0x12C0C0; OnSelectedCandidate / OnFocusedCandidate
// 0x145AD0 / 0x145BB0: the candidate list (empty with the BasicLatin unit).
int32_t OnChangeCandidatesVisible(uint8_t) { return 0; }
SSN_ICALL("Sce.Vsh.ShellUI.Ime.ImeProxy::OnChangeCandidatesVisible", OnChangeCandidatesVisible);
int32_t OnSelectedCandidate(int32_t, void*) {
  if (ime::initialized()) note_once("OnSelectedCandidate (0x145AD0, conversion candidates)");
  return 0;
}
SSN_ICALL("Sce.Vsh.ShellUI.Ime.ImeProxy::OnSelectedCandidate", OnSelectedCandidate);
int32_t OnFocusedCandidate(int32_t, void*) { return 0; }
SSN_ICALL("Sce.Vsh.ShellUI.Ime.ImeProxy::OnFocusedCandidate", OnFocusedCandidate);

// OnDecide(ref ImeButtonEvent) — 0x145A60: returns 0 (buttons go through ImeNativeCall.OnImeEvent).
int32_t OnDecide(void*) { return 0; }
SSN_ICALL("Sce.Vsh.ShellUI.Ime.ImeProxy::OnDecide", OnDecide);

// GetSendDataStatus() — 0x1462F0: framework +136. GetConfirmTextLength() — 0x146260: UI +60.
int32_t GetSendDataStatus() { return ime::send_data_status(); }
SSN_ICALL("Sce.Vsh.ShellUI.Ime.ImeProxy::GetSendDataStatus", GetSendDataStatus);
int32_t GetConfirmTextLength() { return ime::confirm_text_length(); }
SSN_ICALL("Sce.Vsh.ShellUI.Ime.ImeProxy::GetConfirmTextLength", GetConfirmTextLength);

// Getdbgconf(int[], double[]) — 0x146220: returns 0.
int32_t Getdbgconf(void*, void*) { return 0; }
SSN_ICALL("Sce.Vsh.ShellUI.Ime.ImeProxy::Getdbgconf", Getdbgconf);

// SetKeyboardRw(bool, int) — 0x146680 -> 0x1A6F30: debug keyboard random walk.
int32_t SetKeyboardRw(uint8_t, int32_t) {
  if (ime::initialized()) note_once("SetKeyboardRw (0x146680, debug keyboard random walk)");
  return 0;
}
SSN_ICALL("Sce.Vsh.ShellUI.Ime.ImeProxy::SetKeyboardRw", SetKeyboardRw);

// ImeDialogProxy.SetImeDialogCallbackTableList — 0x101020: no ImeDialog instance (qword_EC5020 is
// created by 0x100DD0, the ImeDialog server, not reproduced) -> -1.
int32_t SetImeDialogCallbackTableList(void*) {
  host::note_unimplemented("eboot ImeDialog server (BackgroundInit(InitIme) 0x100DD0) not reproduced: no ImeDialog instance");
  return -1;
}
SSN_ICALL("Sce.Vsh.ShellUI.ImeDialog.ImeDialogProxy::SetImeDialogCallbackTableList", SetImeDialogCallbackTableList);

}  // namespace

// ImeNativeCall.OnImeEventNative(int, string) (CharSet.Unicode) — {name, fn} 0xCD1110 -> 0x144CF0.
SSN_PINVOKE("ImeNativeCall", "OnImeEventNative", int32_t, ssn_ImeNativeCall_OnImeEventNative, (int32_t id, const wchar_t* data)) {
  if (!data) return static_cast<int32_t>(0x80BC00FFu);
  const int n = WideCharToMultiByte(CP_UTF8, 0, data, -1, nullptr, 0, nullptr, nullptr);
  std::string s(n > 0 ? n - 1 : 0, '\0');
  if (n > 1) WideCharToMultiByte(CP_UTF8, 0, data, -1, s.data(), n, nullptr, nullptr);
  return ime::on_ime_event_native(id, s);
}

// NotifyKeyEventNative(DeviceType) — 0x1454C0: client event 543 with the device type.
SSN_PINVOKE("ImeNativeCall", "NotifyKeyEventNative", int32_t, ssn_ImeNativeCall_NotifyKeyEventNative, (int32_t device)) {
  ime::notify_key_event(device);
  return 0;
}

// SetEnableExternalKeyboardForEmbeddedModeNative(bool) — 0x145490 -> 0x1184F0 (embedded OSK only).
SSN_PINVOKE("ImeNativeCall", "SetEnableExternalKeyboardForEmbeddedModeNative", int32_t,
            ssn_ImeNativeCall_SetEnableExternalKeyboardForEmbeddedModeNative, (uint8_t)) {
  host::note_unimplemented("eboot ImeNativeCall.SetEnableExternalKeyboardForEmbeddedModeNative (0x145490, embedded OSK) not reproduced");
  return 0;
}

// SetFrontSceneInfoNative(frontApp, frontScene, gameShown, gameApp, titleId) — 0x145780.
SSN_PINVOKE("ImeNativeCall", "SetFrontSceneInfoNative", int32_t, ssn_ImeNativeCall_SetFrontSceneInfoNative,
            (int32_t app, int32_t scene, uint8_t shown, int32_t game, const char*)) {
  ime::set_front_scene(app, scene);
  ime::set_game_scene_shown(shown != 0, game);
  return 0;
}

// int SetFontSceneTitleIdNative(string titleId) — 0x145680: null -> -1; otherwise stored at
// framework +15608 (font selection for the title), 0.
SSN_PINVOKE("ImeNativeCall", "SetFontSceneTitleIdNative", int32_t, ssn_ImeNativeCall_SetFontSceneTitleIdNative,
            (const char* title_id)) {
  if (!title_id) return -1;
  return 0;
}

}  // namespace ssn::eboot
