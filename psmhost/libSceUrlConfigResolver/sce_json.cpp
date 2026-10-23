// sce::Json subset (libSceJson2.sprx, FW 4.03) — see sce_json.h. Addresses are offsets in
// that module.
#include "libSceUrlConfigResolver/sce_json.h"

#include <cstdio>
#include <cstdlib>
#include <cstring>

#include "host/boundary.h"

namespace ssn::urlcfg::json {

namespace {

const Value kNullValue;
const Object kEmptyObject;
const std::string kEmptyString;

}  // namespace

bool Value::truthy() const {
  switch (type_) {
    case Type::Null: return false;
    case Type::Boolean: return b_;
    case Type::Integer: return i_ != 0;
    case Type::UInteger: return u_ != 0;
    case Type::Real: return r_ != 0.0;
    case Type::String: return s_ && !s_->empty();
    default: return true;
  }
}

const Value& Value::member(const std::string& key) const {
  if (type_ != Type::Object || !o_) return kNullValue;
  auto it = o_->find(key);
  return it == o_->end() ? kNullValue : it->second;
}

const Object& Value::object() const { return (type_ == Type::Object && o_) ? *o_ : kEmptyObject; }
const Array& Value::array() const {
  static const Array kEmptyArray;
  return (type_ == Type::Array && a_) ? *a_ : kEmptyArray;
}
int64_t Value::integer() const {
  if (type_ == Type::Integer) return i_;
  if (type_ == Type::UInteger) return static_cast<int64_t>(u_);
  return 0;
}
const std::string& Value::string() const { return (type_ == Type::String && s_) ? *s_ : kEmptyString; }

Value Value::make_bool(bool b) { Value v; v.type_ = Type::Boolean; v.b_ = b; return v; }
Value Value::make_int(int64_t x) { Value v; v.type_ = Type::Integer; v.i_ = x; return v; }
Value Value::make_uint(uint64_t x) { Value v; v.type_ = Type::UInteger; v.u_ = x; return v; }
Value Value::make_real(double x) { Value v; v.type_ = Type::Real; v.r_ = x; return v; }
Value Value::make_string(std::string s) { Value v; v.type_ = Type::String; v.s_ = std::make_shared<const std::string>(std::move(s)); return v; }
Value Value::make_array(Array a) { Value v; v.type_ = Type::Array; v.a_ = std::make_shared<const Array>(std::move(a)); return v; }
Value Value::make_object(Object o) { Value v; v.type_ = Type::Object; v.o_ = std::make_shared<const Object>(std::move(o)); return v; }

// ---------------------------------------------------------------------------------------
// Parser. The firmware reads through a byte callback with a one-character push-back
// (0x6800 for a memory buffer: next byte, or 0x80848101 at the end of the buffer); any
// failure makes Parser::parse return that status / 0x80848101.
// ---------------------------------------------------------------------------------------
namespace {

class Reader {
 public:
  Reader(const char* p, size_t n) : p_(p), end_(p + n) {}
  // Next byte; false at the end of the buffer.
  bool next(unsigned char& c) {
    if (back_) { back_ = false; c = last_; return ok_; }
    if (p_ >= end_) { ok_ = false; last_ = 0; c = 0; return false; }
    last_ = static_cast<unsigned char>(*p_++);
    ok_ = true;
    c = last_;
    return true;
  }
  void unget() { back_ = true; }
  // Skips tab, LF, CR and space (bit mask 0x100002600); false at the end of the buffer.
  bool skip_ws(unsigned char& c) {
    for (;;) {
      if (!next(c)) return false;
      if (!(c == 9 || c == 10 || c == 13 || c == 32)) return true;
    }
  }

