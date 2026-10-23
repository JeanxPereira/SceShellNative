// NpPapc (eboot.bin, FW 4.03): the parental-control client the ShellUI eboot links and
// publishes to managed code as "NpPapcApi" ({name, fn} table: _sceNpPapcIsPapcTarget
// 0x4BDAF0, CheckGameAgeLevel 0x4BDB50, HasGameAgeLevelRegion 0x4BDD70, CheckGamePcl
// 0x4BDE20, GetUserGamePcl 0x4BDFA0, GetUserBdDvd 0x4BE020, CheckInternetBrowserSwitch
// 0x4BE0D0, GetUserPsvrSwitch 0x4BE190, CheckContentControl 0x4BE210, NotifyRegistryChanged
// 0x4BE290, GetPapcInfoString 0x4BE2A0).
//
// The home asks CheckGameAgeLevel for every PS5 title (ReactNative.Modules.Vsh.PSN
// AccountModule.CheckParentalControl, platform 0) and draws the lock when the title is not
// bootable. The answer comes from the system's parental settings in the registry, the
// user's settings in SceUserService and the user's NP account type.
#include <cerrno>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <string>

#include "host/boundary.h"
#include "libSceNpManager/np_manager.h"
#include "libSceRegMgr/regmgr.h"
#include "shellcore/user_service.h"

namespace ssn::eboot {
namespace {

constexpr int32_t kErrInvalidArgument = static_cast<int32_t>(0x8055B605u);
constexpr int32_t kErrNpNotANumber = static_cast<int32_t>(0x8055800Eu);  // libSceNpCommon uj86YxCYid0
constexpr int32_t kErrNpJsonSyntax = static_cast<int32_t>(0x80550602u);  // HOST VALUE, see Json
// sceNpManagerIntGetAccountType results the eboot treats as "no account".
constexpr int32_t kErrNpAccount1 = static_cast<int32_t>(0x80559B0Cu);
constexpr int32_t kErrNpAccount2 = static_cast<int32_t>(0x80559B19u);

// SECURITY_PARENTAL_papc_all_ok. The eboot reads it with sceRegMgrNonSysGetInt
// (dKeshzt29G4, regmgr_call 25) and the key 0x17AC623A5B4A4093; the kernel's translation of
// non-system keys (7.00 0xffffffff80caae90: checksum, XOR tables 0xffffffff81308e70 /
// 0xffffffff813092a0) maps that key to 0x03800A00. The kernel's per-process access check on
// the entry is not modelled.
constexpr uint32_t kKeyPapcAllOk = 0x03800A00;

// libSceNpCommon's streaming JSON parser (nhgjiwPUIzI 0x23770 init, p5ZkSMRR7AU 0x23840
// parse) as the eboot uses it: the callback sees every scalar value (event 6) with its nesting
// depth (members of the top-level object or array: 1), the member name (none inside an
// array) and the value's text (numbers and strings; "" for true / false / null). Outside a
// value the parser skips every character other than '{', '[' and '"', so an empty text gives
// no event and result 0. A callback result < 0 stops the parse and is returned.
//
// HOST VALUE: the parser's own status codes for malformed text are not traced; the host stops
// there with 0x80550602 (a libSceNpCommon status, the init's invalid-buffer one). Names and
// strings longer than the eboot's 128-byte buffers reach the console's callback in pieces;
// the host passes them whole.
using OnValue = int32_t (*)(void* ctx, int depth, const char* key, const std::string& text);

class Json {
 public:
  Json(const char* s, size_t n, OnValue cb, void* ctx) : s_(s), n_(n), cb_(cb), ctx_(ctx) {}

  int32_t parse() {
    while (i_ < n_) {
      const char c = s_[i_];
      if (c != '{' && c != '[' && c != '"') {
        ++i_;
        continue;
      }
      if (const int32_t r = value(0, nullptr); r < 0) return r;
    }
    return 0;
  }

 private:
  void ws() {
    while (i_ < n_ && (s_[i_] == ' ' || s_[i_] == '\t' || s_[i_] == '\n' || s_[i_] == '\r')) ++i_;
  }

