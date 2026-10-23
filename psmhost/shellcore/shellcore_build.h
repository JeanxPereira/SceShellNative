// Which SceShellCore build the host stands in for: the dump's /system/vsh/SceShellCore.elf,
// identified by its GNU build-id. Addresses in psmhost/shellcore cite the FW 4.03 retail build
// (J01736823). The FW 4.03 devkit build (J01736824) is the same sources with devkit-only paths;
// where those change what the host reproduces, the code asks devkit_build() and cites both
// builds (work/ida_r7diff/report.md §5).
#pragma once

namespace ssn::shellcore {

// True when the dump's SceShellCore is the FW 4.03 devkit build (J01736824). An unknown build is
// treated as the retail reference (logged once).
bool devkit_build();

}  // namespace ssn::shellcore
