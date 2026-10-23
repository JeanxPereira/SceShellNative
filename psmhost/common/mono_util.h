// Small helpers for writing icall implementations against the Mono embedding API.
#pragma once
#include <string>

#include <mono/metadata/appdomain.h>
#include <mono/metadata/object.h>

namespace ssn::psm {

inline std::string utf8(MonoString* s) {
  if (!s) return {};
  char* u = mono_string_to_utf8(s);
  std::string r = u ? u : "";
  mono_free(u);
  return r;
}

inline MonoString* new_string(const std::string& s) { return mono_string_new(mono_domain_get(), s.c_str()); }

// Stores into a managed `out string` / `ref object` slot (with the GC write barrier).
inline void store_ref(MonoString** slot, MonoString* value) { mono_gc_wbarrier_generic_store(slot, reinterpret_cast<MonoObject*>(value)); }

}  // namespace ssn::psm