  bool string(std::string* out) {
    ++i_;  // '"'
    while (i_ < n_ && s_[i_] != '"') {
      if (s_[i_] == '\\' && i_ + 1 < n_) {
        const char e = s_[++i_];
        out->push_back(e == 'n' ? '\n' : e == 't' ? '\t' : e == 'r' ? '\r' : e == 'b' ? '\b' : e == 'f' ? '\f' : e);
        if (e == 'u') host::note_unimplemented("libSceNpCommon JSON parser \\u escapes (0x24BF0) not reproduced");
      } else {
        out->push_back(s_[i_]);
      }
      ++i_;
    }
    if (i_ >= n_) return false;
    ++i_;
    return true;
  }

  int32_t value(int depth, const char* key) {
    ws();
    if (i_ >= n_) return kErrNpJsonSyntax;
    const char c = s_[i_];
    if (c == '{' || c == '[') {
      const char close = c == '{' ? '}' : ']';
      ++i_;
      ws();
      if (i_ < n_ && s_[i_] == close) {
        ++i_;
        return 0;
      }
      for (;;) {
        std::string name;
        if (c == '{') {
          ws();
          if (i_ >= n_ || s_[i_] != '"' || !string(&name)) return kErrNpJsonSyntax;
          ws();
          if (i_ >= n_ || s_[i_] != ':') return kErrNpJsonSyntax;
          ++i_;
        }
        if (const int32_t r = value(depth + 1, c == '{' ? name.c_str() : nullptr); r < 0) return r;
        ws();
        if (i_ >= n_) return kErrNpJsonSyntax;
        if (s_[i_] == ',') {
          ++i_;
          continue;
        }
        if (s_[i_] != close) return kErrNpJsonSyntax;
        ++i_;
        return 0;
      }
    }
    std::string text;
    if (c == '"') {
      if (!string(&text)) return kErrNpJsonSyntax;
    } else if (n_ - i_ >= 4 && (!std::strncmp(s_ + i_, "true", 4) || !std::strncmp(s_ + i_, "null", 4))) {
      i_ += 4;
    } else if (n_ - i_ >= 5 && !std::strncmp(s_ + i_, "false", 5)) {
      i_ += 5;
    } else {
      // State 11: digits and + - . e I.
      const size_t from = i_;
      while (i_ < n_ && ((s_[i_] >= '0' && s_[i_] <= '9') || std::strchr("+-.eI", s_[i_]))) ++i_;
      if (i_ == from) return kErrNpJsonSyntax;
      text.assign(s_ + from, i_ - from);
    }
    return cb_(ctx_, depth, key, text);
  }

