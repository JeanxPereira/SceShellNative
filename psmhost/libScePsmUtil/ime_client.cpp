// Sce.PlayStation.Core.Ime.ImeClient internal calls (libScePsmUtil.sprx FW 4.03; same code in the
// retail and devkit dumps). The inline window type: StartNative 0x2290 -> 0x2DE0 -> LibImeStart
// 0x5980 (sceImeVshOpen), events through the handler 0x5850 into the list qword_397B8 and
// LibImeGetEvent2 0x6AA0 (sceImeVshUpdate), StopNative -> LibImeStop 0x62B0 (sceImeVshClose).
// The dialog window type (0x4580 ImeDialogStart, libSceImeDialog) is not reproduced.
#include <algorithm>
#include <cstdint>
#include <cstring>
#include <deque>
#include <mutex>
#include <string>

#include <mono/metadata/appdomain.h>
#include <mono/metadata/object.h>

#include "common/mono_util.h"
#include "core/log.h"
#include "host/boundary.h"
#include "libSceIme/ime_vsh_client.h"

namespace ssn::psmutil {
namespace {

namespace ic = ssn::ime_client;

template <class T>
T rd(const uint8_t* b, size_t off) {
  T v;
  std::memcpy(&v, b + off, sizeof v);
  return v;
}
template <class T>
void wr(uint8_t* b, size_t off, T v) {
  std::memcpy(b + off, &v, sizeof v);
}

std::u16string chars(MonoString* s) {
  if (!s) return {};
  return std::u16string(reinterpret_cast<const char16_t*>(mono_string_chars(s)), mono_string_length(s));
}

// The accessor block (216 bytes): +0 points at the 4-byte kind (0 inline, 1 dialog), +8 a copy
// of the 208-byte ImeParamInternal.
struct Accessor {
  uint32_t* kind;
  uint8_t param[208];
};

struct Node {  // 112-byte node of qword_397B8: the 72-byte event and its string
  ic::Event ev;
  std::u16string text;
};

std::recursive_mutex g_mutex;
std::deque<Node> g_queue;          // qword_397B8 / qword_397C0
void* g_handle = nullptr;          // qword_397D0
void* g_work = nullptr;            // qword_397D8 (20480 bytes)
std::u16string g_confirm;          // word_397F0
std::u16string g_preedit;          // word_3A800

void handler(void*, const ic::Event* ev) {  // 0x5850
  Node n{*ev, {}};
  if (ev->id == 0x200 || ev->id == 0x201 || ev->id == 0x202 || ev->id == 0x210) {
    const char16_t* s = reinterpret_cast<const char16_t*>(ev->p8);
    if (s) n.text = s;
  }
  g_queue.push_back(std::move(n));
}

// LibImeStart 0x5980: the 248-byte VSH parameter from ImeParamInternal.
int32_t lib_ime_start(const uint8_t* p, MonoString* init_text) {
  ic::VshParam v{};
  uint8_t* b = v.b;
  wr<uint64_t>(b, 0x00, 248);
  wr<uint64_t>(b, 0x08, rd<uint64_t>(p, 0));
  wr<uint32_t>(b, 0x10, p[8]);
  wr<uint32_t>(b, 0x14, rd<uint32_t>(p, 12));
  uint32_t o = rd<uint32_t>(p, 16);
  const uint32_t device = rd<uint32_t>(p, 84);
  if (device == 5) o |= 0x10;
  if (rd<uint32_t>(p, 16) >= 0x40000000) o = (o & 0x3FFFFFFF) | (p[80] ? 0x40000000u : 0x80000000u);
  wr<uint32_t>(b, 0x18, o | 0x100);
  wr<uint64_t>(b, 0x20, 0);
  if (g_work) ::operator delete(g_work);
  g_work = ::operator new(20480);
  std::memset(g_work, 0, 20480);
  wr<void*>(b, 0x28, g_work);
  wr<void*>(b, 0x30, reinterpret_cast<void*>(&handler));
  if (p[81] && rd<void*>(p, 120)) log::warn("ime", "ImeClient text filter delegate (0x6010) not reproduced");
  b[0x40] = p[20];
  std::memcpy(b + 0x44, p + 24, 16);  // DialogPositionInfo X, Y, HAlign, VAlign
  std::memcpy(b + 0x54, p + 136, 16);  // TextFieldPositionInfo
  wr<int64_t>(b, 0x68, rd<int64_t>(p, 152));
  wr<uint32_t>(b, 0x70, rd<uint32_t>(p, 160));
  wr<int32_t>(b, 0x78, rd<int32_t>(p, 40));
  wr<uint32_t>(b, 0x7C, rd<uint16_t>(p, 46));
  wr<uint64_t>(b, 0x80, rd<uint64_t>(p, 72));
  uint32_t ext = (p[92] ? 0x100u : 0u) | 0x1000u;
  if (MonoString* dic = rd<MonoString*>(p, 168); dic && mono_string_length(dic) > 0) {
    ext |= 0x4080;
    log::warn("ime", "ImeClient additional dictionary path not reproduced");
  }
  wr<uint32_t>(b, 0x88, ext);
  // InputManager_GetDeviceIndexBehavior (gW4wqd1GV8E): the ShellUI sets MergeAllConnectedDevice
  // (docs/input.md); mode / value are taken as that behaviour with no specific user.
  wr<uint32_t>(b, 0xA0, 0);
  wr<uint64_t>(b, 0xA8, 0);
  wr<uint32_t>(b, 0xB8, rd<uint32_t>(p, 112));
  const uint32_t dir = rd<uint32_t>(p, 104);
  if (dir - 1 <= 1) b[0xC0] = static_cast<uint8_t>(dir);
  b[0xC1] = p[108];
  wr<uint32_t>(b, 0xC4, rd<uint32_t>(p, 116));
  wr<int32_t>(b, 0xC8, rd<int32_t>(p, 96));
  wr<uint32_t>(b, 0xCC, rd<uint32_t>(p, 176));
  wr<uint32_t>(b, 0xD0, device ? device : 9);
  static std::string app_info, test_id;
  app_info = psm::utf8(rd<MonoString*>(p, 192));
  test_id = psm::utf8(rd<MonoString*>(p, 200));
  wr<const char*>(b, 0xE0, rd<MonoString*>(p, 192) ? app_info.c_str() : nullptr);
  wr<const char*>(b, 0xE8, rd<MonoString*>(p, 200) ? test_id.c_str() : nullptr);
  b[0xF0] = 1;
  g_confirm.clear();
  g_preedit.clear();
  g_queue.clear();
  const std::u16string init = chars(init_text);
  return ic::vsh_open(&g_handle, &v, init.c_str());
}

// StartNative(ref ImeParamInternal, ref ulong) — 0x2290.
int32_t StartNative(uint8_t* param, uint64_t* accessor) {
  std::lock_guard<std::recursive_mutex> lk(g_mutex);
  auto* a = new Accessor();
  if (rd<uint32_t>(param, 88) == 1) param[18] |= 0x80;  // EventProtocol WebView: Option |= 0x800000
  std::memcpy(a->param, param, 208);
  a->kind = new uint32_t(0);
  int32_t r;
  const uint8_t type = a->param[44];
  if (type == 1) {  // Inline
    *a->kind = 0;
    r = lib_ime_start(a->param, rd<MonoString*>(a->param, 56));
  } else {
    *a->kind = 1;
    host::note_unimplemented("libScePsmUtil ImeClient dialog window type (0x4580 ImeDialogStart / sceImeDialogInitInternalWithSubInfo) not reproduced");
    r = -1;
  }
  if (r != 0) {
    log::warn("ime", "ImeClient StartNative: 0x{:08x}", static_cast<uint32_t>(r));
    delete a->kind;
    delete a;
    return r;
  }
  *accessor = reinterpret_cast<uint64_t>(a);
  return 0;
}
SSN_ICALL("Sce.PlayStation.Core.Ime.ImeClient::StartNative", StartNative);

// StopNative(ulong) — 0x2380 -> 0x2E50 -> LibImeStop 0x62B0.
int32_t StopNative(uint64_t accessor) {
  std::lock_guard<std::recursive_mutex> lk(g_mutex);
  auto* a = reinterpret_cast<Accessor*>(accessor);
  if (!a) return -1;
  int32_t r = -1;
  if (*a->kind == 0) {
    g_queue.clear();
    r = ic::vsh_close(g_handle);
    if (g_work) {
      ::operator delete(g_work);
      g_work = nullptr;
    }
  }
  delete a->kind;
  delete a;
  return r;
}
SSN_ICALL("Sce.PlayStation.Core.Ime.ImeClient::StopNative", StopNative);

// LibImeGetEvent2 0x6AA0: one queued event -> ImeEventNative (96 bytes). 1 = an event, 0 = none,
// -2 = dropped (the caller loops).
int32_t get_event2(uint8_t* out) {
  if (g_queue.empty()) {
    ic::vsh_update(g_handle);
    if (g_queue.empty()) return 0;
  }
  std::memset(out, 0, 96);
  Node n = g_queue.front();
  g_queue.pop_front();
  const ic::Event& e = n.ev;
  auto rect = [&] {
    float x, y;
    std::memcpy(&x, reinterpret_cast<const uint8_t*>(&e) + 8, 4);
    std::memcpy(&y, reinterpret_cast<const uint8_t*>(&e) + 12, 4);
    wr<int16_t>(out, 8, static_cast<int16_t>(static_cast<int32_t>(x)));
    wr<int16_t>(out, 10, static_cast<int16_t>(static_cast<int32_t>(y)));
    wr<uint16_t>(out, 12, static_cast<uint16_t>(e.u16));
    wr<uint16_t>(out, 14, static_cast<uint16_t>(e.u20));
  };
  auto set = [&](uint32_t id) { wr<uint32_t>(out, 0, id); };
  switch (e.id) {
    case 0: {  // Open: the panel from sceImeVshGetPanelPositionAndForm, else the event rect
      set(0);
      uint32_t f[7];
      if (ic::vsh_get_panel_position_and_form(g_handle, f) == 0) {
        float x, y;
        std::memcpy(&x, &f[1], 4);
        std::memcpy(&y, &f[2], 4);
        wr<int16_t>(out, 8, static_cast<int16_t>(static_cast<int32_t>(x)));
        wr<int16_t>(out, 10, static_cast<int16_t>(static_cast<int32_t>(y)));
        wr<uint16_t>(out, 12, static_cast<uint16_t>(f[5]));
        wr<uint16_t>(out, 14, static_cast<uint16_t>(f[6]));
        wr<uint32_t>(out, 84, f[0]);
      } else {
        rect();
        wr<uint32_t>(out, 84, 1);
      }
      return 1;
    }
    case 3: set(1); rect(); return 1;
    case 4: set(2); return 1;
    case 5: set(3); return 1;
    case 6: set(23); return 1;
    case 14: set(37); wr<uint32_t>(out, 76, static_cast<uint32_t>(e.p8)); return 1;
    case 15: set(40); return 1;
    case 16: set(41); return 1;
    case 17: set(42); wr<uint32_t>(out, 76, static_cast<uint32_t>(e.p8)); wr<uint32_t>(out, 84, static_cast<uint32_t>(e.p8)); return 1;
    case 512:
      set(4);
      wr<uint32_t>(out, 4, e.u16);
      g_preedit = n.text.substr(0, 2048);
      return 1;
    case 513: set(5); g_confirm = n.text.substr(0, 2048); return 1;
    case 514: set(6); g_confirm = n.text.substr(0, 2048); return 1;
    case 528: set(7); g_confirm = n.text.substr(0, 2048); return 1;
    case 515: set(9); return 1;
    case 516: set(10); return 1;
    case 517: set(11); return 1;
    case 519: set(17); return 1;
    case 520: set(18); wr<uint32_t>(out, 76, static_cast<uint32_t>(e.p8)); return 1;
    case 521: set(19); return 1;
    case 522: set(20); rect(); return 1;
    case 523: set(8); return 1;
    case 524: set(16); return 1;
    case 525: set(22); out[72] = static_cast<uint8_t>(e.p8); return 1;
    case 531: set(43); return 1;
    case 539: set(44); out[80] = static_cast<uint8_t>(e.p8); return 1;
    case 542: set(45); return 1;
    case 543: set(46); wr<uint32_t>(out, 88, static_cast<uint32_t>(e.p8)); return 1;
    case 768: set(12); return 1;
    case 769: set(13); return 1;
    case 770: set(14); return 1;
    case 771: set(15); return 1;
    case 772: set(24); return 1;
    case 773: set(25); return 1;
    case 774: set(28); return 1;
    case 775: set(29); return 1;
    case 776: set(26); return 1;
    case 777: set(27); return 1;
    case 1286: case 1538: set(21); return 1;
    default:
      if (e.id >= 0x100 && e.id <= 0x106)
        host::note_unimplemented("libScePsmUtil ImeClient keyboard events 0x100..0x106 (KeyboardOpen / Keycode*) not reproduced");
      return -2;
  }
}

// GetEventNative2(ulong, ref ImeEventNative) — 0x23E0 -> 0x2ED0.
int32_t GetEventNative2(uint64_t accessor, uint8_t* out) {
  std::lock_guard<std::recursive_mutex> lk(g_mutex);
  auto* a = reinterpret_cast<Accessor*>(accessor);
  if (!a || !out) return -1;
  if (*a->kind != 0) return -1;
  int32_t r;
  do r = get_event2(out);
  while (r == -2);
  return r < 0 ? -1 : r;
}
SSN_ICALL("Sce.PlayStation.Core.Ime.ImeClient::GetEventNative2", GetEventNative2);

// GetShowWindowType(ulong, ref uint) — 0x2410: 2 - (kind == 0).
int32_t GetShowWindowType(uint64_t accessor, uint32_t* type) {
  auto* a = reinterpret_cast<Accessor*>(accessor);
  if (!a || !type) return -1;
  *type = 2 - (*a->kind == 0);
  return 0;
}
SSN_ICALL("Sce.PlayStation.Core.Ime.ImeClient::GetShowWindowType", GetShowWindowType);

// GetLastConfirmStringNative / GetLastPreeditStringNative (0x2560 / 0x25C0).
int32_t GetLastConfirmStringNative(uint64_t accessor, MonoString** out) {
  std::lock_guard<std::recursive_mutex> lk(g_mutex);
  if (!accessor) return -1;
  psm::store_ref(out, mono_string_new_utf16(mono_domain_get(), reinterpret_cast<const mono_unichar2*>(g_confirm.data()),
                                            static_cast<int32_t>(g_confirm.size())));
  return static_cast<int32_t>(g_confirm.size());
}
SSN_ICALL("Sce.PlayStation.Core.Ime.ImeClient::GetLastConfirmStringNative", GetLastConfirmStringNative);
int32_t GetLastPreeditStringNative(uint64_t accessor, MonoString** out) {
  std::lock_guard<std::recursive_mutex> lk(g_mutex);
  if (!accessor) return -1;
  psm::store_ref(out, mono_string_new_utf16(mono_domain_get(), reinterpret_cast<const mono_unichar2*>(g_preedit.data()),
                                            static_cast<int32_t>(g_preedit.size())));
  return static_cast<int32_t>(g_preedit.size());
}
SSN_ICALL("Sce.PlayStation.Core.Ime.ImeClient::GetLastPreeditStringNative", GetLastPreeditStringNative);

bool inline_kind(uint64_t accessor) {
  auto* a = reinterpret_cast<Accessor*>(accessor);
  return a && *a->kind == 0;
}

// ClearPreeditNative — 0x2430 -> sceImeVshClearPreedit. SetCaretIndexInPreeditNative — 0x2620.
int32_t ClearPreeditNative(uint64_t accessor) {
  std::lock_guard<std::recursive_mutex> lk(g_mutex);
  return inline_kind(accessor) ? ic::vsh_clear_preedit(g_handle) : -1;
}
SSN_ICALL("Sce.PlayStation.Core.Ime.ImeClient::ClearPreeditNative", ClearPreeditNative);
int32_t SetCaretIndexInPreeditNative(uint64_t accessor, int32_t idx) {
  std::lock_guard<std::recursive_mutex> lk(g_mutex);
  return inline_kind(accessor) ? ic::vsh_set_caret_index_in_preedit(g_handle, idx) : -1;
}
SSN_ICALL("Sce.PlayStation.Core.Ime.ImeClient::SetCaretIndexInPreeditNative", SetCaretIndexInPreeditNative);

// UpdateContextNative(s, idx) — 0x2A20; UpdateContextNative2 (direction dropped) — 0x2A90.
int32_t UpdateContextNative(uint64_t accessor, MonoString* s, int32_t idx) {
  std::lock_guard<std::recursive_mutex> lk(g_mutex);
  if (!inline_kind(accessor)) return -1;
  std::u16string t = chars(s).substr(0, 2048);
  const int32_t caret = std::min<int32_t>(idx, static_cast<int32_t>(t.size()));
  return ic::vsh_update_context(g_handle, t.c_str(), static_cast<uint32_t>(t.size()), caret);
}
SSN_ICALL("Sce.PlayStation.Core.Ime.ImeClient::UpdateContextNative", UpdateContextNative);
int32_t UpdateContextNative2(uint64_t accessor, MonoString* s, int32_t idx, uint32_t) { return UpdateContextNative(accessor, s, idx); }
SSN_ICALL("Sce.PlayStation.Core.Ime.ImeClient::UpdateContextNative2", UpdateContextNative2);

// SetCurrentTextInfoNative(s, idx) — 0x26E0 (method 40) / ...2 0x2910 (method 43) /
// NotifyCurrentTextInfoNative 0x27C0 (method 49) / SetSelectTextInfoNative 0x2640 (method 9).
int32_t SetCurrentTextInfoNative(uint64_t accessor, MonoString* s, int32_t idx) {
  std::lock_guard<std::recursive_mutex> lk(g_mutex);
  if (!inline_kind(accessor)) return -1;
  std::u16string t = chars(s).substr(0, 2048);
  int32_t a[1] = {std::min<int32_t>(static_cast<int32_t>(t.size()), idx)};
  return ic::vsh_inform_text(g_handle, 40, t.c_str(), static_cast<uint32_t>(t.size()), a, 1);
}
SSN_ICALL("Sce.PlayStation.Core.Ime.ImeClient::SetCurrentTextInfoNative", SetCurrentTextInfoNative);
int32_t SetCurrentTextInfoNative2(uint64_t accessor, MonoString* s, int32_t caret, int32_t pidx, int32_t plen) {
  std::lock_guard<std::recursive_mutex> lk(g_mutex);
  if (!inline_kind(accessor)) return -1;
  std::u16string t = chars(s).substr(0, 2048);
  caret = std::min<int32_t>(static_cast<int32_t>(t.size()), caret);
  if (caret < pidx || caret > pidx + plen) pidx = -1, plen = 0;
  int32_t a[3] = {caret, pidx, plen};
  return ic::vsh_inform_text(g_handle, 43, t.c_str(), static_cast<uint32_t>(t.size()), a, 3);
}
SSN_ICALL("Sce.PlayStation.Core.Ime.ImeClient::SetCurrentTextInfoNative2", SetCurrentTextInfoNative2);
int32_t NotifyCurrentTextInfoNative(uint64_t accessor, MonoString* s, int32_t caret, int32_t pidx, int32_t plen, int32_t sidx,
                                    int32_t slen) {
  std::lock_guard<std::recursive_mutex> lk(g_mutex);
  if (!inline_kind(accessor)) return -1;
  std::u16string t = chars(s).substr(0, 2048);
  caret = std::min<int32_t>(static_cast<int32_t>(t.size()), caret);
  if (caret < pidx || caret > pidx + plen) pidx = -1, plen = 0;
  int32_t a[5] = {caret, pidx, plen, sidx, slen};
  return ic::vsh_inform_text(g_handle, 49, t.c_str(), static_cast<uint32_t>(t.size()), a, 5);
}
SSN_ICALL("Sce.PlayStation.Core.Ime.ImeClient::NotifyCurrentTextInfoNative", NotifyCurrentTextInfoNative);
int32_t SetSelectTextInfoNative(uint64_t accessor, MonoString* s) {
  std::lock_guard<std::recursive_mutex> lk(g_mutex);
  if (!inline_kind(accessor)) return -1;
  std::u16string t = chars(s).substr(0, 2048);
  int32_t a[1] = {static_cast<int32_t>(t.size())};
  return ic::vsh_inform_text(g_handle, 9, t.c_str(), static_cast<uint32_t>(t.size()), a, 1);
}
SSN_ICALL("Sce.PlayStation.Core.Ime.ImeClient::SetSelectTextInfoNative", SetSelectTextInfoNative);

// SetCaretGeometryNative / SetPreeditRectGeometryNative / SetSelectRectGeometryNative (0x2AE0 /
// 0x2B50 / 0x2BC0): the 16-byte struct passed by value.
int32_t geometry(uint64_t accessor, uint32_t method, const void* g) {
  std::lock_guard<std::recursive_mutex> lk(g_mutex);
  return inline_kind(accessor) ? ic::vsh_set_geometry(g_handle, method, g) : -1;
}
int32_t SetCaretGeometryNative(uint64_t accessor, const void* g) { return geometry(accessor, 6, g); }
SSN_ICALL("Sce.PlayStation.Core.Ime.ImeClient::SetCaretGeometryNative", SetCaretGeometryNative);
int32_t SetPreeditRectGeometryNative(uint64_t accessor, const void* g) { return geometry(accessor, 7, g); }
SSN_ICALL("Sce.PlayStation.Core.Ime.ImeClient::SetPreeditRectGeometryNative", SetPreeditRectGeometryNative);
int32_t SetSelectRectGeometryNative(uint64_t accessor, const void* g) { return geometry(accessor, 8, g); }
SSN_ICALL("Sce.PlayStation.Core.Ime.ImeClient::SetSelectRectGeometryNative", SetSelectRectGeometryNative);

// SetPanelPositionNative(ulong, x, y, hA, vA) — 0x2450.
int32_t SetPanelPositionNative(uint64_t accessor, uint32_t x, uint32_t y, uint32_t ha, uint32_t va) {
  std::lock_guard<std::recursive_mutex> lk(g_mutex);
  if (!accessor || ha > 2 || va > 2 || !inline_kind(accessor)) return -1;
  return ic::vsh_set_panel_position(g_handle, ha, va, static_cast<float>(x), static_cast<float>(y));
}
SSN_ICALL("Sce.PlayStation.Core.Ime.ImeClient::SetPanelPositionNative", SetPanelPositionNative);

// GetPanelPositionAndFormNative — 0x2490 -> 0x3150.
int32_t GetPanelPositionAndFormNative(uint64_t accessor, uint32_t* type, uint32_t* x, uint32_t* y, uint32_t* ha, uint32_t* va,
                                      uint32_t* w, uint32_t* h) {
  std::lock_guard<std::recursive_mutex> lk(g_mutex);
  if (!inline_kind(accessor)) return -1;
  uint32_t f[7];
  const int32_t r = ic::vsh_get_panel_position_and_form(g_handle, f);
  if (r != 0) return r;
  float fx, fy;
  std::memcpy(&fx, &f[1], 4);
  std::memcpy(&fy, &f[2], 4);
  *type = f[0];
  *x = static_cast<uint32_t>(static_cast<int64_t>(fx));
  *y = static_cast<uint32_t>(static_cast<int64_t>(fy));
  *ha = f[3];
  *va = f[4];
  *w = f[5];
  *h = f[6];
  return 0;
}
SSN_ICALL("Sce.PlayStation.Core.Ime.ImeClient::GetPanelPositionAndFormNative", GetPanelPositionAndFormNative);

}  // namespace
}  // namespace ssn::psmutil
