// HOST POLICY: the process of an application SceShellCore's launcher starts.
//
// On the console spawnApp (SceShellCore 0x46ECF0) mounts the title and has the kernel start
// its /app0/eboot.bin (sceApplicationSpawn2); the kernel reports the process's end, and the
// launcher's onAppExit (0x436460) follows. The PC cannot run a PS5 executable. A title can
// instead name a program of the PC in <cache>/host/apps.json:
//
//   { "<titleId>": { "exe": "<path>", "args": ["..."], "cwd": "<dir>", "watch": "<dir>" },
//     "<titleId>": { "uri": "<url>", "watch": "<dir>" } }
//
// "exe" starts the program, with "args" as a list of arguments or as command-line text; "uri"
// hands the URL to the PC's shell (a store client's launch link). The application runs while the started process runs and, when "watch" is given,
// while any process whose executable lies under that directory runs (a store client starts
// the game itself: the host waits up to 120 s for it to appear).
#pragma once
#include <cstdint>
#include <functional>
#include <string>
#include <vector>

namespace ssn::shellcore::lnc_host {

// Starts the title's program. `on_exit` runs on a host thread once the application has ended.
// No entry for the title: 0x80020002 (ENOENT, as the kernel's spawn of a title without its
// executable fails). The program fails to start: 0x80020002 as well.
int32_t spawn(const std::string& title_id, const std::vector<std::string>& args, std::function<void()> on_exit);

}  // namespace ssn::shellcore::lnc_host
