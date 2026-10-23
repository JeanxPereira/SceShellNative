#include "host/context.h"

namespace ssn::host {

Context& ctx() {
  static Context c;
  return c;
}

}  // namespace ssn::host
