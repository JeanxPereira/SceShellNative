// ShellUI boot-manager initialisation thread (eboot.bin 0xeea0,
// shell_ui_boot_manager_init.cpp). The eboot starts it alongside the PSM runtime; it
// initialises the shell's native subsystems and publishes progress through the boot
// event flag the managed BootEvent waits on:
//
//   initializeSystem()                       (0xc440)
//   registerSystemCallTable()                (0xce80: registers the eboot's icalls, then sets RegisterSystemCallTable = 0x2)
//   sceSysmoduleLoadModuleInternal(MOUSE, HMD, SCREEN_SHOT, HMD2)
//   SetBootEvent(LoadSysmoduleScreenShot = 0x10)
//   initializeSystemAfterPsm()               (0xcc10)
//   initializeFrameworkDependentModules()    (0xd050, ends with SetBootEvent(InitializeFrameworkDependent = 0x4))
//   load libSceContentListController / InitializeUsbStorage
//   SetBootEvent(BackgroundInit = 0x1)
//
// On the host the eboot's icalls are registered with Mono before the managed entry
// point runs, and the native subsystems behind the other steps are either part of the
// host boundary or not provided; the thread therefore publishes the same events in
// the same order once the runtime is up.
#include <thread>

#include "core/log.h"
#include "eboot/db_preparation.h"
#include "libkernel/event_flag.h"

namespace ssn::eboot {

void set_boot_event(uint64_t e);  // boot_manager.cpp
int32_t enable_user_psm_events();  // user_psm_event.cpp

void start_boot_manager_init() {
  std::thread([] {
    // initializeSystem (0xC440) step 0x56C0: DbPreparation starts SceDbpPreparationThread.
    if (int32_t r = dbp::initialize_system_step(); r < 0)
      log::error("eboot", "initializeSystem: DbPreparation (0x56C0) 0x{:08x}", static_cast<uint32_t>(r));
    set_boot_event(0x2);   // RegisterSystemCallTable
    set_boot_event(0x10);  // LoadSysmoduleScreenShot
    // initializeFrameworkDependentModules (0xD050): after 0x4F6F30 (not reproduced),
    // UserServiceWrapper._EnablePsmEvent (0xDF30 -> 0x4A6A20): user events become PSM events.
    if (int32_t r = enable_user_psm_events(); r < 0)
      log::error("eboot", "initializeFrameworkDependentModules: EnablePsmEvent 0x{:08x}", static_cast<uint32_t>(r));
    // Step 0x5850: DbPreparation user events.
    dbp::initialize_framework_dependent_step();
    set_boot_event(0x4);   // InitializeFrameworkDependent
    set_boot_event(0x1);   // BackgroundInit
    log::info("eboot", "boot-manager init: RegisterSystemCallTable, LoadSysmoduleScreenShot, InitializeFrameworkDependent, BackgroundInit set");
  }).detach();
}

}  // namespace ssn::eboot
