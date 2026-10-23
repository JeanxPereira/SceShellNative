// libSceIme (FW 4.03 /system/common/lib/libSceIme.sprx; same code in the retail and devkit
// dumps, class A2 of work/ida_r7diff/report.md): the VSH client API libScePsmUtil uses for an
// inline OSK. Addresses are offsets in that module. The IPMI transport to "SceImeService" is
// replaced by direct calls into the host's server (psmhost/eboot/ime_server.h).
#pragma once
#include <cstdint>

namespace ssn::ime_client {

// SceImeEvent (72 bytes) as passed to the handler: {u32 id; pad; u64 @+8; u32 @+16 / +20 / +24 /
// +28; u64 @+32; zeros}. For 0x200 / 0x201 / 0x202 / 0x210 the u64 at +8 points at the text.
struct Event {
  uint32_t id;
  uint32_t pad;
  uint64_t p8;
  uint32_t u16, u20, u24, u28;
  uint64_t u32;
  uint8_t rest[32];
};
static_assert(sizeof(Event) == 72);

using Handler = void (*)(void* arg, const Event* ev);

// The 248-byte VSH open parameter (sceImeVshOpen, filled by libScePsmUtil 0x5980).
struct VshParam {
  uint8_t b[248];
};

// sceImeVshOpen (0x9110, NID LBicRa_hj3A).
int32_t vsh_open(void** handle, const VshParam* p, const char16_t* init_text);
// sceImeVshUpdate (0xaab0, NID WqAayyok5p0).
int32_t vsh_update(void* handle);
// sceImeVshClose (0xe300, NID gtoTsGM9vEY).
int32_t vsh_close(void* handle);
// sceImeVshUpdateContext (0xda20, NID O7Fdd_Oc_qQ): deferred to the next update (method 10).
int32_t vsh_update_context(void* handle, const char16_t* text, uint32_t len, int32_t caret);
// sceImeVshClearPreedit (0xd0e0) / sceImeVshConfirmPreedit (0xd450): method 5 now.
int32_t vsh_clear_preedit(void* handle);
// sceImeVshSetCaretIndexInPreedit (0xe270): deferred (method 5 flag 4).
int32_t vsh_set_caret_index_in_preedit(void* handle, int32_t idx);
// sceImeVshInformConfirmdString (0xe6c0, method 40) / 2 (0xe760, method 43) /
// sceImeVshNotifyCurrentTextInfo (0xe820, method 49) / sceImeVshSetSelectionText (0xe9f0, method 9).
int32_t vsh_inform_text(void* handle, uint32_t method, const char16_t* text, uint32_t len, const int32_t* args, int nargs);
// sceImeVshSetCaretGeometry (0xd620, m6) / SetPreeditGeometry (0xd7e0, m7) / SetSelectGeometry
// (0xe8f0, m8).
int32_t vsh_set_geometry(void* handle, uint32_t method, const void* geom16);
// sceImeVshSetPanelPosition (0xed80, method 17).
int32_t vsh_set_panel_position(void* handle, uint32_t ha, uint32_t va, float x, float y);
// sceImeVshGetPanelPositionAndForm (0xeaf0, method 16): {form, x, y, hA, vA, w, h}.
int32_t vsh_get_panel_position_and_form(void* handle, uint32_t out[7]);

}  // namespace ssn::ime_client
