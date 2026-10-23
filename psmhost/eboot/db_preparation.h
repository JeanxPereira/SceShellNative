// The ShellUI eboot's DbPreparation (FW 4.03, eboot.bin 0x33000..0x3B600, vsh/common_lib
// db_preparation + shell_ui glue): the manager that prepares the shell's databases when the
// system starts and when users are created/deleted, the units it prepares, the notifier that
// tells listeners (and SceShellCore) that the databases are ready, and the two worker threads
// the managed Sce.Vsh.DbPreparationWrapper waits on. See docs/app-db.md.
#pragma once
#include <cstdint>

#include "shellcore/db_preparation_server_common.h"

namespace ssn::eboot::dbp {

using Event = ssn::dbp::Event;

// A unit implementation (the impl object a unit handle points at, +48): prepare = vtable+16,
// terminate = vtable+24.
class UnitImpl {
 public:
  virtual ~UnitImpl() = default;
  virtual int32_t prepare(const Event& ev) = 0;
  virtual int32_t terminate(const Event& ev) = 0;
};

// The unit handles of the eboot (0x38740).
enum class UnitSlot {
  AppDbCex,            // unk_EBF1A8 "app.db" (CEX list, Kratos list 1); impl EBA448 (0x520 / 0x5E0)
  AppDbKratos,         // unk_EBF1E0 "app.db" (Kratos list 2); impl EBA480
  MediaEventListener,  // unk_EBF248 "media_event_listener"; impl 21EF858 (0x7238A0)
};

// sub_38C90 / sub_38CD0: an implementation registers itself into its handle (the eboot does
// it from static initialisers 0xC80 / 0x723A40). The AppDb client (eboot 0x520: InitClient
// 0x30E230, AppBrowse builders) registers into AppDbCex.
void register_unit_impl(UnitSlot slot, UnitImpl* impl);

// A notifier listener (vtable +16 on-initialized, +24 on-terminating).
class Listener {
 public:
  virtual ~Listener() = default;
  virtual void on_initialized(const Event& ev) = 0;
  virtual void on_terminating(const Event& ev) = 0;
};
// 0x35130 / notifier 0x35870: adds the listener to list `which` (0 or 1) unless already listed.
void register_listener(Listener* l, int32_t which);
// 0x35150 / notifier 0x35990.
void unregister_listener(Listener* l);

// 0x33B00: a callback run for every invalid (deleted) user id by DeleteInvalidUser (0x33E90).
int32_t register_invalid_user_callback(void (*cb)(int32_t user_id));

// initializeSystem (0xC440) step 0x56C0: installs the pre-notification hook and, on a retail
// (CEX, non-kiosk) unit, starts "SceDbpPreparationThread" with the CEX INITIALIZE event.
int32_t initialize_system_step();
// initializeFrameworkDependentModules (0xD050) step 0x5850: registers the CEX user-event
// callback (0x33570: user created -> CREATE_USER, deleted -> DESTROY_USER).
int32_t initialize_framework_dependent_step();

}  // namespace ssn::eboot::dbp