 private:
  const char* p_;
  const char* end_;
  unsigned char last_ = 0;
  bool back_ = false;
  bool ok_ = true;
};

bool parse_value(Reader& r, Value& out);

int hex_digit(unsigned char c) {
  if (c >= '0' && c <= '9') return c - '0';
  if (c >= 'A' && c <= 'F') return c - 'A' + 10;
  if (c >= 'a' && c <= 'f') return c - 'a' + 10;
  return -1;
}

bool read_hex4(Reader& r, int& v) {
  v = 0;
  for (int i = 0; i < 4; ++i) {
    unsigned char c;
    if (!r.next(c)) return false;
    const int d = hex_digit(c);
    if (d < 0) { r.unget(); return false; }
    v = v * 16 + d;
  }
  return true;
}

// \uXXXX (0x5440): a high surrogate must be followed by \u and a low surrogate; the code
// point is appended as UTF-8.
bool parse_unicode_escape(Reader& r, std::string& s) {
  int cp;
  if (!read_hex4(r, cp)) return false;
  if ((cp & 0xF800) == 0xD800) {
    if (cp > 0xDBFF) return false;
    unsigned char c;
    if (!r.next(c) || c != '\\') return false;
    if (!r.next(c) || c != 'u') return false;
    int lo;
    if (!read_hex4(r, lo)) return false;
    if ((lo & 0xFC00) != 0xDC00) return false;
    cp = 0x10000 + ((cp << 10) & 0xFFC00) + (lo & 0x3FF);
  }
  if (cp > 0x7F) {
    if (cp > 0x7FF) {
      if (cp > 0xFFFF) {
        s += static_cast<char>(((cp >> 18) & 7) | 0xF0);
        s += static_cast<char>(((cp >> 12) & 0x3F) | 0x80);
      } else {
        s += static_cast<char>((cp >> 12) | 0xE0);
      }
      s += static_cast<char>(((cp >> 6) & 0x3F) | 0x80);
    } else {
      s += static_cast<char>((cp >> 6) | 0xC0);
    }
    s += static_cast<char>((cp & 0x3F) | 0x80);
  } else {
    s += static_cast<char>(cp);
  }
  return true;
}

// String body after the opening quote (0x57e0): bytes below 0x20 are rejected, bytes from
// 0x80 up are copied as they are; escapes \" \/ \\ \b \f \n \r \t \uXXXX.
bool parse_string_body(Reader& r, std::string& s) {
  for (;;) {
    unsigned char c;
    if (!r.next(c)) return false;
    if (c <= 31) { r.unget(); return false; }
    if (c == '"') return true;
    if (c != '\\') { s += static_cast<char>(c); continue; }
    if (!r.next(c)) return false;
    switch (c) {
      case '"': case '/': case '\\': s += static_cast<char>(c); break;
      case 'b': s += '\b'; break;
      case 'f': s += '\f'; break;
      case 'n': s += '\n'; break;
      case 'r': s += '\r'; break;
      case 't': s += '\t'; break;
      case 'u': if (!parse_unicode_escape(r, s)) return false; break;
      default: return false;
    }
  }
}

// Number (0x63d0). Collects digits and + - . E e ('+', '.', 'E', 'e' and '-' mark the
// number as real when they are not the first character); a non-real number is accumulated
// as an unsigned 64-bit value with the firmware's wrap check and becomes UInteger, or
// Integer when it starts with '-' and fits; everything else is converted as a real.
bool parse_number(Reader& r, Value& out) {
  std::string buf;
  bool real = false;
  for (;;) {
    unsigned char c;
    r.next(c);  // at the end of the buffer c is 0, which ends the number
    if (c >= '0' && c <= '9') { buf += static_cast<char>(c); continue; }
    if (c == '+' || c == '.' || c == 'E' || c == 'e' || c == '-') {
      if (!buf.empty()) real = true;
      buf += static_cast<char>(c);
      continue;
    }
    // 'i' starts "-inf" only with the extended (InitParameter2) mode; see serialize().
    r.unget();
    break;
  }
  if (!real) {
    const char* p = buf.c_str();
    if (*p == '-') ++p;
    uint64_t v = 0;
    bool overflow = false;
    for (; *p; ++p) {
      const uint64_t prev = v;
      v = static_cast<uint64_t>(static_cast<int64_t>(static_cast<signed char>(*p))) + 10 * v - 48;
      if (v < prev) { overflow = true; break; }
    }
    if (!overflow) {
      if (buf[0] != '-') { out = Value::make_uint(v); return true; }
      if (v <= 0x8000000000000000ull) { out = Value::make_int(static_cast<int64_t>(0 - v)); return true; }
    }
  }
  // The firmware converts with its own routine (0x75b0), not transcribed; the C library's
  // strtod is used instead.
  host::note_unimplemented("libSceJson2 real-number conversion (0x75b0) approximated with strtod");
  out = Value::make_real(std::strtod(buf.c_str(), nullptr));
  return true;
}

bool expect_literal(Reader& r, const char* rest) {
  for (const char* p = rest; *p; ++p) {
    unsigned char c;
    if (!r.next(c)) return false;
    if (c != static_cast<unsigned char>(*p)) { r.unget(); return false; }
  }
  return true;
}

bool parse_array(Reader& r, Value& out) {  // 0x5980
  Array a;
  unsigned char c;
  if (!r.skip_ws(c)) return false;
  if (c == ']') { out = Value::make_array(std::move(a)); return true; }
  r.unget();
  for (;;) {
    Value v;
    if (!parse_value(r, v)) return false;
    a.push_back(std::move(v));
    if (!r.skip_ws(c)) return false;
    if (c != ',') break;
  }
  if (c != ']') return false;
  out = Value::make_array(std::move(a));
  return true;
}

bool parse_object(Reader& r, Value& out) {  // 0x60e0
  Object o;
  unsigned char c;
  if (!r.skip_ws(c)) return false;
  if (c == '}') { out = Value::make_object(std::move(o)); return true; }
  r.unget();
  for (;;) {
    if (!r.skip_ws(c) || c != '"') return false;
    std::string key;
    if (!parse_string_body(r, key)) return false;
    if (!r.skip_ws(c) || c != ':') return false;
    Value v;
    if (!parse_value(r, v)) return false;
    o[key] = std::move(v);
    if (!r.skip_ws(c)) return false;
    if (c != ',') break;
  }
  if (c != '}') return false;
  out = Value::make_object(std::move(o));
  return true;
}

bool parse_value(Reader& r, Value& out) {  // 0x5bc0
  unsigned char c;
  if (!r.skip_ws(c)) return false;
  switch (c) {
    case '[': return parse_array(r, out);
    case '{': return parse_object(r, out);
    case 't': if (!expect_literal(r, "rue")) return false; out = Value::make_bool(true); return true;
    case 'f': if (!expect_literal(r, "alse")) return false; out = Value::make_bool(false); return true;
    case 'n': if (!expect_literal(r, "ull")) return false; out = Value::make_null(); return true;
    case '"': {
      std::string s;
      if (!parse_string_body(r, s)) return false;
      out = Value::make_string(std::move(s));
      return true;
    }
    default:
      if (c != '-' && (c < '0' || c > '9')) return false;
      r.unget();
      return parse_number(r, out);
  }
}

}  // namespace

int32_t parse(Value& out, const char* buf, size_t size) {
  Reader r(buf, size);
  Value v;
  if (!parse_value(r, v)) return kErrorParse;
  out = std::move(v);
  return 0;
}

// ---------------------------------------------------------------------------------------
// Serializer (0x2cb0): no whitespace; object members in key order; strings escaped by
// 0x3530; null/boolean/numbers by Value::toString (0x26f0).
// ---------------------------------------------------------------------------------------
class Serializer {
 public:
  static void quote(std::string& o, const std::string& s) {  // 0x3530
    o += '"';
    for (unsigned char c : s) {
      switch (c) {
        case 0x08: o += "\\b"; break;
        case 0x09: o += "\\t"; break;
        case 0x0A: o += "\\n"; break;
        case 0x0C: o += "\\f"; break;
        case 0x0D: o += "\\r"; break;
        case '"': o += "\\\""; break;
        case '/': o += "\\/"; break;
        case '\\': o += "\\\\"; break;
        default:
          if (c >= 0x20 && c != 0x7F) {
            o += static_cast<char>(c);
          } else {
            char u[8];
            std::snprintf(u, sizeof(u), "\\u%04x", c);
            o += u;
          }
      }
    }
    o += '"';
  }

