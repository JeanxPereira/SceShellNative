// The parts of libSceJson2 (sce::Json, FW 4.03 /system/common/lib/libSceJson2.sprx) that
// libSceUrlConfigResolver uses: Value, the text parser and the serializer.
//
// Value types are sce::Json::ValueType (Value::getType, 0x4240, reads the dword at +28):
//   0 null, 1 boolean, 2 integer (int64), 3 unsigned integer (uint64), 4 real (double),
//   5 string, 6 array, 7 object.
// sce::Json::Object is an ordered map keyed by the C string (insert at 0xe090 walks a
// red-black tree comparing with strcmp), so objects iterate and serialize in byte order of
// their keys; a repeated key in the text overwrites the earlier member (the parser assigns
// through Object::operator[], 0xd8b0).
#pragma once
#include <cstdint>
#include <map>
#include <memory>
#include <string>
#include <vector>

namespace ssn::urlcfg::json {

enum class Type : uint32_t { Null = 0, Boolean = 1, Integer = 2, UInteger = 3, Real = 4, String = 5, Array = 6, Object = 7 };

class Value;
using Array = std::vector<Value>;
using Object = std::map<std::string, Value>;

class Value {
 public:
  Value() = default;

  Type type() const { return type_; }

  // Value::operator bool (0x2070): null -> false, boolean -> its value, (u)integer -> != 0,
  // real -> != 0.0, string -> non-empty, array/object -> true.
  bool truthy() const;

  // Value::getValue(key) (HwDt5lD9Bfo, 0x52f0 -> 0x4f30): the member when this is an object
  // holding the key, otherwise the null value.
  const Value& member(const std::string& key) const;

  // Value::getObject (IlsmvBtMkak, 0x4750): the object, or the library's empty static object
  // for any other type. Value::getString (epJ6x2LV0kU, 0x4470): likewise for strings.
  const Object& object() const;
  const std::string& string() const;
  uint64_t uinteger() const { return u_; }  // Value::getUInteger storage (+16)
  // Value::getArray (ONT8As5R1ug): the array, or an empty one for any other type.
  const Array& array() const;
  // Value::getInteger (sn4HNCtNRzY): the 64-bit number of an (unsigned) integer value — the
  // parser stores non-negative numbers as unsigned in the same 8 bytes — and 0 otherwise.
  int64_t integer() const;

  static Value make_null() { return Value(); }
  static Value make_bool(bool b);
  static Value make_int(int64_t v);
  static Value make_uint(uint64_t v);
  static Value make_real(double v);
  static Value make_string(std::string s);
  static Value make_array(Array a);
  static Value make_object(Object o);

 private:
  friend class Serializer;
  Type type_ = Type::Null;
  bool b_ = false;
  int64_t i_ = 0;
  uint64_t u_ = 0;
  double r_ = 0.0;
  std::shared_ptr<const std::string> s_;
  std::shared_ptr<const Array> a_;
  std::shared_ptr<const Object> o_;
};

// SCE_JSON parser error: the reader ran out of input or the text is malformed
// (Parser::parse, 0x66a0, returns the reader status or 0x80848101).
constexpr int32_t kErrorParse = static_cast<int32_t>(0x80848101u);

// sce::Json::Parser::parse(Value&, const char* buf, size_t size) (S5JxQnoGF3E, 0x67a0).
// Parses one value from the buffer (leading tab/LF/CR/space skipped; trailing bytes are not
// examined). Returns 0 or kErrorParse.
int32_t parse(Value& out, const char* buf, size_t size);

// sce::Json::Value::serialize(String&) (R7FDWtcN6f8, 0x3760 -> 0x2cb0): compact text.
std::string serialize(const Value& v);

}  // namespace ssn::urlcfg::json
