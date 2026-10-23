// The system IME server that ShellUI hosts (eboot.bin, FW 4.03 devkit build J01736824;
// vsh/shell/shell_ui/src/ime_server, src/ime, src/ime/framework). Addresses are devkit-build
// addresses; the retail build (J01736823) has the same code 0xF40 lower in this area
// (e.g. IME create 0x145850 devkit = 0x144910 retail) — see work/ida_r7diff/report.md.
//
// On the console the IME clients (libSceIme in any process, here ShellUI itself through
// libScePsmUtil) reach this server over IPMI ("SceImeService", server object 0x1A0940). The host
// has no IPMI: the host libSceIme (psmhost/libSceIme) calls the service's connect / sync-method /
// async-method handlers below directly, with the same buffers the IPMI messages carry.
#pragma once
#include <cstddef>
#include <cstdint>
#include <string>

namespace ssn::eboot::ime {

// One IPMI buffer descriptor {ptr, size}.
struct Buf {
  void* p;
  size_t n;
};

// BackgroundInit(InitIme), 0xD210 case 7.
void initialize();
bool initialized();  // byte_EC9538

// Connection request (service class 0x1991B0). `info` is the 24-byte connect info libSceIme sends
// (sub_A330: +0 u32 1, +4 u64 sdkInfo, +12 u8, +16 u32 callerSceneId, +20 u32 userId).
// Returns the IPMI status (0 or the server's error) and the client id.
int32_t connect(const void* info, uint32_t* client);
void disconnect(uint32_t client);

// Synchronous method (0x199820 onSyncMethodDispatch). `ret` is the method result.
int32_t sync_method(uint32_t client, uint32_t method, const Buf* in, int nin, int32_t* ret, Buf* out, int nout);

// Asynchronous method 4 (the update/poll: 0x19C260 -> case 4 of 0x199820). The result the client
// later collects with tryGetResult: `count` records of 312 bytes in out[0], bulk text in out[1].
int32_t async_update(uint32_t client, const void* param456, size_t size, int32_t* count, Buf* out, int nout);

// ImeNativeCall.OnImeEventNative(int eventId, string json) — 0x144CF0.
int32_t on_ime_event_native(int32_t id, const std::string& json_utf8);

// Physical keyboard path (see ime_keyboard.cpp).
void post_keyboard_record(const void* rec320);

// The SceImeUpdate wake-up: libScePsm's vblank callback (qword_5273D0), set by XmlFvKZlm7k.
void on_vblank();

// --- ImeProxy / ImeDialogProxy icall support (ime_proxy.cpp) ---
void set_ui_event_delegates(void* large_text_check, void* ui_event);  // SetImeCallbackTableList
void on_change_ui_status(int32_t status);                            // OnChangeUIStatus 0x145D80
void on_change_unit(int32_t lang, int32_t type, int32_t method, int32_t plugin, int32_t device);
void update_position_and_form(const void* form28);                   // UpdatePositionAndForm
void set_init_input_mode(int32_t mode);
void set_has_focused_candidate(bool b);
void set_shell_ui_app_id(int32_t id);
void set_front_scene(int32_t app, int32_t scene);
void set_game_scene_shown(bool shown, int32_t app);
int32_t send_data_status();
int32_t confirm_text_length();
void notify_key_event(int32_t device);  // ImeNativeCall.NotifyKeyEventNative 0x1454C0

}  // namespace ssn::eboot::ime
