// Sce.Vsh.ShellUI.AppMain.NativeCall._BackgroundInit(BackgroundInitType) — "libSceAppMain"
// BackgroundInit, eboot.bin ({name, fn} entry 0xcc4b58 -> 0xD240, init_system.cpp
// processBackgroundInit*): one native initialisation step per type (BeforeBGLayer 0,
// BeforeBasePlugin 1, BeforeFarsightUI 2, InitNetwork 3, InitCamera 4, InitVcsIf 5,
// InitWebKit 6, InitIme 7, InitBgft 8, InitDaemons 9, BeforeReadyGameBoot 10).
#include <cstdint>
#include <string>

#include "host/boundary.h"

namespace ssn::eboot {

void np_pbtc_initialize();  // np_pbtc.cpp
namespace ime { void initialize(); }  // ime_server.cpp

// Case 3 (InitNetwork) loads eight system modules (unk_B8D660) and runs 0xA680, 0xB910,
// 0xBC90, 0xA790, 0xAEF0, 0xC110, 0x4F4080, 0xBE20, 0x1C4DD0 (LoginMgr
// initializeFrameworkDependent: event bit 1), 0xA760, 0xBA80, 0xBAC0, 0xBAF0, 0xA570, 0xA650,
// 0x6E10, 0x51A0, 0x6DE0, 0x53B0. 0xA790 is the Np initialisation: NP title id
// (TJqSgUEzexM, "NPXS40087_00"), the "Np" memory pool, and the NpPbtcLib initialisation
// 0x4C2590 at its end. The host reproduces that last step only.
SSN_PINVOKE("libSceAppMain", "BackgroundInit", void, ssn_AppMain_BackgroundInit, (int32_t type)) {
  if (type == 3) {
    host::note_unimplemented(
        "eboot BackgroundInit(InitNetwork) (0xD240 case 3): only the NpPbtcLib init of 0xA790 is reproduced (NP title id, "
        "pools, sysmodules and the other steps not)");
    np_pbtc_initialize();
    return;
  }
  if (type == 7) {  // InitIme (devkit 0xD210 case 7 / retail 0xD240): the IME framework, service and update thread
    ime::initialize();
    return;
  }
  host::note_unimplemented("eboot BackgroundInit(" + std::to_string(type) + ") (0xD240) steps not reproduced");
}

}  // namespace ssn::eboot
