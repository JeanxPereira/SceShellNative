// ShellUI's system IME server (eboot.bin FW 4.03 devkit J01736824) — see ime_server.h.
//
// What is reproduced, by firmware object:
//   * BackgroundInit(InitIme) 0xD210 case 7: the IME framework (qword_EC9168, ctor 0x10CE90,
//     Initialize 0x10D720), the UI singleton (qword_EC9210, 0x12A820), the "SceImeUpdate" thread
//     (0x145850; run 0x146D20) woken by libScePsm's vblank callback (0x146E00), the IME service
//     class (qword_ECA648, 0x198E00) and the keyboard manager (ime_keyboard.cpp).
//   * The service: connect 0x1991B0, the sync dispatch 0x199820 (methods 2, 3, 5, 9, 10, 16, 17, 40,
//     43, 45, 46, 47, 48, 49), the update/poll method 4 (command 0 + 0x19B420), the command queue
//     0x19D320 executed by 0x19DB10 on the update thread, and the client event list (0x19D4F0: at
//     most 100 records of 312 bytes, a 531 not repeated).
//   * The framework paths an inline OSK session takes: open 0x11B9F0 (unit choice 0x11CFE0 ->
//     0x11A040, PSM event 270 {EventId 21}), the unit manager's request queue / update 0x13F090
//     (SendChangeUnitEvent 0x123F80 -> PSM event 270 {EventId 2}, the preparation job 0x142B70,
//     activation 0x13C3E0), OnUnitLoadFinish 0x11AC50, the UI-ready events 0x11B7C0, the managed
//     button events (OnImeEventNative 0x144CF0 -> PostButtonEvent 0x113EA0 -> ImeUnitClient queue
//     0x1146C0 / delivery 0x13C160 -> the OSK unit's handler), the BasicLatin OSK unit
//     (SceImeOskBasicLatinUnit 0x16CDD0, OnDecide 0x16CFF0) with its backend (type 27,
//     SceImeBasicLatinBackendUnit 0x16DD00), and the framework edit operations that backend calls:
//     insert 0x111070 (with the character filter 0x111270 / 0x1378D0 and libSceIme
//     sceImeFilterText), backspace 0x10F1F0, delete 0x10F690, caret moves 0x10FFB0, enter 0x112190,
//     close 0x112860; context 0x10AB90 (PSM event 204), panel position / form 0x122880 / 0x122A00 /
//     0x12C6C0 (PSM event 206); close 0x11B320 (PSM event 270 {EventId 22}).
// Not reproduced (logged once when reached): the other OSK units (European scripts, Japanese,
// Korean, Chinese, number, voice / remote-play plugins) and their conversion engines, the ImeDialog
// server, text-to-speech, the user dictionary, selection mode, the per-user input-language list
// (ImeInputSettings) and last-unit memory (ImeLastUnit), the event cache mode of PostButtonEvent.
#include "eboot/ime_server.h"

#include <windows.h>

#include <atomic>
#include <condition_variable>
#include <cstring>
#include <deque>
#include <list>
#include <memory>
#include <mutex>
#include <thread>
#include <vector>

#include "core/log.h"
#include "eboot/ime_keyboard.h"
#include "host/boundary.h"
#include "libScePsm/runtime/event_hub.h"
#include "libSceRegMgr/regmgr.h"
#include "libSceUrlConfigResolver/sce_json.h"

namespace ssn::psm::gfx {
void set_vblank_callback(void (*cb)());  // libScePsm XmlFvKZlm7k (graphics_device.cpp)
}

