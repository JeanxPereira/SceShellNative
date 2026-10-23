// Raw views over managed arrays passed to icalls (System.Array / T[]).
#pragma once
#include <cstddef>
#include <cstdint>

#include <mono/metadata/class.h>
#include <mono/metadata/object.h>

namespace ssn::psm {

struct ArrayView {
  uint8_t* data = nullptr;
  size_t length = 0;     // elements
  size_t elem_size = 0;  // bytes per element
  size_t bytes() const { return length * elem_size; }
};

inline ArrayView view(MonoArray* a) {
  ArrayView v;
  if (!a) return v;
  MonoClass* k = mono_object_get_class(reinterpret_cast<MonoObject*>(a));
  v.elem_size = static_cast<size_t>(mono_array_element_size(k));
  v.length = mono_array_length(a);
  v.data = reinterpret_cast<uint8_t*>(mono_array_addr_with_size(a, static_cast<int>(v.elem_size), 0));
  return v;
}

}  // namespace ssn::psm
