// Sce.PlayStation.Core.Error (libScePsm.sprx, FW 4.03).
#include "common/mono_util.h"
#include "host/boundary.h"
#include "libScePsm/imaging/psm_common.h"

namespace ssn::psm {
namespace {

// GetExceptionInfoNative(out string message, out string param) — 0x2f5c70: hands out the
// pending exception message (0x542400) and parameter name (0x542418) — null when empty —
// then clears both; returns 0. The host keeps the message (psm_common); parameter names
// are not recorded yet, so param is always null.
int32_t GetExceptionInfoNative(MonoString** message, MonoString** param) {
  const std::string m = get_exception_info();
  store_ref(message, m.empty() ? nullptr : new_string(m));
  store_ref(param, nullptr);
  set_exception_info("");
  return 0;
}
SSN_ICALL("Sce.PlayStation.Core.Error::GetExceptionInfoNative", GetExceptionInfoNative);

}  // namespace
}  // namespace ssn::psm
