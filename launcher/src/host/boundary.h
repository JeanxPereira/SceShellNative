// The host boundary: the set of firmware native entry points the launcher serves.
//
//  * icalls   — [MethodImpl(InternalCall)] methods, registered with
//               mono_add_internal_call under Mono's name ("Ns.Type::Method" or, for
//               overloads, "Ns.Type::Method(int,string&)").
//  * pinvokes — [DllImport] methods. The launcher exports a C function for each one;
//               SceShellNative.Prep points the method at "__Internal" with that
//               export's symbol as its entry point.
//  * libraries — [DllImport] libraries passed through to a real native library
//               (e.g. the open-source Yoga the firmware's SceFacebook.Yoga.sprx is
//               built from); entries the library does not export are stubbed.
//
// Everything else is rewritten by Prep into a logging stub (see Boundary.Unimplemented).
#pragma once
#include <string>
#include <vector>

namespace ssn::host {

struct IcallEntry {
  const char* name;
  const void* fn;
};

struct PinvokeEntry {
  const char* lib;
  const char* entry;
  const char* symbol;
};

struct LibraryEntry {
  std::string lib;
  std::string path;
};

class Boundary {
 public:
  static Boundary& get();

  void add_icall(const char* name, const void* fn) { icalls_.push_back({name, fn}); }
  void add_pinvoke(const char* lib, const char* entry, const char* symbol) { pinvokes_.push_back({lib, entry, symbol}); }
  void add_library(std::string lib, std::string path) { libraries_.push_back({std::move(lib), std::move(path)}); }
  // Declared host policies that change how Prep prepares the firmware assemblies.
  void add_policy(std::string p) { policies_.push_back(std::move(p)); }

  // Text table consumed by SceShellNative.Prep (one entry per line).
  std::string table() const;
  // mono_add_internal_call for every icall, dllmap for every passthrough library.
  void install_into_mono() const;

  const std::vector<IcallEntry>& icalls() const { return icalls_; }
  // Host path of a passthrough library ("" when none is configured).
  std::string library_path(const std::string& lib) const {
    for (const auto& l : libraries_)
      if (l.lib == lib) return l.path;
    return {};
  }

 private:
  std::vector<IcallEntry> icalls_;
  std::vector<PinvokeEntry> pinvokes_;
  std::vector<LibraryEntry> libraries_;
  std::vector<std::string> policies_;
};

struct IcallRegistrar {
  IcallRegistrar(const char* name, const void* fn) { Boundary::get().add_icall(name, fn); }
};
struct PinvokeRegistrar {
  PinvokeRegistrar(const char* lib, const char* entry, const char* symbol) { Boundary::get().add_pinvoke(lib, entry, symbol); }
};

// First-hit log for rewritten stubs; called from SceShellNative.Boundary::Unimplemented.
void note_unimplemented(const std::string& id);
void dump_unimplemented_counts();

}  // namespace ssn::host

#define SSN_CAT2(a, b) a##b
#define SSN_CAT(a, b) SSN_CAT2(a, b)

// Registers an icall implementation: SSN_ICALL("Ns.Type::Method", fn);
#define SSN_ICALL(name, fn) \
  static ::ssn::host::IcallRegistrar SSN_CAT(ssn_icall_reg_, __LINE__)(name, reinterpret_cast<const void*>(&fn))

// Declares an exported P/Invoke implementation and registers it:
//   SSN_PINVOKE("libSceBootManager", "SetBootTimeStamp", int, ssn_BootManager_SetBootTimeStamp, (int t)) { ... }
#define SSN_PINVOKE(lib, entry, ret, sym, params)                                   \
  extern "C" __declspec(dllexport) ret sym params;                                   \
  static ::ssn::host::PinvokeRegistrar SSN_CAT(ssn_pinv_reg_, __LINE__)(lib, entry, #sym); \
  extern "C" __declspec(dllexport) ret sym params
