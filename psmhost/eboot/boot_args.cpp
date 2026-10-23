// ShellUI's boot arguments: "liblx.a" lx::lxUT::GetBootArgumentsCount / GetBootArgument — ShellUI
// eboot (NPXS40087, FW 4.03) symbol table entries 0xCC4840 / 0xCC4850 -> 0x9150 / 0x9190, over the
// argc / argv that main (0xDFC0) stores with sub_357480 (dword_EE1C60 / qword_EE1C68).
//
// The managed side (Sce.Vsh.Lx UT.BootArguments.Utility) lists every argument and only asks
// whether "--cold-boot", "--restart-due-to-crash" or "--detected-bad-power-down" is among them;
// "--cold-boot" makes BootManager.IsColdBoot true, which enables the health warning and with it
// GlobalBackgroundState.ColdBootAnimation (ThemeColourIndex 69: the LightParticle "coldboot"
// effect, mode 3, with the large particles).
//
// Who passes them: SceShellCore's run-level change (shellcore 0x768650) starts each process of
// the level; for NPXS40087 started at run level 80 (power-on) it formats the argument block
// "--cold-boot\0" (or "--detected-bad-power-down\0--cold-boot\0" when the flag byte_1A76208,
// set by 0x75FC90 when the file systems report an unclean shutdown, is set and the import
// nQ5AHlyJghQ returns <= 0) and launches it through sceLncServiceLaunchApp (0x479760). Started
// at run level 100 (main on standby) the shell gets no arguments.
//
// HOST POLICY (--cold-boot): which of those two launches the host plays. Without the option the
// shell runs as launched at run level 100 (no arguments, the behaviour so far); with it, as at
// power-on after a clean shutdown ("--cold-boot"; the PC has no unclean-shutdown state).
// HOST VALUE: argv[0], the path the kernel gives the process, is "/system_ex/app/NPXS40087/eboot.bin";
// only sub_357480's "/host" prefix test reads it.
#include <cstdint>
#include <cstring>
#include <string>
#include <vector>

#include "host/boundary.h"
#include "host/context.h"

namespace ssn::eboot {
namespace {

const std::vector<std::string>& argv() {
  static const std::vector<std::string> args = [] {
    std::vector<std::string> a{"/system_ex/app/NPXS40087/eboot.bin"};
    if (host::ctx().cold_boot) a.push_back("--cold-boot");
    host::note_unimplemented(host::ctx().cold_boot
                                 ? "HOST POLICY boot-args=cold-boot: ShellUI argv as SceShellCore launches it at run level 80 (\"--cold-boot\")"
                                 : "HOST POLICY boot-args=none: ShellUI argv as SceShellCore launches it at run level 100 (no arguments)");
    return a;
  }();
  return args;
}

}  // namespace

// GetBootArgumentsCount() — 0x9150: argc as main received it.
SSN_PINVOKE("liblx.a", "lx::lxUT::GetBootArgumentsCount", int32_t, ssn_lx_UT_GetBootArgumentsCount, ()) {
  return static_cast<int32_t>(argv().size());
}

// GetBootArgument(index, buf, size) — 0x9190: null buf with a non-zero size -> 0xA0000002; an
// index past argc -> 0 (and buf[0] = 0 when size != 0); otherwise strlcpy(buf, argv[i], size)
// and strlen(argv[i]) (the managed caller retries with a larger buffer when that exceeds it).
SSN_PINVOKE("liblx.a", "lx::lxUT::GetBootArgument", int32_t, ssn_lx_UT_GetBootArgument, (int32_t index, char* buf, uint32_t size)) {
  if (!buf && size != 0) return static_cast<int32_t>(0xA0000002u);
  const auto& a = argv();
  if (index < 0 || static_cast<size_t>(index) >= a.size()) {  // signed compare in 0x91D1 (jle)
    if (size != 0) buf[0] = 0;
    return 0;
  }
  const std::string& s = a[static_cast<size_t>(index)];
  if (size != 0) {
    const size_t n = s.size() < size - 1 ? s.size() : size - 1;
    std::memcpy(buf, s.data(), n);
    buf[n] = 0;
  }
  return static_cast<int32_t>(s.size());
}

}  // namespace ssn::eboot