  const char* s_;
  size_t n_;
  size_t i_ = 0;
  OnValue cb_;
  void* ctx_;
};

// uj86YxCYid0 (libSceNpCommon 0x7a00): strtoul base 10; no digits, overflow or a value above
// 32 bits -> 0x8055800E.
int32_t to_u32(const std::string& text, uint32_t* out) {
  errno = 0;
  char* end = nullptr;
  const unsigned long long v = std::strtoull(text.c_str(), &end, 10);
  if (end == text.c_str() || (v == ~0ull && errno == ERANGE) || (v >> 32) != 0) return kErrNpNotANumber;
  *out = static_cast<uint32_t>(v);
  return 0;
}

// The parsed ageLevel (2056 bytes, zeroed by 0x4C2260): the "default" age, then (region, age)
// pairs; a region is the first two bytes of the member name.
struct AgeLevel {
  uint32_t default_age = 0;
  uint32_t count = 0;
  struct Entry {
    char region[4];
    uint32_t age;
  } entries[256] = {};
};

// 0x4C2440, the ageLevel callback.
int32_t on_age_level(void* ctx, int depth, const char* key, const std::string& text) {
  if (depth != 1) return 0;
  auto* a = static_cast<AgeLevel*>(ctx);
  if (!key) {
    // The console compares the absent name with "default" (strncmp on a null pointer).
    host::note_unimplemented("eboot NpPapc ageLevel callback (0x4C2440) for a value inside an array not reproduced");
    return 0;
  }
  uint32_t age = 0;
  if (const int32_t r = to_u32(text, &age); r < 0) return r;
  if (!std::strncmp(key, "default", 128)) {
    a->default_age = age;
  } else if (a->count < 256) {  // the console writes past the 2056-byte buffer instead
    AgeLevel::Entry& e = a->entries[a->count++];
    std::memcpy(e.region, key, key[0] ? 2 : 1);
    e.age = age;
  }
  return 0;
}

// 0x4C2540, the whitelist callback: a value at depth 1 equal (16 bytes) to the title id.
struct Whitelist {
  char title_id[17];
  bool found;
};

int32_t on_whitelist(void* ctx, int depth, const char* /*key*/, const std::string& text) {
  auto* w = static_cast<Whitelist*>(ctx);
  if (depth == 1 && !text.empty() && !std::strncmp(w->title_id, text.c_str(), 16)) w->found = true;
  return 0;
}

// 0x4C0F90: sceRegMgrNonSysGetInt(papc_all_ok).
int32_t papc_all_ok(bool* all_ok) {
  int32_t v = 0;
  if (const int32_t r = regmgr::get_int(kKeyPapcAllOk, &v); r < 0) return r;
  *all_ok = v != 0;
  return 0;
}

// -7XgCmEwKrs, sceUserServiceIsSharePlayClientUser.
bool is_share_play_client(int32_t user) { return (static_cast<uint32_t>(user) & 0xF0000000u) == 0x30000000u; }

// 0x4C2320: the title is in the user's whitelist (SceUserService parameter 2215, a JSON
// text of up to 2000 bytes, read through _5ZJPtZf6EaY / 0x4C1AD0).
void whitelisted(int32_t user, const char* title_id, bool* found) {
  *found = false;
  if (!title_id || !*title_id) return;
  Whitelist w{};
  std::strncpy(w.title_id, title_id, 16);
  char text[2000] = {};
  if (shellcore::users::get_str(user, shellcore::users::kParamPapcWhitelist, text, sizeof text) < 0) return;
  if (Json(text, sizeof text, on_whitelist, &w).parse() >= 0) *found = w.found;
}

// 0x4C21D0: the region the user's age limit is compared in. Share Play client, or an NP
// account type other than 1: sceNpManagerIntGetAccountCountryA (result ignored). Account type
// 1, or 0x80559B0C / 0x80559B19: 0x4C1A40, the first two bytes of SceUserService parameter
// 2245 (dkPbHlL9O8, 3 bytes). Any other error: no region.
void user_region(int32_t user, char region[8]) {
  std::memset(region, 0, 8);
  int32_t type = 0;
  int32_t r = 0;
  if (!is_share_play_client(user)) {
    r = np_manager::get_account_type(user, &type);
    if (r < 0 && r != kErrNpAccount1 && r != kErrNpAccount2) return;
  }
  if (is_share_play_client(user) || (r >= 0 && type != 1)) {
    char country[4] = {};
    np_manager::get_account_country(user, false, country);
    std::memcpy(region, country, 4);
    return;
  }
  char s[3] = {};
  if (shellcore::users::get_str(user, shellcore::users::kParamGameAgeLimitRegion, s, sizeof s) >= 0) std::memcpy(region, s, 2);
}

// 0x4C1E60(user, 0): are the user's games under the age limit? Share Play client: no.
// 0x80559B0C / 0x80559B19 from GetAccountType: yes. Otherwise 0x4C1C90 by account type: 1
// yes, 2-3 and 4-5 ask NP (_70Swvw7h6ck / ossvuXednsc), others no.
int32_t age_limited(int32_t user, bool* limited) {
  *limited = false;
  if (is_share_play_client(user)) return 0;
  int32_t type = 0;
  const int32_t r = np_manager::get_account_type(user, &type);
  if (r < 0) {
    if (r == kErrNpAccount1 || r == kErrNpAccount2) *limited = true;
    return r == kErrNpAccount1 || r == kErrNpAccount2 ? 0 : r;
  }
  if (type == 1) *limited = true;
  else if (type >= 2 && type <= 5)
    host::note_unimplemented("eboot NpPapc 0x4C1C90 for NP account types 2-5 (_70Swvw7h6ck / ossvuXednsc) not reproduced");
  return 0;
}

// 0x4C05D0 (object 0xCF0A18, slot +24): the user's age limit. A kiosk unit (nQ5AHlyJghQ == 1)
// has none (-1); HOST VALUE retail unit: SceUserService parameter 2193 (oXARzvLAiyc ->
// 0x4C1770).
int32_t user_age_limit(int32_t user, int32_t* limit) {
  int32_t v = -1;
  if (const int32_t r = shellcore::users::get_int(user, shellcore::users::kParamGameAgeLimit, &v); r < 0) return r;
  *limit = v;
  return 0;
}

// 0x4C0410: may this user start a title with this age level?
int32_t user_bootable(int32_t user, const char* title_id, const AgeLevel& age, uint8_t* bootable) {
  *bootable = 0;
  if (user == -1) return kErrInvalidArgument;
  bool found = false;
  whitelisted(user, title_id, &found);
  if (found) {
    *bootable = 1;
    return 0;
  }
  char region[8];
  user_region(user, region);
  bool limited = false;
  if (const int32_t r = age_limited(user, &limited); r < 0) return r;
  bool too_young = false;
  if (limited) {
    int32_t limit = -1;
    if (const int32_t r = user_age_limit(user, &limit); r < 0) return r;
    if (limit != -1) {
      uint32_t game_age = age.default_age;
      for (uint32_t i = 0; i < age.count; ++i)
        if (!std::strncmp(region, age.entries[i].region, 2)) {
          game_age = age.entries[i].age;
          break;
        }
      too_young = static_cast<uint32_t>(limit) < game_age;
    }
  }
  *bootable = !too_young;
  return 0;
}

}  // namespace

// _sceNpPapcCheckGameAgeLevel(userId, titleId, ageLevelParamJson, ref bootable,
// ref NpPapcUserIdList restrictedUserIdList) — 0x4BDB50. Null title, flag or list ->
// 0x8055B605. bootable = 0; papc_all_ok set -> bootable. The ageLevel JSON is parsed
// (0x4C2260). One user: 0x4C0410, and a user who may not start it is reported as
// {user, -1, -1, -1}. User -1: 0x4C02A0 over the logged-in users (list first all -1, each
// restricted user added; bootable when none is).
SSN_PINVOKE("NpPapcApi", "_sceNpPapcCheckGameAgeLevel", int32_t, ssn_NpPapc_CheckGameAgeLevel,
            (int32_t user, const char* title_id, const char* age_level, uint8_t* bootable, int32_t* restricted)) {
  if (!age_level || !bootable || !restricted) return kErrInvalidArgument;
  *bootable = 0;
  bool all_ok = false;
  if (const int32_t r = papc_all_ok(&all_ok); r < 0) return r;
  if (all_ok) {
    *bootable = 1;
    return 0;
  }
  AgeLevel age;
  if (const int32_t r = Json(age_level, std::strlen(age_level) + 1, on_age_level, &age).parse(); r < 0) return r;
  if (user != -1) {
    if (const int32_t r = user_bootable(user, title_id, age, bootable); r < 0) return r;
    if (!*bootable) {
      restricted[0] = user;
      restricted[1] = restricted[2] = restricted[3] = -1;
    }
    return 0;
  }
  for (int i = 0; i < 4; ++i) restricted[i] = -1;
  int32_t logged[shellcore::users::kMaxLogin];
  shellcore::users::login_user_ids(logged);
  int n = 0;
  for (int32_t id : logged) {
    if (id == -1) continue;
    user_bootable(id, title_id, age, bootable);  // result ignored
    if (!*bootable) restricted[n++] = id;
  }
  *bootable = n == 0;
  return 0;
}

}  // namespace ssn::eboot
