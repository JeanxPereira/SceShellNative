// Embedded Mono 6.8 runtime (desktop build, upstream branch 2019-10 — the branch the
// firmware's libmonosgen was built from; MONO_CORLIB_VERSION ABB721D6-…).
#pragma once
#include <string>
#include <vector>

namespace ssn::mono {

struct RuntimePaths {
  std::string mono_root;       // <Mono>/ : lib/mono/4.5 is the desktop BCL, etc/ the config
  std::string assemblies_dir;  // prepared firmware assemblies (MONO_PATH)
};

// Initializes the JIT, installs the host boundary and runs <assembly> as the main
// program on the calling thread. Returns the managed exit code.
int run_program(const RuntimePaths& paths, const std::string& assembly, const std::vector<std::string>& args);

}  // namespace ssn::mono
