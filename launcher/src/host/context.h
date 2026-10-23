// State shared by the host implementations (set up by main before the shell runs).
#pragma once
#include <cstdint>
#include <string>

#include "core/firmware.h"

namespace ssn::host {


struct Context {
  Firmware* firmware = nullptr;
  std::string rnps_root;        // directory holding rnps/apps/<titleId>/ (installed RNPS apps)
  std::string cache_dir;        // the launcher cache (persistent console state lives here)
  uint32_t main_thread_id = 0;  // thread that runs the managed Main (PSM "main thread")
  bool cold_boot = false;       // --cold-boot: launched as SceShellCore launches ShellUI at power-on
};

Context& ctx();

}  // namespace ssn::host
