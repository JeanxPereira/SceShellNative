// "NpTrophyApi" / "NpTrophy2Api" CheckCallback P/Invokes of Sce.Vsh.Np.Trophy / Trophy2
// (ShellUI eboot, FW 4.03). ShellUI polls them every frame of its NP job.
//
// The eboot's {name, fn} tables jump straight to the libraries:
//   _sceNpTrophySystemCheckCallback  (eboot 0xcf0000) -> libSceNpTrophy  JvSjnwiki2A (0x3530)
//   _sceNpTrophy2SystemCheckCallback (eboot 0xcf0380) -> libSceNpTrophy2 1HrxhQoPEBs (0x25e0)
// Both have the same shape:
//   library not initialised (dword_301A0 / dword_18088 == 0) -> 0x80551601 / 0x80553901;
//   otherwise 0x96C0 / 0x6880: a zero-timeout wait on the client's event queues (title-update,
//   then title-synced; 0xDD70/0xDE60 -> 0xF070); for each pending event the registered
//   callback (+896 / +912) is called; then 0.
// The libraries initialise themselves at load (.init_proc -> 0xF330 -> 0xEC60: IPMI client of
// "SceNpTrophy"), so on the console the answer is 0, with callbacks dispatched only when the
// trophy service has queued a title-update / title-synced event. Those events come from
// trophy synchronisation with the server; the host has no network and no NP sign-in (and the
// title callbacks are not registered: their Register* entries stay stubs), so no event is
// pending and the answer is 0 — the managed stub's former default, now stated with its source.
#include <cstdint>

#include "host/boundary.h"

namespace ssn::np_trophy {

SSN_PINVOKE("NpTrophyApi", "_sceNpTrophySystemCheckCallback", int32_t, ssn_NpTrophy_SystemCheckCallback, ()) { return 0; }

SSN_PINVOKE("NpTrophy2Api", "_sceNpTrophy2SystemCheckCallback", int32_t, ssn_NpTrophy2_SystemCheckCallback, ()) { return 0; }

}  // namespace ssn::np_trophy
