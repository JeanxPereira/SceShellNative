// Sce.Vsh.ProfileCache.CacheOperator internal calls (eboot.bin, FW 4.03, profile cache server).
//
// The eboot's profile cache server object (qword_EF5460) is created by
// sceProfileCacheServerInitialize (0x4F42F0: "SceProfileCacheServer" thread, 0x4F53A0 object,
// 0x4F5400 / 0x4F4C30 start), reached through 0x4F4080 from BackgroundInit(InitNetwork)
// (0xD240 case 3). The host's BackgroundInit reproduces only the NpPbtcLib step of that case
// (eboot/background_init.cpp), so the object is never created.
#include <cstdint>

#include "host/boundary.h"

namespace ssn::eboot {
namespace {

// _UpdateNative(int userId, int option) — 0x4F4740: with the server object, an
// {"UserId", "Type": "Update", "Option"} message is posted to it (event 39) and 0 returned;
// without it 0x811E0001.
int32_t UpdateNative(int32_t, int32_t) {
  static const bool noted = (host::note_unimplemented(
                                 "eboot profile cache server (sceProfileCacheServerInitialize 0x4F42F0, BackgroundInit(InitNetwork)) "
                                 "not created: CacheOperator._UpdateNative -> 0x811E0001"),
                             true);
  (void)noted;
  return static_cast<int32_t>(0x811E0001u);
}
SSN_ICALL("Sce.Vsh.ProfileCache.CacheOperator::_UpdateNative", UpdateNative);

}  // namespace
}  // namespace ssn::eboot
