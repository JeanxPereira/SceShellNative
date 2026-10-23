// sceNetGetIfList — "libSysBridgeNet" (libSceSysBridge {name, fn} table 0x789B0 -> the libSceNet
// import HoV-GJyx7YY), called by Sce.Vsh.SysBridge.Net.GetIfList (e.g. the home's DebugInfoView
// getIpMacHost with SCE_NET_IF_NAME_DBG0 and SCE_NET_IF_NAME_PHYSICAL, one entry each).
//
// libSceNet (FW 4.03; same code in both dumps) sceNetGetIfList 0x4B0:
//   library not initialised (dword_400A0, set by the module's init 0x4440) -> 0x804100C8;
//   else r = 0x5E00(name, list, n); r < 0 also sets the library errno (not visible here).
// 0x5E00:
//   name == -1 (SCE_NET_IF_NAME_NONE): the kernel's interface list straight into the caller's
//     buffer (0x94A0: syscall netgetiflist(list, n), EINTR retried) -> the number of entries;
//   otherwise only (list == null && n == 0) or (list != null && n == 1) are accepted, else
//     0x80410116; a 4800-byte buffer (10 entries of 480 bytes, SceNetIfList) is allocated
//     (failure 0x804100C9), the kernel list read into it, and the entry whose 16-byte name equals
//     names[name] (table 0x3C000: lo0 eth0 eth1 dbg0 wlan0 wlan1 gbe0 bt0 phone0 pppoe0; an index
//     >= 10 compares with "") is copied to the caller (480 bytes) -> 1; no such entry -> 0 and the
//     caller's buffer is not written.
//
// HOST VALUE (network hardware): the PC has no PS5 network device, so the host kernel's interface
// list holds no eth / dbg / wlan / gbe / bt / phone / pppoe interface: every named lookup other
// than lo0 finds no entry and answers 0 without writing. The loopback record lo0 (what the kernel
// fills in for it) is not traced: a lookup that would return it (name 0, or the whole list with
// name -1) is logged and answers 0 entries.
#include <cstdint>

#include "host/boundary.h"

namespace ssn::sysbridge {
namespace {

constexpr int32_t kErrInval = static_cast<int32_t>(0x80410116u);

void host_note() {
  static const bool noted = (host::note_unimplemented(
                                 "HOST VALUE kernel network interfaces: no network hardware (no eth/dbg/wlan interface)"),
                             true);
  (void)noted;
}

int32_t get_if_list(int32_t name, void* list, int32_t n) {
  host_note();
  if (name == -1) {
    host::note_unimplemented("libSceNet sceNetGetIfList: kernel lo0 record not traced; whole list answered with 0 entries");
    return 0;
  }
  if (!((list == nullptr && n == 0) || (list != nullptr && n == 1))) return kErrInval;
  if (name == 0) {
    host::note_unimplemented("libSceNet sceNetGetIfList: kernel lo0 record not traced; lo0 lookup answered as not found");
    return 0;
  }
  return 0;  // no interface of that name
}

}  // namespace

SSN_PINVOKE("libSysBridgeNet", "sceNetGetIfList", int32_t, ssn_SysBridgeNet_GetIfList,
            (int32_t name, void* list, int32_t n)) {
  return get_if_list(name, list, n);
}

}  // namespace ssn::sysbridge