  static void write(std::string& o, const Value& v) {
    char tmp[256];
    switch (v.type_) {
      case Type::Null: o += "null"; break;
      case Type::Boolean: o += v.b_ ? "true" : "false"; break;
      case Type::Integer: std::snprintf(tmp, sizeof(tmp), "%lld", static_cast<long long>(v.i_)); o += tmp; break;
      case Type::UInteger: std::snprintf(tmp, sizeof(tmp), "%llu", static_cast<unsigned long long>(v.u_)); o += tmp; break;
      case Type::Real: {
        // Value::toString formats reals with "%1.17g" when the library was initialised
        // with InitParameter2 and with "%f" otherwise, then drops an all-zero fraction.
        // Which initialisation the ShellUI process uses is not traced; "%f" is used.
        host::note_unimplemented("libSceJson2 real serialization (init mode of the process not traced)");
        std::snprintf(tmp, sizeof(tmp), "%f", v.r_);
        if (char* dot = std::strchr(tmp, '.')) {
          const char* q = dot + 1;
          while (*q == '0') ++q;
          if (*q == 0) *dot = 0;
        }
        o += tmp;
        break;
      }
      case Type::String: quote(o, v.string()); break;
      case Type::Array: {
        o += '[';
        bool first = true;
        if (v.a_) {
          for (const Value& e : *v.a_) {
            if (!first) o += ',';
            first = false;
            write(o, e);
          }
        }
        o += ']';
        break;
      }
      case Type::Object: {
        o += '{';
        bool first = true;
        for (const auto& [k, e] : v.object()) {
          if (!first) o += ',';
          first = false;
          quote(o, k);
          o += ':';
          write(o, e);
        }
        o += '}';
        break;
      }
    }
  }
};

std::string serialize(const Value& v) {
  std::string o;
  Serializer::write(o, v);
  return o;
}

}  // namespace ssn::urlcfg::json