namespace ssn::eboot::ime {
namespace json = ssn::urlcfg::json;

namespace {

void note(const std::string& what) { host::note_unimplemented("eboot IME: " + what); }

// ---------------------------------------------------------------------------------------------
// Client event record (312 bytes, the unit of the service event list and of method 4's output):
// +0 u32 id; +8 UTF-16 text (<= 120 chars + nul) or a value / rect; +252 u32 caret (id 512).
struct Record {
  uint8_t b[312] = {};
  uint32_t id() const { uint32_t v; std::memcpy(&v, b, 4); return v; }
};

Record rec_simple(uint32_t id) {  // sub_129220
  Record r;
  std::memcpy(r.b, &id, 4);
  return r;
}
Record rec_value(uint32_t id, uint32_t v) {  // sub_128E70 / sub_129560
  Record r = rec_simple(id);
  std::memcpy(r.b + 8, &v, 4);
  return r;
}
Record rec_text(uint32_t id, const std::u16string& s, uint32_t caret) {  // sub_129010
  Record r = rec_simple(id);
  const size_t n = std::min<size_t>(s.size(), 120);
  std::memcpy(r.b + 8, s.data(), n * 2);
  if (id == 512) std::memcpy(r.b + 252, &caret, 4);
  return r;
}
Record rec_rect(uint32_t id, float x, float y, uint32_t w, uint32_t h) {  // sub_1290C0
  Record r = rec_simple(id);
  std::memcpy(r.b + 8, &x, 4);
  std::memcpy(r.b + 12, &y, 4);
  std::memcpy(r.b + 16, &w, 4);
  std::memcpy(r.b + 20, &h, 4);
  return r;
}

std::string utf8(const std::u16string& s) {
  if (s.empty()) return {};
  const int n = WideCharToMultiByte(CP_UTF8, 0, reinterpret_cast<const wchar_t*>(s.data()), static_cast<int>(s.size()), nullptr, 0,
                                    nullptr, nullptr);
  std::string o(n, '\0');
  WideCharToMultiByte(CP_UTF8, 0, reinterpret_cast<const wchar_t*>(s.data()), static_cast<int>(s.size()), o.data(), n, nullptr, nullptr);
  return o;
}
std::u16string utf16(const std::string& s) {
  if (s.empty()) return {};
  const int n = MultiByteToWideChar(CP_UTF8, 0, s.data(), static_cast<int>(s.size()), nullptr, 0);
  std::u16string o(n, u'\0');
  MultiByteToWideChar(CP_UTF8, 0, s.data(), static_cast<int>(s.size()), reinterpret_cast<wchar_t*>(o.data()), n);
  return o;
}

// PSM event 270 with the object `o` (sub_113A90 / sub_10E040: fSb2oQTNrgA + 7W6jR0LyiSg).
void post_psm(json::Object o) { psm::events::post(270, json::serialize(json::Value::make_object(std::move(o)))); }

// ---------------------------------------------------------------------------------------------
// Unit ids (16 bytes: language index, type, input method, plugin type; sub_10A1B0..0x10A2F0).
struct UnitId {
  uint32_t lang = 0, type = 0, method = 0, plugin = 0;
  bool operator==(const UnitId&) const = default;
};
// sub_10A300: SCE_IME_TYPE -> unit type (256..258 -> 5..7).
uint32_t unit_type(uint32_t t) { return t - 256 <= 2 ? t - 251 : t; }
// sub_10A3F0: language index -> SCE_IME_LANGUAGE bit.
uint64_t lang_bit(uint32_t idx) { return idx ? 1ull << (idx - 1) : 0; }
// sub_13E730: unit -> language bit (index 14 / 17 / 41 / 42 special cases).
uint64_t unit_lang_bit(const UnitId& u) {
  switch (u.lang) {
    case 14: return u.method == 2 ? 0x200000 : 0x2000;
    case 17: case 42: return 0x20000000000ull;
    case 41: return u.method == 3 ? 0x800000 : (u.method == 2 ? 0x400000 : 0x10000000000ull);
    case 22: case 23: case 24: case 34: case 35: case 36: case 37: case 38: case 39: case 40: return 0;
    default: return u.lang && u.lang <= 33 ? lang_bit(u.lang) : 0;
  }
}
// sub_14B650: the system language (registry 0x2020000) -> language index, for a unit id without one.
uint32_t system_language_index() {
  int32_t v = 0;
  regmgr::get_int(0x2020000, &v);
  static const int8_t kMap[30] = {14, 3,  5,  4,  2,  6,  7,  10, 11, 15, 41, 16, -1, -1, 1,
                                  8,  9,  18, 19, 20, 21, 25, 26, 28, 33, 29, 32, -1, 31, 30};
  if (v < 0 || v >= 30) return 0;
  return kMap[v] < 0 ? static_cast<uint32_t>(v) : static_cast<uint32_t>(kMap[v]);
}

// ---------------------------------------------------------------------------------------------
// The managed button event as the framework holds it (sub_146750: +0 controlEventId, +4
// buttonType, +8 inputString, +48 fullWidth / shiftState, +56 SelectCancelFactor, +64 keycodes).
struct ButtonEvent {
  int32_t control = 0x10000;
  uint32_t type = 0;
  std::u16string text;
  int32_t full_width = 0, shift = 0;
  struct Key { uint16_t keycode, character; uint32_t status; };
  std::vector<Key> keys;
};

struct Framework;

// SceImeBasicLatinBackendUnit (type 27, 0x16DD00; vtable 0xCD48C8): every operation is a call on
// the framework (its +8).
struct BasicLatinBackend {
  Framework* fw;
  void insert(const std::u16string& s);  // +24 0x16E1C0
  void enter();                          // +48 0x16E030
  void close();                          // +64 0x16DE10
  void caret(uint32_t dir);              // +80 0x16DEF0
  void backspace();                      // +88 0x16DF70
  void del();                            // +96 0x16DFD0
};

// SceImeOskBasicLatinUnit (0x16CDD0; vtable 0xCD47B0, handler interface at +72 0xCD48A0).
struct OskBasicLatinUnit {
  Framework* fw;
  std::unique_ptr<BasicLatinBackend> backend;  // +88
  uint32_t shift_state = 0;                    // +80
  void prepare() { backend = std::make_unique<BasicLatinBackend>(BasicLatinBackend{fw}); }  // +32 0x16CE90 (sub_16D990 type 27)
  void release() { backend.reset(); }                                                       // +40 0x16CEF0
  void activate();                                                                          // +56 0x16CF20
  void deactivate();                                                                        // +64 0x16CFA0
  void on_decide(const ButtonEvent& e);                                                     // 0x16CFF0
};

// Ime::ImeUnitClient (0x13BD90): the unit, its id, the queued button events, active / loaded.
struct UnitClient {
  UnitId id;
  std::unique_ptr<OskBasicLatinUnit> unit;
  std::deque<ButtonEvent> queue;  // +16 (0x1146C0 appends)
  bool active = false;            // +40
  bool loaded = false;            // +41
};

// The UI singleton qword_EC9210 (0x12A820).
struct UiProxy {
  bool has_focused_candidate = false;  // +9
  int32_t init_input_mode = 0;         // +16
  OskBasicLatinUnit* handler = nullptr;  // +24 (the active OSK unit's handler interface)
  int32_t ui_status = 3;               // +48 (ImeUIStatus: 0 Opened, 1 Ready, 2 Closing, 3 Closed)
  int32_t len52 = 0, len56 = 0, confirm_len = 0;  // +52 / +56 / +60
};

// The service class object qword_ECA648 (0x198E00).
struct Service {
  std::mutex clients_mutex;          // "SceImeClientList"
  struct Client { uint32_t id; bool flag5; };
  std::list<Client> clients;         // +24
  int32_t client = -1;               // +48
  int32_t open_client = -1;          // +52
  std::mutex events_mutex;           // "SceImeEventQueue" (+88)
  std::deque<Record> events;         // +72 / +80
  std::mutex commands_mutex;         // "SceImeCommandQueue"
  struct Command { int32_t id; int32_t client; std::vector<uint8_t> data; };
  std::deque<Command> commands;      // +56
  std::u16string init_text;          // +120 (method 45)
  bool flag112 = false, flag113 = false;
};

// Ime::ImeFramework (qword_EC9168, 20032 bytes).
struct Framework {
  int32_t state = 3;           // +740 (0 opening, 1 open, 2 closing, 3 closed)
  bool ready_sent = false;     // +744
  bool b746 = false, b747 = false;
  uint8_t param[456] = {};     // +176 (the internal param of method 4)
  std::u16string preedit;      // +152 (capacity 120, sub_12F0D0)
  uint32_t caret = 0;          // +168
  int32_t send_status = 0;     // +136
  float form_x = 0, form_y = 0;            // +716 / +720 (doubled)
  uint32_t form[7] = {};                   // +712.. (UpdatePositionAndForm copy)
  int32_t form_w = 0, form_h = 0;          // +732 / +736 (doubled)
  uint8_t pending_end = 0;                 // +9154
  int32_t sel_mode = 0;                    // +9248
  int32_t hwkb_state = 0;                  // +9076
  bool unit_switching = false;             // +9153
  int32_t remaining = 0;                   // +9260 (maxLength - text length)
  int32_t device_state_9284 = 0, device_state_9304 = -1;
  int32_t app_id = -1, front_app = -1, front_scene = -1, game_app = 0;  // +9320 / +9312 / +9316 / +9308
  int32_t client_id = -1;                  // +9256
  UnitClient* unit = nullptr;              // +784
  // the unit manager (+792, 0x13C850)
  std::vector<std::unique_ptr<UnitClient>> units;  // +24
  UnitClient* current = nullptr;                    // +96
  UnitId initial_unit, current_unit;                // +188 / +220
  struct Request { int type; UnitClient* unit; };
  std::deque<Request> requests;                     // +128
  bool job_done = false;                            // +113
  uint64_t languages = 0;                           // the language list's mask (sub_13EAA0)
  void* ui_event = nullptr;                         // +15528 (managed uiEventDelegate)

