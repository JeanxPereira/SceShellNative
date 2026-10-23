// libSceIme VSH client — see ime_vsh_client.h. Module offsets of libSceIme.sprx (FW 4.03).
#include "libSceIme/ime_vsh_client.h"

#include <algorithm>
#include <cmath>
#include <cstring>
#include <mutex>
#include <string>
#include <vector>

#include "core/log.h"
#include "eboot/ime_server.h"
#include "host/boundary.h"

namespace ssn::ime_client {
namespace {

using eboot::ime::Buf;

void note(const std::string& s) { host::note_unimplemented("libSceIme: " + s); }

constexpr int32_t kErrBusy = static_cast<int32_t>(0x80BC0001u);
constexpr int32_t kErrNotOpened = static_cast<int32_t>(0x80BC0002u);
constexpr int32_t kErrConnection = static_cast<int32_t>(0x80BC0004u);
constexpr int32_t kErrType = static_cast<int32_t>(0x80BC0011u);
constexpr int32_t kErrLanguages = static_cast<int32_t>(0x80BC0012u);
constexpr int32_t kErrEnterLabel = static_cast<int32_t>(0x80BC0013u);
constexpr int32_t kErrHandler = static_cast<int32_t>(0x80BC0022u);
constexpr int32_t kErrWork = static_cast<int32_t>(0x80BC0020u);
constexpr int32_t kErrParam = static_cast<int32_t>(0x80BC0030u);

// sub_D0 -> qword_40020: {u32 sdkVersion, u32 isApp}. The ShellUI process links the 4.03 SDK and
// is not an application process.
constexpr uint32_t kSdkVersion = 0x04030000;
constexpr uint32_t kIsApp = 0;

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

// The client object libSceIme builds in the caller's work area (offsets in the comments).
struct Client {
  void* work = nullptr;
  bool waiting_open = true;      // +1
  bool last_insert_space = false;  // +2
  uint8_t cmd[48] = {};          // +4..+51 command block (method 5)
  bool dirty = false;            // +52
  void* arg = nullptr;           // +56
  Handler handler = nullptr;     // +64
  void* filter = nullptr;        // +72
  VshParam param{};              // +3208
  uint32_t ipmi = 0;             // +3464 (the host server's client id)
  bool pending = false;          // +3476
  std::vector<uint8_t> result;   // async method 4 result (records)
  int32_t result_count = 0;
  uint32_t ctx[2] = {};          // +7688 / +7692 (method 10)
  uint32_t window_type = 0;      // +7696
  bool offline = false;          // +7649
};

std::mutex g_mutex;   // unk_40138
bool g_opened = false;  // byte_40131
Client* g_client = nullptr;

uint32_t opt(const Client& c) { return rd<uint32_t>(c.param.b, 0x18); }

// sub_8B30 EnsureParamInternal: validation and the 456-byte internal parameter.
int32_t ensure_param(const VshParam& p, uint8_t out[456]) {
  const uint8_t* b = p.b;
  if (!rd<void*>(b, 0x28)) return kErrWork;
  const uint64_t langs = rd<uint64_t>(b, 0x08);
  if (langs & 0xFFFFFCFE00E00000ull) return kErrLanguages;  // non-app mask
  const uint32_t forced = rd<uint32_t>(b, 0x10);
  if (forced > 1) return kErrParam;
  const uint32_t type = rd<uint32_t>(b, 0x14);
  if (!(type < 5 || (type >= 256 && type <= 258))) return kErrType;
  const uint32_t option = rd<uint32_t>(b, 0x18);
  if ((option & 5) == 5) return kErrParam;
  if (!rd<void*>(b, 0x30)) return kErrHandler;
  if (reinterpret_cast<uintptr_t>(rd<void*>(b, 0x28)) & 3) return kErrWork;
  if (b[0x40] > 3) return kErrEnterLabel;
  if ((rd<uint32_t>(b, 0xB8) & 3) == 3) return static_cast<int32_t>(0x80BC001Cu);
  const uint32_t input_mode = rd<uint32_t>(b, 0xC4);
  if (input_mode > 4) return kErrParam;
  if (rd<uint32_t>(b, 0x70) > 2) return kErrParam;
  if (type != 0 && input_mode == 4) return kErrType;

  std::memset(out, 0, 456);
  const bool latin = type >= 1 && type <= 3;
  wr<uint64_t>(out, 0, latin ? 0 : langs);
  wr<uint32_t>(out, 8, latin ? 0 : forced);
  wr<uint32_t>(out, 12, type);
  wr<uint32_t>(out, 16, option | ((rd<uint32_t>(b, 0xB8) & 1) ? 0x10u : 0u));
  wr<uint32_t>(out, 20, b[0x40]);
  const float scale = (option & 0x4000) ? 1.0f : 2.0f;
  wr<float>(out, 24, rd<float>(b, 0x44) * scale);
  wr<float>(out, 28, rd<float>(b, 0x48) * scale);
  wr<uint32_t>(out, 32, rd<uint32_t>(b, 0x4C));
  wr<uint32_t>(out, 36, rd<uint32_t>(b, 0x50));
  std::memcpy(out + 40, b + 0x54, 16);
  wr<int64_t>(out, 56, rd<int64_t>(b, 0x68));
  wr<uint32_t>(out, 64, rd<uint32_t>(b, 0x70));
  wr<int32_t>(out, 72, rd<int32_t>(b, 0x78));
  wr<uint32_t>(out, 80, rd<uint32_t>(b, 0x7C));
  wr<uint64_t>(out, 88, rd<uint64_t>(b, 0x80));
  const uint32_t ext = rd<uint32_t>(b, 0x88);
  wr<uint32_t>(out, 96, (ext & ~1u) | ((option & 0x10000000) ? 2u : 0u));
  wr<uint32_t>(out, 100, (option & 0x10000000) ? 1u : rd<uint32_t>(b, 0x8C));
  wr<float>(out, 104, rd<float>(b, 0x90));
  wr<uint32_t>(out, 124, rd<uint32_t>(b, 0xB8));
  wr<uint32_t>(out, 128, rd<uint32_t>(b, 0xBC));
  wr<uint64_t>(out, 132, (static_cast<uint64_t>(kIsApp) << 32) | kSdkVersion);
  wr<uint32_t>(out, 152, rd<uint32_t>(b, 0xA0));
  wr<uint64_t>(out, 160, rd<uint64_t>(b, 0xA8));
  out[168] = b[0xC0];
  out[169] = b[0xC1];
  wr<uint32_t>(out, 172, input_mode);
  wr<int32_t>(out, 176, rd<int32_t>(b, 0xC8));
  wr<int32_t>(out, 180, rd<int32_t>(b, 0xCC));
  wr<int32_t>(out, 192, rd<int32_t>(b, 0xD0));
  if (const char* s = rd<const char*>(b, 0xE0)) std::strncpy(reinterpret_cast<char*>(out + 196), s, 128);
  if (const char* s = rd<const char*>(b, 0xE8)) std::strncpy(reinterpret_cast<char*>(out + 325), s, 128);
  out[454] = b[0xF0];
  return 0;
}

void emit(Client& c, const Event& e) { c.handler(c.arg, &e); }

// sceImeVshUpdate's record -> handler event conversion (0xaab0 PostEventToHandler).
void post_record(Client& c, const uint8_t* r) {
  const uint32_t id = rd<uint32_t>(r, 0);
  if (c.waiting_open && id != 0 && id != 538) return;
  Event e{};
  e.id = id;
  const bool raw = (opt(c) & 0x4000) != 0;
  auto scaled_rect = [&] {
    float x = rd<float>(r, 8), y = rd<float>(r, 12);
    uint32_t w = rd<uint32_t>(r, 16), h = rd<uint32_t>(r, 20);
    if (!raw) {
      x = std::floor(x * 0.5f);
      y = std::floor(y * 0.5f);
      w >>= 1;
      h >>= 1;
    }
    std::memcpy(reinterpret_cast<uint8_t*>(&e) + 8, &x, 4);
    std::memcpy(reinterpret_cast<uint8_t*>(&e) + 12, &y, 4);
    e.u16 = w;
    e.u20 = h;
  };
  char16_t text[2049];
  auto inline_text = [&] {
    std::memset(text, 0, sizeof text);
    std::memcpy(text, r + 8, std::min<size_t>(244, 120 * 2));
    text[120] = 0;
    e.p8 = reinterpret_cast<uint64_t>(text);
  };
  switch (id) {
    case 0:
      c.waiting_open = false;
      scaled_rect();
      break;
    case 3:
      scaled_rect();
      break;
    case 512:
      inline_text();
      e.u16 = rd<uint32_t>(r, 252);
      break;
    case 513:
    case 514:
      if (c.filter) note("client text filter (records 513 / 514 with a filter) not reproduced");
      inline_text();
      if (id == 514) {
        size_t n = 0;
        while (n < 120 && text[n]) ++n;
        c.last_insert_space = n && text[n - 1] == u' ';
      }
      break;
    case 520: case 543: case 10: case 14: case 18: case 1283:
      e.p8 = rd<uint32_t>(r, 8);
      break;
    case 525: case 539:
      e.p8 = r[8];
      break;
    case 17:
      c.window_type = rd<uint32_t>(r, 8);
      e.p8 = c.window_type;
      break;
    case 526: case 537: case 540: case 541:
      return;  // internal to libSceIme
    case 532:
      if (c.last_insert_space) {
        Event b{};
        b.id = 515;
        emit(c, b);
      }
      return;
    case 529: case 1282: case 1537: case 1288: case 1539:
      note("record " + std::to_string(id) + " (string fetch / close with text / filter request) not reproduced");
      return;
    default:
      break;  // passed through with no parameters
  }
  emit(c, e);
}

}  // namespace

int32_t vsh_open(void** handle, const VshParam* p, const char16_t* init_text) {
  std::lock_guard<std::mutex> lk(g_mutex);
  if (g_opened) return kErrBusy;
  uint8_t internal[456];
  if (int32_t r = ensure_param(*p, internal); r < 0) return r;
  auto* c = new Client();
  c->param = *p;
  c->work = rd<void*>(p->b, 0x28);
  std::memset(c->work, 0, 10240);
  c->arg = rd<void*>(p->b, 0x20);
  c->handler = rd<Handler>(p->b, 0x30);
  c->filter = rd<void*>(p->b, 0x38);
  // sub_A330: connect info {+0 1, +4 sdkInfo, +12 u8, +16 callerSceneId, +20 userId}.
  uint8_t info[24] = {};
  wr<uint32_t>(info, 0, 1);
  wr<uint64_t>(info, 4, (static_cast<uint64_t>(kIsApp) << 32) | kSdkVersion);
  wr<int32_t>(info, 16, rd<int32_t>(p->b, 0xC8));
  wr<int32_t>(info, 20, rd<int32_t>(internal, 72));
  const int32_t r = eboot::ime::connect(info, &c->ipmi);
  if (r < 0) {
    delete c;
    if (r == static_cast<int32_t>(0x80020003u)) return static_cast<int32_t>(0x80BC0008u);
    return kErrConnection;
  }
  // method 46: TTS setting {userId, type, languages, sdkInfo, extOpt & 0x1000}
  uint8_t m46[32] = {};
  wr<int32_t>(m46, 0, rd<int32_t>(p->b, 0x78));
  wr<uint32_t>(m46, 4, rd<uint32_t>(p->b, 0x14));
  wr<uint64_t>(m46, 8, rd<uint64_t>(p->b, 0x08));
  wr<uint64_t>(m46, 16, (static_cast<uint64_t>(kIsApp) << 32) | kSdkVersion);
  m46[24] = (rd<uint32_t>(p->b, 0x88) & 0x1000) != 0;
  uint8_t out46[16] = {};
  Buf in{m46, 32}, out{out46, 16};
  int32_t ret = 0;
  eboot::ime::sync_method(c->ipmi, 46, &in, 1, &ret, &out, 1);
  // method 45: the initial text
  uint32_t len = 0;
  if (init_text)
    while (len < 2048 && init_text[len]) ++len;
  Buf in45[2] = {{&len, 4}, {const_cast<char16_t*>(init_text ? init_text : u""), 2 * len + 2}};
  eboot::ime::sync_method(c->ipmi, 45, in45, 2, &ret, nullptr, 0);
  g_opened = true;
  g_client = c;
  *handle = c->work;
  log::info("ime", "sceImeVshOpen: type={} option=0x{:x} maxTextLength={} user={}", rd<uint32_t>(p->b, 0x14),
            rd<uint32_t>(p->b, 0x18), rd<uint32_t>(p->b, 0x7C), rd<int32_t>(p->b, 0x78));
  return 0;
}

int32_t vsh_update(void* handle) {
  std::unique_lock<std::mutex> lk(g_mutex);
  Client* c = g_client;
  if (!g_opened || !c || c->work != handle) return kErrNotOpened;
  uint8_t internal[456];
  ensure_param(c->param, internal);
  int32_t ret = 0;
  if (c->dirty) {  // method 5 (command block) and method 10 (context)
    Buf b5{c->cmd, 48}, b10{c->ctx, 8};
    eboot::ime::sync_method(c->ipmi, 5, &b5, 1, &ret, nullptr, 0);
    eboot::ime::sync_method(c->ipmi, 10, &b10, 1, &ret, nullptr, 0);
    std::memset(c->cmd, 0, sizeof c->cmd);
    c->dirty = false;
  }
  std::vector<uint8_t> records;
  int32_t count = 0;
  if (c->pending) {  // tryGetResult(4, ...)
    records.swap(c->result);
    count = c->result_count;
    c->pending = false;
  }
  if (!c->pending) {  // async invoke of method 4 with the internal param
    c->result.assign(3120, 0);
    std::vector<uint8_t> bulk(4098);
    Buf out[2] = {{c->result.data(), c->result.size()}, {bulk.data(), bulk.size()}};
    eboot::ime::async_update(c->ipmi, internal, 456, &c->result_count, out, 2);
    c->pending = true;
  }
  for (int32_t i = 0; i < count && g_opened; ++i) post_record(*c, records.data() + 312 * i);
  return 0;
}

int32_t vsh_close(void* handle) {
  std::lock_guard<std::mutex> lk(g_mutex);
  Client* c = g_client;
  if (!g_opened || !c || c->work != handle) return kErrNotOpened;
  uint8_t zero[456] = {};
  Buf in{zero, 456};
  int32_t ret = 0;
  eboot::ime::sync_method(c->ipmi, 2, &in, 1, &ret, nullptr, 0);
  eboot::ime::disconnect(c->ipmi);
  std::memset(c->work, 0, 8920);
  delete c;
  g_client = nullptr;
  g_opened = false;
  log::info("ime", "sceImeVshClose");
  return 0;
}

int32_t vsh_update_context(void* handle, const char16_t* text, uint32_t len, int32_t caret) {
  std::lock_guard<std::mutex> lk(g_mutex);
  Client* c = g_client;
  if (!g_opened || !c || c->work != handle) return kErrNotOpened;
  const uint32_t max = rd<uint32_t>(c->param.b, 0x7C);
  uint32_t flags;
  if (len == 0) flags = 0x1F;
  else if (len > max) flags = 0x20;
  else if (caret == 0) flags = 0x16;
  else if (static_cast<uint32_t>(caret) == len) flags = 0x9;
  else flags = 0;
  c->ctx[0] = len;
  c->ctx[1] = flags;
  // auto-capitalisation (command block flag 2, +12) from the character before the caret
  int32_t i = std::min<int32_t>(caret, static_cast<int32_t>(len)) - 1;
  bool after_space = false;
  while (i >= 0 && text && text[i] == u' ') {
    after_space = true;
    --i;
  }
  uint32_t cap = 0;
  if (i < 0 || text[i] == u'\n' || text[i] == u'\r' || text[i] == u'¿') cap = 1;
  else if ((text[i] == u'.' || text[i] == u'!' || text[i] == u'?') && after_space) cap = 1;
  uint32_t f = rd<uint32_t>(c->cmd, 0) | 2;
  wr<uint32_t>(c->cmd, 0, f);
  wr<uint32_t>(c->cmd, 8, cap);
  c->dirty = true;
  return 0;
}

int32_t vsh_clear_preedit(void* handle) {
  std::lock_guard<std::mutex> lk(g_mutex);
  Client* c = g_client;
  if (!g_opened || !c || c->work != handle) return kErrNotOpened;
  uint8_t blk[48] = {};
  wr<uint32_t>(blk, 0, 1);
  wr<uint32_t>(blk, 4, 1);
  Buf in{blk, 48};
  int32_t ret = 0;
  eboot::ime::sync_method(c->ipmi, 5, &in, 1, &ret, nullptr, 0);
  return 0;
}

int32_t vsh_set_caret_index_in_preedit(void* handle, int32_t idx) {
  std::lock_guard<std::mutex> lk(g_mutex);
  Client* c = g_client;
  if (!g_opened || !c || c->work != handle) return kErrNotOpened;
  wr<uint32_t>(c->cmd, 0, rd<uint32_t>(c->cmd, 0) | 4);
  wr<int32_t>(c->cmd, 12, idx);
  c->dirty = true;
  return 0;
}

int32_t vsh_inform_text(void* handle, uint32_t method, const char16_t* text, uint32_t len, const int32_t* args, int nargs) {
  std::lock_guard<std::mutex> lk(g_mutex);
  Client* c = g_client;
  if (!g_opened || !c || c->work != handle) return kErrNotOpened;
  Buf in[2] = {{const_cast<int32_t*>(args), static_cast<size_t>(nargs) * 4}, {const_cast<char16_t*>(text), 2 * len + 2}};
  int32_t ret = 0;
  eboot::ime::sync_method(c->ipmi, method, in, 2, &ret, nullptr, 0);
  return ret;
}

int32_t vsh_set_geometry(void* handle, uint32_t method, const void* geom16) {
  std::lock_guard<std::mutex> lk(g_mutex);
  Client* c = g_client;
  if (!g_opened || !c || c->work != handle) return kErrNotOpened;
  Buf in{const_cast<void*>(geom16), 16};
  int32_t ret = 0;
  eboot::ime::sync_method(c->ipmi, method, &in, 1, &ret, nullptr, 0);
  return 0;
}

int32_t vsh_set_panel_position(void* handle, uint32_t ha, uint32_t va, float x, float y) {
  std::lock_guard<std::mutex> lk(g_mutex);
  Client* c = g_client;
  if (!g_opened || !c || c->work != handle) return kErrNotOpened;
  const float s = (opt(*c) & 0x4000) ? 1.0f : 2.0f;
  uint8_t b[28] = {};
  wr<uint32_t>(b, 0, 1);
  wr<float>(b, 4, x * s);
  wr<float>(b, 8, y * s);
  wr<uint32_t>(b, 12, ha);
  wr<uint32_t>(b, 16, va);
  Buf in{b, 28};
  int32_t ret = 0;
  eboot::ime::sync_method(c->ipmi, 17, &in, 1, &ret, nullptr, 0);
  return ret;
}

int32_t vsh_get_panel_position_and_form(void* handle, uint32_t out7[7]) {
  std::lock_guard<std::mutex> lk(g_mutex);
  Client* c = g_client;
  if (!g_opened || !c || c->work != handle) return kErrNotOpened;
  uint32_t o[7] = {};
  Buf out{o, 28};
  int32_t ret = 0;
  eboot::ime::sync_method(c->ipmi, 16, nullptr, 0, &ret, &out, 1);
  if (ret < 0) return ret;
  if (!(opt(*c) & 0x4000)) {
    float x, y;
    std::memcpy(&x, &o[1], 4);
    std::memcpy(&y, &o[2], 4);
    x = std::floor(x * 0.5f);
    y = std::floor(y * 0.5f);
    std::memcpy(&o[1], &x, 4);
    std::memcpy(&o[2], &y, 4);
    o[5] >>= 1;
    o[6] >>= 1;
  }
  std::memcpy(out7, o, 28);
  return 0;
}

}  // namespace ssn::ime_client
