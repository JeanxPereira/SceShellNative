#include "mono/mono_host.h"

#include <mono/jit/jit.h>
#include <mono/metadata/assembly.h>
#include <mono/metadata/environment.h>
#include <mono/metadata/mono-config.h>
#include <mono/metadata/mono-debug.h>
#include <mono/metadata/threads.h>

#include <chrono>
#include <cstdlib>
#include <thread>

#include "core/log.h"
#include "host/boundary.h"
#include "libSceRegMgr/regmgr.h"

namespace ssn::mono {
namespace {

std::string to_utf8(MonoString* s) {
  if (!s) return {};
  char* u = mono_string_to_utf8(s);
  std::string r = u ? u : "";
  mono_free(u);
  return r;
}

// SceShellNative.Boundary::Unimplemented(string) — target of every rewritten stub.
void boundary_unimplemented(MonoString* id) { host::note_unimplemented(to_utf8(id)); }

}  // namespace

int run_program(const RuntimePaths& paths, const std::string& assembly, const std::vector<std::string>& args) {
  const std::string lib = paths.mono_root + "/lib";
  const std::string etc = paths.mono_root + "/etc";
  mono_set_dirs(lib.c_str(), etc.c_str());
  mono_set_assemblies_path(paths.assemblies_dir.c_str());
  mono_config_parse(nullptr);
  mono_debug_init(MONO_DEBUG_FORMAT_MONO);
  // Diagnostics: SSN_MONO_TRACE=<spec> is Mono's --trace=<spec> (calls of the matching
  // managed methods are printed to stdout), e.g. "M:Ns.Type:Method,M:Ns.Outer/Inner:Method".
  if (const char* t = std::getenv("SSN_MONO_TRACE")) mono_jit_set_trace_options(t);

  MonoDomain* domain = mono_jit_init_version("SceShellNative", "v4.0.30319");
  if (!domain) {
    log::error("mono", "mono_jit_init_version failed");
    return -1;
  }
  mono_add_internal_call("SceShellNative.Boundary::Unimplemented", reinterpret_cast<const void*>(&boundary_unimplemented));
  host::Boundary::get().install_into_mono();
  regmgr::set_runtime_ready();

  MonoAssembly* asm_ = mono_domain_assembly_open(domain, assembly.c_str());
  if (!asm_) {
    log::error("mono", "cannot open {}", assembly);
    return -1;
  }
  std::vector<char*> argv;
  std::vector<std::string> storage{assembly};
  storage.insert(storage.end(), args.begin(), args.end());
  for (auto& s : storage) argv.push_back(s.data());
  // Diagnostics: SSN_THREAD_DUMP_AFTER=<seconds> prints every managed thread's stack
  // (Mono's thread dump) once that many seconds after start, then every 10 s.
  if (const char* d = std::getenv("SSN_THREAD_DUMP_AFTER")) {
    const int secs = std::atoi(d);
    std::thread([secs] {
      std::this_thread::sleep_for(std::chrono::seconds(secs));
      for (;;) {
        log::info("mono", "requesting managed thread dump");
        mono_threads_request_thread_dump();
        std::this_thread::sleep_for(std::chrono::seconds(10));
      }
    }).detach();
  }
  log::info("mono", "exec {}", assembly);
  mono_jit_exec(domain, asm_, static_cast<int>(argv.size()), argv.data());
  const int code = mono_environment_exitcode_get();
  host::dump_unimplemented_counts();
  // mono_jit_cleanup is intentionally not called: the firmware leaves foreground
  // threads running and a clean shutdown would wait on them.
  return code;
}

}  // namespace ssn::mono
