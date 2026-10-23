// Sce.Vsh.DbRecoveryUtilityWrapper internal calls (eboot.bin, FW 4.03, DbRecovery utility).
#include <cstdint>

#include "host/boundary.h"

namespace ssn::eboot {
namespace {

// DbRecoveryCheckInitializationNative() — eboot icall 0x580090: `return 0`.
int32_t DbRecoveryCheckInitializationNative() { return 0; }
SSN_ICALL("Sce.Vsh.DbRecoveryUtilityWrapper::DbRecoveryCheckInitializationNative", DbRecoveryCheckInitializationNative);

}  // namespace
}  // namespace ssn::eboot