  // param accessors (framework offset - 176)
  template <class T>
  T p(size_t off) const { T v; std::memcpy(&v, param + off, sizeof v); return v; }
  uint32_t type() const { return p<uint32_t>(12); }       // +188
  uint32_t option() const { return p<uint32_t>(16); }     // +192
  int32_t user() const { return p<int32_t>(72); }         // +248
  uint32_t max_len() const { return p<uint32_t>(80); }    // +256
  uint32_t ext_option() const { return p<uint32_t>(96); }  // +272

  void emit(const Record& r);
  bool option_check(int which) const;
  void open(const uint8_t* p456, bool* opened);
  void close();
  void request_activate(UnitClient* u);
  void unit_manager_update();
  void on_unit_load_finish(UnitClient* u);
  void ui_ready();
  void post_button_event(const ButtonEvent& e);
  void deliver(UnitClient* u);
  void send_change_unit_event(const UnitId& id);
  // editing (vtable 0xCCE4F8)
  void insert(const std::u16string& s);  // +56 0x111070
  void backspace();                      // +0 0x10F1F0
  void del();                            // +8 0x10F690
  void caret_move(uint32_t dir);         // +40 0x10FFB0
  void enter();                          // +168 0x112190
  void close_button();                   // +176 0x112860
  std::u16string filter(const std::u16string& s);  // 0x111270
};

std::recursive_mutex g_lock;  // the framework mutex (sub_10AED0) taken by every entry
bool g_initialized = false;   // byte_EC9538
Framework* g_fw = nullptr;
UiProxy* g_ui = nullptr;
Service* g_srv = nullptr;
std::mutex g_wake_mutex;
std::condition_variable g_wake;
bool g_wake_flag = false;  // "SceImeUpdateFlags" bit 1

void service_push(const Record& r);

void Framework::emit(const Record& r) {  // sub_128D10: the sink (+648) is the service (set at connect)
  if (g_srv && g_srv->client != -1) service_push(r);
}

// sub_11DD80, the cases this path reads.
bool Framework::option_check(int which) const {
  const uint8_t* o = param + 16;  // +192..+195
  switch (which) {
    case 0: return o[0] & 1;                          // multiline
    case 10: return (o[3] & 2) != 0;
    case 25: return true;                             // client SDK: non-app (sub_10B560 -> {_, 0})
    case 27: {                                        // screen-reader speech (sub_123C30)
      static const bool n = (note("HOST VALUE screen-reader speech (option 27, sub_123C30 / COqIT6fJpzc): off"), true);
      (void)n;
      return false;
    }
    case 34: return p<uint32_t>(64) != 0;             // +240 embedded OSK target
    default: return false;
  }
}

// ---------------------------------------------------------------------------------------------
// Service.

void service_push(const Record& r) {  // 0x19D4F0
  std::lock_guard<std::mutex> lk(g_srv->events_mutex);
  const size_t n = g_srv->events.size();
  if (n != 0 && (n > 99 || (r.id() == 531 && g_srv->events.back().id() == 531))) return;
  g_srv->events.push_back(r);
}

void post_command(int32_t id, int32_t client, const void* data, size_t size) {  // 0x19D320
  std::lock_guard<std::mutex> lk(g_srv->commands_mutex);
  Service::Command c{id, client, {}};
  if (data && size <= 0x103F) c.data.assign(static_cast<const uint8_t*>(data), static_cast<const uint8_t*>(data) + size);
  c.data.resize(4160);
  g_srv->commands.push_back(std::move(c));
}

// 0x19DB10, run for each queued command by 0x19D960 on the update thread.
void execute_command(const Service::Command& c) {
  if (c.client != -1 && c.client != g_srv->client) return;
  switch (c.id) {
    case 0:  // open (0x10A890 -> 0x11B9F0)
      if (g_srv->client != -1) {
        bool opened = false;
        g_fw->open(c.data.data(), &opened);
        g_srv->open_client = c.client;
      }
      break;
    case 1:  // close (0x10A8F0 -> 0x11B320)
      g_fw->close();
      g_srv->open_client = -1;
      break;
    case 12: {  // context (0x10AB90): +0 u32 length, +4 u32 flags
      uint32_t len, flags;
      std::memcpy(&len, c.data.data(), 4);
      std::memcpy(&flags, c.data.data() + 4, 4);
      if (g_fw->preedit.empty()) {
        if (g_fw->max_len() - 1 < len) flags |= 0x20;
        g_ui->len56 = 0;
        g_ui->len52 = static_cast<int32_t>(flags);
        g_ui->confirm_len = static_cast<int32_t>(len);
        if (g_fw->state == 1 && g_ui->ui_status == 1) {  // 0x10E600: PSM event 204
          post_psm({{"EventId", json::Value::make_uint(204)},
                    {"ErrorSoundFlag", json::Value::make_uint(flags)},
                    {"Length", json::Value::make_uint(0)},
                    {"MaxLength", json::Value::make_uint(g_fw->max_len())}});
        }
        // 0x10E840: remaining characters
        g_fw->remaining = std::max<int32_t>(0, static_cast<int16_t>(g_fw->max_len() - len));
      } else {
        note("context with a non-empty preedit (0x10AB90 preedit branch) not reproduced");
      }
      break;
    }
    default:
      note("service command " + std::to_string(c.id) + " (0x19DB10) not reproduced");
      break;
  }
}

void process_commands() {  // sub_198CA0 -> 0x199100 -> 0x19D960
  std::deque<Service::Command> cmds;
  {
    std::lock_guard<std::mutex> lk(g_srv->commands_mutex);
    cmds.swap(g_srv->commands);
  }
  for (const auto& c : cmds) execute_command(c);
}

// ---------------------------------------------------------------------------------------------
// Framework: open / unit selection / unit manager.

// 0x11A040 for an inline client: the initial unit. The unit manager's language list is the
// firmware's default mask 0x301FF1EFFFF (sub_13DA50 with no forced languages and no per-user
// input languages); the language of the unit is the user's last unit, which the host does not
// keep (ImeLastUnit), so the unit id has no language and 0x14B650 takes the system language.
UnitId choose_initial_unit(const Framework& fw) {
  UnitId u;
  u.lang = system_language_index();
  u.type = unit_type(fw.type());
  u.method = (u.lang == 18 ? 2 : 1);
  u.plugin = 0;
  static const bool n = (note("initial unit: user input-language list / ImeLastUnit not kept; system language (0x14B650)"), true);
  (void)n;
  return u;
}

// ImeUnitManager::CreateUnit 0x13D130 / GetUnit 0x13D050.
UnitClient* get_unit(Framework& fw, const UnitId& id) {
  for (auto& u : fw.units)
    if (u->id == id) return u.get();
  if (id.plugin != 0 || !(id.type == 1 || id.type == 5 || id.type == 7)) {
    note("OSK unit lang " + std::to_string(id.lang) + " type " + std::to_string(id.type) + " plugin " + std::to_string(id.plugin) +
         " (0x13D130: European scripts / Japanese / Korean / Chinese / number / plugin units) not reproduced");
    return nullptr;
  }
  auto c = std::make_unique<UnitClient>();
  c->id = id;
  c->unit = std::make_unique<OskBasicLatinUnit>(OskBasicLatinUnit{&fw, nullptr, 0});
  log::info("ime", "CreateUnit id lang={} type={} method={} plugin={} (SceImeOskBasicLatinUnit)", id.lang, id.type, id.method, id.plugin);
  fw.units.push_back(std::move(c));
  return fw.units.back().get();
}

void Framework::request_activate(UnitClient* u) {  // RequestToActivateUnit 0x13FE00
  if (current) requests.push_back({0, current});
  requests.push_back({1, u});
}

void Framework::send_change_unit_event(const UnitId& id) {  // SendChangeUnitEvent 0x123F80 -> 0x113520
  auto uid = [](const UnitId& u) {
    return json::Value::make_object({{"language", json::Value::make_uint(u.lang)},
                                     {"type", json::Value::make_int(u.type)},
                                     {"inputMethod", json::Value::make_uint(u.method)},
                                     {"pluginType", json::Value::make_uint(u.plugin)}});
  };
  uint64_t sdk;
  std::memcpy(&sdk, param + 132, 8);
  post_psm({{"EventId", json::Value::make_int(2)},
            {"currentUnitId", uid(id)},
            {"initialUnitId", uid(initial_unit)},
            {"requestUnitId", uid(id)},
            {"lastdefaultUnitId", uid(id)},
            {"versionInfo", json::Value::make_object({{"version", json::Value::make_uint(static_cast<uint32_t>(sdk))},
                                                      {"platform", json::Value::make_uint(sdk >> 32)}})},
            {"option", json::Value::make_uint(option())},
            {"maxLength", json::Value::make_uint(max_len())},
            {"enterLabel", json::Value::make_uint(p<uint8_t>(20))},
            {"supportedLanguages", json::Value::make_uint(languages)},
            {"userId", json::Value::make_uint(static_cast<uint32_t>(user()))}});
}

// 0x13F090 (run by 0x123F60 from the update thread).
void Framework::unit_manager_update() {
  if (job_done) {
    job_done = false;
    if (!requests.empty()) {
      Request r = requests.front();
      requests.pop_front();
      if (r.type == 1) {
        if (!r.unit->active) {  // 0x13C3E0
          r.unit->active = true;
          r.unit->unit->activate();
        }
        current = r.unit;
        on_unit_load_finish(r.unit);
      }
    }
    return;
  }
  if (requests.empty()) return;
  Request& r = requests.front();
  if (r.type == 1) {
    if (initial_unit == UnitId{}) initial_unit = r.unit->id;
    current_unit = r.unit->id;
    send_change_unit_event(r.unit->id);
    // SceImeUnitPreparationJobItem (0x142B70), run here instead of on the job queue thread.
    if (!r.unit->loaded) {
      r.unit->unit->prepare();
      r.unit->loaded = true;
    }
  } else {
    if (r.unit) {
      r.unit->active = false;  // 0x13C0B0
      r.unit->unit->deactivate();
      r.unit->queue.clear();
    }
    requests.pop_front();
    if (requests.empty()) return;
    return unit_manager_update();
  }
  job_done = true;  // the job sets +113
}

// 0x11B9F0.
void Framework::open(const uint8_t* p456, bool* opened) {
  *opened = false;
  if (state != 3) return;
  std::memcpy(param, p456, 456);
  preedit.clear();
  caret = 0;
  send_status = 0;
  // 0x11CFE0 SetUserId: -1 / 0 become 254 (deviceIndexBehaviorMode 1 takes its value)
  int32_t uid = user();
  if (static_cast<uint32_t>(uid + 1) < 2) {
    const uint32_t mode = p<uint32_t>(152);
    uid = mode == 1 ? static_cast<int32_t>(p<uint64_t>(160)) : 254;
    std::memcpy(param + 72, &uid, 4);
  }
  // 0x11A040
  b747 = true;
  languages = 0x301FF1EFFFFull;
  UnitId id = choose_initial_unit(*this);
  unit = get_unit(*this, id);
  caret = 0;
  b746 = false;
  state = 0;
  if (unit) {
    request_activate(unit);
    unit->queue.clear();  // 0x13C400
  }
  // PSM event 270 {EventId 21} with the open parameters.
  uint64_t sdk;
  std::memcpy(&sdk, param + 132, 8);
  float x, y, tf[4], scaling;
  std::memcpy(&x, param + 24, 4);
  std::memcpy(&y, param + 28, 4);
  std::memcpy(tf, param + 40, 16);
  std::memcpy(&scaling, param + 104, 4);
  auto cstr = [&](size_t off, size_t max) { return std::string(reinterpret_cast<const char*>(param + off), strnlen(reinterpret_cast<const char*>(param + off), max)); };
  const uint64_t first_lang = p<uint64_t>(144) ? p<uint64_t>(144) : (unit ? unit_lang_bit(unit->id) : 0);
  post_psm({{"supportedLanguages", json::Value::make_uint(languages)},
            {"firstUnitLanguage", json::Value::make_uint(first_lang)},
            {"type", json::Value::make_uint(unit_type(type()))},
            {"option", json::Value::make_uint(option())},
            {"enterLabel", json::Value::make_uint(p<uint32_t>(20))},
            {"X", json::Value::make_real(x * 0.5)},
            {"Y", json::Value::make_real(y * 0.5)},
            {"HorizontalAlignment", json::Value::make_uint(p<uint32_t>(32))},
            {"VerticalAlignment", json::Value::make_uint(p<uint32_t>(36))},
            {"deviceIndexBehaviorMode", json::Value::make_uint(p<uint32_t>(152))},
            {"deviceIndexBehaviorValue", json::Value::make_uint(p<uint64_t>(160))},
            {"userId", json::Value::make_int(user())},
            {"appId", json::Value::make_int(app_id)},
            {"version", json::Value::make_uint(static_cast<uint32_t>(sdk))},
            {"platform", json::Value::make_uint(static_cast<uint32_t>(sdk >> 32))},
            {"scaling", json::Value::make_int(static_cast<int32_t>(scaling))},
            {"sceneId", json::Value::make_int(p<int32_t>(176))},
            {"imeInputOperationMethod", json::Value::make_int(0)},
            {"extendedOption", json::Value::make_int(static_cast<int32_t>(ext_option()))},
            {"priority", json::Value::make_int(p<int32_t>(100))},
            {"disableDevice", json::Value::make_int(p<int32_t>(124))},
            {"extKeyboardMode", json::Value::make_uint(p<uint32_t>(128))},
            {"addExitFocusOption", json::Value::make_uint(p<uint8_t>(169))},
            {"initialInputMode", json::Value::make_int(p<int32_t>(172))},
            {"textFieldPosition.x", json::Value::make_real(tf[0])},
            {"textFieldPosition.y", json::Value::make_real(tf[1])},
            {"textFieldPosition.width", json::Value::make_real(tf[2])},
            {"textFieldPosition.height", json::Value::make_real(tf[3])},
            {"embeddedOskTargetInfo", json::Value::make_object({{"targetId", json::Value::make_int(p<int64_t>(56))},
                                                                {"embeddedType", json::Value::make_int(p<int32_t>(64))}})},
            {"PuiType", json::Value::make_int(p<int32_t>(180))},
            {"EventId", json::Value::make_int(21)},
            {"TtsEnabled", json::Value::make_bool(option_check(27))},
            {"initialText", json::Value::make_string(utf8(g_srv->init_text))},
            {"resumeLanguage", json::Value::make_uint(p<uint64_t>(144))},
            {"startDeviceType", json::Value::make_int(p<int32_t>(192))},
            {"textFieldDirectionType", json::Value::make_uint(p<uint8_t>(168))},
            {"additionalDicName", json::Value::make_string(cstr(108, 16))},
            {"callerApplicationInfo", json::Value::make_string(cstr(196, 129))},
            {"calledViaSystemTextField", json::Value::make_bool(p<uint8_t>(454) != 0)},
            {"testId", json::Value::make_string(cstr(325, 129))}});
  state = 0;
  *opened = true;
  ready_sent = false;
  hwkb_state = 0;
  remaining = static_cast<int32_t>(max_len());
  pending_end = 0;
  unit_switching = false;
  log::info("ime", "open: type={} option=0x{:x} maxLength={} user={} unit lang={}", type(), option(), max_len(), user(),
            unit ? unit->id.lang : 0);
}

// OnUnitLoadFinish 0x11AC50.
void Framework::on_unit_load_finish(UnitClient* u) {
  const int32_t prev = state;
  if (prev == 3 && g_ui->ui_status != 3) return;
  state = 1;
  if (prev == 2) {
    close();
    return;
  }
  deliver(u);
  ui_ready();
  unit_switching = false;
}

// 0x11B7C0: once the managed UI reports Ready, the client gets the open events.
void Framework::ui_ready() {
  if (ready_sent || state != 1 || g_ui->ui_status != 1) return;
  if (option_check(27)) emit(rec_simple(538));
  emit(rec_rect(0, form_x, form_y, static_cast<uint32_t>(form_w), static_cast<uint32_t>(form_h)));
  if (!preedit.empty()) emit(rec_text(512, preedit, caret));
  emit(rec_simple((unit && unit->id.lang == 31) ? 0x21C : 0x21D));
  ready_sent = true;
  ime_keyboard::on_ime_ready(user(), option());  // 0x121E50: take the user's keyboard
  if (!(option() & 0x10)) emit(rec_value(14, 1));  // operation device: controller (0x128E70)
  if (device_state_9304 != device_state_9284) {
    emit(rec_value(18, static_cast<uint32_t>(device_state_9284)));
    device_state_9304 = device_state_9284;
  }
  switch (pending_end) {
    case 1: emit(rec_simple(5)); break;
    case 2: emit(rec_simple(4)); break;
    case 3: emit(rec_simple(6)); break;
    default: break;
  }
  pending_end = 0;
}

// Close 0x11B320.
void Framework::close() {
  if ((state & ~1) == 2) return;
  state = 2;
  ime_keyboard::on_ime_close();  // 0x1A5E80
  preedit.clear();
  caret = 0;
  if (unit && unit->active) {
    unit->active = false;
    unit->unit->deactivate();
    unit->queue.clear();
  }
  g_ui->init_input_mode = 0;
  post_psm({{"EventId", json::Value::make_uint(22)}});
  // the job queue empty-wait (SceImeJobQueueEmptyWaitJobItem) then 0x128940 return to state 3
  state = 3;
  ready_sent = false;
  current = nullptr;
  requests.clear();
  job_done = false;
  log::info("ime", "close");
}

// 0x13C160: queued button events go to the UI singleton's handler (the active OSK unit).
void Framework::deliver(UnitClient* u) {
  if (!u || !u->loaded || !u->active) return;
  while (!u->queue.empty()) {
    ButtonEvent e = u->queue.front();
    u->queue.pop_front();
    if (g_ui->handler && (e.type != 0 || e.control != 0x10000)) g_ui->handler->on_decide(e);
    if (!u->active) {
      u->queue.clear();
      return;
    }
  }
}

// PostButtonEvent 0x113EA0 (no event cache, no hardware keyboard record).
void Framework::post_button_event(const ButtonEvent& e) {
  if (!unit) return;
  if (sel_mode != 0 && e.type != 0) note("button during range selection (0x113EA0 selection mode) not reproduced");
  if (unit_switching) {
    note("button during a unit switch (0x1146C0 on +9104) queued");
  }
  if (hwkb_state == 2) {
    ime_keyboard::leave_hwkb_mode();  // 0x1147B0
  } else if (e.control == 65563) {
    return;
  }
  emit(rec_simple(531));            // +15539 set: SDK newer than 0x44FFFFF or non-app client
  unit->queue.push_back(e);         // vtable +16 0x13C150 -> 0x1146C0
  deliver(unit);                    // 0x13C160
}

// ---------------------------------------------------------------------------------------------
// Editing operations (the preedit is always empty with the BasicLatin backend).

// 0x111270: strip characters (option 10: a space; multiline off: CR LF), then the character
// filter 0x1378D0 per grapheme through libSceIme sceImeFilterText: types 1 / 3 / 258 keep code
// points <= 0x7E (libSceIme 0x4350, LABEL_38).
std::u16string Framework::filter(const std::u16string& in) {
  std::u16string s;
  for (char16_t c : in) {
    if (option_check(10) && c == u' ') continue;
    if (!option_check(0) && (c == u'\r' || c == u'\n')) continue;
    s.push_back(c);
  }
  const uint32_t t = type();
  if (t == 1 || t == 3 || t == 258) {
    std::u16string o;
    for (char16_t c : s)
      if (c <= 0x7E) o.push_back(c);
    return o;
  }
  if (t != 0 && t != 2 && t != 256 && t != 257 && t != 4) note("character filter for type " + std::to_string(t) + " not reproduced");
  return s;
}

void Framework::insert(const std::u16string& s) {  // 0x111070
  if (sel_mode != 0) backspace();
  if (!preedit.empty()) note("insert with a preedit (0x111070 confirm branch) not reproduced");
  const std::u16string f = filter(s);
  emit(rec_text(514, f.size() == s.size() ? s : f, 0));
}

void Framework::backspace() {  // 0x10F1F0
  if (preedit.empty()) {
    emit(rec_simple(515));
    return;
  }
  note("backspace inside a preedit not reproduced");
}

void Framework::del() {  // 0x10F690
  if (preedit.empty()) {
    emit(rec_simple(523));
    return;
  }
  note("delete inside a preedit not reproduced");
}

void Framework::caret_move(uint32_t dir) {  // 0x10FFB0, preedit empty
  if (state != 1 || g_ui->ui_status != 1 || !b747) return;
  static const uint32_t kIds[10] = {768, 769, 770, 771, 776, 777, 772, 773, 774, 775};
  if (!preedit.empty()) {
    note("caret move inside a preedit not reproduced");
    return;
  }
  if (dir < 10) emit(rec_simple(kIds[dir]));
}

void Framework::enter() {  // 0x112190
  if (option_check(34) && hwkb_state == 2) return;
  send_status = 2;  // (sub_103F70() == 0) + 1: not an ImeDialog
  emit(rec_simple(5));
  note("last-unit memory (0x112490 -> ImeLastUnit) not reproduced");
  if (!ready_sent) pending_end = 1;
}

void Framework::close_button() {  // 0x112860
  preedit.clear();  // 0x111440
  send_status = 2;
  emit(rec_simple(4));
  if (!ready_sent) pending_end = 2;
}

// ---------------------------------------------------------------------------------------------
// The BasicLatin backend and OSK unit.

void BasicLatinBackend::insert(const std::u16string& s) {  // 0x16E1C0: option 10 drops a lone space
  if (s.size() == 1 && fw->option_check(10) && s[0] == u' ') return;
  fw->insert(s);
}
void BasicLatinBackend::enter() { fw->enter(); }
void BasicLatinBackend::close() { fw->close_button(); }
void BasicLatinBackend::caret(uint32_t dir) { fw->caret_move(dir); }
void BasicLatinBackend::backspace() { fw->backspace(); }
void BasicLatinBackend::del() { fw->del(); }

void OskBasicLatinUnit::activate() {  // 0x16CF20
  g_ui->handler = this;
  shift_state = 0;
}
void OskBasicLatinUnit::deactivate() {  // 0x16CFA0
  if (g_ui->handler == this) g_ui->handler = nullptr;
}

// 0x16DB90: control id -> caret direction.
uint32_t caret_dir(int32_t id) {
  switch (id) {
    case 65548: return 1;
    case 65549: return 2;
    case 65550: return 3;
    case 131079: return 4;
    case 131080: return 5;
    case 131081: return 6;
    case 131082: return 7;
    case 131083: return 8;
    case 131084: return 9;
    default: return 0;
  }
}

void OskBasicLatinUnit::on_decide(const ButtonEvent& e) {  // 0x16CFF0
  if (!backend) return;
  switch (e.type) {
    case 0:
      switch (e.control) {
        case 65537: backend->backspace(); return;              // Backspace
        case 65539: case 65540: backend->close(); return;      // Close / Exit
        case 65541: note("ChangeUnit button (+72 -> 0x112F60) not reproduced"); return;
        case 65544: backend->enter(); return;                  // Enter
        case 65547: case 65548: case 65549: case 65550:       // caret
        case 131079: case 131080: case 131081: case 131082: case 131083: case 131084:
          backend->caret(caret_dir(e.control));
          return;
        case 65585: case 65586: backend->insert(e.text); return;  // Space / LineBreak
        case 131072: return;  // IME_KEY_HWKB_MODE_IN: backend +16 = 1
        case 131073: return;  // HWKB_MODE_OUT
        case 131077: backend->del(); return;
        case 131097: shift_state = 0; return;
        case 65545: case 65551: case 65559: case 65560: case 65582: case 131074: case 131075: case 131076: case 131078:
          note("OSK control " + std::to_string(e.control) + " (tab / copy / paste / cut / speak / selection) not reproduced");
          return;
        default: return;
      }
    case 1: backend->insert(e.text); return;  // Character
    case 2:                                    // PairedCharacter: insert, then caret left
      backend->insert(e.text);
      fw->caret_move(0);
      return;
    case 3: backend->insert(e.text); return;   // Emoji (+32 -> +24)
    default:
      log::warn("ime", "OnDecide: Invalid ImeButtonType = {}", e.type);
  }
}

// ---------------------------------------------------------------------------------------------
// The update thread (0x146D20).

void update_thread() {
  SetThreadDescription(GetCurrentThread(), L"SceImeUpdate");
  int counter = 0;  // dword_EC9110
  for (;;) {
    {
      std::unique_lock<std::mutex> lk(g_wake_mutex);
      g_wake.wait(lk, [] { return g_wake_flag; });
      g_wake_flag = false;
    }
    std::lock_guard<std::recursive_mutex> lk(g_lock);
    process_commands();             // sub_198CA0
    ime_keyboard::update();         // sub_10B160 (0x1186F0) + sub_1A2BF0
    if (counter++ >= 10) {          // sub_10C690
      counter = 0;
    } else {
      g_fw->unit_manager_update();  // 0x123F60 -> 0x13F090
    }
  }
}

}  // namespace

// ---------------------------------------------------------------------------------------------

void initialize() {
  std::lock_guard<std::recursive_mutex> lk(g_lock);
  if (g_initialized) return;
  g_ui = new UiProxy();       // 0x12A820
  g_fw = new Framework();     // 0x10D690 / 0x10CE90 / 0x10D720
  g_srv = new Service();      // 0x1988D0
  ime_keyboard::initialize(); // 0x1A7AE0 / 0x1A30D0
  std::thread(update_thread).detach();  // "SceImeUpdate" (prio 700)
  psm::gfx::set_vblank_callback(&on_vblank);  // XmlFvKZlm7k(0x146E00)
  g_initialized = true;
  log::info("ime", "BackgroundInit(InitIme): IME framework, service \"SceImeService\" and SceImeUpdate thread created");
  note("BackgroundInit(InitIme) not reproduced: ImeDialog server (0x100DD0), ForCA / backend clients (0x1A19D0 / 0x197940), "
       "dictionary memory and the libSceIme module load");
}

bool initialized() { return g_initialized; }

void on_vblank() {  // 0x146E00
  if (!g_initialized) return;
  {
    std::lock_guard<std::mutex> lk(g_wake_mutex);
    g_wake_flag = true;
  }
  g_wake.notify_one();
}

int32_t connect(const void* info, uint32_t* client) {  // 0x1991B0
  if (!g_initialized) return static_cast<int32_t>(0x80020003u);  // no server
  std::lock_guard<std::recursive_mutex> lk(g_lock);
  uint8_t ci[24];
  std::memcpy(ci, info, 24);
  static uint32_t next = 1;
  const uint32_t id = next++;
  {
    std::lock_guard<std::mutex> l2(g_srv->clients_mutex);  // 0x199370
    if (!g_srv->clients.empty()) return static_cast<int32_t>(0x80BC07B8u);
    g_srv->clients.push_back({id, ci[12] != 0});
    g_srv->client = static_cast<int32_t>(id);
  }
  g_fw->client_id = static_cast<int32_t>(id);  // 0x10AAF0
  post_psm({{"EventId", json::Value::make_uint(20)}});  // 0x10A7C0 (OnInitialize)
  *client = id;
  return 0;
}

void disconnect(uint32_t client) {  // vtable +40 / +48
  std::lock_guard<std::recursive_mutex> lk(g_lock);
  std::lock_guard<std::mutex> l2(g_srv->clients_mutex);
  g_srv->clients.remove_if([&](const Service::Client& c) { return c.id == client; });
  if (g_srv->client == static_cast<int32_t>(client)) g_srv->client = -1;
  std::lock_guard<std::mutex> l3(g_srv->events_mutex);
  g_srv->events.clear();
}

int32_t sync_method(uint32_t client, uint32_t method, const Buf* in, int nin, int32_t* ret, Buf* out, int nout) {
  std::lock_guard<std::recursive_mutex> lk(g_lock);
  *ret = 0;
  const int32_t cid = static_cast<int32_t>(client);
  auto u32 = [&](int i, size_t off) { uint32_t v = 0; if (i < nin && in[i].n >= off + 4) std::memcpy(&v, static_cast<uint8_t*>(in[i].p) + off, 4); return v; };
  switch (method) {
    case 2:  // close: release, then command 1
      post_command(1, -1, nullptr, 0);
      return 0;
    case 3:  // set param: command 2
      post_command(2, cid, nin ? in[0].p : nullptr, nin ? in[0].n : 0);
      return 0;
    case 5: {  // command block: preedit op / caret in preedit / context flags
      const uint32_t flags = u32(0, 0);
      if ((flags & 1) && u32(0, 4)) post_command(4, cid, static_cast<uint8_t*>(in[0].p) + 4, 4);
      if (flags & 4) post_command(5, cid, static_cast<uint8_t*>(in[0].p) + 12, 4);
      if (flags & 2) post_command(3, cid, static_cast<uint8_t*>(in[0].p) + 8, 4);
      return 0;
    }
    case 6: case 7: case 8: {  // caret / preedit / select geometry -> commands 8 / 9 / 11
      post_command(method == 6 ? 8 : method == 7 ? 9 : 11, cid, nin ? in[0].p : nullptr, 16);
      return 0;
    }
    case 9:
      post_command(10, cid, nin > 1 ? in[1].p : nullptr, nin > 1 ? in[1].n : 0);
      return 0;
    case 10:
      post_command(12, cid, nin ? in[0].p : nullptr, 8);
      return 0;
    case 16: {  // GetPanelPositionAndForm 0x10B880 -> 0x122A00
      if (g_fw->state != 1 && g_ui->ui_status != 1) {
        *ret = static_cast<int32_t>(0x80BC0033u);
        return 0;
      }
      if (nout && out[0].n >= 28) {
        uint32_t f[7];
        std::memcpy(f, g_fw->form, 28);
        std::memcpy(&f[1], &g_fw->form_x, 4);
        std::memcpy(&f[2], &g_fw->form_y, 4);
        f[5] = static_cast<uint32_t>(g_fw->form_w);
        f[6] = static_cast<uint32_t>(g_fw->form_h);
        std::memcpy(out[0].p, f, 28);
        out[0].n = 28;
      }
      return 0;
    }
    case 17: {  // SetPanelPosition 0x10BA60 -> 0x12C6C0: PSM event 206 with the halved position
      float x, y;
      std::memcpy(&x, static_cast<uint8_t*>(in[0].p) + 4, 4);
      std::memcpy(&y, static_cast<uint8_t*>(in[0].p) + 8, 4);
      const uint32_t ha = u32(0, 12), va = u32(0, 16);
      if (va <= 2 && static_cast<int32_t>(ha) >= 0)
        post_psm({{"EventId", json::Value::make_int(206)}, {"X", json::Value::make_real(x * 0.5f)},
                  {"Y", json::Value::make_real(y * 0.5f)}, {"Valign", json::Value::make_int(va)},
                  {"Halign", json::Value::make_int(ha)}});
      return 0;
    }
    case 40: case 43: case 49:  // the client's confirmed text (TTS / accessibility only)
      post_command(method == 40 ? 6 : 7, cid, nullptr, 0);
      return 0;
    case 45: {  // initial text
      const uint32_t n = std::min<uint32_t>(std::min<uint32_t>(u32(0, 0), 2048), nin > 1 ? static_cast<uint32_t>(in[1].n / 2) : 0);
      g_srv->init_text.assign(nin > 1 ? static_cast<const char16_t*>(in[1].p) : u"", n);
      return 0;
    }
    case 46:  // TTS setting query: screen reader off (see option 27)
      if (nout && out[0].n >= 16) {
        std::memset(out[0].p, 0, 16);
        out[0].n = 16;
      }
      return 0;
    case 47: case 48: case 23: case 11:
      return 0;  // accessibility text / JSON param / dictionary upload: accepted, unused on this path
    default:
      note("SceImeService method " + std::to_string(method) + " (0x199820) not reproduced");
      return 0;
  }
}

int32_t async_update(uint32_t client, const void* param456, size_t size, int32_t* count, Buf* out, int nout) {  // method 4
  std::lock_guard<std::recursive_mutex> lk(g_lock);
  if (param456 && size == 456) post_command(0, static_cast<int32_t>(client), param456, 456);  // +184 = srv+120
  // 0x19B420: up to out[0].n / 312 records.
  *count = 0;
  if (!nout) return 0;
  std::lock_guard<std::mutex> l2(g_srv->events_mutex);
  size_t n = std::min<size_t>(out[0].n / 312, g_srv->events.size());
  for (size_t i = 0; i < n; ++i) {
    std::memcpy(static_cast<uint8_t*>(out[0].p) + i * 312, g_srv->events.front().b, 312);
    g_srv->events.pop_front();
  }
  *count = static_cast<int32_t>(n);
  return 0;
}

// OnImeEventNative 0x144CF0.
int32_t on_ime_event_native(int32_t id, const std::string& text) {
  json::Value v;
  if (json::parse(v, text.data(), text.size()) != 0) {
    log::warn("ime", "OnImeEvent: Failed to parse UI event, id={}", id);
    return static_cast<int32_t>(0x80BC00FFu);
  }
  if (id == 1) {
    ButtonEvent e;  // 0x146750
    e.text = utf16(v.member("inputString").string());
    e.control = static_cast<int32_t>(v.member("controlEventId").integer());
    e.type = static_cast<uint32_t>(v.member("buttonType").integer());
    const json::Value& co = v.member("controlOption");
    e.full_width = static_cast<int32_t>(co.member("fullWidth").integer());
    e.shift = static_cast<int32_t>(co.member("shiftState").integer());
    for (const json::Value& k : v.member("keycodeInfo").array())
      e.keys.push_back({static_cast<uint16_t>(k.member("keycode").integer()), static_cast<uint16_t>(k.member("character").integer()),
                        static_cast<uint32_t>(k.member("status").integer())});
    if (!g_initialized) return 0;
    std::lock_guard<std::recursive_mutex> lk(g_lock);
    switch (e.control) {
      case 65542: case 65580: case 65581: case 65582: return 0;
      case 65551: case 65552: case 65555: case 65556: case 65557: case 65558: case 65559:
        note("OnImeEvent control " + std::to_string(e.control) + " (copy / options / selection / paste) not reproduced");
        return 0;
      default: g_fw->post_button_event(e); return 0;
    }
  }
  if (id == 3) {
    note("unit custom event (0x113B70) not reproduced");
    return 0;
  }
  log::warn("ime", "OnImeEvent: Invalid unit event, id={}", id);
  return 0;
}

void post_keyboard_record(const void*) {}

void set_ui_event_delegates(void*, void* ui_event) {
  if (!g_initialized) return;
  std::lock_guard<std::recursive_mutex> lk(g_lock);
  if (ui_event) g_fw->ui_event = ui_event;
}

void on_change_ui_status(int32_t status) {  // 0x145D80 -> 0x12A940 -> 0x11B7C0
  if (!g_initialized) return;
  std::lock_guard<std::recursive_mutex> lk(g_lock);
  log::info("ime", "current UI state is {}, new UI state is {}", g_ui->ui_status, status);
  g_ui->ui_status = status;
  g_fw->ui_ready();
}

void on_change_unit(int32_t lang, int32_t type, int32_t method, int32_t plugin, int32_t device) {  // 0x145CF0 -> 0x112F90
  if (!g_initialized) return;
  std::lock_guard<std::recursive_mutex> lk(g_lock);
  UnitClient* u = get_unit(*g_fw, UnitId{static_cast<uint32_t>(lang), static_cast<uint32_t>(type), static_cast<uint32_t>(method),
                                         static_cast<uint32_t>(plugin)});
  (void)device;
  if (!u) return;
  if (g_fw->state == 1 || g_ui->ui_status == 1) {
    g_fw->request_activate(u);
    g_fw->unit = u;
  }
}

void update_position_and_form(const void* f) {  // 0x146310 -> 0x122880
  if (!g_initialized) return;
  std::lock_guard<std::recursive_mutex> lk(g_lock);
  uint32_t v[7];
  std::memcpy(v, f, 28);
  float x, y;
  std::memcpy(&x, &v[1], 4);
  std::memcpy(&y, &v[2], 4);
  Framework& fw = *g_fw;
  const bool same = fw.form_x == 2 * x && fw.form_y == 2 * y && fw.form_w == static_cast<int32_t>(2 * v[5]) &&
                    fw.form_h == static_cast<int32_t>(2 * v[6]);
  if (fw.state == 1 && g_ui->ui_status == 1 && !same)
    fw.emit(rec_rect(3, 2 * x, 2 * y, 2 * v[5], 2 * v[6]));
  std::memcpy(fw.form, v, 28);
  fw.form_x = 2 * x;
  fw.form_y = 2 * y;
  fw.form_w = static_cast<int32_t>(2 * v[5]);
  fw.form_h = static_cast<int32_t>(2 * v[6]);
}

void set_init_input_mode(int32_t m) {
  if (!g_initialized) return;
  std::lock_guard<std::recursive_mutex> lk(g_lock);
  g_ui->init_input_mode = m;
}
void set_has_focused_candidate(bool b) {
  if (!g_initialized) return;
  std::lock_guard<std::recursive_mutex> lk(g_lock);
  g_ui->has_focused_candidate = b;
}
void set_shell_ui_app_id(int32_t id) {
  if (!g_initialized) return;
  std::lock_guard<std::recursive_mutex> lk(g_lock);
  g_fw->app_id = id;
}
void set_front_scene(int32_t app, int32_t scene) {
  if (!g_initialized) return;
  std::lock_guard<std::recursive_mutex> lk(g_lock);
  g_fw->front_app = app;
  g_fw->front_scene = scene;
}
void set_game_scene_shown(bool shown, int32_t app) {
  if (!g_initialized) return;
  std::lock_guard<std::recursive_mutex> lk(g_lock);
  if (shown) g_fw->game_app = app;
}
int32_t send_data_status() { return g_initialized ? g_fw->send_status : 0; }
int32_t confirm_text_length() { return g_initialized ? g_ui->confirm_len : 0; }
void notify_key_event(int32_t device) {  // 0x1454C0: event 543 with the device type
  if (!g_initialized) return;
  std::lock_guard<std::recursive_mutex> lk(g_lock);
  g_fw->emit(rec_value(543, static_cast<uint32_t>(device)));
}

// Keyboard bridge used by ime_keyboard.cpp.
namespace detail {
std::recursive_mutex& lock() { return g_lock; }
bool ime_open() { return g_initialized && g_fw->state == 1 && g_ui->ui_status == 1; }
int32_t ime_user() { return g_fw ? g_fw->user() : -1; }
void button_event(uint32_t type, int32_t id, const std::u16string& text) {  // 0x1148C0 + unit queue / 0x13C160
  ButtonEvent e;
  e.control = id;
  e.type = type;
  e.text = text;
  if (g_fw->unit) {
    g_fw->unit->queue.push_back(e);
    g_fw->deliver(g_fw->unit);
  }
}
void emit_value(uint32_t id, uint32_t v) { g_fw->emit(rec_value(id, v)); }
void emit_simple(uint32_t id) { g_fw->emit(rec_simple(id)); }
int32_t& hwkb_state() { return g_fw->hwkb_state; }
uint32_t type() { return g_fw->type(); }
bool filter_ok(const std::u16string& s) { return g_fw->filter(s).size() == s.size(); }
void post_psm_object(json::Object o) { post_psm(std::move(o)); }
uint32_t unit_lang() { return g_fw->unit ? g_fw->unit->id.lang : 0; }
uint32_t unit_type_() { return g_fw->unit ? g_fw->unit->id.type : 0; }
}  // namespace detail

}  // namespace ssn::eboot::ime
